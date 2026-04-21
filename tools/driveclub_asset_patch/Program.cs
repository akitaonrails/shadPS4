using System.Buffers.Binary;
using System.Text;
using DriveClubFS.Resources;

if (args.Length < 2)
{
    Console.Error.WriteLine("usage: driveclub_asset_patch <input.rpk> <output.rpk>");
    return 1;
}

var inputPath = args[0];
var outputPath = args[1];
var mode = args.Length >= 3 ? args[2] : "default";

if (!File.Exists(inputPath))
{
    Console.Error.WriteLine($"missing rpk: {inputPath}");
    return 2;
}

File.Copy(inputPath, outputPath, overwrite: true);

using var pack = ResourcePack.Open(inputPath);
var fileBytes = File.ReadAllBytes(outputPath);

var patched = new List<string>();

foreach (var info in pack.ResourceInfos.Values)
{
    var name = info.Names.FirstOrDefault() ?? string.Empty;
    var rawOffset = checked((int)info.Offset);
    var rawSize = checked((int)info.Size);
    if (rawOffset < 0 || rawOffset + rawSize > fileBytes.Length)
    {
        continue;
    }

    var raw = fileBytes.AsSpan(rawOffset, rawSize);

    if (info.ResourceId.Type == ResourceTypeId.RTUID_ACTOR_DATA)
    {
        if (mode != "global-fades-only" &&
            name.Contains("PostFXConfig_", StringComparison.OrdinalIgnoreCase))
        {
            PatchScalar(raw, "TemporalFade", 0.0f, patched, $"{name}.TemporalFade");
            PatchScalar(raw, "MasterBrightness", 1.0f, patched, $"{name}.MasterBrightness");
            PatchScalar(raw, "ManualExposureLog2", 0.0f, patched, $"{name}.ManualExposureLog2");
            PatchScalar(raw, "ManualAutoMix", 0.0f, patched, $"{name}.ManualAutoMix");
            PatchScalar(raw, "WeatherOverrideMix", 0.0f, patched, $"{name}.WeatherOverrideMix");
            PatchScalar(raw, "MotionBlurLevel", 0.0f, patched, $"{name}.MotionBlurLevel");
            // Iteration 2.1 bucket A (TXAAOverallWeight / TXAAColourClamping /
            // NonBloomedAttenuation -> 0) is DISABLED. The 2026-04-21 run
            // showed that turning these off removed the natural temporal
            // decay that was fading the latched UI overlay out over time in
            // Iter 1, making the overlay permanent. They are not the
            // overlay's source — they are its decay path. Do not re-enable
            // these specific zeros without a corresponding "identify and
            // remove the overlay source" patch.
        }

        if (mode != "global-fades-only" &&
            name.Contains("PostFX_BloomOverride_", StringComparison.OrdinalIgnoreCase))
        {
            PatchScalar(raw, "TemporalFade", 0.0f, patched, $"{name}.TemporalFade");
            PatchScalar(raw, "OverrideBlend", 0.0f, patched, $"{name}.OverrideBlend");
        }

        if (mode != "global-fades-only" &&
            name.Contains("PostFX_ColourGradingOverride", StringComparison.OrdinalIgnoreCase))
        {
            PatchScalar(raw, "OverrideBlend", 0.0f, patched, $"{name}.OverrideBlend");
        }

        if (mode != "global-fades-only" &&
            (name.Contains("preracecam", StringComparison.OrdinalIgnoreCase) ||
             name.Contains("CutsceneCamera_", StringComparison.OrdinalIgnoreCase) ||
             name.Contains("WorldCamera_", StringComparison.OrdinalIgnoreCase) ||
             name.Contains("track_preview", StringComparison.OrdinalIgnoreCase) ||
             name.Contains("time_lapse", StringComparison.OrdinalIgnoreCase) ||
             name.Contains("time_lace", StringComparison.OrdinalIgnoreCase)))
        {
            PatchScalar(raw, "Fade", 0.0f, patched, $"{name}.Fade");
        }
    }

    if (info.ResourceId.Type == ResourceTypeId.RTUID_ANIMATIONLIB &&
        mode != "global-fades-only" &&
        info.SourceAssetPaths.Any(p => p.Contains("india_posteffects.lvl", StringComparison.OrdinalIgnoreCase)))
    {
        PatchAsciiString(
            raw,
            "colourcuberemap_india_interior_night_linear.dds",
            "colourcuberemap_india_interior_linear.dds",
            patched,
            $"{name}.interior_remap");
        PatchAsciiString(
            raw,
            "colourcuberemap_india_night_linear.dds",
            "colourcuberemap_india_linear.dds",
            patched,
            $"{name}.exterior_remap");

        // Iteration 2 bucket C (animationlib scalar-curve overrides) is
        // currently DISABLED. The 2026-04-21 run using
        // PatchNamedTrackKeyframes caused a null-ptr read in the game's
        // .rpk parser at race-loading. The 0x2c/0x4c/0x6c offset assumption
        // is only safe for the tracks we hand-verified; applying it blindly
        // to animlib curves with different strides corrupts struct layout.
        // Keep this block empty until a safer keyframe-row decoder exists.
    }

    if (info.ResourceId.Type == ResourceTypeId.RTUID_ANIMATIONLIB &&
        mode != "global-fades-only" &&
        info.SourceAssetPaths.Any(p => p.Contains("prerace_cams.lvl", StringComparison.OrdinalIgnoreCase)))
    {
        PatchFadeTracks(raw, patched, $"{name}.FadeTrack");
        // Iteration 2 bucket B (prerace animlib Transition tracks) is
        // currently DISABLED for the same reason as bucket C above.
        // PatchTransitionTracks reuses the Fade offset layout and crashed
        // the game's parser. Re-enable only with a targeted per-sequence
        // decoder rather than a global ASCII-name sweep.
    }

    if (info.ResourceId.Type == ResourceTypeId.RTUID_ANIMATIONLIB &&
        info.SourceAssetPaths.Any(p => p.Contains("globaldata.lvl", StringComparison.OrdinalIgnoreCase)))
    {
        PatchNamedFadeSequence(raw, "FadeOut", patched, $"{name}.FadeOut");
        PatchNamedFadeSequence(raw, "FadeIn", patched, $"{name}.FadeIn");
        PatchNamedFadeSequence(raw, "FadeIn_Fast", patched, $"{name}.FadeIn_Fast");
        PatchNamedFadeSequence(raw, "FadeOut_Fast", patched, $"{name}.FadeOut_Fast");
        if (mode != "global-fades-only")
        {
            PatchNamedScalarSequence(raw, "RTT_BLUR_ON", "TemporalFade", 0.0f, patched, $"{name}.RTT_BLUR_ON.TemporalFade");
            PatchNamedScalarSequence(raw, "RTT_BLUR_OFF", "TemporalFade", 0.0f, patched, $"{name}.RTT_BLUR_OFF.TemporalFade");
            PatchNamedScalarSequence(raw, "RTT_BLUR_ON", "OverrideBlend", 0.0f, patched, $"{name}.RTT_BLUR_ON.OverrideBlend");
            PatchNamedScalarSequence(raw, "RTT_BLUR_OFF", "OverrideBlend", 0.0f, patched, $"{name}.RTT_BLUR_OFF.OverrideBlend");
            PatchNamedScalarSequence(raw, "RTT_GRADING_ON", "OverrideBlend", 0.0f, patched, $"{name}.RTT_GRADING_ON.OverrideBlend");
            PatchNamedScalarSequence(raw, "RTT_GRADING_OFF", "OverrideBlend", 0.0f, patched, $"{name}.RTT_GRADING_OFF.OverrideBlend");
        }
    }
}

