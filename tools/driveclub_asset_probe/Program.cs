using System.Text;
using DriveClubFS.Resources;

if (args.Length < 1)
{
    Console.Error.WriteLine("usage: driveclub_asset_probe <rpk-path> [dump-dir]");
    return 1;
}

var rpkPath = args[0];
var dumpDir = args.Length >= 2 ? args[1] : Path.Combine(Path.GetDirectoryName(rpkPath) ?? ".", "probe_dump");

if (!File.Exists(rpkPath))
{
    Console.Error.WriteLine($"missing rpk: {rpkPath}");
    return 2;
}

Directory.CreateDirectory(dumpDir);

using var pack = ResourcePack.Open(rpkPath);

var interestingNameTerms = new[]
{
    "prerace",
    "pre_race",
    "postfx",
    "cutsccam",
    "worldcam",
    "track_preview",
    "fade",
    "grade",
    "blur",
    "colourgradingoverride",
    "bloomoverride",
    "camvolume",
    "cutscenecamera"
};

var interestingSourceTerms = new[]
{
    "india_posteffects.lvl",
    "prerace_cams.lvl"
};

bool IsInteresting(ResourceInfo info)
{
    foreach (var name in info.Names)
    {
        var lower = name.ToLowerInvariant();
        if (interestingNameTerms.Any(lower.Contains))
        {
            return true;
        }
    }

    foreach (var path in info.SourceAssetPaths)
    {
        var lower = path.ToLowerInvariant();
        if (interestingSourceTerms.Any(lower.Contains))
        {
            return true;
        }
    }

    return false;
}

var matches = pack.ResourceInfos.Values
    .Where(IsInteresting)
    .OrderBy(v => v.ResourceId.Type)
    .ThenBy(v => v.Names.FirstOrDefault() ?? string.Empty)
    .ToList();

Console.WriteLine($"interesting resources: {matches.Count}");

var summaryPath = Path.Combine(dumpDir, "summary.txt");
using var summary = new StreamWriter(summaryPath, false, Encoding.UTF8);

summary.WriteLine($"RPK: {rpkPath}");
summary.WriteLine($"Root: {pack.RootIdentifier}");
summary.WriteLine($"Interesting resources: {matches.Count}");
summary.WriteLine();

using var fs = File.OpenRead(rpkPath);
foreach (var info in matches)
{
    var name = info.Names.FirstOrDefault() ?? $"unnamed_{info.ResourceId.Id:X}";
    var safeName = Sanitize(name);
    var header = $"{name} | type={info.ResourceId.Type} | uid=0x{info.ResourceId.Uid:X16} | size={info.Size} | offset=0x{info.Offset:X}";
    Console.WriteLine(header);
    summary.WriteLine(header);

    if (info.SourceAssetPaths.Count > 0)
    {
        foreach (var path in info.SourceAssetPaths)
        {
            summary.WriteLine($"  src: {path}");
        }
    }

    if (info.Dependancies.Count > 0)
    {
        foreach (var dep in info.Dependancies.Take(16))
        {
            summary.WriteLine($"  dep: 0x{dep.Uid:X16} ({dep.Type})");
        }
        if (info.Dependancies.Count > 16)
        {
            summary.WriteLine($"  dep: ... {info.Dependancies.Count - 16} more");
        }
    }

    var binPath = Path.Combine(dumpDir, $"{safeName}__{info.ResourceId.Type}__0x{info.ResourceId.Uid:X16}.bin");
    DumpRange(fs, info.Offset, info.Size, binPath);

    var txtPath = Path.ChangeExtension(binPath, ".strings.txt");
    DumpAsciiStrings(binPath, txtPath, 4);
}

Console.WriteLine($"summary: {summaryPath}");
return 0;

static void DumpRange(FileStream fs, uint offset, uint size, string outPath)
{
    fs.Position = offset;
    var buffer = new byte[size];
    var read = fs.Read(buffer, 0, checked((int)size));
    File.WriteAllBytes(outPath, read == buffer.Length ? buffer : buffer[..read]);
}

static void DumpAsciiStrings(string inPath, string outPath, int minLen)
{
    var bytes = File.ReadAllBytes(inPath);
    using var writer = new StreamWriter(outPath, false, Encoding.UTF8);
    var sb = new StringBuilder();
    foreach (var b in bytes)
    {
        if (b >= 0x20 && b <= 0x7E)
        {
            sb.Append((char)b);
        }
        else
        {
            Flush(sb, writer, minLen);
        }
    }
    Flush(sb, writer, minLen);
}

static void Flush(StringBuilder sb, StreamWriter writer, int minLen)
{
    if (sb.Length >= minLen)
    {
        writer.WriteLine(sb.ToString());
    }
    sb.Clear();
}

static string Sanitize(string value)
{
    var invalid = Path.GetInvalidFileNameChars();
    var chars = value.Select(ch => invalid.Contains(ch) ? '_' : ch).ToArray();
    return new string(chars).Replace(' ', '_');
}
