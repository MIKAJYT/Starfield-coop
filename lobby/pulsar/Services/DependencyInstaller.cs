using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using SharpCompress.Archives;

namespace Pulsar.Services;

// SFSE and the Address Library may not be shipped with PULSAR or downloaded for the player:
// SFSE's site says its files come from Nexus (or Steam) and asks not to link to them
// directly, and Nexus needs the player's own login. So the player clicks Download on the two
// Nexus pages and this finds the archives in the Downloads folder, checks they are for this
// game version, and installs them: SFSE's loader and DLL into the game folder (plus its Data
// files), the Address Library's versionlib-*.bin into Data\SFSE\Plugins.
public static class DependencyInstaller
{
    public const string SfsePage = "https://www.nexusmods.com/starfield/mods/106?tab=files";
    public const string AddressLibraryPage = "https://www.nexusmods.com/starfield/mods/3256?tab=files";

    public sealed record Result(bool Installed, string Message);

    // "1_16_244" for Starfield.exe 1.16.244.0; null if the version can't be read.
    public static string? GameVersion(string gameRoot)
    {
        try
        {
            var v = FileVersionInfo.GetVersionInfo(Path.Combine(gameRoot, "Starfield.exe"));
            return v.FileMajorPart + "_" + v.FileMinorPart + "_" + v.FileBuildPart;
        }
        catch { return null; }
    }

    public static bool HasSfse(string gameRoot, string? version) =>
        File.Exists(Path.Combine(gameRoot, "sfse_loader.exe"))
        && (version == null || File.Exists(Path.Combine(gameRoot, "sfse_" + version + ".dll")));

    public static bool HasAddressLibrary(string gameRoot, string? version)
    {
        var dir = Path.Combine(gameRoot, "Data", "SFSE", "Plugins");
        try
        {
            return version == null
                ? Directory.Exists(dir) && Directory.EnumerateFiles(dir, "versionlib-*.bin").Any()
                : File.Exists(Path.Combine(dir, "versionlib-" + version.Replace('_', '-') + "-0.bin"));
        }
        catch { return false; }
    }

    // Look through recent archives in Downloads and install whatever is still missing. Tests
    // pass their own folder and game version ("1_16_244").
    public static List<Result> InstallFromDownloads(string gameRoot, string? downloadsDir = null,
                                                    string? gameVersion = null)
    {
        var results = new List<Result>();
        var version = gameVersion ?? GameVersion(gameRoot);
        bool needSfse = !HasSfse(gameRoot, version);
        bool needAddr = !HasAddressLibrary(gameRoot, version);
        if (!needSfse && !needAddr) return results;

        foreach (var file in RecentArchives(downloadsDir))
        {
            if (!needSfse && !needAddr) break;
            try
            {
                using var archive = ArchiveFactory.OpenArchive(file);
                var entries = archive.Entries.Where(e => !e.IsDirectory).ToList();
                if (needSfse && entries.Any(e => Name(e.Key).Equals("sfse_loader.exe", StringComparison.OrdinalIgnoreCase)))
                {
                    var r = InstallSfse(entries, gameRoot, version, Path.GetFileName(file));
                    results.Add(r);
                    if (r.Installed) needSfse = false;
                }
                else if (needAddr && entries.Any(e => Name(e.Key).StartsWith("versionlib-", StringComparison.OrdinalIgnoreCase)))
                {
                    var r = InstallAddressLibrary(entries, gameRoot, version, Path.GetFileName(file));
                    results.Add(r);
                    if (r.Installed) needAddr = false;
                }
            }
            catch (Exception ex)
            {
                // Still downloading, locked or not an archive: try again on the next scan.
                Debug.WriteLine("DependencyInstaller: " + file + ": " + ex.Message);
            }
        }
        return results;
    }