Console.WriteLine($"patched {patched.Count} entries");
foreach (var item in patched)
{
    Console.WriteLine(item);
}

File.WriteAllBytes(outputPath, fileBytes);
return 0;

static void PatchScalar(Span<byte> raw, string fieldName, float value, List<string> patched, string label)
{
    var nameBytes = Encoding.ASCII.GetBytes(fieldName);
    var off = raw.IndexOf(nameBytes);
    if (off < 0)
    {
        return;
    }

    var valueOffset = off + 72;
    if (valueOffset + 4 > raw.Length)
    {
        return;
    }

    BinaryPrimitives.WriteSingleLittleEndian(raw[valueOffset..(valueOffset + 4)], value);
    patched.Add($"{label}={value}");
}

static void PatchAsciiString(
    Span<byte> raw,
    string oldValue,
    string newValue,
    List<string> patched,
    string label)
{
    var oldBytes = Encoding.ASCII.GetBytes(oldValue);
    var newBytes = Encoding.ASCII.GetBytes(newValue);
    var off = raw.IndexOf(oldBytes);
    if (off < 0)
    {
        return;
    }

    if (newBytes.Length > oldBytes.Length)
    {
        throw new InvalidOperationException($"replacement longer than source for {label}");
    }

    raw[off..(off + oldBytes.Length)].Clear();
    newBytes.CopyTo(raw[off..(off + newBytes.Length)]);
    patched.Add($"{label}={newValue}");
}

