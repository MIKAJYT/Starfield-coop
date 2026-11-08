using System.IO.Compression;
using Pulsar.Services;
using Xunit;

namespace Companion.Tests;

// The launcher's setup: installing SFSE and the Address Library from archives the player
// downloaded from Nexus (Services/DependencyInstaller.cs). Archives are built here with the
// real layouts: SFSE in a top folder (sfse_0_2_21\sfse_loader.exe, sfse_1_16_244.dll,
// Data\Scripts\...), the Address Library as SFSE\Plugins\versionlib-*.bin.
public sealed class DependencyInstallerTests : IDisposable
{
    private const string Version = "1_16_244";
    private readonly string _tmp = Path.Combine(Path.GetTempPath(), "pulsar-deps-" + Guid.NewGuid().ToString("N"));
    private string Game => Path.Combine(_tmp, "game");
    private string Downloads => Path.Combine(_tmp, "Downloads");

    public DependencyInstallerTests()
    {
        Directory.CreateDirectory(Path.Combine(Game, "Data"));
        Directory.CreateDirectory(Downloads);
    }

    public void Dispose()
    {
        try { Directory.Delete(_tmp, recursive: true); } catch { }
    }

    private void Zip(string name, params (string path, string text)[] files)
    {
        using var zip = ZipFile.Open(Path.Combine(Downloads, name), ZipArchiveMode.Create);
        foreach (var (path, text) in files)
        {
            using var w = new StreamWriter(zip.CreateEntry(path).Open());
            w.Write(text);
        }
    }

    private void SfseArchive(string dllVersion) => Zip("sfse_0_2_21.zip",
        ("sfse_0_2_21/sfse_loader.exe", "loader"),
        ("sfse_0_2_21/sfse_" + dllVersion + ".dll", "dll"),
        ("sfse_0_2_21/sfse_readme.txt", "readme"),
        ("sfse_0_2_21/src/sfse/main.cpp", "source"),
        ("sfse_0_2_21/Data/Scripts/ScriptObject.pex", "pex"));

    [Fact]
    public void InstallsSfseIntoTheGameFolderAndItsDataFiles()
    {
        SfseArchive(Version);
        var r = DependencyInstaller.InstallFromDownloads(Game, Downloads, Version);

        Assert.Contains(r, x => x.Installed && x.Message.Contains("SFSE"));
        Assert.True(File.Exists(Path.Combine(Game, "sfse_loader.exe")));
        Assert.True(File.Exists(Path.Combine(Game, "sfse_1_16_244.dll")));
        Assert.True(File.Exists(Path.Combine(Game, "Data", "Scripts", "ScriptObject.pex")));
        Assert.False(File.Exists(Path.Combine(Game, "sfse_readme.txt")));   // not a game file
        Assert.False(Directory.Exists(Path.Combine(Game, "src")));
        Assert.True(DependencyInstaller.HasSfse(Game, Version));
    }

    [Fact]
    public void RefusesSfseForAnotherGameVersion()
    {
        SfseArchive("1_15_216");
        var r = DependencyInstaller.InstallFromDownloads(Game, Downloads, Version);

        Assert.Contains(r, x => !x.Installed && x.Message.Contains("different game version"));
        Assert.False(File.Exists(Path.Combine(Game, "sfse_loader.exe")));
    }

    [Fact]
    public void InstallsTheAddressLibraryForThisVersionOnly()
    {
        Zip("Address Library for SFSE Plugins-3256-11-1700000000.zip",
            ("SFSE/Plugins/versionlib-1-16-244-0.bin", "bin"),
            ("SFSE/Plugins/versionlib-1-15-216-0.bin", "older"));
        var r = DependencyInstaller.InstallFromDownloads(Game, Downloads, Version);

        Assert.Contains(r, x => x.Installed && x.Message.Contains("Address Library"));
        Assert.True(DependencyInstaller.HasAddressLibrary(Game, Version));
    }

    [Fact]
    public void RefusesAnAddressLibraryWithoutThisVersion()
    {
        Zip("addrlib-old.zip", ("SFSE/Plugins/versionlib-1-15-216-0.bin", "older"));
        var r = DependencyInstaller.InstallFromDownloads(Game, Downloads, Version);

        Assert.Contains(r, x => !x.Installed);
        Assert.False(DependencyInstaller.HasAddressLibrary(Game, Version));
    }

    [Fact]
    public void IgnoresUnrelatedArchivesAndDoesNothingWhenAllIsInstalled()
    {
        Zip("holiday-photos.zip", ("beach.jpg", "jpg"));
        Assert.Empty(DependencyInstaller.InstallFromDownloads(Game, Downloads, Version));

        SfseArchive(Version);
        Zip("addrlib.zip", ("SFSE/Plugins/versionlib-1-16-244-0.bin", "bin"));
        Assert.Equal(2, DependencyInstaller.InstallFromDownloads(Game, Downloads, Version).Count(x => x.Installed));
        Assert.Empty(DependencyInstaller.InstallFromDownloads(Game, Downloads, Version));
    }

    // A real SFSE download (.7z, solid) when one is named in PULSAR_TEST_SFSE_7Z; a copy is
    // used, the original is only read. Without the variable this test does nothing.
    [Fact]
    public void InstallsFromARealSfse7zDownload()
    {
        var real = Environment.GetEnvironmentVariable("PULSAR_TEST_SFSE_7Z");
        if (string.IsNullOrEmpty(real) || !File.Exists(real)) return;
        File.Copy(real, Path.Combine(Downloads, Path.GetFileName(real)));

        var r = DependencyInstaller.InstallFromDownloads(Game, Downloads, Version);

        Assert.Contains(r, x => x.Installed && x.Message.Contains("SFSE"));
        Assert.True(DependencyInstaller.HasSfse(Game, Version));
        Assert.True(new FileInfo(Path.Combine(Game, "sfse_loader.exe")).Length > 10_000);
    }

    [Fact]
    public void NeverWritesOutsideTheGameFolder()
    {
        Zip("sfse_evil.zip",
            ("sfse_0_2_21/sfse_loader.exe", "loader"),
            ("sfse_0_2_21/sfse_1_16_244.dll", "dll"),
            ("sfse_0_2_21/Data/../../escaped.exe", "x"));
        DependencyInstaller.InstallFromDownloads(Game, Downloads, Version);

        Assert.False(File.Exists(Path.Combine(_tmp, "escaped.exe")));
    }
}