    private static Result InstallSfse(List<IArchiveEntry> entries, string gameRoot, string? version, string from)
    {
        if (version != null && !entries.Any(e => Name(e.Key).Equals("sfse_" + version + ".dll", StringComparison.OrdinalIgnoreCase)))
            return new Result(false, $"{from} is SFSE for a different game version (this game is {version.Replace('_', '.')}). Download the current SFSE.");

        // Paths are relative to the archive's top folder (sfse_0_2_21\...): the loader and
        // DLLs go to the game folder, the archive's Data\ tree into the game's Data.
        var loader = entries.First(e => Name(e.Key).Equals("sfse_loader.exe", StringComparison.OrdinalIgnoreCase));
        var top = Dir(loader.Key);
        int copied = 0;
        foreach (var e in entries)
        {
            var rel = Rel(e.Key, top);
            if (rel == null) continue;
            string? dst = null;
            if (!rel.Contains('/') && (rel.EndsWith(".exe", StringComparison.OrdinalIgnoreCase)
                                       || rel.EndsWith(".dll", StringComparison.OrdinalIgnoreCase)))
                dst = Path.Combine(gameRoot, rel);
            else if (rel.StartsWith("Data/", StringComparison.OrdinalIgnoreCase))
                dst = Path.Combine(gameRoot, rel.Replace('/', Path.DirectorySeparatorChar));
            if (dst == null) continue;
            Write(e, dst);
            copied++;
        }
        return new Result(true, $"SFSE installed from {from} ({copied} files).");
    }

    private static Result InstallAddressLibrary(List<IArchiveEntry> entries, string gameRoot, string? version, string from)
    {
        var bins = entries.Where(e => Name(e.Key).StartsWith("versionlib-", StringComparison.OrdinalIgnoreCase)
                                      && Name(e.Key).EndsWith(".bin", StringComparison.OrdinalIgnoreCase)).ToList();
        var want = version == null ? null : "versionlib-" + version.Replace('_', '-') + "-0.bin";
        if (want != null && !bins.Any(e => Name(e.Key).Equals(want, StringComparison.OrdinalIgnoreCase)))
            return new Result(false, $"{from} is an Address Library for a different game version (needs {want}). Download the current one.");
        var dir = Path.Combine(gameRoot, "Data", "SFSE", "Plugins");
        foreach (var e in bins) Write(e, Path.Combine(dir, Name(e.Key)));
        return new Result(true, $"Address Library installed from {from} ({bins.Count} files).");
    }

    // .7z / .zip files in Downloads from the last 30 days, newest first.
    private static IEnumerable<string> RecentArchives(string? downloadsDir)
    {
        var downloads = downloadsDir
            ?? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "Downloads");
        if (!Directory.Exists(downloads)) return Array.Empty<string>();
        var since = DateTime.Now.AddDays(-30);
        try
        {
            return new DirectoryInfo(downloads).EnumerateFiles()
                .Where(f => (f.Extension.Equals(".7z", StringComparison.OrdinalIgnoreCase)
                             || f.Extension.Equals(".zip", StringComparison.OrdinalIgnoreCase))
                            && f.LastWriteTime > since && f.Length < 200L * 1024 * 1024)
                .OrderByDescending(f => f.LastWriteTime)
                .Select(f => f.FullName)
                .ToList();
        }
        catch { return Array.Empty<string>(); }
    }

    private static string Norm(string? key) => (key ?? "").Replace('\\', '/').TrimStart('/');
    private static string Name(string? key) { var k = Norm(key); return k[(k.LastIndexOf('/') + 1)..]; }
    private static string Dir(string? key) { var k = Norm(key); int i = k.LastIndexOf('/'); return i < 0 ? "" : k[..(i + 1)]; }

    private static string? Rel(string? key, string top)
    {
        var k = Norm(key);
        if (!k.StartsWith(top, StringComparison.OrdinalIgnoreCase)) return null;
        var rel = k[top.Length..];
        return rel.Length == 0 || rel.Contains("..") ? null : rel;
    }

    private static void Write(IArchiveEntry e, string dst)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(dst)!);
        using var src = e.OpenEntryStream();
        using var fs = File.Create(dst);
        src.CopyTo(fs);
    }
}