static void PatchFadeTracks(Span<byte> raw, List<string> patched, string label)
{
    PatchTrackKeyframesByName(raw, "Fade"u8, 0.0f, patched, label);
}

static void PatchTransitionTracks(Span<byte> raw, List<string> patched, string label)
{
    // Older prerace_01..08 sequences expose "Transition" as a scalar
    // keyframed track at the same layout used for Fade. Zero it so the
    // prerace camera blend doesn't drive the composite independently.
    PatchTrackKeyframesByName(raw, "Transition"u8, 0.0f, patched, label);
}

static void PatchTrackKeyframesByName(
    Span<byte> raw,
    ReadOnlySpan<byte> needle,
    float value,
    List<string> patched,
    string label)
{
    var baseOffset = 0;
    var hit = 0;

    while (true)
    {
        var off = raw[baseOffset..].IndexOf(needle);
        if (off < 0)
        {
            return;
        }

        var fieldOff = baseOffset + off;
        // Animationlib tracks for this pack write keyed values at these
        // three offsets after the ASCII field name. Mirrors the original
        // PatchFadeTracks layout.
        foreach (var valueOff in new[] { fieldOff + 0x2c, fieldOff + 0x4c, fieldOff + 0x6c })
        {
            if (valueOff + 4 > raw.Length)
            {
                continue;
            }

            BinaryPrimitives.WriteSingleLittleEndian(raw[valueOff..(valueOff + 4)], value);
        }

        hit++;
        patched.Add($"{label}[{hit}]={value}");

        baseOffset = fieldOff + needle.Length;
        if (baseOffset >= raw.Length)
        {
            return;
        }
    }
}

static void PatchNamedTrackKeyframes(
    Span<byte> raw,
    string fieldName,
    float value,
    List<string> patched,
    string label)
{
    // Same keyframe layout as PatchTrackKeyframesByName but treats an
    // ASCII-named scalar track in an animationlib. Separate entry point
    // so callers can target a specific named track rather than sweeping
    // every occurrence — here we intentionally sweep so animated curves
    // that split into multiple sub-rows (e.g. repeated SunElevation
    // sibling rows) all get zeroed.
    var needle = System.Text.Encoding.ASCII.GetBytes(fieldName);
    PatchTrackKeyframesByName(raw, needle, value, patched, label);
}

static void PatchNamedFadeSequence(Span<byte> raw, string sequenceName, List<string> patched, string label)
{
    var seqBytes = Encoding.ASCII.GetBytes(sequenceName);
    var seqOff = raw.IndexOf(seqBytes);
    if (seqOff < 0)
    {
        return;
    }

    var fadeBytes = "Fade"u8;
    var fadeRel = raw[seqOff..].IndexOf(fadeBytes);
    if (fadeRel < 0)
    {
        return;
    }

    var fadeOff = seqOff + fadeRel;
    foreach (var valueOff in new[] { fadeOff + 0x2c, fadeOff + 0x4c })
    {
        if (valueOff + 4 > raw.Length)
        {
            continue;
        }

        BinaryPrimitives.WriteSingleLittleEndian(raw[valueOff..(valueOff + 4)], 0.0f);
    }

    patched.Add($"{label}=0");
}

static void PatchNamedScalarSequence(
    Span<byte> raw,
    string sequenceName,
    string fieldName,
    float value,
    List<string> patched,
    string label)
{
    var seqBytes = Encoding.ASCII.GetBytes(sequenceName);
    var seqOff = raw.IndexOf(seqBytes);
    if (seqOff < 0)
    {
        return;
    }

    var fieldBytes = Encoding.ASCII.GetBytes(fieldName);
    var fieldRel = raw[seqOff..].IndexOf(fieldBytes);
    if (fieldRel < 0)
    {
        return;
    }

    var fieldOff = seqOff + fieldRel;
    var valueOff = fieldOff + 0x2c;
    if (valueOff + 4 > raw.Length)
    {
        return;
    }

    BinaryPrimitives.WriteSingleLittleEndian(raw[valueOff..(valueOff + 4)], value);
    patched.Add($"{label}={value}");
}
