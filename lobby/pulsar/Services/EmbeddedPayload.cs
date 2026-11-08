using System;
using System.IO;
using System.IO.Compression;
using System.Reflection;
using System.Security.Cryptography;

namespace Pulsar.Services;

// The one-file setup (tools/package_single_exe.ps1): the mod payload (mod/Data/...)
// and PulsarCompanion.exe travel inside Pulsar.exe as the embedded resource
// "pulsar-payload.zip". On first use they are unpacked to
// %LOCALAPPDATA%\PULSAR\payload\<hash>\, so ModInstaller and DaemonBridge find them there just
// as they would next to the exe in the zip package. A build without the resource (dev build,
// zip package) returns null and everything works as before.
public static class EmbeddedPayload
{
    private const string ResourceName = "pulsar-payload.zip";
    private static readonly object Gate = new();
    private static bool _tried;
    private static string? _dir;

    public static string? Dir
    {
        get
        {
            lock (Gate)
            {
                if (_tried) return _dir;
                _tried = true;
                try { _dir = Extract(); }
                catch { _dir = null; }   // unreadable resource or disk: behave like a zip package
                return _dir;
            }
        }
    }

    private static string? Extract()
    {
        using var res = Assembly.GetExecutingAssembly().GetManifestResourceStream(ResourceName);
        if (res == null) return null;
        using var buf = new MemoryStream();
        res.CopyTo(buf);
        var hash = Convert.ToHexString(SHA256.HashData(buf.ToArray()))[..16];
        var root = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                                "PULSAR", "payload", hash);
        var done = Path.Combine(root, ".complete");
        if (File.Exists(done)) return root;

        // Unpack into a scratch folder first, so a crash halfway never leaves a payload that
        // looks complete.
        var tmp = root + ".tmp";
        if (Directory.Exists(tmp)) Directory.Delete(tmp, recursive: true);
        buf.Position = 0;
        using (var zip = new ZipArchive(buf, ZipArchiveMode.Read))
            zip.ExtractToDirectory(tmp);
        if (Directory.Exists(root)) Directory.Delete(root, recursive: true);
        Directory.Move(tmp, root);
        File.WriteAllText(done, DateTime.UtcNow.ToString("O"));
        return root;
    }
}
