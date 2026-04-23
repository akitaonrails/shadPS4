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
var singleGlobalSequence = args.Length >= 4 ? args[3] : string.Empty;
var singleGlobalField = args.Length >= 5 ? args[4] : string.Empty;
var singleActorName = args.Length >= 6 ? args[5] : string.Empty;
var singleActorField = args.Length >= 7 ? args[6] : string.Empty;

if (!File.Exists(inputPath))
{
    Console.Error.WriteLine($"missing rpk: {inputPath}");
    return 2;
}

if (!Path.GetFullPath(inputPath).Equals(Path.GetFullPath(outputPath), StringComparison.OrdinalIgnoreCase))
{
    File.Copy(inputPath, outputPath, overwrite: true);
}

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
        if (!string.IsNullOrEmpty(singleActorName) && !string.IsNullOrEmpty(singleActorField) &&
            name.Contains(singleActorName, StringComparison.OrdinalIgnoreCase))
        {
            PatchScalar(raw, singleActorField, 0.0f, patched, $"{name}.{singleActorField}");
        }

        if (mode != "global-fades-only" && mode != "global-rtt-only" &&
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

        if (mode != "global-fades-only" && mode != "global-rtt-only" &&
            (name.Contains("PostFX_BloomOverride", StringComparison.OrdinalIgnoreCase) ||
             name.Contains("PostFX_bloom", StringComparison.OrdinalIgnoreCase)))
        {
            PatchScalar(raw, "TemporalFade", 0.0f, patched, $"{name}.TemporalFade");
            PatchScalar(raw, "OverrideBlend", 0.0f, patched, $"{name}.OverrideBlend");
            PatchScalar(raw, "MasterBrightness", 1.0f, patched, $"{name}.MasterBrightness");
            PatchScalar(raw, "NonBloomedAttenuation", 0.0f, patched,
                        $"{name}.NonBloomedAttenuation");
        }

        if (mode != "global-fades-only" && mode != "global-rtt-only" &&
            name.Contains("PostFX_TXAAOverride", StringComparison.OrdinalIgnoreCase))
        {
            PatchScalar(raw, "OverrideBlend", 0.0f, patched, $"{name}.OverrideBlend");
            PatchScalar(raw, "TXAAOverallWeight", 0.0f, patched, $"{name}.TXAAOverallWeight");
            PatchScalar(raw, "TXAAColourClamping", 0.0f, patched,
                        $"{name}.TXAAColourClamping");
        }

        if (mode != "global-fades-only" && mode != "global-rtt-only" &&
            name.Contains("PostFX_ColourGradingOverride", StringComparison.OrdinalIgnoreCase))
        {
            PatchScalar(raw, "OverrideBlend", 0.0f, patched, $"{name}.OverrideBlend");
        }

        if (mode != "global-fades-only" && mode != "global-rtt-only" &&
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

        // 2026-04-22: scripted-fade hunt. The video recorded this day
        // proved the blackout is a scripted MasterBrightness curve dipping
        // to ~0.0007 during prerace then jumping back to 1.0 at the cockpit
        // handoff. The earlier blind-offset sweep crashed the parser; this
        // version does VALUE-matched surgical writes: it finds the MasterBrightness
        // track name in the animlib, scans up to 80 bytes after it, and replaces
        // only the specific 4-byte float windows that currently read 0.003 or
        // 0.0007. Any byte that parses as a different float is left alone —
        // no structural integers, no timing fields, no slopes. Value-matched
        // writes only.
        //
        // Expected effect: MasterBrightness animated curve stays at 1.0 for the
        // entire prerace window. The blackout should either disappear entirely
        // or (if there is a second animated scalar driving it) become visibly
        // reduced so we can narrow further.
        PatchAnimlibFloatValues(
            raw,
            "MasterBrightness",
            new[] { (0.003f, 1.0f), (0.0007f, 1.0f) },
            scanBytes: 96,
            patched,
            $"{name}.MasterBrightness.curve");

        // 2026-04-22 Iteration 11 — combined multi-scalar patch.
        //
        // Run 1 (MasterBrightness alone) moved the floor from 1.6/255 to
        // 6.5/255 but that's still perceptually black. A second scalar is
        // compounding the dim. From the 48-byte per-record scan of
        // india_posteffects.lvl animlib:
        //
        //   ManualAutoMix       +30=0.99034  +34=0.13865  (oscillation
        //                       between all-manual and all-auto)
        //   AutoTargetLuminance +33=0.38  (low target — crushes dusk scene)
        //   ManualExposureLog2  +31=5.8, +60=4.5  (positive log2 stops,
        //                       these are BRIGHTENING keyframes — do not touch)
        //
        // Plan: force ManualAutoMix to 1.0 (pure manual) at both of its
        // animated keyframes, so the manual exposure path wins and the
        // auto-exposure loop cannot drag the scene down. And bump
        // AutoTargetLuminance from 0.38 to 1.0 so even if some code path
        // still consults the auto pipeline, it targets a bright luminance
        // instead of the dusk-target 0.38.
        PatchAnimlibFloatValues(
            raw,
            "ManualAutoMix",
            new[] { (0.99034f, 1.0f), (0.13865f, 1.0f) },
            scanBytes: 48,
            patched,
            $"{name}.ManualAutoMix.curve");

        PatchAnimlibFloatValues(
            raw,
            "AutoTargetLuminance",
            new[] { (0.38f, 1.0f) },
            scanBytes: 48,
            patched,
            $"{name}.AutoTargetLuminance.curve");
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
        if (mode == "global-rtt-only")
        {
            PatchNamedScalarSequence(raw, "RTT_BLUR_ON", "TemporalFade", 0.0f, patched,
                                     $"{name}.RTT_BLUR_ON.TemporalFade");
            PatchNamedScalarSequence(raw, "RTT_BLUR_OFF", "TemporalFade", 0.0f, patched,
                                     $"{name}.RTT_BLUR_OFF.TemporalFade");
            PatchNamedScalarSequence(raw, "RTT_BLUR_ON", "OverrideBlend", 0.0f, patched,
                                     $"{name}.RTT_BLUR_ON.OverrideBlend");
            PatchNamedScalarSequence(raw, "RTT_BLUR_OFF", "OverrideBlend", 0.0f, patched,
                                     $"{name}.RTT_BLUR_OFF.OverrideBlend");
            PatchNamedScalarSequence(raw, "RTT_GRADING_ON", "OverrideBlend", 0.0f, patched,
                                     $"{name}.RTT_GRADING_ON.OverrideBlend");
            PatchNamedScalarSequence(raw, "RTT_GRADING_OFF", "OverrideBlend", 0.0f, patched,
                                     $"{name}.RTT_GRADING_OFF.OverrideBlend");
        }
        else if (!string.IsNullOrEmpty(singleGlobalSequence) && !string.IsNullOrEmpty(singleGlobalField))
        {
            PatchNamedScalarSequence(raw, singleGlobalSequence, singleGlobalField, 0.0f, patched,
                                     $"{name}.{singleGlobalSequence}.{singleGlobalField}");
        }
        else if (string.IsNullOrEmpty(singleGlobalSequence))
        {
            PatchNamedFadeSequence(raw, "FadeOut", patched, $"{name}.FadeOut");
            PatchNamedFadeSequence(raw, "FadeIn", patched, $"{name}.FadeIn");
            PatchNamedFadeSequence(raw, "FadeIn_Fast", patched, $"{name}.FadeIn_Fast");
            PatchNamedFadeSequence(raw, "FadeOut_Fast", patched, $"{name}.FadeOut_Fast");
        }
        else
        {
            PatchNamedFadeSequence(raw, singleGlobalSequence, patched, $"{name}.{singleGlobalSequence}");
        }
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

// Value-matched animlib keyframe patch.
//
// Looks up `trackName` as an ASCII substring. For every occurrence, scans
// up to `scanBytes` bytes after the name; at every 4-byte-aligned or
// unaligned window it reads as a little-endian float. For each entry in
// `replacements`, finds the first window whose current value matches
// `oldValue` within a tight tolerance and overwrites it with `newValue`.
// Only one match is written per (oldValue, newValue) pair per name-
// occurrence; every other byte stays untouched, which is how we avoid the
// April-20 "blind sweep overwrites a keyframe stride" crash mode.
//
// Use this for animationlib keyframed-scalar curves where we have
// identified specific float values that need to change but do not yet know
// the exact record layout.
static void PatchAnimlibFloatValues(
    Span<byte> raw,
    string trackName,
    (float oldValue, float newValue)[] replacements,
    int scanBytes,
    List<string> patched,
    string label)
{
    var nameBytes = Encoding.ASCII.GetBytes(trackName);
    var searchStart = 0;
    var occurrence = 0;

    while (true)
    {
        var relOff = raw[searchStart..].IndexOf(nameBytes);
        if (relOff < 0)
        {
            return;
        }

        var nameOff = searchStart + relOff;
        occurrence++;

        var windowStart = nameOff + nameBytes.Length;
        var windowEnd = Math.Min(windowStart + scanBytes, raw.Length - 4);

        foreach (var (oldValue, newValue) in replacements)
        {
            // Tight tolerance — 0.5% of the target magnitude, floored at 1e-7.
            // This prevents us from accidentally matching a different nearby
            // float (e.g. an interpolation slope that shares a similar order
            // of magnitude but is not the value we intend to rewrite).
            var tolerance = Math.Max(Math.Abs(oldValue) * 0.005f, 1e-7f);
            for (var scan = windowStart; scan < windowEnd; scan++)
            {
                var current = BinaryPrimitives.ReadSingleLittleEndian(raw.Slice(scan, 4));
                if (Math.Abs(current - oldValue) < tolerance)
                {
                    BinaryPrimitives.WriteSingleLittleEndian(raw.Slice(scan, 4), newValue);
                    patched.Add(
                        $"{label}[{occurrence}] {trackName}+{scan - nameOff}: {oldValue:G6} -> {newValue:G6}");
                    break;
                }
            }
        }

        searchStart = nameOff + nameBytes.Length;
    }
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
