using System;
using System.Collections.Generic;
using System.IO;
using System.Threading.Tasks;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using Pulsar.Services;

namespace Pulsar.ViewModels;

// "Play with friends, no server": host or join with a join code. The game itself finds the
// addresses a friend can reach (LAN, public address from a STUN server, a port the router
// opens by UPnP) and connects host to player directly, without the lobby, relay or port
// forwarding (DLL: include/Network/DirectSession.h). The tool only writes PULSAR.ini,
// launches the game and passes the codes:
//   PULSAR_JoinCode.txt    game -> tool (host): the code to send to friends
//   PULSAR_ReplyCode.txt   game -> tool (friend): only if it can't reach the host by itself
//   PULSAR_ReplyCodeIn.txt tool -> game (host): a friend's reply code
public partial class PlayViewModel
{
    private static readonly TimeSpan CodeWait = TimeSpan.FromMinutes(3);

    [ObservableProperty] private string _directJoinCode = "";     // friend: the host's code
    [ObservableProperty] private string _friendReplyCode = "";    // host: a friend's reply code

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(HasMyJoinCode))]
    private string _myJoinCode = "";                               // host: shown after launch

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(HasMyReplyCode))]
    private string _myReplyCode = "";                              // friend: shown if needed

    [ObservableProperty] private string _directStatus = "";
    [ObservableProperty] private bool _isDirectHost;

    public bool HasMyJoinCode => !string.IsNullOrEmpty(MyJoinCode);
    public bool HasMyReplyCode => !string.IsNullOrEmpty(MyReplyCode);

    private string? PluginDir()
    {
        var ini = PulsarIni.ResolvePath(_shell.Config.StarfieldPath);
        return ini == null ? null : Path.GetDirectoryName(ini);
    }

    private bool WriteSessionIni(bool host, string joinCode)
    {
        var ini = PulsarIni.ResolvePath(_shell.Config.StarfieldPath);
        if (ini == null)
        {
            DirectStatus = "Set the Starfield path first (Game & launch).";
            return false;
        }
        try
        {
            PulsarIni.Merge(ini, new List<KeyValuePair<string, string>>
            {
                new("Mode", host ? "host" : "client"),
                new("DirectSession", "1"),
                new("JoinCode", joinCode),
                new("ReplyCode", ""),
            });
            return true;
        }
        catch (Exception ex)
        {
            DirectStatus = "Could not write PULSAR.ini: " + ex.Message;
            return false;
        }
    }

    [RelayCommand]
    private async Task HostWithCode()
    {
        var dir = PluginDir();
        if (dir == null || !WriteSessionIni(true, "")) return;
        IsDirectHost = true;
        MyJoinCode = "";
        var started = DateTime.Now;
        DirectStatus = "Starting Starfield. The join code appears here once the game has started.";
        _shell.Log("Hosting with a join code (no server).");
        _shell.LaunchGameNow();
        var code = await WaitForCodeFile(Path.Combine(dir, "PULSAR_JoinCode.txt"), started);
        if (code == null)
        {
            DirectStatus = "No join code yet. Is the game running with the co-op mod? (Check PULSAR.log.)";
            return;
        }
        MyJoinCode = code;
        DirectStatus = "Send this code to your friends. Load your save; they join when they paste it.";
    }

    [RelayCommand]
    private async Task JoinWithDirectCode()
    {
        var normalized = JoinCodeText.Normalize(DirectJoinCode);
        var decoded = JoinCodeText.Decode(normalized);
        if (decoded == null || decoded.Kind != JoinCodeText.Kind.Host)
        {
            DirectStatus = "That is not a valid join code. Check it for typos (it starts with PSR-).";
            return;
        }
        var dir = PluginDir();
        if (dir == null || !WriteSessionIni(false, normalized)) return;
        IsDirectHost = false;
        MyReplyCode = "";
        var started = DateTime.Now;
        DirectStatus = "Starting Starfield. Load a save; your game then connects to the host.";
        _shell.Log($"Joining with a join code ({decoded.Endpoints.Count} host address(es)).");
        _shell.LaunchGameNow();
        var reply = await WaitForCodeFile(Path.Combine(dir, "PULSAR_ReplyCode.txt"), started);
        if (reply != null)
        {
            MyReplyCode = reply;
            DirectStatus = "Your game can't reach the host by itself. Send this reply code to the host.";
        }
    }

    [RelayCommand]
    private void SendReplyCode()
    {
        var normalized = JoinCodeText.Normalize(FriendReplyCode);
        var decoded = JoinCodeText.Decode(normalized);
        if (decoded == null || decoded.Kind != JoinCodeText.Kind.Reply)
        {
            DirectStatus = "That is not a valid reply code. Check it for typos.";
            return;
        }
        var dir = PluginDir();
        if (dir == null) { DirectStatus = "Set the Starfield path first (Game & launch)."; return; }
        try
        {
            File.WriteAllText(Path.Combine(dir, "PULSAR_ReplyCodeIn.txt"), normalized + Environment.NewLine);
            FriendReplyCode = "";
            DirectStatus = "Reply code handed to your game. Your friend should connect within a few seconds.";
        }
        catch (Exception ex)
        {
            DirectStatus = "Could not hand the reply code to the game: " + ex.Message;
        }
    }

    // The code file written after a_since, or null after CodeWait.
    private static async Task<string?> WaitForCodeFile(string path, DateTime since)
    {
        var end = DateTime.Now + CodeWait;
        while (DateTime.Now < end)
        {
            try
            {
                if (File.Exists(path) && File.GetLastWriteTime(path) >= since.AddSeconds(-1))
                {
                    var text = (await File.ReadAllTextAsync(path)).Trim();
                    if (text.Length > 0) return text;
                }
            }
            catch (IOException) { }   // the game is writing it; try again
            await Task.Delay(1000);
        }
        return null;
    }
}
