<#
.SYNOPSIS
  One-command co-op QA for PULSAR: two isolated Starfield test copies on one PC,
  one hosts, one joins over loopback, the in-game TestBot on each side writes what
  it sees, and this script scores it. Prints exactly one final line:
  RESULT: PASS | RESULT: FAIL | RESULT: TIMEOUT | RESULT: ERROR

.DESCRIPTION
  coop-anything playbook/automated-qa.md and multi-instance-testing.md:
  - test copies are hard-linked slots (instances\<slot>\game) with the test-only
    PulsarTestHarness (folder redirect, single-instance fix, Steam Cloud block);
  - the owner's install, Steam Cloud saves and profile folders are hashed or listed
    before and after, and any change fails the run;
  - only processes this script started are ever closed (pid + start time);
  - RAM guard: the client starts only with >= 5 GB free; below 1 GB it is closed.

  Test "host-join-follow": both copies load a copy of -SaveFile at the main menu
  (sStartingConsoleCommand), the client connects to 127.0.0.1, PULSAR bridges it to
  the host's save, both spawn the partner's clone. The host bot then moves the host
  -MoveMetres on X. PASS needs, on both sides, the clone within -Tolerance metres
  of the partner's real position before the move, and on the client after it.

.EXAMPLE
  .\Run-CoopQA.ps1 -SaveFile "<backup>\Save11_....sfs"
  .\Run-CoopQA.ps1 -SelfTest     # scorer self-test only, no game
#>
param(
    [string]$SaveFile,
    [string]$Instances    = (Join-Path $PSScriptRoot "..\..\instances"),
    [string]$HostSlot     = "sf-a",
    [string]$ClientSlot   = "sf-b",
    [int]   $Port         = 7777,
    [int]   $ClientPort   = 0,        # twin: a different port must make the run fail
    [int]   $TimeoutSec   = 600,
    [double]$MoveMetres   = 5.0,
    [double]$Tolerance    = 1.5,
    [string]$GameDir      = "C:\Program Files (x86)\Steam\steamapps\common\Starfield",
    [switch]$SelfTest,
    # Memory profile: start only the host slot, wait until in world, sample for
    # -MeasureSec, print peak working set / private bytes / GPU memory, close.
    [switch]$MeasureOnly,
    [int]   $MeasureSec   = 60,
    # Test copies run with memory-saving settings unless this is set (see LowMemoryIni).
    [switch]$NoLowMemory,
    # follow: host moves, client's clone must follow. door: host uses a load door,
    # client must stay (CoopDoorFollow=0). -DoorFollow sets CoopDoorFollow=1: twin.
    # loot: host kills an NPC and takes all; the client's corpse must keep its
    # items (CoopInstancedEnemyLoot=1). -SharedLoot sets it to 0: twin.
    # reward: host gets credits + a weapon as quest rewards; the client must get
    # both, once (CoopQuestRewards=1). -NoQuestRewards sets it to 0: twin.
    # takeoff: host pilots off a landed ship; the launch must wait for the
    # client's ready (CoopTakeoffVote=1). Needs a save aboard a landed ship
    # (Save10). -NoTakeoffVote sets it to 0: twin.
    # friendlyfire: host hits the client's figure twice; the figure must heal back and the
    # client's own health must not move (CoopFriendlyFireGuard=1). -NoFriendlyFireGuard: twin.
    # aim: host looks up, then down, aiming; the client's clone must turn its head and take
    # the sights pose (CoopAimSync=1). -NoAimSync: twin.
    # jetpack: host jumps and boosts 2.5 s; the client's clone must rise with it and fire its
    # own boostpack (CoopJetpackSync=1). -NoJetpackSync: twin.
    # pickup: host takes a loose item lying in the world; it must be gone on the client too
    # (CoopSharedWorldItems=1). -NoSharedWorldItems: twin.
    # reconnect: the client's network goes dark for 25 s; the session must come back by
    # itself (AutoReconnect=1). -NoAutoReconnect: twin.
    # separate: each player changes its own items, a perk and skill points; the host then
    # bridges a fresh save to the client; each must keep only its own changes
    # (CoopRestoreOwnCharacter=1). -NoRestoreOwnCharacter: twin.
    # damage: the host hits one NPC, the client another; each hit must show on the other
    # machine (CoopNpcHealthSync=1). -NoNpcHealthSync: twin.
    # space: takeoff, the host moves its ship to The Eye and bridges its save; each bot lists
    # the space around the ship and the lists must match. No twin yet (no fix yet).
    # scene: the host starts a scene (its quest first); the client must play the same
    # scene within 10 s (CoopSceneSync=1). -NoSceneSync: twin.
    # quest: the host starts a stopped Akila quest; the client must start the same quest
    # within 10 s (CoopQuestStartSync=1). -NoQuestStartSync: twin.
    # mission: the client stops a running mission-board quest quietly, then the host
    # advances a stage in it; the client must get it back at the host's stage through
    # the host's save (CoopMissionBridge=1). -NoMissionBridge: twin.
    [ValidateSet("follow", "door", "loot", "reward", "takeoff", "papyrus", "friendlyfire", "aim", "jetpack", "pickup", "reconnect", "separate", "damage", "space", "scene", "quest", "mission")][string]$Scenario = "follow",
    [switch]$NoRestoreOwnCharacter,
    [switch]$NoNpcHealthSync,
    [switch]$NoSceneSync,
    [switch]$NoQuestStartSync,
    [switch]$NoMissionBridge,
    [switch]$NoAutoReconnect,
    [switch]$NoSharedWorldItems,
    [switch]$NoFriendlyFireGuard,
    [switch]$NoAimSync,
    [switch]$NoJetpackSync,
    [switch]$DoorFollow,
    [switch]$SharedLoot,
    [switch]$NoQuestRewards,
    [switch]$NoTakeoffVote,
    # takeoff: the client leaves the ship before the vote; the vote must move it aboard.
    [switch]$PartnerOutside,
    # takeoff: the pilot presses F8 (the regroup key) during the vote; the launch must still wait.
    [switch]$F8DuringVote,
    # Direct session: the client joins only by the join code the host published (no HostIP).
    # -StaleJoinCode (twin) gives the client the code of the previous host session instead.
    [switch]$JoinByCode,
    # 2-4 players: once both copies see each other, a headless fake player joins as player 3
    # (tests\RelWithDebInfo\pulsar_fakepeer.exe). The two copies must play on undisturbed.
    [switch]$ThirdPlayer,
    # -ThirdPlayer + takeoff: player 3 answers the takeoff vote "ready" this many ms after the
    # request (-1 = never: twin, the ship must not take off).
    [int]$ThirdPlayerVoteMs = 20000,
    # Debugging: Papyrus script logs in the test copies (Documents\My Games\Starfield\Logs\Script).
    [switch]$PapyrusLog,
    [switch]$StaleJoinCode,
    # Extra PULSAR.ini lines for both copies ("Key = value", separated by ";"), for bisects.
    [string]$ExtraIni = "",
    # Extra PulsarTestBot.ini lines, same format (e.g. "NotifyTest = hello").
    [string]$ExtraBotIni = "",
    # Hard working-set cap per test copy (MB, 0 = off): Windows pages out the
    # test game's memory instead of the owner's programs (PulsarTestHarness).
    [int]$MaxWorkingSetMB = 0
)

# Memory-saving settings for test copies. Names are setting names found in
# Starfield.exe ("<name>:<section>" strings); values other than the defaults are
# guesses checked by -MeasureOnly runs (docs/TESTING.md).
$LowMemoryIni = "[Archive]`r`nsResourceArchiveMemoryCacheList=`r`nsResourceMiscArchiveMemoryCacheList=`r`n`r`n" +
                "[BackgroundLoad]`r`nbMinimizeCachedExteriors=1`r`nbDelayOuterGridCells=1`r`n`r`n" +
                "[Quality]`r`nuGlobalRendererQuality=0`r`n"

$ErrorActionPreference = "Stop"
if ($PartnerOutside) { $ExtraBotIni = "TakeoffPartnerOutside = 1;" + $ExtraBotIni }
if ($F8DuringVote) { $ExtraBotIni = "TakeoffF8DuringVote = 1;" + $ExtraBotIni }
$JoinByCode = $JoinByCode -or $StaleJoinCode
# Bots never touch the router (UPnP), a public STUN server, the clipboard or the F2 key.
if ($JoinByCode) { $ExtraIni = "DirectSession = 1;StunServers = ;UseUpnp = 0;CopyCodeToClipboard = 0;HotkeyPasteReplyCode = 0;" + $ExtraIni }
Set-StrictMode -Version 2

# ============================================================================
# Scorer: a pure function of the two bots' samples (no game, no files).
# ============================================================================
function Get-Dist($a, $b) {
    $dx = [double]$a[0] - [double]$b[0]; $dy = [double]$a[1] - [double]$b[1]; $dz = [double]$a[2] - [double]$b[2]
    return [math]::Sqrt($dx * $dx + $dy * $dy + $dz * $dz)
}

function Invoke-CoopScore {
    param($HostSamples, $ClientSamples, [double]$Tolerance, [double]$MoveMetres, [bool]$Final)
    # NOTE: PowerShell variable names are case-insensitive ($H is $h), so the lists
    # and their items need different names.
    $r = @{ Result = "PENDING"; Reasons = New-Object System.Collections.Generic.List[string] }
    $hostList = @($HostSamples | Sort-Object wallMs); $clientList = @($ClientSamples | Sort-Object wallMs)
    if ($hostList.Count -eq 0 -or $clientList.Count -eq 0) {
        if ($Final) { $r.Result = "ERROR"; $r.Reasons.Add("no bot state from " + $(if ($hostList.Count -eq 0) { "host" } else { "client" })) }
        return $r
    }
    # Precondition: the mode we think we are in (playbook: no hollow passes).
    if (@($hostList | Where-Object { $_.role -ne "host" }).Count -gt 0 -or @($clientList | Where-Object { $_.role -ne "client" }).Count -gt 0) {
        $r.Result = "FAIL"; $r.Reasons.Add("wrong roles in bot state"); return $r
    }
    # Pair each client sample with the nearest host sample (<= 600 ms apart).
    $pairs = New-Object System.Collections.Generic.List[object]
    $hi = 0
    foreach ($cs in $clientList) {
        while ($hi + 1 -lt $hostList.Count -and [math]::Abs($hostList[$hi + 1].wallMs - $cs.wallMs) -le [math]::Abs($hostList[$hi].wallMs - $cs.wallMs)) { $hi++ }
        $hs = $hostList[$hi]
        if ([math]::Abs($hs.wallMs - $cs.wallMs) -le 600 -and $hs.cloneVisible -and $cs.cloneVisible) {
            $pairs.Add([pscustomobject]@{ hostSample = $hs; clientSample = $cs
                dClientSeesHost = (Get-Dist $cs.clone $hs.player)
                dHostSeesClient = (Get-Dist $hs.clone $cs.player) })
        }
    }
    $moveAt = 0; foreach ($hs in $hostList) { if ($hs.acted -and $hs.actedAtWallMs -gt 0) { $moveAt = [long]$hs.actedAtWallMs; break } }
    $before = @($pairs | Where-Object { $moveAt -eq 0 -or $_.clientSample.wallMs -lt $moveAt })
    $after  = @($pairs | Where-Object { $moveAt -gt 0 -and $_.clientSample.wallMs -ge $moveAt + 5000 })

    if ($before.Count -lt 5 -or $moveAt -eq 0 -or $after.Count -lt 5) {
        if ($Final) {
            $r.Result = "TIMEOUT"
            if ($pairs.Count -eq 0) { $r.Reasons.Add("the clones were never visible on both sides at the same time") }
            elseif ($moveAt -eq 0) { $r.Reasons.Add("the host bot never made the test move") }
            else { $r.Reasons.Add("not enough samples (before move $($before.Count), after move $($after.Count), need 5 each)") }
        }
        return $r
    }
    # The step really happened: the host's own position moved by about MoveMetres.
    $hBefore = @($hostList | Where-Object { $_.wallMs -lt $moveAt })[-1]
    $hAfter  = @($hostList | Where-Object { $_.wallMs -ge $moveAt + 5000 })[0]
    $hostMoved = Get-Dist $hBefore.player $hAfter.player
    if ($hostMoved -lt $MoveMetres * 0.8) { $r.Result = "FAIL"; $r.Reasons.Add(("host did not really move: {0:N2} m" -f $hostMoved)); return $r }

    $okBefore = @($before | Where-Object { $_.dClientSeesHost -le $Tolerance -and $_.dHostSeesClient -le $Tolerance }).Count
    $okAfter  = @($after  | Where-Object { $_.dClientSeesHost -le $Tolerance }).Count
    $r.Details = "before move: {0}/{1} pairs within {2} m both ways; after move: {3}/{4} client-clone within {2} m; host moved {5:N2} m" -f `
        $okBefore, $before.Count, $Tolerance, $okAfter, $after.Count, $hostMoved
    if ($okBefore -ge [math]::Ceiling($before.Count * 0.8) -and $okAfter -ge [math]::Ceiling($after.Count * 0.8)) { $r.Result = "PASS" }
    else {
        $r.Result = "FAIL"
        $worstB = ($before | Measure-Object -Property dClientSeesHost -Maximum).Maximum
        $worstA = ($after  | Measure-Object -Property dClientSeesHost -Maximum).Maximum
        $r.Reasons.Add(("clone off by up to {0:N2} m before / {1:N2} m after the move" -f $worstB, $worstA))
    }
    return $r
}

# ---- door scenario ------------------------------------------------------------
# The host bot goes through the nearest load door. With CoopDoorFollow=0 (the
# spec: "doors only move the player who uses them") the host's cell must change
# and the client's must not, for at least 20 s, while both stay in session.
function Invoke-DoorScore {
    param($HostSamples, $ClientSamples, [bool]$Final, [string[]]$ClientDecisions = @())
    $r = @{ Result = "PENDING"; Reasons = New-Object System.Collections.Generic.List[string] }
    $hostList = @($HostSamples | Sort-Object wallMs); $clientList = @($ClientSamples | Sort-Object wallMs)
    if ($hostList.Count -eq 0 -or $clientList.Count -eq 0) {
        if ($Final) { $r.Result = "ERROR"; $r.Reasons.Add("no bot state from " + $(if ($hostList.Count -eq 0) { "host" } else { "client" })) }
        return $r
    }
    if (@($hostList | Where-Object { $_.role -ne "host" }).Count -gt 0 -or @($clientList | Where-Object { $_.role -ne "client" }).Count -gt 0) {
        $r.Result = "FAIL"; $r.Reasons.Add("wrong roles in bot state"); return $r
    }
    $act = $hostList | Where-Object { $_.acted } | Select-Object -First 1
    if (-not $act) {
        if ($Final) { $r.Result = "TIMEOUT"; $r.Reasons.Add("the host bot never used a door (clone never settled?)") }
        return $r
    }
    # The bot calls the engine's Activate as the player (1 = accepted); whether
    # the host really went through shows as a cell change below.
    if ($act.actionInfo -notmatch 'activate=1') { $r.Result = "ERROR"; $r.Reasons.Add("host bot could not use a door: $($act.actionInfo)"); return $r }
    $t0 = [long]$act.actedAtWallMs
    # Precondition: co-op worked before the door (both clones visible).
    $hBefore = @($hostList | Where-Object { $_.wallMs -lt $t0 -and $_.cloneVisible })
    $cBefore = @($clientList | Where-Object { $_.wallMs -lt $t0 -and $_.cloneVisible })
    if ($hBefore.Count -lt 3 -or $cBefore.Count -lt 3) {
        $r.Result = "FAIL"; $r.Reasons.Add("co-op was not working before the door (clone samples host $($hBefore.Count), client $($cBefore.Count))"); return $r
    }
    $hostCellBefore = $hBefore[-1].playerCell; $clientCellBefore = $cBefore[-1].playerCell
    $hAfter = @($hostList   | Where-Object { $_.wallMs -ge $t0 + 15000 })
    $cAfter = @($clientList | Where-Object { $_.wallMs -ge $t0 })
    $cLate  = @($cAfter | Where-Object { $_.wallMs -ge $t0 + 15000 })
    if ($hAfter.Count -lt 3 -or $cLate.Count -lt 20) {
        if ($Final) { $r.Result = "TIMEOUT"; $r.Reasons.Add("not enough samples after the door (host $($hAfter.Count), client $($cLate.Count) of 20)") }
        return $r
    }
    $hostCellAfter = $hAfter[-1].playerCell
    $clientCells = @($cAfter | ForEach-Object { $_.playerCell } | Sort-Object -Unique)
    $r.Details = "door: $($act.actionInfo); host cell $hostCellBefore -> $hostCellAfter; client cells after the door: $($clientCells -join ', ') (before $clientCellBefore)"
    if ($hostCellAfter -eq $hostCellBefore) { $r.Result = "FAIL"; $r.Reasons.Add("the host did not change cell through the door"); return $r }
    if (@($clientCells | Where-Object { $_ -ne $clientCellBefore }).Count -gt 0) { $r.Result = "FAIL"; $r.Reasons.Add("the client was moved through the door with the host"); return $r }
    if (@($cLate | Where-Object { -not $_.connected }).Count -gt 0) { $r.Result = "FAIL"; $r.Reasons.Add("the client left the session after the door"); return $r }
    # The client staying put is not enough on its own: with CoopDoorFollow=1 it also
    # stays when the old follow path gives up (an interior door without a fixed
    # destination, run 20261028-004820). Require the client's own decision.
    $doorId = if ($act.actionInfo -match '^door ([0-9A-F]{8})') { $Matches[1] } else { "" }
    if (-not ($ClientDecisions | Where-Object { $_ -match "host used door 0x$doorId .*staying here" })) {
        $r.Result = "FAIL"; $r.Reasons.Add("the client never logged that it stays for door $doorId (CoopDoorFollow=0 decision missing)"); return $r
    }
    $r.Result = "PASS"
    return $r
}

# ---- loot scenario ------------------------------------------------------------
# The host bot kills an NPC (a save-file reference: same ID on both machines),
# waits a few seconds, then takes everything from the corpse. With
# CoopInstancedEnemyLoot=1 (the spec: "every player gets their own loot") the
# client's copy of that NPC must keep its items for at least 15 s afterwards, and
# the client must log that it kept them. The death must also reach the client:
# its copy has to be dead by the end of the 15 s window (death sync; before the
# 2026-10-28 death watch it never was, runs 20261028-012531 to -015140).
function Invoke-LootScore {
    param($HostSamples, $ClientSamples, [bool]$Final, [string[]]$ClientDecisions = @())
    $r = @{ Result = "PENDING"; Reasons = New-Object System.Collections.Generic.List[string] }
    $hostList = @($HostSamples | Sort-Object wallMs); $clientList = @($ClientSamples | Sort-Object wallMs)
    if ($hostList.Count -eq 0 -or $clientList.Count -eq 0) {
        if ($Final) { $r.Result = "ERROR"; $r.Reasons.Add("no bot state from " + $(if ($hostList.Count -eq 0) { "host" } else { "client" })) }
        return $r
    }
    if (@($hostList | Where-Object { $_.role -ne "host" }).Count -gt 0 -or @($clientList | Where-Object { $_.role -ne "client" }).Count -gt 0) {
        $r.Result = "FAIL"; $r.Reasons.Add("wrong roles in bot state"); return $r
    }
    $act = $hostList | Where-Object { $_.acted } | Select-Object -First 1
    if (-not $act) {
        if ($Final) { $r.Result = "TIMEOUT"; $r.Reasons.Add("the host bot never looted a corpse (clone never settled?)") }
        return $r
    }
    if ($act.actionInfo -notmatch '^loot: corpse ([0-9A-F]{8}) killed, items before=(-?\d+) after=(-?\d+) moved=(-?\d+)') {
        $r.Result = "ERROR"; $r.Reasons.Add("host bot could not loot: $($act.actionInfo)"); return $r
    }
    $id = $Matches[1]; $moved = [int]$Matches[4]
    if ($moved -le 0) { $r.Result = "ERROR"; $r.Reasons.Add("host took nothing from the corpse: $($act.actionInfo)"); return $r }
    $t0 = [long]$act.actedAtWallMs
    $itemsOn = { param($s) $c = @($s.npcs | Where-Object { $_.id -eq $id }); if ($c.Count -gt 0) { [int]$c[0].items } else { $null } }
    # Precondition: the client has its own copy of the NPC before the take.
    $cBefore = @($clientList | Where-Object { $_.wallMs -lt $t0 -and $null -ne (& $itemsOn $_) })
    if ($cBefore.Count -eq 0) { $r.Result = "ERROR"; $r.Reasons.Add("the client has no copy of NPC $id before the host looted it"); return $r }
    $before = & $itemsOn $cBefore[-1]
    if ($before -le 0) { $r.Result = "ERROR"; $r.Reasons.Add("the client's corpse $id was already empty before the take ($before items)"); return $r }
    $cAfter = @($clientList | Where-Object { $_.wallMs -ge $t0 })
    $cLate  = @($cAfter | Where-Object { $_.wallMs -ge $t0 + 15000 })
    if ($cLate.Count -lt 5) {
        if ($Final) { $r.Result = "TIMEOUT"; $r.Reasons.Add("not enough client samples after the take ($($cLate.Count) of 5)") }
        return $r
    }
    $counts = @($cAfter | ForEach-Object { & $itemsOn $_ })
    $least = ($counts | Where-Object { $null -ne $_ } | Measure-Object -Minimum).Minimum
    $diedOnClient = @($clientList | Where-Object { @($_.npcs | Where-Object { $_.id -eq $id -and $_.dead }).Count -gt 0 }).Count -gt 0
    $r.Details = "loot: $($act.actionInfo); client copy of $id items before the take $before, lowest after $least; died on the client: $(if ($diedOnClient) { 'yes' } else { 'NO (death sync)' })"
    if (@($counts | Where-Object { $null -eq $_ }).Count -gt 0) { $r.Result = "FAIL"; $r.Reasons.Add("the client's corpse $id disappeared after the host looted it"); return $r }
    if ($least -lt $before) { $r.Result = "FAIL"; $r.Reasons.Add("the client's corpse lost items when the host looted ($before -> $least)"); return $r }
    if (@($cLate | Where-Object { -not $_.connected }).Count -gt 0) { $r.Result = "FAIL"; $r.Reasons.Add("the client left the session after the take"); return $r }
    if (-not $diedOnClient) { $r.Result = "FAIL"; $r.Reasons.Add("the host's kill of $id never reached the client (its copy is still alive)"); return $r }
    # Unchanged counts alone could also mean the take never arrived; require the
    # client's own decision for this corpse.
    if (-not ($ClientDecisions | Where-Object { $_ -match "from corpse 0x$id; keeping our own copy" })) {
        $r.Result = "FAIL"; $r.Reasons.Add("the client never logged keeping its own copy of corpse $id (take not received or not instanced)"); return $r
    }
    $r.Result = "PASS"
    return $r
}

# ---- reward scenario ----------------------------------------------------------
# The host bot gives itself N credits and one weapon the way a quest script does
# (no source container, reason kQuestReward). The client bot answers the first
# reward that reaches it by giving itself the same weapon, as its own copy of the
# quest would through a replicated stage script. With CoopQuestRewards=1 both
# players must end with +N credits and exactly +1 weapon: the mirror must not
# double the weapon on the client, nor the client's copy echo back to the host.
function Invoke-RewardScore {
    param($HostSamples, $ClientSamples, [bool]$Final, [string[]]$ClientDecisions = @())
    $r = @{ Result = "PENDING"; Reasons = New-Object System.Collections.Generic.List[string] }
    $hostList = @($HostSamples | Sort-Object wallMs); $clientList = @($ClientSamples | Sort-Object wallMs)
    if ($hostList.Count -eq 0 -or $clientList.Count -eq 0) {
        if ($Final) { $r.Result = "ERROR"; $r.Reasons.Add("no bot state from " + $(if ($hostList.Count -eq 0) { "host" } else { "client" })) }
        return $r
    }
    if (@($hostList | Where-Object { $_.role -ne "host" }).Count -gt 0 -or @($clientList | Where-Object { $_.role -ne "client" }).Count -gt 0) {
        $r.Result = "FAIL"; $r.Reasons.Add("wrong roles in bot state"); return $r
    }
    $act = $hostList | Where-Object { $_.acted } | Select-Object -First 1
    if (-not $act) {
        if ($Final) { $r.Result = "TIMEOUT"; $r.Reasons.Add("the host bot never got its reward (clone never settled?)") }
        return $r
    }
    if ($act.actionInfo -notmatch '^reward: credits x(\d+) given=1, item ([0-9A-F]{8}) x1 given=1') {
        $r.Result = "ERROR"; $r.Reasons.Add("host bot could not give the reward: $($act.actionInfo)"); return $r
    }
    $n = [int]$Matches[1]; $item = $Matches[2]
    if ($item -eq "00000000") { $r.Result = "ERROR"; $r.Reasons.Add("no weapon in the host's inventory to give"); return $r }
    $t0 = [long]$act.actedAtWallMs
    $hBefore = @($hostList | Where-Object { $_.wallMs -lt $t0 -and $_.reward.item -eq $item -and $_.reward.credits -ge 0 })
    $cBefore = @($clientList | Where-Object { $_.wallMs -lt $t0 -and $_.reward.item -eq $item -and $_.reward.credits -ge 0 })
    if ($hBefore.Count -eq 0 -or $cBefore.Count -eq 0) { $r.Result = "ERROR"; $r.Reasons.Add("no inventory count for weapon $item before the reward"); return $r }
    $cLate = @($clientList | Where-Object { $_.wallMs -ge $t0 + 20000 })
    $hLate = @($hostList | Where-Object { $_.wallMs -ge $t0 + 20000 })
    if ($cLate.Count -lt 3 -or $hLate.Count -lt 3) {
        if ($Final) { $r.Result = "TIMEOUT"; $r.Reasons.Add("not enough samples 20 s after the reward (client $($cLate.Count), host $($hLate.Count) of 3)") }
        return $r
    }
    $hb = $hBefore[-1].reward; $cb = $cBefore[-1].reward; $ha = $hLate[-1].reward; $ca = $cLate[-1].reward
    $dhc = $ha.credits - $hb.credits; $dhi = $ha.items - $hb.items
    $dcc = $ca.credits - $cb.credits; $dci = $ca.items - $cb.items
    $r.Details = "reward: $($act.actionInfo); host credits +$dhc weapon +$dhi; client credits +$dcc weapon +$dci; client's own copy: $($ca.ownCopy); peer rewards received: host $($ha.peerRewards), client $($ca.peerRewards)"
    if ($dhc -ne $n -or $dhi -ne 1) { $r.Result = "FAIL"; $r.Reasons.Add("the host should have +$n credits and +1 weapon, has +$dhc and +$dhi"); return $r }
    if ($dcc -ne $n) { $r.Result = "FAIL"; $r.Reasons.Add("the client should have +$n credits from the host's reward, has +$dcc"); return $r }
    if (-not $ca.ownCopy) { $r.Result = "ERROR"; $r.Reasons.Add("the client bot never gave itself its own copy of the weapon"); return $r }
    if ($dci -ne 1) { $r.Result = "FAIL"; $r.Reasons.Add("the client should have exactly +1 weapon (own quest copy, mirror cancelled), has +$dci"); return $r }
    if (@($cLate | Where-Object { -not $_.connected }).Count -gt 0) { $r.Result = "FAIL"; $r.Reasons.Add("the client left the session after the reward"); return $r }
    if (-not ($ClientDecisions | Where-Object { $_ -match "\+0x0000000F x$n \(partner's quest reward\)" })) {
        $r.Result = "FAIL"; $r.Reasons.Add("the client never logged granting the host's credits"); return $r
    }
    if (-not ($ClientDecisions | Where-Object { $_ -match "partner's reward 0x$item`: x1 already given by our own quest" })) {
        $r.Result = "FAIL"; $r.Reasons.Add("the client never logged cancelling the mirror of weapon $item against its own copy"); return $r
    }
    $r.Result = "PASS"
    return $r
}

# ---- friendlyfire scenario ----------------------------------------------------
# The host hits the client's figure twice as the attacker (60 %, then more than all of
# its health, 4 s apart). PASS: on the host the figure is back to full health 2.5 s
# after each hit and never dead; on the client its own health never drops and it is
# never downed. The hits must have landed (health right after < before), or the run
# cannot tell anything (ERROR).
function Invoke-FriendlyFireScore {
    param($HostSamples, $ClientSamples, [bool]$Final)
    $r = @{ Result = "PENDING"; Reasons = New-Object System.Collections.Generic.List[string] }
    $hostList = @($HostSamples | Sort-Object wallMs); $clientList = @($ClientSamples | Sort-Object wallMs)
    if ($hostList.Count -eq 0 -or $clientList.Count -eq 0) {
        if ($Final) { $r.Result = "ERROR"; $r.Reasons.Add("no bot state from " + $(if ($hostList.Count -eq 0) { "host" } else { "client" })) }
        return $r
    }
    if (@($hostList | Where-Object { $_.role -ne "host" }).Count -gt 0 -or @($clientList | Where-Object { $_.role -ne "client" }).Count -gt 0) {
        $r.Result = "FAIL"; $r.Reasons.Add("wrong roles in bot state"); return $r
    }
    $act = $hostList | Where-Object { $_.acted } | Select-Object -First 1
    $second = $hostList | Where-Object { $_.ff.hits -ge 2 } | Select-Object -First 1
    if (-not $act -or -not $second) {
        if ($Final) { $r.Result = "TIMEOUT"; $r.Reasons.Add("the host bot never hit the figure twice (clone never settled?)") }
        return $r
    }
    $t0 = [long]$act.actedAtWallMs; $t2 = [long]$second.wallMs
    $info = $second.actionInfo
    $hits = @([regex]::Matches($info, 'of (-?[\d.]+), figure health right after (-?[\d.]+)'))
    if ($hits.Count -lt 2) { $r.Result = "ERROR"; $r.Reasons.Add("cannot read the hits: $info"); return $r }
    foreach ($h in $hits) {
        if ([double]$h.Groups[2].Value -ge [double]$h.Groups[1].Value - 1) { $r.Result = "ERROR"; $r.Reasons.Add("a hit did not take the figure's health down: $info"); return $r }
    }
    $base = [double]$hits[0].Groups[1].Value
    $hLate = @($hostList | Where-Object { $_.wallMs -ge $t2 + 8000 })
    $cLate = @($clientList | Where-Object { $_.wallMs -ge $t2 + 8000 })
    if ($hLate.Count -lt 2 -or $cLate.Count -lt 2) {
        if ($Final) { $r.Result = "TIMEOUT"; $r.Reasons.Add("not enough samples 8 s after the second hit") }
        return $r
    }
    $cBefore = @($clientList | Where-Object { $_.wallMs -lt $t0 -and $_.ff.own -gt 0 })
    if ($cBefore.Count -eq 0) { $r.Result = "ERROR"; $r.Reasons.Add("no client health before the hits"); return $r }
    $ownBase = [double]$cBefore[-1].ff.own
    $cAfter = @($clientList | Where-Object { $_.wallMs -ge $t0 })
    $ownMin = ($cAfter | ForEach-Object { [double]$_.ff.own } | Measure-Object -Minimum).Minimum
    # Host samples that should show the figure healed: 2.5 s or more after a hit.
    $healedWindow = @($hostList | Where-Object { ($_.wallMs -ge $t0 + 2500 -and $_.wallMs -lt $t2) -or $_.wallMs -ge $t2 + 2500 } | Where-Object { $_.cloneVisible })
    $low = @($healedWindow | Where-Object { [double]$_.ff.figure -lt 0.95 * $base })
    $dead = @($hostList | Where-Object { $_.wallMs -ge $t0 -and $_.ff.figureDead })
    $lowest = ($healedWindow | ForEach-Object { [double]$_.ff.figure } | Measure-Object -Minimum).Minimum
    $r.Details = "friendlyfire: $info; figure health before $base, lowest 2.5 s+ after a hit: $lowest; heals $($hLate[-1].ff.heals); client health $ownBase -> lowest $ownMin"
    if ($dead.Count -gt 0) { $r.Result = "FAIL"; $r.Reasons.Add("the client's figure died on the host's screen"); return $r }
    if ($low.Count -gt 0) { $r.Result = "FAIL"; $r.Reasons.Add("the figure kept the damage: $($low.Count) samples below 95 % health 2.5 s or more after a hit"); return $r }
    if ($ownMin -lt $ownBase - 1) { $r.Result = "FAIL"; $r.Reasons.Add("the client's own health dropped from $ownBase to $ownMin"); return $r }
    if (@($cAfter | Where-Object { $_.ff.ownDowned }).Count -gt 0) { $r.Result = "FAIL"; $r.Reasons.Add("the client went down"); return $r }
    if (@($cLate | Where-Object { -not $_.connected }).Count -gt 0) { $r.Result = "FAIL"; $r.Reasons.Add("the client left the session after the hits"); return $r }
    $r.Result = "PASS"
    return $r
}

# ---- aim scenario --------------------------------------------------------------
# The host draws its weapon and aims 8 s up (+0.6 rad), 8 s down (-0.6 rad), then 8 s
# level without aiming. The client reports the pitch it received and its clone's head
# bone rotation. PASS: the pitch arrived, the head turned 0.35 rad (20 degrees) or more
# between the up and the down phase, the clone took the sights pose while the host
# aimed and left it after.
function Get-RotAngle($a, $b) {
    $tr = 0.0; for ($k = 0; $k -lt 9; $k++) { $tr += [double]$a[$k] * [double]$b[$k] }
    $c = ($tr - 1.0) / 2.0; if ($c -gt 1) { $c = 1 }; if ($c -lt -1) { $c = -1 }
    return [math]::Acos($c)
}
function Invoke-AimScore {
    param($HostSamples, $ClientSamples, [bool]$Final)
    $r = @{ Result = "PENDING"; Reasons = New-Object System.Collections.Generic.List[string] }
    $hostList = @($HostSamples | Sort-Object wallMs); $clientList = @($ClientSamples | Sort-Object wallMs)
    if ($hostList.Count -eq 0 -or $clientList.Count -eq 0) {
        if ($Final) { $r.Result = "ERROR"; $r.Reasons.Add("no bot state from " + $(if ($hostList.Count -eq 0) { "host" } else { "client" })) }
        return $r
    }
    if (@($hostList | Where-Object { $_.role -ne "host" }).Count -gt 0 -or @($clientList | Where-Object { $_.role -ne "client" }).Count -gt 0) {
        $r.Result = "FAIL"; $r.Reasons.Add("wrong roles in bot state"); return $r
    }
    $up = @($hostList | Where-Object { $_.aim.steering -and $_.aim.sent -gt 0.5 })
    $down = @($hostList | Where-Object { $_.aim.steering -and $_.aim.sent -lt -0.5 })
    $level = @($hostList | Where-Object { $_.aim.steering -and [math]::Abs($_.aim.sent) -lt 0.01 })
    if ($up.Count -lt 3 -or $down.Count -lt 3 -or $level.Count -lt 3) {
        if ($Final) { $r.Result = "TIMEOUT"; $r.Reasons.Add("the host bot never ran all aim phases (up $($up.Count), down $($down.Count), level $($level.Count) samples)") }
        return $r
    }
    $endLevel = [long]$level[-1].wallMs
    if (@($clientList | Where-Object { $_.wallMs -ge $endLevel }).Count -lt 1) {
        if ($Final) { $r.Result = "TIMEOUT"; $r.Reasons.Add("no client samples after the aim phases") }
        return $r
    }
    # Client samples well inside each phase (2 s for the pitch to arrive and the head to turn).
    $cUp = @($clientList | Where-Object { $_.wallMs -ge [long]$up[0].wallMs + 2000 -and $_.wallMs -le [long]$up[-1].wallMs -and $_.aim.gotHead })
    $cDown = @($clientList | Where-Object { $_.wallMs -ge [long]$down[0].wallMs + 2000 -and $_.wallMs -le [long]$down[-1].wallMs -and $_.aim.gotHead })
    $cLevel = @($clientList | Where-Object { $_.wallMs -ge [long]$level[0].wallMs + 3000 -and $_.wallMs -le $endLevel })
    if ($cUp.Count -lt 2 -or $cDown.Count -lt 2) { $r.Result = "ERROR"; $r.Reasons.Add("no head bone readings on the client (up $($cUp.Count), down $($cDown.Count))"); return $r }
    $recvUp = @($cUp | Where-Object { [math]::Abs([math]::Abs([double]$_.aim.recv) - 0.6) -lt 0.05 }).Count
    $recvDown = @($cDown | Where-Object { [math]::Abs([math]::Abs([double]$_.aim.recv) - 0.6) -lt 0.05 }).Count
    $turn = Get-RotAngle $cUp[-1].aim.head $cDown[-1].aim.head
    $noise = [math]::Max((Get-RotAngle $cUp[0].aim.head $cUp[-1].aim.head), (Get-RotAngle $cDown[0].aim.head $cDown[-1].aim.head))
    $sightedWhileAiming = @($cUp + $cDown | Where-Object { $_.aim.cloneSighted -eq 1 }).Count
    $sightedAfter = @($cLevel | Where-Object { $_.aim.cloneSighted -eq 1 }).Count
    $r.Details = "aim: $($hostList | Where-Object { $_.acted } | Select-Object -First 1 -ExpandProperty actionInfo); pitch arrived up $recvUp/$($cUp.Count), down $recvDown/$($cDown.Count); head turned {0:N2} rad between up and down (within a phase at most {1:N2}); clone in sights while aiming $sightedWhileAiming/$($cUp.Count + $cDown.Count), after $sightedAfter/$($cLevel.Count)" -f $turn, $noise
    if ($recvUp -eq 0 -or $recvDown -eq 0) { $r.Result = "FAIL"; $r.Reasons.Add("the host's pitch did not arrive on the client"); return $r }
    if ($turn -lt 0.35) { $r.Result = "FAIL"; $r.Reasons.Add(("the clone's head turned only {0:N2} rad between looking up and down" -f $turn)); return $r }
    if ($sightedWhileAiming -eq 0) { $r.Result = "FAIL"; $r.Reasons.Add("the clone never took the sights pose while the host aimed"); return $r }
    if ($sightedAfter -gt 0) { $r.Result = "FAIL"; $r.Reasons.Add("the clone stayed in the sights pose after the host stopped aiming"); return $r }
    $r.Result = "PASS"
    return $r
}

# ---- jetpack scenario ----------------------------------------------------------
# The host jumps and fires its boostpack for 2.5 s. Both bots track heights every frame
# (min and max per sample). PASS: the host rose 1 m or more, the client's clone rose at
# least 70 % as high within 8 s, and the client's figure fired its own boostpack.
function Invoke-JetpackScore {
    param($HostSamples, $ClientSamples, [bool]$Final)
    $r = @{ Result = "PENDING"; Reasons = New-Object System.Collections.Generic.List[string] }
    $hostList = @($HostSamples | Sort-Object wallMs); $clientList = @($ClientSamples | Sort-Object wallMs)
    if ($hostList.Count -eq 0 -or $clientList.Count -eq 0) {
        if ($Final) { $r.Result = "ERROR"; $r.Reasons.Add("no bot state from " + $(if ($hostList.Count -eq 0) { "host" } else { "client" })) }
        return $r
    }
    if (@($hostList | Where-Object { $_.role -ne "host" }).Count -gt 0 -or @($clientList | Where-Object { $_.role -ne "client" }).Count -gt 0) {
        $r.Result = "FAIL"; $r.Reasons.Add("wrong roles in bot state"); return $r
    }
    $act = $hostList | Where-Object { $_.acted } | Select-Object -First 1
    if (-not $act) {
        if ($Final) { $r.Result = "TIMEOUT"; $r.Reasons.Add("the host bot never jumped (clone never settled?)") }
        return $r
    }
    $t0 = [long]$act.actedAtWallMs
    if (@($clientList | Where-Object { $_.wallMs -ge $t0 + 10000 }).Count -lt 2 -or @($hostList | Where-Object { $_.wallMs -ge $t0 + 10000 }).Count -lt 2) {
        if ($Final) { $r.Result = "TIMEOUT"; $r.Reasons.Add("not enough samples 10 s after the jump") }
        return $r
    }
    $hBefore = @($hostList | Where-Object { $_.wallMs -lt $t0 }); $cBefore = @($clientList | Where-Object { $_.wallMs -lt $t0 -and $_.cloneVisible })
    if ($hBefore.Count -eq 0 -or $cBefore.Count -eq 0) { $r.Result = "ERROR"; $r.Reasons.Add("no heights before the jump"); return $r }
    $hBase = [double]$hBefore[-1].player[2]; $cBase = [double]$cBefore[-1].clone[2]
    $hWin = @($hostList | Where-Object { $_.wallMs -ge $t0 - 1000 -and $_.wallMs -le $t0 + 8000 })
    $cWin = @($clientList | Where-Object { $_.wallMs -ge $t0 - 1000 -and $_.wallMs -le $t0 + 9000 })
    $hRise = ($hWin | ForEach-Object { [double]$_.jet.zMax } | Measure-Object -Maximum).Maximum - $hBase
    $cRise = ($cWin | Where-Object { [double]$_.jet.cloneZMax -gt -1e8 } | ForEach-Object { [double]$_.jet.cloneZMax } | Measure-Object -Maximum).Maximum - $cBase
    $mask = 0; foreach ($s in $hWin) { $mask = $mask -bor [int]$s.jet.jumpMask }
    $cLast = $clientList[-1].jet; $cFirst = $cBefore[-1].jet
    $starts = [int]$cLast.boostStarts - [int]$cFirst.boostStarts; $accepted = [int]$cLast.boostAccepted - [int]$cFirst.boostAccepted
    $r.Details = "jetpack: $($hostList[-1].actionInfo); host rose {0:N2} m (jump states seen: mask $mask), clone rose {1:N2} m; figure boostpack starts $starts, accepted $accepted" -f $hRise, $cRise
    if ($hRise -lt 1.0) { $r.Result = "ERROR"; $r.Reasons.Add(("the host rose only {0:N2} m: the bot's boost did not lift it" -f $hRise)); return $r }
    if ($cRise -lt 0.7 * $hRise) { $r.Result = "FAIL"; $r.Reasons.Add(("the clone rose {0:N2} m, the host {1:N2} m" -f $cRise, $hRise)); return $r }
    if ($accepted -lt 1) { $r.Result = "FAIL"; $r.Reasons.Add("the client's figure never fired its boostpack (starts $starts, accepted $accepted)"); return $r }
    $r.Result = "PASS"
    return $r
}

# ---- separate scenario ---------------------------------------------------------
# Separate inventories and skill trees. Both players load the same save; each then adds
# to its own item (host: 3 of the lowest MISC form, client: 5 of the lowest AMMO form),
# raises one of its own perks by a rank and adds skill points (host 3, client 1). The
# host bridges a fresh save (which carries only the host's changes) to the client.
# PASS, 15 s after the client loaded that save: the client has its own item count,
# perk rank and skill points and none of the host's; the host still has its own.
function Get-SepPerkRank($sample, [string]$perk) {
    foreach ($e in ($sample.sep.perks -split ",")) { if ($e -match "^$($perk):(-?\d+)$") { return [int]$Matches[1] } }
    return -1
}
function Invoke-SeparateScore {
    param($HostSamples, $ClientSamples, [bool]$Final)
    $r = @{ Result = "PENDING"; Reasons = New-Object System.Collections.Generic.List[string] }
    $hostList = @($HostSamples | Sort-Object wallMs); $clientList = @($ClientSamples | Sort-Object wallMs)
    if ($hostList.Count -eq 0 -or $clientList.Count -eq 0) {
        if ($Final) { $r.Result = "ERROR"; $r.Reasons.Add("no bot state from " + $(if ($hostList.Count -eq 0) { "host" } else { "client" })) }
        return $r
    }
    if (@($hostList | Where-Object { $_.role -ne "host" }).Count -gt 0 -or @($clientList | Where-Object { $_.role -ne "client" }).Count -gt 0) {
        $r.Result = "FAIL"; $r.Reasons.Add("wrong roles in bot state"); return $r
    }
    $hAct = $hostList | Where-Object { $_.acted -and $_.sep } | Select-Object -First 1
    $cAct = $clientList | Where-Object { $_.acted -and $_.sep } | Select-Object -First 1
    if (-not $hAct -or -not $cAct) {
        if ($Final) { $r.Result = "TIMEOUT"; $r.Reasons.Add("the " + $(if (-not $hAct) { "host" } else { "client" }) + " bot never changed its character (clone never settled?)") }
        return $r
    }
    $h = $hAct.sep; $c = $cAct.sep
    if ($h.itemH -ne $c.itemH -or $h.itemC -ne $c.itemC -or $h.itemH -eq "00000000" -or $h.itemC -eq "00000000") {
        $r.Result = "ERROR"; $r.Reasons.Add("the two bots picked different or no items (host $($h.itemH)/$($h.itemC), client $($c.itemH)/$($c.itemC))"); return $r
    }
    if ($h.perk -eq "00000000" -or $c.perk -eq "00000000" -or $h.perk -eq $c.perk) {
        $r.Result = "ERROR"; $r.Reasons.Add("no own perk could be raised (host $($h.perk), client $($c.perk)): $($hAct.actionInfo) | $($cAct.actionInfo)"); return $r
    }
    $bridge = $hostList | Where-Object { $_.sep -and [long]$_.sep.bridgeAtWallMs -gt 0 } | Select-Object -First 1
    if (-not $bridge) {
        if ($Final) { $r.Result = "ERROR"; $r.Reasons.Add("the host never bridged a save to the client") }
        return $r
    }
    $loadedFirst = $clientList | Where-Object { $_.sep -and $_.sep.loadedSinceAct } | Select-Object -First 1
    if (-not $loadedFirst) {
        if ($Final) { $r.Result = "ERROR"; $r.Reasons.Add("the client never loaded the bridged save") }
        return $r
    }
    $tLate = [long]$loadedFirst.wallMs + 15000
    $cLate = @($clientList | Where-Object { $_.wallMs -ge $tLate -and $_.sep -and $_.inWorld })
    $hLate = @($hostList | Where-Object { $_.wallMs -ge $tLate -and $_.sep })
    if ($cLate.Count -lt 3 -or $hLate.Count -lt 1) {
        if ($Final) { $r.Result = "TIMEOUT"; $r.Reasons.Add("not enough samples 15 s after the client loaded the bridged save") }
        return $r
    }
    $cs = $cLate[-1]; $hs = $hLate[-1]
    $want = @(
        @{ Who = "client"; S = $cs; What = "host's item $($h.itemH)"; Got = [int]$cs.sep.cntH; Want = [int]$h.baseH },
        @{ Who = "client"; S = $cs; What = "own item $($c.itemC)";    Got = [int]$cs.sep.cntC; Want = [int]$c.baseC + 5 },
        @{ Who = "client"; S = $cs; What = "skill points";           Got = [int]$cs.sep.sp;   Want = [int]$c.baseSp + 1 },
        @{ Who = "client"; S = $cs; What = "own perk $($c.perk) rank"; Got = (Get-SepPerkRank $cs $c.perk); Want = [int]$c.perkAfter },
        @{ Who = "client"; S = $cs; What = "host's perk $($h.perk) rank"; Got = (Get-SepPerkRank $cs $h.perk); Want = [int]$h.perkBefore },
        @{ Who = "host";   S = $hs; What = "own item $($h.itemH)";    Got = [int]$hs.sep.cntH; Want = [int]$h.baseH + 3 },
        @{ Who = "host";   S = $hs; What = "client's item $($c.itemC)"; Got = [int]$hs.sep.cntC; Want = [int]$h.baseC },
        @{ Who = "host";   S = $hs; What = "skill points";           Got = [int]$hs.sep.sp;   Want = [int]$h.baseSp + 3 },
        @{ Who = "host";   S = $hs; What = "own perk $($h.perk) rank"; Got = (Get-SepPerkRank $hs $h.perk); Want = [int]$h.perkAfter },
        @{ Who = "host";   S = $hs; What = "client's perk $($c.perk) rank"; Got = (Get-SepPerkRank $hs $c.perk); Want = [int]$c.perkBefore }
    )
    $wrong = @($want | Where-Object { $_.Got -ne $_.Want })
    $r.Details = "separate: host: $($hAct.actionInfo) | client: $($cAct.actionInfo); client loaded the bridged save at +$([long]$loadedFirst.wallMs - [long]$bridge.sep.bridgeAtWallMs) ms (restores $($cs.sep.restores - $c.restoresAtAct)); " +
                 (($want | ForEach-Object { "$($_.Who) $($_.What) $($_.Got)/$($_.Want)" }) -join ", ")
    foreach ($w in $wrong) { $r.Reasons.Add("$($w.Who): $($w.What) is $($w.Got), should be $($w.Want)") }
    if ($wrong.Count -gt 0) { $r.Result = "FAIL"; return $r }
    if (@($cLate | Where-Object { -not $_.connected }).Count -gt 0) { $r.Result = "FAIL"; $r.Reasons.Add("the client left the session after the bridge"); return $r }
    $r.Result = "PASS"
    return $r
}

# ---- damage scenario -----------------------------------------------------------
# NPC damage both ways. The host hits a save-file NPC by 40 %, the client 20 s later
# another one by 30 % (the bot marks a local attack, as a shot does). PASS: within 3-15 s
# the client's copy of the host's victim lost at least half the host's damage, and the
# host's copy of the client's victim lost at least half the client's damage.
function Get-NpcHealth($sample, [string]$id) {
    $n = @($sample.npcs | Where-Object { $_.id -eq $id })
    if ($n.Count -gt 0) { return [double]$n[0].health }
    return $null
}
function Invoke-DamageScore {
    param($HostSamples, $ClientSamples, [bool]$Final)
    $r = @{ Result = "PENDING"; Reasons = New-Object System.Collections.Generic.List[string] }
    $hostList = @($HostSamples | Sort-Object wallMs); $clientList = @($ClientSamples | Sort-Object wallMs)
    if ($hostList.Count -eq 0 -or $clientList.Count -eq 0) {
        if ($Final) { $r.Result = "ERROR"; $r.Reasons.Add("no bot state from " + $(if ($hostList.Count -eq 0) { "host" } else { "client" })) }
        return $r
    }
    if (@($hostList | Where-Object { $_.role -ne "host" }).Count -gt 0 -or @($clientList | Where-Object { $_.role -ne "client" }).Count -gt 0) {
        $r.Result = "FAIL"; $r.Reasons.Add("wrong roles in bot state"); return $r
    }
    $hAct = $hostList | Where-Object { $_.acted -and $_.dmg } | Select-Object -First 1
    $cAct = $clientList | Where-Object { $_.acted -and $_.dmg } | Select-Object -First 1
    if (-not $hAct -or -not $cAct) {
        if ($Final) { $r.Result = "TIMEOUT"; $r.Reasons.Add("the " + $(if (-not $hAct) { "host" } else { "client" }) + " bot never hit an NPC (clone never settled?)") }
        return $r
    }
    foreach ($a in @($hAct, $cAct)) {
        if ($a.dmg.target -eq "00000000" -or [double]$a.dmg.after -gt [double]$a.dmg.before - 1) {
            $r.Result = "ERROR"; $r.Reasons.Add("the $($a.role) bot's hit did nothing: $($a.actionInfo)"); return $r
        }
    }
    $t1 = [long]$hAct.dmg.hitAtWallMs; $id1 = $hAct.dmg.target; $d1 = [double]$hAct.dmg.before - [double]$hAct.dmg.after
    $t2 = [long]$cAct.dmg.hitAtWallMs; $id2 = $cAct.dmg.target; $d2 = [double]$cAct.dmg.before - [double]$cAct.dmg.after
    $cWin = @($clientList | Where-Object { $_.wallMs -ge $t1 + 3000 -and $_.wallMs -lt [math]::Min($t1 + 15000, $t2) })
    $hWin = @($hostList | Where-Object { $_.wallMs -ge $t2 + 3000 -and $_.wallMs -lt $t2 + 15000 })
    if ($cWin.Count -lt 2 -or $hWin.Count -lt 2) {
        if ($Final) { $r.Result = "TIMEOUT"; $r.Reasons.Add("not enough samples after the hits (client $($cWin.Count), host $($hWin.Count))") }
        return $r
    }
    $cBefore = $clientList | Where-Object { $_.wallMs -lt $t1 } | Select-Object -Last 1
    $hBefore = $hostList | Where-Object { $_.wallMs -lt $t2 } | Select-Object -Last 1
    $c1Before = if ($cBefore) { Get-NpcHealth $cBefore $id1 } else { $null }
    $h2Before = if ($hBefore) { Get-NpcHealth $hBefore $id2 } else { $null }
    if ($null -eq $c1Before) { $r.Result = "ERROR"; $r.Reasons.Add("the client has no copy of the host's victim $id1"); return $r }
    if ($null -eq $h2Before) { $r.Result = "ERROR"; $r.Reasons.Add("the host has no copy of the client's victim $id2"); return $r }
    $c1Min = ($cWin | ForEach-Object { Get-NpcHealth $_ $id1 } | Where-Object { $null -ne $_ } | Measure-Object -Minimum).Minimum
    $h2Min = ($hWin | ForEach-Object { Get-NpcHealth $_ $id2 } | Where-Object { $null -ne $_ } | Measure-Object -Minimum).Minimum
    $r.Details = ("damage: host hit {0} by {1:N1} (client's copy {2:N1} -> lowest {3:N1}); client hit {4} by {5:N1} (host's copy {6:N1} -> lowest {7:N1})" -f `
        $id1, $d1, $c1Before, $c1Min, $id2, $d2, $h2Before, $h2Min)
    if ($null -eq $c1Min -or $c1Min -gt $c1Before - 0.5 * $d1) { $r.Result = "FAIL"; $r.Reasons.Add(("the client's copy of {0} did not take the host's hit ({1:N1} -> {2:N1}, hit {3:N1})" -f $id1, $c1Before, $c1Min, $d1)) }
    if ($null -eq $h2Min -or $h2Min -gt $h2Before - 0.5 * $d2) { $r.Result = "FAIL"; $r.Reasons.Add(("the host's copy of {0} did not take the client's hit ({1:N1} -> {2:N1}, hit {3:N1})" -f $id2, $h2Before, $h2Min, $d2)) }
    if ($r.Result -eq "FAIL") { return $r }
    $r.Result = "PASS"
    return $r
}

# ---- pickup scenario -----------------------------------------------------------
# The host takes the nearest loose item lying in the world (a save-file reference, the
# same on both machines). PASS: the client saw the item before, the host's copy is gone,
# and 8 s after the take the client's copy is gone too (disabled, deleted or no longer
# in the cell).
function Invoke-PickupScore {
    param($HostSamples, $ClientSamples, [bool]$Final)
    $r = @{ Result = "PENDING"; Reasons = New-Object System.Collections.Generic.List[string] }
    $hostList = @($HostSamples | Sort-Object wallMs); $clientList = @($ClientSamples | Sort-Object wallMs)
    if ($hostList.Count -eq 0 -or $clientList.Count -eq 0) {
        if ($Final) { $r.Result = "ERROR"; $r.Reasons.Add("no bot state from " + $(if ($hostList.Count -eq 0) { "host" } else { "client" })) }
        return $r
    }
    if (@($hostList | Where-Object { $_.role -ne "host" }).Count -gt 0 -or @($clientList | Where-Object { $_.role -ne "client" }).Count -gt 0) {
        $r.Result = "FAIL"; $r.Reasons.Add("wrong roles in bot state"); return $r
    }
    $act = $hostList | Where-Object { $_.acted } | Select-Object -First 1
    if (-not $act) {
        if ($Final) { $r.Result = "TIMEOUT"; $r.Reasons.Add("the host bot never took an item (clone never settled?)") }
        return $r
    }
    if ($act.actionInfo -notmatch '^pickup: took ([0-9A-F]{8}) .*activate=1') { $r.Result = "ERROR"; $r.Reasons.Add("host bot could not take an item: $($act.actionInfo)"); return $r }
    $id = $Matches[1]; $t0 = [long]$act.actedAtWallMs
    $cLate = @($clientList | Where-Object { $_.wallMs -ge $t0 + 8000 }); $hLate = @($hostList | Where-Object { $_.wallMs -ge $t0 + 8000 })
    if ($cLate.Count -lt 3 -or $hLate.Count -lt 1) {
        if ($Final) { $r.Result = "TIMEOUT"; $r.Reasons.Add("not enough samples 8 s after the take") }
        return $r
    }
    $cBefore = @($clientList | Where-Object { $_.wallMs -lt $t0 } | Select-Object -Last 3)
    $seenBefore = @($cBefore | Where-Object { @($_.items | Where-Object { $_.id -eq $id -and $_.present }).Count -gt 0 }).Count
    $hostStill = @($hLate | Where-Object { @($_.items | Where-Object { $_.id -eq $id -and $_.present }).Count -gt 0 }).Count
    $clientStill = @($cLate | Where-Object { @($_.items | Where-Object { $_.id -eq $id -and $_.present }).Count -gt 0 }).Count
    $r.Details = "pickup: $($act.actionInfo); on the client before: $seenBefore/$($cBefore.Count) samples; still there 8 s+ after: host $hostStill/$($hLate.Count), client $clientStill/$($cLate.Count)"
    if ($seenBefore -eq 0) { $r.Result = "ERROR"; $r.Reasons.Add("the client never saw item $id before the take"); return $r }
    if ($hostStill -gt 0) { $r.Result = "ERROR"; $r.Reasons.Add("item $id is still there on the host after its own take"); return $r }
    if ($clientStill -gt 0) { $r.Result = "FAIL"; $r.Reasons.Add("item $id is still there on the client 8 s after the host took it"); return $r }
    if (@($cLate | Where-Object { -not $_.connected }).Count -gt 0) { $r.Result = "FAIL"; $r.Reasons.Add("the client left the session after the take"); return $r }
    $r.Result = "PASS"
    return $r
}

# ---- reconnect scenario --------------------------------------------------------
# The client's network goes dark for 25 s. PASS: the client lost the connection, then
# reconnected by itself within 150 s of the outage's end, and afterwards both sides see
# the partner's figure again.
function Invoke-ReconnectScore {
    param($HostSamples, $ClientSamples, [bool]$Final)
    $r = @{ Result = "PENDING"; Reasons = New-Object System.Collections.Generic.List[string] }
    $hostList = @($HostSamples | Sort-Object wallMs); $clientList = @($ClientSamples | Sort-Object wallMs)
    if ($hostList.Count -eq 0 -or $clientList.Count -eq 0) {
        if ($Final) { $r.Result = "ERROR"; $r.Reasons.Add("no bot state from " + $(if ($hostList.Count -eq 0) { "host" } else { "client" })) }
        return $r
    }
    if (@($hostList | Where-Object { $_.role -ne "host" }).Count -gt 0 -or @($clientList | Where-Object { $_.role -ne "client" }).Count -gt 0) {
        $r.Result = "FAIL"; $r.Reasons.Add("wrong roles in bot state"); return $r
    }
    $act = $clientList | Where-Object { $_.acted } | Select-Object -First 1
    if (-not $act) {
        if ($Final) { $r.Result = "TIMEOUT"; $r.Reasons.Add("the client bot never cut its network (clone never settled?)") }
        return $r
    }
    $t0 = [long]$act.actedAtWallMs; $tEnd = $t0 + 25000
    $dropped = $clientList | Where-Object { $_.wallMs -ge $t0 -and -not $_.connected } | Select-Object -First 1
    $back = $clientList | Where-Object { $_.wallMs -ge $tEnd -and $_.connected -and $_.cloneVisible -and $_.reconnects -ge 1 } | Select-Object -First 1
    $hostBack = if ($back) { $hostList | Where-Object { $_.wallMs -ge [long]$back.wallMs -and $_.cloneVisible } | Select-Object -First 1 } else { $null }
    $deadline = $tEnd + 150000
    $lastSeen = [long]$clientList[-1].wallMs
    $r.Details = "reconnect: $($act.actionInfo); dropped at +$(if ($dropped) { [long]$dropped.wallMs - $t0 } else { '-' }) ms; back with the partner's figure at +$(if ($back) { [long]$back.wallMs - $t0 } else { '-' }) ms (reconnects $($clientList[-1].reconnects)); host sees the client's figure again at +$(if ($hostBack) { [long]$hostBack.wallMs - $t0 } else { '-' }) ms"
    if (-not $dropped) {
        if ($lastSeen -ge $tEnd) { $r.Result = "ERROR"; $r.Reasons.Add("the connection never dropped during the outage"); return $r }
        if ($Final) { $r.Result = "TIMEOUT"; $r.Reasons.Add("no client samples after the outage began") }
        return $r
    }
    if ($back -and $hostBack) { $r.Result = "PASS"; return $r }
    if ($lastSeen -ge $deadline -or ($Final -and $lastSeen -ge $tEnd)) {
        $r.Result = "FAIL"
        $r.Reasons.Add($(if (-not $back) { "the client did not come back within 150 s of the outage's end" } else { "the host never saw the client's figure again" }))
        return $r
    }
    return $r
}

# ---- papyrus scenario ---------------------------------------------------------
# C++ -> Papyrus dispatch check. The host bot dispatches PULSAR's own
# PulsarFurnSync.DoPlayerFollowMoveTo (the move-aboard / follow path) at itself.
# PASS when the VM accepted it and the host is still running and connected 20 s
# later; a game that ends is the failure this looks for (the launcher reports it).
function Invoke-PapyrusScore {
    param($HostSamples, $ClientSamples, [bool]$Final)
    $r = @{ Result = "PENDING"; Reasons = New-Object System.Collections.Generic.List[string] }
    $hostList = @($HostSamples | Sort-Object wallMs)
    $act = $hostList | Where-Object { $_.acted } | Select-Object -First 1
    if (-not $act) {
        if ($Final) { $r.Result = "TIMEOUT"; $r.Reasons.Add("the host bot never dispatched") }
        return $r
    }
    if ($act.actionInfo -notmatch '^papyrus: .* dispatched=1') {
        $r.Result = "FAIL"; $r.Reasons.Add("the VM refused the dispatch: $($act.actionInfo)"); return $r
    }
    $late = @($hostList | Where-Object { $_.wallMs -ge [long]$act.actedAtWallMs + 20000 })
    if ($late.Count -lt 3) {
        if ($Final) { $r.Result = "TIMEOUT"; $r.Reasons.Add("not enough host samples 20 s after the dispatch") }
        return $r
    }
    $r.Details = "papyrus: $($act.actionInfo); host alive 20 s later, connected=$($late[-1].connected)"
    if (-not $late[-1].connected) { $r.Result = "FAIL"; $r.Reasons.Add("the host lost the session after the dispatch"); return $r }
    $r.Result = "PASS"
    return $r
}

# ---- space scenario -----------------------------------------------------------
# Phase 5: stations and encounters can differ between players in space. The
# takeoff scenario flies both players up (use the takeoff save); every 10 s each
# bot lists the references in the space cell around the ship ("ID:BASE:TYPE:KM").
# 40 s after the host reached space, the last census of each side must match:
# the same cell, the same save-file references (by ID) and the same runtime
# ones (0xFF IDs differ per game, so those compare by base form, as a multiset).
function ConvertFrom-SpaceRefs {
    param([string]$Refs)
    $out = @()
    foreach ($e in ($Refs -split ',')) {
        $p = $e -split ':'
        if ($p.Count -ge 4) { $out += [pscustomobject]@{ id = $p[0]; base = $p[1]; type = $p[2]; km = [double]$p[3] } }
    }
    return ,$out
}

function Invoke-SpaceScore {
    param($HostSamples, $ClientSamples, [bool]$Final, [string[]]$ClientDecisions = @(), [bool]$SpaceTravel = $false)
    $r = Invoke-TakeoffScore -HostSamples $HostSamples -ClientSamples $ClientSamples -Final $Final -ClientDecisions $ClientDecisions
    if ($r.Result -ne "PASS") { return $r }
    $hostList = @($HostSamples | Sort-Object wallMs); $clientList = @($ClientSamples | Sort-Object wallMs)
    $space = @($hostList | Where-Object { $_.ship.inSpace }) | Select-Object -First 1
    # Phase 2: 30 s after reaching space the host moves its ship next to a station
    # and bridges its save, as an arrival does; compare 150 s after the move (the
    # orbit right after takeoff is empty).
    $trav = @($hostList | Where-Object { $_.PSObject.Properties["travelInfo"] -and $_.travelInfo }) | Select-Object -Last 1
    $travelAt = [long](@($hostList | ForEach-Object { if ($_.space -and $_.space.PSObject.Properties["travelAtWallMs"]) { [long]$_.space.travelAtWallMs } else { 0 } }) + 0 | Measure-Object -Maximum).Maximum
    if ($trav) { $r.Details += "; $($trav.travelInfo)" }
    if ($trav -and $trav.travelInfo -match 'queued=0') { $r.Result = "ERROR"; $r.Reasons.Add("the host bot could not move the ship: $($trav.travelInfo)"); return $r }
    $tCheck = if ($travelAt -gt 0) { [long]$travelAt + 150000 } else { [long]$space.wallMs + 40000 }
    $hc = @($hostList | Where-Object { $_.space -and [long]$_.space.atWallMs -ge $tCheck }) | Select-Object -Last 1
    $cc = @($clientList | Where-Object { $_.space -and [long]$_.space.atWallMs -ge $tCheck }) | Select-Object -Last 1
    if (-not $hc -or -not $cc -or ($travelAt -eq 0 -and -not $Final -and $SpaceTravel)) {
        $r.Result = $(if ($Final) { "TIMEOUT" } else { "PENDING" })
        if ($Final) { $r.Reasons.Add("no space census from the " + $(if (-not $hc) { "host" } else { "client" }) + " after takeoff/travel") }
        return $r
    }
    $h = ConvertFrom-SpaceRefs $hc.space.refs; $c = ConvertFrom-SpaceRefs $cc.space.refs
    $hS = @($h | Where-Object { -not $_.id.StartsWith("FF") } | ForEach-Object { $_.id }); $cS = @($c | Where-Object { -not $_.id.StartsWith("FF") } | ForEach-Object { $_.id })
    $hD = @($h | Where-Object { $_.id.StartsWith("FF") } | ForEach-Object { $_.base } | Sort-Object); $cD = @($c | Where-Object { $_.id.StartsWith("FF") } | ForEach-Object { $_.base } | Sort-Object)
    $missS = @($hS | Where-Object { $cS -notcontains $_ }); $extraS = @($cS | Where-Object { $hS -notcontains $_ })
    $dd = @(Compare-Object -ReferenceObject $hD -DifferenceObject $cD)
    $missD = @($dd | Where-Object { $_.SideIndicator -eq "<=" } | ForEach-Object { $_.InputObject })
    $extraD = @($dd | Where-Object { $_.SideIndicator -eq "=>" } | ForEach-Object { $_.InputObject })
    $show = { param($ids, $list) ($ids | Select-Object -First 8 | ForEach-Object { $id = $_; $e = $list | Where-Object { $_.id -eq $id -or $_.base -eq $id } | Select-Object -First 1; if ($e) { "$($e.id)/$($e.base)/t$($e.type)@$($e.km)km" } else { $id } }) -join ", " }
    $r.Details += "; space census: host cell $($hc.space.cell) $($h.Count) refs ($($hS.Count) save-file, $($hD.Count) runtime), client cell $($cc.space.cell) $($c.Count) refs ($($cS.Count), $($cD.Count))"
    # Nothing around the host: the comparison would pass on two empty lists (run
    # 20261031-004820: the ship never moved, both censuses empty).
    if ($h.Count -eq 0) { $r.Result = "ERROR"; $r.Reasons.Add("nothing in space around the host to compare (cell $($hc.space.cell))"); return $r }
    if ($hc.space.cell -ne $cc.space.cell) { $r.Result = "FAIL"; $r.Reasons.Add("different space cells (host $($hc.space.cell), client $($cc.space.cell))") }
    if ($missS.Count) { $r.Result = "FAIL"; $r.Reasons.Add("$($missS.Count) save-file refs only the host has: $(& $show $missS $h)") }
    if ($extraS.Count) { $r.Result = "FAIL"; $r.Reasons.Add("$($extraS.Count) save-file refs only the client has: $(& $show $extraS $c)") }
    if ($missD.Count) { $r.Result = "FAIL"; $r.Reasons.Add("$($missD.Count) runtime refs only the host has (by base): $(& $show $missD $h)") }
    if ($extraD.Count) { $r.Result = "FAIL"; $r.Reasons.Add("$($extraD.Count) runtime refs only the client has (by base): $(& $show $extraD $c)") }
    return $r
}

# ---- scene scenario -----------------------------------------------------------
# The host bot starts a scene (SceneID, default SQ_CCT_Enviro_RakingScene 000D8783,
# its quest first). Each bot reports the scene events it saw ("ID:TYPE:WALLMS",
# type 0 started, 1 ended). The host must have played it; the client must start
# the same scene within 10 s of the host.
function Get-SceneStarts {
    param($Samples, [string]$Id)
    $starts = @{}
    foreach ($s in $Samples) {
        if (-not $s.scenes) { continue }
        foreach ($e in ($s.scenes -split ',')) {
            $p = $e -split ':'
            if ($p.Count -eq 3 -and $p[0] -eq $Id -and $p[1] -eq "0") { $starts[[long]$p[2]] = $true }
        }
    }
    return @($starts.Keys | Sort-Object)
}

function Invoke-SceneScore {
    param($HostSamples, $ClientSamples, [bool]$Final)
    $r = @{ Result = "PENDING"; Reasons = New-Object System.Collections.Generic.List[string] }
    $hostList = @($HostSamples | Sort-Object wallMs); $clientList = @($ClientSamples | Sort-Object wallMs)
    if ($hostList.Count -eq 0 -or $clientList.Count -eq 0) {
        if ($Final) { $r.Result = "ERROR"; $r.Reasons.Add("no bot state from " + $(if ($hostList.Count -eq 0) { "host" } else { "client" })) }
        return $r
    }
    $act = $hostList | Where-Object { $_.acted } | Select-Object -First 1
    if (-not $act) {
        if ($Final) { $r.Result = "TIMEOUT"; $r.Reasons.Add("the host bot never started a scene") }
        return $r
    }
    if ($act.actionInfo -notmatch '^scene: ([0-9A-F]{8}) found=1') { $r.Result = "ERROR"; $r.Reasons.Add("host bot could not start the scene: $($act.actionInfo)"); return $r }
    $id = $Matches[1]; $t0 = [long]$act.actedAtWallMs
    $hStart = @(Get-SceneStarts $hostList $id | Where-Object { $_ -ge $t0 - 1000 }) | Select-Object -First 1
    if (-not $hStart) {
        if ($Final -or (NowWallMsOr $hostList) -gt $t0 + 30000) { $r.Result = "ERROR"; $r.Reasons.Add("the scene $id never started on the host (30 s)") }
        return $r
    }
    $cStart = @(Get-SceneStarts $clientList $id | Where-Object { $_ -ge [long]$hStart - 2000 -and $_ -le [long]$hStart + 10000 }) | Select-Object -First 1
    $r.Details = "scene: $($act.actionInfo); host played $id at +$([long]$hStart - $t0) ms; client " + $(if ($cStart) { "at +$([long]$cStart - $t0) ms" } else { "never" })
    if ($cStart) { $r.Result = "PASS"; return $r }
    $lastClient = [long]$clientList[-1].wallMs
    if ($lastClient -gt [long]$hStart + 15000 -or $Final) { $r.Result = "FAIL"; $r.Reasons.Add("the client did not play scene $id within 10 s of the host") }
    return $r
}

# Latest wall time seen in a sample list (scorers run on recorded runs too).
function NowWallMsOr { param($List) return [long]($List | Select-Object -Last 1).wallMs }

# ---- quest scenario -----------------------------------------------------------
# The host bot starts a stopped Akila quest (not main quest). Each bot reports the
# quests that started on its machine ("ID:WALLMS"). The host's quest must have
# started; the client must start the same quest within 10 s of the host.
function Get-QuestStarts {
    param($Samples, [string]$Id)
    $starts = @{}
    foreach ($s in $Samples) {
        if (-not $s.questStarts) { continue }
        foreach ($e in ($s.questStarts -split ',')) {
            $p = $e -split ':'
            if ($p.Count -eq 2 -and $p[0] -eq $Id) { $starts[[long]$p[1]] = $true }
        }
    }
    return @($starts.Keys | Sort-Object)
}

function Invoke-QuestScore {
    param($HostSamples, $ClientSamples, [bool]$Final)
    $r = @{ Result = "PENDING"; Reasons = New-Object System.Collections.Generic.List[string] }
    $hostList = @($HostSamples | Sort-Object wallMs); $clientList = @($ClientSamples | Sort-Object wallMs)
    if ($hostList.Count -eq 0 -or $clientList.Count -eq 0) {
        if ($Final) { $r.Result = "ERROR"; $r.Reasons.Add("no bot state from " + $(if ($hostList.Count -eq 0) { "host" } else { "client" })) }
        return $r
    }
    $act = $hostList | Where-Object { $_.acted } | Select-Object -First 1
    if (-not $act) {
        if ($Final) { $r.Result = "TIMEOUT"; $r.Reasons.Add("the host bot never started a quest") }
        return $r
    }
    if ($act.actionInfo -notmatch '^quest: ([0-9A-F]{8}) found=1') { $r.Result = "ERROR"; $r.Reasons.Add("host bot could not start a quest: $($act.actionInfo)"); return $r }
    $id = $Matches[1]; $t0 = [long]$act.actedAtWallMs
    $hStart = @(Get-QuestStarts $hostList $id | Where-Object { $_ -ge $t0 - 1000 }) | Select-Object -First 1
    if (-not $hStart) {
        if ($Final -or (NowWallMsOr $hostList) -gt $t0 + 30000) { $r.Result = "ERROR"; $r.Reasons.Add("quest $id runs on the host but its start was not recorded") }
        return $r
    }
    $cStart = @(Get-QuestStarts $clientList $id | Where-Object { $_ -ge [long]$hStart - 2000 -and $_ -le [long]$hStart + 10000 }) | Select-Object -First 1
    $r.Details = "quest: $($act.actionInfo); host started $id at +$([long]$hStart - $t0) ms; client " + $(if ($cStart) { "at +$([long]$cStart - $t0) ms" } else { "never" })
    if ($cStart) { $r.Result = "PASS"; return $r }
    if ([long]$clientList[-1].wallMs -gt [long]$hStart + 15000 -or $Final) { $r.Result = "FAIL"; $r.Reasons.Add("the client did not start quest $id within 10 s of the host") }
    return $r
}

# ---- mission scenario ---------------------------------------------------------
# Both bots pick the same running mission-board quest. The client stops it quietly
# (a mission the host has and the client lacks); the host then advances a stage in
# it. Within 120 s the client must run the quest again at the host's new stage.
function Invoke-MissionScore {
    param($HostSamples, $ClientSamples, [bool]$Final)
    $r = @{ Result = "PENDING"; Reasons = New-Object System.Collections.Generic.List[string] }
    $hostList = @($HostSamples | Sort-Object wallMs); $clientList = @($ClientSamples | Sort-Object wallMs)
    if ($hostList.Count -eq 0 -or $clientList.Count -eq 0) {
        if ($Final) { $r.Result = "ERROR"; $r.Reasons.Add("no bot state from " + $(if ($hostList.Count -eq 0) { "host" } else { "client" })) }
        return $r
    }
    $act = $hostList | Where-Object { $_.acted } | Select-Object -First 1
    if (-not $act) {
        if ($Final) { $r.Result = "TIMEOUT"; $r.Reasons.Add("the host bot never advanced a mission") }
        return $r
    }
    if ($act.actionInfo -notmatch '^mission: ([0-9A-F]{8}) found=1') { $r.Result = "ERROR"; $r.Reasons.Add("no mission to test: $($act.actionInfo)"); return $r }
    $id = $Matches[1]; $t0 = [long]$act.actedAtWallMs
    $before = @($hostList | Where-Object { $_.wallMs -lt $t0 -and $_.mission -and [int]$_.mission.stage -ge 0 }) | Select-Object -Last 1
    $stopped = @($clientList | Where-Object { $_.wallMs -lt $t0 -and $_.mission -and $_.mission.id -eq $id -and [long]$_.mission.stoppedAtWallMs -gt 0 -and -not $_.mission.running })
    if ($stopped.Count -eq 0) { $r.Result = "ERROR"; $r.Reasons.Add("the client's copy of $id was never stopped before the host acted"); return $r }
    $hostNow = @($hostList | Where-Object { $_.wallMs -ge $t0 + 5000 -and $_.mission -and [int]$_.mission.stage -ge 0 }) | Select-Object -Last 1
    if (-not $hostNow) {
        if ($Final) { $r.Result = "ERROR"; $r.Reasons.Add("no host stage after the advance") }
        return $r
    }
    $hb = if ($before) { [int]$before.mission.stage } else { -1 }
    $ha = [int]$hostNow.mission.stage
    if ($ha -le $hb) {
        if ($Final -or [long]$hostNow.wallMs -gt $t0 + 30000) { $r.Result = "ERROR"; $r.Reasons.Add("the host's stage of $id did not rise ($hb -> $ha)") }
        return $r
    }
    $ok = @($clientList | Where-Object { $_.wallMs -ge $t0 -and $_.mission -and $_.mission.running -and [int]$_.mission.stage -eq $ha }) | Select-Object -First 1
    $lastC = $clientList[-1]
    $r.Details = "mission: $($act.actionInfo); host stage $hb -> $ha; client " + $(if ($ok) { "runs it at stage $ha from +$([long]$ok.wallMs - $t0) ms (loaded the host's save: $($ok.mission.loadedSincePick))" } else { "running=$($lastC.mission.running) stage=$($lastC.mission.stage)" })
    if ($ok) { $r.Result = "PASS"; return $r }
    if ($Final -or [long]$lastC.wallMs -gt $t0 + 120000) { $r.Result = "FAIL"; $r.Reasons.Add("the client does not run $id at the host's stage $ha (running=$($lastC.mission.running), stage=$($lastC.mission.stage))") }
    return $r
}

# ---- takeoff scenario ---------------------------------------------------------
# Both players start aboard the landed ship (Save10, "Star Eagle"). The host bot
# sits in the pilot seat, opens the takeoff menu and asks for the launch. The
# client bot answers the ready vote 10 s after the request arrives. With
# CoopTakeoffVote=1 the ship must not leave before the client's answer, must
# leave after it, and the client must end up in space with it (save bridge).
function Invoke-TakeoffScore {
    param($HostSamples, $ClientSamples, [bool]$Final, [string[]]$ClientDecisions = @(), [bool]$PartnerOutside = $false, [bool]$F8DuringVote = $false)
    $r = @{ Result = "PENDING"; Reasons = New-Object System.Collections.Generic.List[string] }
    $hostList = @($HostSamples | Sort-Object wallMs); $clientList = @($ClientSamples | Sort-Object wallMs)
    if ($hostList.Count -eq 0 -or $clientList.Count -eq 0) {
        if ($Final) { $r.Result = "ERROR"; $r.Reasons.Add("no bot state from " + $(if ($hostList.Count -eq 0) { "host" } else { "client" })) }
        return $r
    }
    if (@($hostList | Where-Object { $_.role -ne "host" }).Count -gt 0 -or @($clientList | Where-Object { $_.role -ne "client" }).Count -gt 0) {
        $r.Result = "FAIL"; $r.Reasons.Add("wrong roles in bot state"); return $r
    }
    $act = $hostList | Where-Object { $_.acted } | Select-Object -First 1
    if (-not $act) {
        if ($Final) { $r.Result = "TIMEOUT"; $r.Reasons.Add("the host bot never asked for the launch") }
        return $r
    }
    if ($act.actionInfo -notmatch '^takeoff: seat ([0-9A-F]{8}) activate=1, seated=1, launch fired=1') {
        $r.Result = "ERROR"; $r.Reasons.Add("host bot could not ask for the launch: $($act.actionInfo)"); return $r
    }
    $t0 = [long]$act.actedAtWallMs
    if (@($hostList | Where-Object { $_.wallMs -lt $t0 -and $_.ship.inSpace }).Count -gt 0) {
        $r.Result = "ERROR"; $r.Reasons.Add("the host was in space before the launch request"); return $r
    }
    $space = @($hostList | Where-Object { $_.wallMs -ge $t0 -and $_.ship.inSpace }) | Select-Object -First 1
    $readyAt = ($clientList | ForEach-Object { [long]$_.vote.readyAtWallMs } | Measure-Object -Maximum).Maximum
    $seenAt = ($clientList | ForEach-Object { [long]$_.vote.seenAtWallMs } | Measure-Object -Maximum).Maximum
    if ($space -and ($readyAt -eq 0 -or [long]$space.wallMs -lt $readyAt)) {
        $r.Result = "FAIL"; $r.Reasons.Add("the ship took off before the crew was ready (in space at +$([long]$space.wallMs - $t0) ms, ready $(if ($readyAt) { "at +$($readyAt - $t0) ms" } else { 'never' }))"); return $r
    }
    if (-not $space) {
        if ($Final) {
            $r.Result = "FAIL"
            $r.Reasons.Add($(if ($readyAt) { "the crew was ready but the ship never took off" } elseif ($seenAt) { "the client saw the request but never answered" } else { "the takeoff request never reached the client" }))
        }
        return $r
    }
    $late = @($clientList | Where-Object { $_.wallMs -ge [long]$space.wallMs + 40000 })
    if ($late.Count -lt 3) {
        if ($Final) { $r.Result = "TIMEOUT"; $r.Reasons.Add("not enough client samples 40 s after takeoff ($($late.Count) of 3)") }
        return $r
    }
    $r.Details = "takeoff: $($act.actionInfo); request seen by the client at +$($seenAt - $t0) ms, ready at +$($readyAt - $t0) ms, host in space at +$([long]$space.wallMs - $t0) ms; client now in space: $($late[-1].ship.inSpace)"
    if (-not $late[-1].ship.inSpace) { $r.Result = "FAIL"; $r.Reasons.Add("the client was left behind (not in space 40 s after takeoff)"); return $r }
    if (@($late | Where-Object { -not $_.connected }).Count -gt 0) { $r.Result = "FAIL"; $r.Reasons.Add("the client left the session after takeoff"); return $r }
    if (-not ($ClientDecisions | Where-Object { $_ -match "\[TAKEOFF-VOTE\] vote \d+ passed - boarding: " })) {
        $r.Result = "FAIL"; $r.Reasons.Add("the client never logged boarding for the vote"); return $r
    }
    if ($PartnerOutside) {
        # The client must have left the ship before the request, and the vote must
        # have moved it into the host's cell before the ship reached space (later,
        # the save bridge would put it there anyway).
        $shipCell = $act.playerCell
        $out = @($clientList | Where-Object { $_.wallMs -lt $seenAt -and $_.exitInfo -match 'activate=1' -and $_.playerCell -ne $shipCell })
        if ($out.Count -eq 0) { $r.Result = "ERROR"; $r.Reasons.Add("the client never left the ship before the request"); return $r }
        if (-not ($ClientDecisions | Where-Object { $_ -match "boarding: moving aboard" })) {
            $r.Result = "FAIL"; $r.Reasons.Add("the client did not move aboard (no 'boarding: moving aboard')"); return $r
        }
        $aboard = @($clientList | Where-Object { $_.wallMs -ge $readyAt -and $_.wallMs -lt [long]$space.wallMs -and $_.playerCell -eq $shipCell }) | Select-Object -First 1
        if (-not $aboard) { $r.Result = "FAIL"; $r.Reasons.Add("the client was not aboard (cell $shipCell) before the ship reached space"); return $r }
        $r.Details += "; client left the ship to cell $($out[-1].playerCell), aboard again at +$([long]$aboard.wallMs - $t0) ms"
    }
    if ($F8DuringVote) {
        # The pilot must have pressed F8 while the vote waited; otherwise the run
        # proves nothing about F8 (an early takeoff already failed above).
        $f8At = ($hostList | ForEach-Object { [long]$_.vote.f8AtWallMs } | Measure-Object -Maximum).Maximum
        if (-not $f8At -or $f8At -lt $t0 -or $f8At -ge $readyAt) {
            $r.Result = "ERROR"; $r.Reasons.Add("the host bot never pressed F8 during the vote"); return $r
        }
        $r.Details += "; F8 pressed at +$($f8At - $t0) ms"
    }
    $r.Result = "PASS"
    return $r
}

# "host->local" for every client match of a host save-file NPC (FormID below
# 0xFF000000, the same reference on both machines) to a different local NPC.
function Find-WrongNpcMatches([string[]]$Lines) {
    foreach ($l in $Lines) {
        if ($l -match 'MATCHED hostRef=0x([0-9A-F]{8}) .*-> localRef=0x([0-9A-F]{8})') {
            if (-not $Matches[1].StartsWith("FF") -and $Matches[1] -ne $Matches[2]) { "$($Matches[1])->$($Matches[2])" }
        }
    }
}

# ---- scorer self-test (playbook: test the scorer, not only the test) -------
function New-Sample($role, $t, $player, $clone, [bool]$visible = $true, [bool]$acted = $false, [long]$actedAt = 0) {
    [pscustomobject]@{ role = $role; wallMs = [long]$t; player = $player; clone = $clone; cloneVisible = $visible; acted = $acted; actedAtWallMs = $actedAt }
}
function New-DoorRun([bool]$clientFollows = $false, [bool]$hostChanges = $true, [string]$info = "door 0001ABCD of 2 load doors (2.8 m away), activate=1", [bool]$acts = $true, [bool]$decides = $true) {
    $hostRun = @(); $clientRun = @(); $t0 = 1000000; $doorAt = $t0 + 10000
    for ($i = 0; $i -lt 45; $i++) {
        $t = $t0 + $i * 1000; $after = $acts -and $t -ge $doorAt
        $hCell = if ($after -and $hostChanges) { "0000BEEF" } else { "032928FE" }
        $cCell = if ($after -and $clientFollows) { "0000BEEF" } else { "032928FE" }
        $hs = New-Sample "host" ($t + 50) @(0, 0, 0) @(1, 0, 0) $true $after $(if ($acts) { $doorAt } else { 0 })
        $hs | Add-Member playerCell $hCell; $hs | Add-Member actionInfo $(if ($acts) { $info } else { "" }); $hs | Add-Member connected $true
        $cs = New-Sample "client" $t @(1, 0, 0) @(0, 0, 0) $true
        $cs | Add-Member playerCell $cCell; $cs | Add-Member connected $true
        $hostRun += $hs; $clientRun += $cs
    }
    return @{ H = $hostRun; C = $clientRun; D = $(if ($decides) { @("[PULSAR] AutoFollow: host used door 0x0001ABCD to 0x0000BEEF - staying here (CoopDoorFollow=0)") } else { @() }) }
}
function New-LootRun([int]$clientAfter = 5, [bool]$diesOnClient = $true, [int]$moved = 5, [bool]$decides = $true, [bool]$acts = $true, [bool]$hasCopy = $true) {
    $hostRun = @(); $clientRun = @(); $t0 = 1000000; $deathAt = $t0 + 5000; $takeAt = $t0 + 15000
    $info = "loot: corpse 0001ABCD killed, items before=5 after=$(5 - $moved) moved=$moved"
    for ($i = 0; $i -lt 40; $i++) {
        $t = $t0 + $i * 1000; $taken = $acts -and $t -ge $takeAt
        $hs = New-Sample "host" ($t + 50) @(0, 0, 0) @(1, 0, 0) $true $taken $(if ($acts) { $takeAt } else { 0 })
        $hs | Add-Member actionInfo $(if ($acts) { $info } else { "" }); $hs | Add-Member connected $true
        $hs | Add-Member npcs @([pscustomobject]@{ id = "0001ABCD"; dead = ($t -ge $deathAt); items = $(if ($taken) { 5 - $moved } else { 5 }) })
        $cs = New-Sample "client" $t @(1, 0, 0) @(0, 0, 0) $true
        $cs | Add-Member connected $true
        $cs | Add-Member npcs @($(if ($hasCopy) { [pscustomobject]@{ id = "0001ABCD"; dead = ($diesOnClient -and $t -ge $deathAt + 2000); items = $(if ($taken) { $clientAfter } else { 5 }) } }) | Where-Object { $_ })
        $hostRun += $hs; $clientRun += $cs
    }
    $d = if ($decides) { @("[PULSAR] ContainerSync: instanced enemy loot - peer took item 0x0000000F x5 from corpse 0x0001ABCD; keeping our own copy") } else { @() }
    return @{ H = $hostRun; C = $clientRun; D = $d }
}
function New-RewardRun([int]$clientCredits = 500, [int]$clientItems = 1, [int]$hostItems = 1, [bool]$ownCopy = $true, [bool]$decides = $true, [bool]$acts = $true, [string]$item = "0001ABCD") {
    $hostRun = @(); $clientRun = @(); $t0 = 1000000; $actAt = $t0 + 10000
    $info = "reward: credits x500 given=1, item $item x1 given=1"
    for ($i = 0; $i -lt 40; $i++) {
        $t = $t0 + $i * 1000; $after = $acts -and $t -ge $actAt + 6000
        $hs = New-Sample "host" ($t + 50) @(0, 0, 0) @(1, 0, 0) $true ($acts -and $t -ge $actAt) $(if ($acts) { $actAt } else { 0 })
        $hs | Add-Member actionInfo $(if ($acts) { $info } else { "" }); $hs | Add-Member connected $true
        $hs | Add-Member reward ([pscustomobject]@{ item = $item; items = $(if ($after) { 2 + $hostItems } else { 2 }); credits = $(if ($after) { 1500 } else { 1000 }); peerRewards = 0; ownCopy = $false })
        $cs = New-Sample "client" $t @(1, 0, 0) @(0, 0, 0) $true
        $cs | Add-Member connected $true
        $cs | Add-Member reward ([pscustomobject]@{ item = $item; items = $(if ($after) { 2 + $clientItems } else { 2 }); credits = $(if ($after) { 1000 + $clientCredits } else { 1000 }); peerRewards = $(if ($after) { 2 } else { 0 }); ownCopy = ($ownCopy -and $after) })
        $hostRun += $hs; $clientRun += $cs
    }
    $d = if ($decides) { @("[PULSAR] [REWARD] +0x0000000F x500 (partner's quest reward)", "[PULSAR] [REWARD] partner's reward 0x$($item): x1 already given by our own quest") } else { @() }
    return @{ H = $hostRun; C = $clientRun; D = $d }
}
function New-FriendlyFireRun([bool]$heals = $true, [bool]$clientHurt = $false, [bool]$acts = $true, [bool]$lands = $true, [bool]$dies = $false) {
    $hostRun = @(); $clientRun = @(); $t0 = 1000000; $actAt = $t0 + 10000; $hit2 = $actAt + 4000
    $after1 = if ($lands) { 99 } else { 250 }; $after2 = if ($lands) { -126 } else { 250 }
    $info1 = "friendlyfire: hit 1: 60% of 250, figure health right after $after1"
    $info2 = "$info1; hit 2: 150% of 250, figure health right after $after2"
    for ($i = 0; $i -lt 40; $i++) {
        $t = $t0 + $i * 1000; $hitsNow = if (-not $acts) { 0 } elseif ($t -ge $hit2) { 2 } elseif ($t -ge $actAt) { 1 } else { 0 }
        $fig = 250.0
        if ($hitsNow -ge 1 -and -not $heals) { $fig = if ($hitsNow -eq 2) { 0.0 } else { 99.0 } }
        $hs = New-Sample "host" ($t + 50) @(0, 0, 0) @(1, 0, 0) $true ($hitsNow -ge 1) $(if ($acts) { $actAt } else { 0 })
        $hs | Add-Member actionInfo $(if ($hitsNow -eq 2) { $info2 } elseif ($hitsNow -eq 1) { $info1 } else { "" }); $hs | Add-Member connected $true
        $hs | Add-Member ff ([pscustomobject]@{ figure = $fig; figureDead = ($dies -and $hitsNow -eq 2); heals = $(if ($heals) { $hitsNow } else { 0 }); own = 300.0; ownDowned = $false; hits = $hitsNow })
        $cs = New-Sample "client" $t @(1, 0, 0) @(0, 0, 0) $true
        $cs | Add-Member connected $true
        $cs | Add-Member ff ([pscustomobject]@{ figure = 250.0; figureDead = $false; heals = 0; own = $(if ($clientHurt -and $t -ge $actAt + 1000) { 120.0 } else { 300.0 }); ownDowned = $false; hits = 0 })
        $hostRun += $hs; $clientRun += $cs
    }
    return @{ H = $hostRun; C = $clientRun; D = @() }
}
function New-AimRun([bool]$turns = $true, [bool]$arrives = $true, [bool]$sights = $true, [bool]$acts = $true, [bool]$sticks = $false) {
    $hostRun = @(); $clientRun = @(); $t0 = 1000000; $start = $t0 + 10000
    $hUp = @(1, 0, 0, 0, 0.825, -0.565, 0, 0.565, 0.825); $hDown = @(1, 0, 0, 0, 0.825, 0.565, 0, -0.565, 0.825); $hLevel = @(1, 0, 0, 0, 1, 0, 0, 0, 1)
    for ($i = 0; $i -lt 50; $i++) {
        $t = $t0 + $i * 1000; $e = $t - $start
        $steer = $acts -and $e -ge 3000 -and $e -lt 27000
        $sent = if (-not $steer) { 0.0 } elseif ($e -lt 11000) { 0.6 } elseif ($e -lt 19000) { -0.6 } else { 0.0 }
        $sentSighted = $steer -and $e -lt 19000
        $hs = New-Sample "host" ($t + 50) @(0, 0, 0) @(1, 0, 0) $true ($acts -and $e -ge 0) $(if ($acts) { $start } else { 0 })
        $hs | Add-Member actionInfo "aim: weapon draw called=1"; $hs | Add-Member connected $true
        $hs | Add-Member aim ([pscustomobject]@{ steering = $steer; sent = $sent; sentSighted = $sentSighted; recv = 0.0; cloneSighted = 0; head = $hLevel; gotHead = $false })
        $ce = $e - 500
        $recv = if (-not $arrives -or -not $acts -or $ce -lt 3000 -or $ce -ge 27000) { 0.0 } elseif ($ce -lt 11000) { 0.6 } elseif ($ce -lt 19000) { -0.6 } else { 0.0 }
        $head = if (-not $turns) { $hLevel } elseif ($recv -gt 0.5) { $hUp } elseif ($recv -lt -0.5) { $hDown } else { $hLevel }
        $cs = New-Sample "client" $t @(1, 0, 0) @(0, 0, 0) $true
        $cs | Add-Member connected $true
        $cSighted = if ($sticks -and $acts -and $ce -ge 3000) { 1 } elseif ($sights -and [math]::Abs($recv) -gt 0.5) { 1 } else { 0 }
        $cs | Add-Member aim ([pscustomobject]@{ steering = $false; sent = 0.0; sentSighted = $false; recv = $recv; cloneSighted = $cSighted; head = $head; gotHead = $true })
        $hostRun += $hs; $clientRun += $cs
    }
    return @{ H = $hostRun; C = $clientRun; D = @() }
}
function New-JetpackRun([double]$hostRise = 6.0, [double]$cloneRise = 5.5, [int]$accepted = 1, [bool]$acts = $true) {
    $hostRun = @(); $clientRun = @(); $t0 = 1000000; $actAt = $t0 + 10000
    for ($i = 0; $i -lt 30; $i++) {
        $t = $t0 + $i * 1000; $e = $t - $actAt; $up = $acts -and $e -ge 0 -and $e -lt 4000
        $hs = New-Sample "host" ($t + 50) @(0, 0, 10) @(1, 0, 10) $true ($acts -and $e -ge 0) $(if ($acts) { $actAt } else { 0 })
        $hs | Add-Member actionInfo "jetpack: jump 00001234 ok=1 from z 10.00, boostpack start 00005678 ok=1"; $hs | Add-Member connected $true
        $hs | Add-Member jet ([pscustomobject]@{ step = 3; zMin = 10.0; zMax = $(if ($up) { 10 + $hostRise } else { 10.0 }); cloneZMin = 10.0; cloneZMax = 10.0; jumpMask = $(if ($up) { 15 } else { 1 }); boostStarts = 0; boostAccepted = 0 })
        $cs = New-Sample "client" $t @(1, 0, 10) @(0, 0, 10) $true
        $cs | Add-Member connected $true
        $cUp = $acts -and $e -ge 500 -and $e -lt 4500
        $done = $acts -and $e -ge 1000
        $cs | Add-Member jet ([pscustomobject]@{ step = 0; zMin = 10.0; zMax = 10.0; cloneZMin = 10.0; cloneZMax = $(if ($cUp) { 10 + $cloneRise } else { 10.0 }); jumpMask = 1; boostStarts = $(if ($done) { 1 } else { 0 }); boostAccepted = $(if ($done) { $accepted } else { 0 }) })
        $hostRun += $hs; $clientRun += $cs
    }
    return @{ H = $hostRun; C = $clientRun; D = @() }
}
function New-PickupRun([bool]$clientRemoves = $true, [bool]$hostRemoves = $true, [bool]$clientSees = $true, [bool]$acts = $true, [bool]$found = $true) {
    $hostRun = @(); $clientRun = @(); $t0 = 1000000; $actAt = $t0 + 10000; $id = "0012ABCD"
    $info = if ($found) { "pickup: took $id (2.1 m), activate=1, 5 loose items within 20 m" } else { "pickup: no loose item within 20 m (0 seen, none present)" }
    for ($i = 0; $i -lt 30; $i++) {
        $t = $t0 + $i * 1000; $after = $acts -and $t -ge $actAt + 2000
        $hs = New-Sample "host" ($t + 50) @(0, 0, 0) @(1, 0, 0) $true ($acts -and $t -ge $actAt) $(if ($acts) { $actAt } else { 0 })
        $hs | Add-Member actionInfo $(if ($acts) { $info } else { "" }); $hs | Add-Member connected $true
        $hs | Add-Member items @([pscustomobject]@{ id = $id; present = -not ($after -and $hostRemoves) }, [pscustomobject]@{ id = "0012ABCE"; present = $true })
        $cs = New-Sample "client" $t @(1, 0, 0) @(0, 0, 0) $true
        $cs | Add-Member connected $true
        $cItems = @([pscustomobject]@{ id = "0012ABCE"; present = $true })
        if ($clientSees -and -not ($after -and $clientRemoves)) { $cItems += [pscustomobject]@{ id = $id; present = $true } }
        $cs | Add-Member items $cItems
        $hostRun += $hs; $clientRun += $cs
    }
    return @{ H = $hostRun; C = $clientRun; D = @() }
}
function New-SeparateRun([bool]$restores = $true, [bool]$keepsPerk = $true, [bool]$hostKeeps = $true, [bool]$bridges = $true, [string]$perk = "0002A001", [bool]$acts = $true) {
    $hostRun = @(); $clientRun = @(); $t0 = 1000000; $actAt = $t0 + 5000; $bridgeAt = $t0 + 35000; $loadAt = $t0 + 60000
    $hostPerk = $perk; $clientPerk = "0002AFFF"
    for ($i = 0; $i -lt 100; $i++) {
        $t = $t0 + $i * 1000; $acted = $acts -and $t -ge $actAt; $loaded = $acted -and $bridges -and $t -ge $loadAt
        # host: +3 item H, +3 skill points, host perk 1 -> 2; client: +5 item C, +1 skill point, client perk 0 -> 1
        $hH = if ($acted -and ($hostKeeps -or $t -lt $loadAt)) { 13 } else { 10 }
        $hPerks = "$($clientPerk):0," + $(if ($acted -and $hostPerk -ne "00000000") { "$($hostPerk):2" } else { "0002A001:1" })
        $hSep = [pscustomobject]@{ itemH = "0001A000"; itemC = "0001B000"; baseH = 10; baseC = 20; cntH = $hH; cntC = 20; baseSp = 4; sp = $(if ($acted) { 7 } else { 4 })
                                   perk = $(if ($acted) { $hostPerk } else { "00000000" }); perkBefore = 1; perkAfter = $(if ($hostPerk -ne "00000000") { 2 } else { -1 })
                                   restores = 0; restoresAtAct = 0; bridgeAtWallMs = $(if ($bridges -and $t -ge $bridgeAt) { $bridgeAt } else { 0 }); loadedSinceAct = $false; perks = $hPerks }
        $hs = New-Sample "host" ($t + 50) @(0, 0, 0) @(1, 0, 0) $true $acted $(if ($acts) { $actAt } else { 0 })
        $hs | Add-Member actionInfo $(if ($acts) { "separate: item 0001A000 +3 ok=1" } else { "" }); $hs | Add-Member connected $true; $hs | Add-Member inWorld $true; $hs | Add-Member sep $hSep
        $own = -not $loaded -or $restores
        $cPerkRank = if ($acted -and $own -and ($keepsPerk -or -not $loaded)) { 1 } else { 0 }
        $cHostPerkRank = if ($loaded -and -not $restores) { 2 } else { 1 }
        $cSep = [pscustomobject]@{ itemH = "0001A000"; itemC = "0001B000"; baseH = 10; baseC = 20
                                   cntH = $(if ($loaded -and -not $restores) { 13 } else { 10 }); cntC = $(if ($acted -and $own) { 25 } else { 20 })
                                   baseSp = 4; sp = $(if ($loaded -and -not $restores) { 7 } elseif ($acted) { 5 } else { 4 })
                                   perk = $(if ($acted) { $clientPerk } else { "00000000" }); perkBefore = 0; perkAfter = 1
                                   restores = $(if ($loaded -and $restores) { 1 } else { 0 }); restoresAtAct = 0; bridgeAtWallMs = 0; loadedSinceAct = $loaded
                                   perks = "$($clientPerk):$cPerkRank,0002A001:$cHostPerkRank" }
        $cs = New-Sample "client" $t @(1, 0, 0) @(0, 0, 0) $true $acted $(if ($acts) { $actAt } else { 0 })
        $cs | Add-Member actionInfo $(if ($acts) { "separate: item 0001B000 +5 ok=1" } else { "" }); $cs | Add-Member connected $true; $cs | Add-Member inWorld $true; $cs | Add-Member sep $cSep
        $hostRun += $hs; $clientRun += $cs
    }
    return @{ H = $hostRun; C = $clientRun; D = @() }
}
function New-DamageRun([bool]$mirrors = $true, [bool]$forwards = $true, [bool]$found = $true, [bool]$acts = $true) {
    $hostRun = @(); $clientRun = @(); $t0 = 1000000; $hAt = $t0 + 5000; $cAt = $t0 + 25000
    for ($i = 0; $i -lt 50; $i++) {
        $t = $t0 + $i * 1000; $hHit = $acts -and $t -ge $hAt; $cHit = $acts -and $t -ge $cAt
        $n1Host = if ($hHit -and $found) { 60.0 } else { 100.0 }
        $n2Host = if ($cHit -and $found -and $forwards) { 140.0 } else { 200.0 }
        $n1Client = if ($hHit -and $found -and $mirrors) { 60.0 } else { 100.0 }
        $n2Client = if ($cHit -and $found) { 140.0 } else { 200.0 }
        $hDmg = [pscustomobject]@{ target = $(if ($hHit -and $found) { "00011111" } else { "00000000" }); hitAtWallMs = $(if ($hHit) { $hAt } else { 0 }); before = $(if ($found) { 100.0 } else { -1.0 }); after = $(if ($found) { 60.0 } else { -1.0 }) }
        $cDmg = [pscustomobject]@{ target = $(if ($cHit -and $found) { "00022222" } else { "00000000" }); hitAtWallMs = $(if ($cHit) { $cAt } else { 0 }); before = $(if ($found) { 200.0 } else { -1.0 }); after = $(if ($found) { 140.0 } else { -1.0 }) }
        $hs = New-Sample "host" ($t + 50) @(0, 0, 0) @(1, 0, 0) $true $hHit $(if ($acts) { $hAt } else { 0 })
        $hs | Add-Member actionInfo $(if ($acts -and $found) { "damage: hit 00011111 (3.0 m) by 40%" } elseif ($acts) { "damage: no unguarded living NPC within 30 m (0 NPCs)" } else { "" })
        $hs | Add-Member connected $true; $hs | Add-Member dmg $hDmg
        $hs | Add-Member npcs @([pscustomobject]@{ id = "00011111"; dead = $false; health = $n1Host }, [pscustomobject]@{ id = "00022222"; dead = $false; health = $n2Host })
        $cs = New-Sample "client" $t @(1, 0, 0) @(0, 0, 0) $true $cHit $(if ($acts) { $cAt } else { 0 })
        $cs | Add-Member actionInfo $(if ($acts -and $found) { "damage: hit 00022222 (3.0 m) by 30%" } elseif ($acts) { "damage: no unguarded living NPC within 30 m (0 NPCs)" } else { "" })
        $cs | Add-Member connected $true; $cs | Add-Member dmg $cDmg
        $cs | Add-Member npcs @([pscustomobject]@{ id = "00011111"; dead = $false; health = $n1Client }, [pscustomobject]@{ id = "00022222"; dead = $false; health = $n2Client })
        $hostRun += $hs; $clientRun += $cs
    }
    return @{ H = $hostRun; C = $clientRun; D = @() }
}
function New-ReconnectRun([int]$backAfter = 40000, [bool]$drops = $true, [bool]$acts = $true, [bool]$hostSees = $true) {
    $hostRun = @(); $clientRun = @(); $t0 = 1000000; $actAt = $t0 + 10000
    for ($i = 0; $i -lt 220; $i++) {
        $t = $t0 + $i * 1000; $e = $t - $actAt
        $down = $acts -and $drops -and $e -ge 12000 -and ($backAfter -lt 0 -or $e -lt $backAfter)
        $isBack = $acts -and $drops -and $backAfter -ge 0 -and $e -ge $backAfter
        $hs = New-Sample "host" ($t + 50) @(0, 0, 0) @(1, 0, 0) (-not $down -and ($hostSees -or -not $isBack)) $false 0
        $hs | Add-Member connected (-not $down); $hs | Add-Member reconnects 0
        $cs = New-Sample "client" $t @(1, 0, 0) @(0, 0, 0) (-not $down) ($acts -and $e -ge 0) $(if ($acts) { $actAt } else { 0 })
        $cs | Add-Member connected (-not $down); $cs | Add-Member actionInfo "reconnect: network outage 25000 ms"
        $cs | Add-Member reconnects $(if ($isBack) { 1 } else { 0 })
        $hostRun += $hs; $clientRun += $cs
    }
    return @{ H = $hostRun; C = $clientRun; D = @() }
}
function New-TakeoffRun([int]$readyAfter = 12000, [int]$spaceAfter = 16000, [bool]$clientFollows = $true, [bool]$boards = $true, [bool]$acts = $true, [bool]$answers = $true, [bool]$outside = $false, [bool]$movedAboard = $true, [bool]$left = $true, [int]$f8At = -1) {
    $hostRun = @(); $clientRun = @(); $t0 = 1000000; $askAt = $t0 + 5000
    $info = "takeoff: seat 0001ABCD activate=1, seated=1, launch fired=1"
    for ($i = 0; $i -lt 90; $i++) {
        $t = $t0 + $i * 1000
        $hostUp = $acts -and $spaceAfter -ge 0 -and $t -ge $askAt + $spaceAfter
        $hs = New-Sample "host" ($t + 50) @(0, 0, 0) @(1, 0, 0) $true ($acts -and $t -ge $askAt) $(if ($acts) { $askAt } else { 0 })
        $hs | Add-Member actionInfo $(if ($acts) { $info } else { "" }); $hs | Add-Member connected $true
        $hs | Add-Member ship ([pscustomobject]@{ inSpace = $hostUp; ship = "0001ABCE"; seated = $true })
        $hs | Add-Member playerCell "032928FE"
        $hs | Add-Member vote ([pscustomobject]@{ f8AtWallMs = $(if ($acts -and $f8At -ge 0 -and $t -ge $askAt + $f8At) { $askAt + $f8At } else { 0 }) })
        $ready = if ($acts -and $answers -and $t -ge $askAt + $readyAfter) { $askAt + $readyAfter } else { 0 }
        $cs = New-Sample "client" $t @(1, 0, 0) @(0, 0, 0) $true
        $cs | Add-Member connected $true
        $cs | Add-Member ship ([pscustomobject]@{ inSpace = ($clientFollows -and $hostUp -and $t -ge $askAt + $spaceAfter + 8000); ship = "0001ABCE"; seated = $false })
        $cs | Add-Member vote ([pscustomobject]@{ state = "idle"; seenAtWallMs = $(if ($acts -and $t -ge $askAt + 1000) { $askAt + 1000 } else { 0 }); readyAtWallMs = $ready })
        $isOut = $outside -and $left -and $t -ge $t0 + 2000 -and -not ($movedAboard -and $t -ge $askAt + $readyAfter + 2000) -and -not ($hostUp -and $t -ge $askAt + $spaceAfter + 8000)
        $cs | Add-Member playerCell $(if ($isOut) { "0000AAAA" } else { "032928FE" })
        $cs | Add-Member exitInfo $(if ($outside -and $t -ge $t0 + 2000) { "left the ship: 1 load doors, tried 1, activate=$(if ($left) { 1 } else { 0 }), from cell 032928FE" } else { "" })
        $hostRun += $hs; $clientRun += $cs
    }
    $how = if (-not $outside) { "already aboard" } elseif ($movedAboard) { "moving aboard" } else { "move failed" }
    $d = if ($boards) { @("[PULSAR] [TAKEOFF-VOTE] vote 1 passed - boarding: $how") } else { @() }
    return @{ H = $hostRun; C = $clientRun; D = $d }
}
function New-MissionRun([bool]$acts = $true, [bool]$found = $true, [bool]$stops = $true, [bool]$rises = $true, [int]$clientBackAfter = 20000, [int]$clientStage = 20) {
    $hostRun = @(); $clientRun = @(); $t0 = 1000000; $actAt = $t0 + 30000; $id = "0002C0DE"
    for ($i = 0; $i -lt 90; $i++) {
        $t = $t0 + $i * 2000
        $acted = $acts -and $t -ge $actAt
        $hs = New-Sample "host" ($t + 50) @(0, 0, 0) @(1, 0, 0) $true $acted $(if ($acts) { $actAt } else { 0 })
        $hs | Add-Member actionInfo $(if (-not $acts) { "" } elseif ($found) { "mission: $id found=1 MB_Test advance queued=1" } else { "mission: picked 00000000 - (0 running MB_ quests)" })
        $hs | Add-Member mission ([pscustomobject]@{ id = $id; running = $true; stage = $(if ($rises -and $t -ge $actAt + 2000) { 20 } else { 10 }); stoppedAtWallMs = 0; loadedSincePick = $false })
        $back = $clientBackAfter -ge 0 -and $acted -and $t -ge $actAt + $clientBackAfter
        $cs = New-Sample "client" $t @(1, 0, 0) @(0, 0, 0) $true
        $cs | Add-Member mission ([pscustomobject]@{ id = $id; running = ($back -or -not $stops); stage = $(if ($back) { $clientStage } else { 10 }); stoppedAtWallMs = $(if ($stops) { $t0 + 1000 } else { 0 }); loadedSincePick = $back })
        $hostRun += $hs; $clientRun += $cs
    }
    return @{ H = $hostRun; C = $clientRun; D = @() }
}
function New-QuestRun([bool]$acts = $true, [bool]$found = $true, [bool]$hostRecords = $true, [int]$clientAfter = 1200) {
    $hostRun = @(); $clientRun = @(); $t0 = 1000000; $actAt = $t0 + 5000; $id = "0001B0B0"
    for ($i = 0; $i -lt 40; $i++) {
        $t = $t0 + $i * 1000
        $acted = $acts -and $t -ge $actAt
        $hs = New-Sample "host" ($t + 50) @(0, 0, 0) @(1, 0, 0) $true $acted $(if ($acts) { $actAt } else { 0 })
        $hs | Add-Member actionInfo $(if (-not $acts) { "" } elseif ($found) { "quest: $id found=1 City_AK_Test started on try 2 (x)" } else { "quest: none of 0 candidates started (x)" })
        $hs | Add-Member questStarts $(if ($acted -and $hostRecords -and $t -ge $actAt + 300) { "$($id):$($actAt + 300)" } else { "" })
        $cs = New-Sample "client" $t @(1, 0, 0) @(0, 0, 0) $true
        $cs | Add-Member questStarts $(if ($acted -and $clientAfter -ge 0 -and $t -ge $actAt + 300 + $clientAfter) { "0000AAAA:$($t0 + 100),$($id):$($actAt + 300 + $clientAfter)" } else { "0000AAAA:$($t0 + 100)" })
        $hostRun += $hs; $clientRun += $cs
    }
    return @{ H = $hostRun; C = $clientRun; D = @() }
}
function New-SceneRun([bool]$acts = $true, [bool]$found = $true, [bool]$hostPlays = $true, [int]$clientAfter = 1500) {
    $hostRun = @(); $clientRun = @(); $t0 = 1000000; $actAt = $t0 + 5000; $id = "000D8783"
    for ($i = 0; $i -lt 40; $i++) {
        $t = $t0 + $i * 1000
        $acted = $acts -and $t -ge $actAt
        $hs = New-Sample "host" ($t + 50) @(0, 0, 0) @(1, 0, 0) $true $acted $(if ($acts) { $actAt } else { 0 })
        $hs | Add-Member actionInfo $(if ($acts) { "scene: $id found=$(if ($found) { 1 } else { 0 }) start queued=1" } else { "" })
        $hs | Add-Member scenes $(if ($acted -and $hostPlays -and $t -ge $actAt + 500) { "$($id):0:$($actAt + 500)" } else { "" })
        $cs = New-Sample "client" $t @(1, 0, 0) @(0, 0, 0) $true
        $cs | Add-Member scenes $(if ($acted -and $hostPlays -and $clientAfter -ge 0 -and $t -ge $actAt + 500 + $clientAfter) { "000AAAAA:0:$($t0 + 100),$($id):0:$($actAt + 500 + $clientAfter)" } else { "000AAAAA:0:$($t0 + 100)" })
        $hostRun += $hs; $clientRun += $cs
    }
    return @{ H = $hostRun; C = $clientRun; D = @() }
}
function New-SpaceRun([string]$hostRefs = "0001F00D:0001F00E:3D:12.5,FF001234:00ABCDEF:3D:3.2", [string]$clientRefs = "0001F00D:0001F00E:3D:12.5,FF009999:00ABCDEF:3D:3.4", [bool]$census = $true, [string]$clientCell = "0000C0DE") {
    $run = New-TakeoffRun
    foreach ($pair in @(@($run.H, $hostRefs, "0000C0DE"), @($run.C, $clientRefs, $clientCell))) {
        foreach ($s in $pair[0]) {
            $sp = if ($census -and $s.ship.inSpace) { [pscustomobject]@{ cell = $pair[2]; atWallMs = [long]$s.wallMs; n = @($pair[1] -split ',').Count; refs = $pair[1] } } else { $null }
            $s | Add-Member space $sp
        }
    }
    return $run
}
function New-Run([double]$cloneErr = 0.2, [bool]$hostMoves = $true, [bool]$clientFollows = $true, [bool]$visible = $true) {
    $hostRun = @(); $clientRun = @(); $t0 = 1000000; $moveAt = $t0 + 10000
    for ($i = 0; $i -lt 30; $i++) {
        $t = $t0 + $i * 1000
        $hm = $hostMoves -and $t -ge $moveAt
        $hx = if ($hm) { 5.0 } else { 0.0 }
        $seen = if ($clientFollows -or -not $hm) { $hx } else { 0.0 }
        # (PowerShell: the comma binds tighter than +, so parenthesise sums inside arrays.)
        $hostRun   += New-Sample "host"   ($t + 50) @($hx, 0, 0) @((20.0 + $cloneErr), 0, 0) $visible $hm $(if ($hostMoves) { $moveAt } else { 0 })
        $clientRun += New-Sample "client" $t        @(20.0, 0, 0) @(($seen + $cloneErr), 0, 0) $visible
    }
    return @{ H = $hostRun; C = $clientRun }
}
# Join codes as the DLL makes them (include/Network/JoinCode.h): "PSR-" + Crockford base32
# of [version][kind][secret 16 | tag 4][count][count x via, ip 4, port 2][CRC-16 big endian].
$JoinAlphabet = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"
function Get-Crc16([byte[]]$b, [int]$n) {
    $crc = 0xFFFF
    for ($i = 0; $i -lt $n; $i++) {
        $crc = $crc -bxor ([int]$b[$i] -shl 8)
        for ($k = 0; $k -lt 8; $k++) { $crc = if ($crc -band 0x8000) { (($crc -shl 1) -bxor 0x1021) -band 0xFFFF } else { ($crc -shl 1) -band 0xFFFF } }
    }
    return $crc
}
function ConvertFrom-JoinCode([string]$text) {
    $s = ($text.ToUpperInvariant() -replace '[\s-]', '')
    if (-not $s.StartsWith("PSR")) { return $null }
    $out = New-Object System.Collections.Generic.List[byte]; $acc = 0; $bits = 0
    foreach ($ch in $s.Substring(3).ToCharArray()) {
        $v = $JoinAlphabet.IndexOf($ch); if ($v -lt 0) { return $null }
        $acc = (($acc -shl 5) -bor $v) -band 0xFFFFFF; $bits += 5
        if ($bits -ge 8) { $out.Add([byte](($acc -shr ($bits - 8)) -band 0xFF)); $bits -= 8 }
    }
    $b = $out.ToArray()
    if ($b.Length -lt 9) { return $null }
    if ((Get-Crc16 $b ($b.Length - 2)) -ne (([int]$b[$b.Length - 2] -shl 8) -bor $b[$b.Length - 1])) { return $null }
    return ,$b
}
function ConvertTo-JoinCode([byte[]]$body) {
    $crc = Get-Crc16 $body $body.Length
    $b = [byte[]]($body + @([byte](($crc -shr 8) -band 0xFF), [byte]($crc -band 0xFF)))
    $raw = New-Object System.Text.StringBuilder; $acc = 0; $bits = 0
    foreach ($x in $b) {
        $acc = (($acc -shl 8) -bor $x) -band 0xFFFFFF; $bits += 8
        while ($bits -ge 5) { [void]$raw.Append($JoinAlphabet[($acc -shr ($bits - 5)) -band 31]); $bits -= 5 }
    }
    if ($bits -gt 0) { [void]$raw.Append($JoinAlphabet[($acc -shl (5 - $bits)) -band 31]) }
    $t = "PSR"; for ($i = 0; $i -lt $raw.Length; $i++) { if ($i % 5 -eq 0) { $t += "-" }; $t += $raw[$i] }
    return $t
}
# A reply code for the session of a host code: [1][2][secret 0..3][1][via 2][203.0.113.9][port].
function New-ReplyCodeFor([string]$hostCode, [bool]$wrongSession = $false) {
    $h = ConvertFrom-JoinCode $hostCode
    if (-not $h -or $h[1] -ne 1) { return $null }
    $tag = [byte[]]$h[2..5]; if ($wrongSession) { $tag[0] = $tag[0] -bxor 0xFF }
    return ConvertTo-JoinCode ([byte[]](@([byte]1, [byte]2) + $tag + @([byte]1, [byte]2, [byte]203, [byte]0, [byte]113, [byte]9, [byte]0xEA, [byte]0x61)))
}

function Invoke-ScorerSelfTest {
    $cases = @(
        @{ Name = "good run";                Run = (New-Run);                          Expect = "PASS" },
        @{ Name = "clone 10 m off";          Run = (New-Run -cloneErr 10);             Expect = "FAIL" },
        @{ Name = "client clone stays put";  Run = (New-Run -clientFollows $false);    Expect = "FAIL" },
        @{ Name = "host never moves";        Run = (New-Run -hostMoves $false);        Expect = "TIMEOUT" },
        @{ Name = "clones never visible";    Run = (New-Run -visible $false);          Expect = "TIMEOUT" },
        @{ Name = "no client state";         Run = @{ H = (New-Run).H; C = @() };      Expect = "ERROR" },
        @{ Name = "roles swapped";           Run = @{ H = (New-Run).C; C = (New-Run).H }; Expect = "FAIL" },
        @{ Name = "door: client stays";      Run = (New-DoorRun);                      Expect = "PASS";    Door = $true },
        @{ Name = "door: client follows";    Run = (New-DoorRun -clientFollows $true); Expect = "FAIL";    Door = $true },
        @{ Name = "door: stays, no decision"; Run = (New-DoorRun -decides $false);     Expect = "FAIL";    Door = $true },
        @{ Name = "door: host stays";        Run = (New-DoorRun -hostChanges $false);  Expect = "FAIL";    Door = $true },
        @{ Name = "door: no door found";     Run = (New-DoorRun -info "no load door found"); Expect = "ERROR"; Door = $true },
        @{ Name = "door: engine refused";    Run = (New-DoorRun -info "door 0001ABCD of 2 load doors (2.8 m away), activate=0"); Expect = "ERROR"; Door = $true },
        @{ Name = "door: never acted";       Run = (New-DoorRun -acts $false);         Expect = "TIMEOUT"; Door = $true },
        @{ Name = "loot: client keeps";      Run = (New-LootRun);                      Expect = "PASS";    Loot = $true },
        @{ Name = "loot: client loses";      Run = (New-LootRun -clientAfter 0);       Expect = "FAIL";    Loot = $true },
        @{ Name = "loot: keeps, no decision"; Run = (New-LootRun -decides $false);     Expect = "FAIL";    Loot = $true },
        @{ Name = "loot: alive on client, kept"; Run = (New-LootRun -diesOnClient $false); Expect = "FAIL"; Loot = $true },
        @{ Name = "loot: no copy on client"; Run = (New-LootRun -hasCopy $false); Expect = "ERROR";   Loot = $true },
        @{ Name = "loot: took nothing";      Run = (New-LootRun -moved 0);             Expect = "ERROR";   Loot = $true },
        @{ Name = "loot: never acted";       Run = (New-LootRun -acts $false);         Expect = "TIMEOUT"; Loot = $true },
        @{ Name = "reward: both get it once"; Run = (New-RewardRun);                   Expect = "PASS";    Reward = $true },
        @{ Name = "reward: client gets none"; Run = (New-RewardRun -clientCredits 0);  Expect = "FAIL";    Reward = $true },
        @{ Name = "reward: client weapon x2"; Run = (New-RewardRun -clientItems 2);    Expect = "FAIL";    Reward = $true },
        @{ Name = "reward: echo doubles host"; Run = (New-RewardRun -hostItems 2);     Expect = "FAIL";    Reward = $true },
        @{ Name = "reward: no decision lines"; Run = (New-RewardRun -decides $false);  Expect = "FAIL";    Reward = $true },
        @{ Name = "reward: no own copy";     Run = (New-RewardRun -ownCopy $false);    Expect = "ERROR";   Reward = $true },
        @{ Name = "reward: no weapon";       Run = (New-RewardRun -item "00000000");   Expect = "ERROR";   Reward = $true },
        @{ Name = "reward: never acted";     Run = (New-RewardRun -acts $false);       Expect = "TIMEOUT"; Reward = $true },
        @{ Name = "takeoff: waits for ready"; Run = (New-TakeoffRun);                  Expect = "PASS";    Takeoff = $true },
        @{ Name = "takeoff: leaves early";   Run = (New-TakeoffRun -spaceAfter 3000);  Expect = "FAIL";    Takeoff = $true },
        @{ Name = "takeoff: no answer, leaves"; Run = (New-TakeoffRun -answers $false); Expect = "FAIL";   Takeoff = $true },
        @{ Name = "takeoff: ready, stays";   Run = (New-TakeoffRun -spaceAfter -1);    Expect = "FAIL";    Takeoff = $true },
        @{ Name = "takeoff: client left";    Run = (New-TakeoffRun -clientFollows $false); Expect = "FAIL"; Takeoff = $true },
        @{ Name = "takeoff: no boarding log"; Run = (New-TakeoffRun -boards $false);   Expect = "FAIL";    Takeoff = $true },
        @{ Name = "takeoff: never asked";    Run = (New-TakeoffRun -acts $false);      Expect = "TIMEOUT"; Takeoff = $true },
        @{ Name = "board: moved aboard";     Run = (New-TakeoffRun -outside $true);    Expect = "PASS";    Takeoff = $true; Outside = $true },
        @{ Name = "board: not moved";        Run = (New-TakeoffRun -outside $true -movedAboard $false); Expect = "FAIL"; Takeoff = $true; Outside = $true },
        @{ Name = "board: never left ship";  Run = (New-TakeoffRun -outside $true -left $false); Expect = "ERROR"; Takeoff = $true; Outside = $true },
        @{ Name = "board: stayed inside";    Run = (New-TakeoffRun);                   Expect = "ERROR";   Takeoff = $true; Outside = $true },
        @{ Name = "f8: vote still holds";    Run = (New-TakeoffRun -f8At 3000);        Expect = "PASS";    Takeoff = $true; F8 = $true },
        @{ Name = "f8: launched on F8";      Run = (New-TakeoffRun -f8At 3000 -spaceAfter 4000); Expect = "FAIL"; Takeoff = $true; F8 = $true },
        @{ Name = "f8: never pressed";       Run = (New-TakeoffRun);                   Expect = "ERROR";   Takeoff = $true; F8 = $true },
        @{ Name = "ff: figure heals";        Run = (New-FriendlyFireRun);              Expect = "PASS";    FF = $true },
        @{ Name = "ff: figure keeps damage"; Run = (New-FriendlyFireRun -heals $false); Expect = "FAIL";   FF = $true },
        @{ Name = "ff: figure dies";         Run = (New-FriendlyFireRun -dies $true);  Expect = "FAIL";    FF = $true },
        @{ Name = "ff: client hurt";         Run = (New-FriendlyFireRun -clientHurt $true); Expect = "FAIL"; FF = $true },
        @{ Name = "ff: hits did nothing";    Run = (New-FriendlyFireRun -lands $false); Expect = "ERROR";  FF = $true },
        @{ Name = "ff: never hit";           Run = (New-FriendlyFireRun -acts $false); Expect = "TIMEOUT"; FF = $true },
        @{ Name = "aim: head follows";       Run = (New-AimRun);                       Expect = "PASS";    Aim = $true },
        @{ Name = "aim: head stays level";   Run = (New-AimRun -turns $false);         Expect = "FAIL";    Aim = $true },
        @{ Name = "aim: pitch never arrives"; Run = (New-AimRun -arrives $false -turns $false); Expect = "FAIL"; Aim = $true },
        @{ Name = "aim: never in sights";    Run = (New-AimRun -sights $false);        Expect = "FAIL";    Aim = $true },
        @{ Name = "aim: sights stuck";       Run = (New-AimRun -sticks $true);         Expect = "FAIL";    Aim = $true },
        @{ Name = "aim: never aimed";        Run = (New-AimRun -acts $false);          Expect = "TIMEOUT"; Aim = $true },
        @{ Name = "jet: clone boosts";       Run = (New-JetpackRun);                   Expect = "PASS";    Jet = $true },
        @{ Name = "jet: clone stays down";   Run = (New-JetpackRun -cloneRise 0.5);    Expect = "FAIL";    Jet = $true },
        @{ Name = "jet: no figure boost";    Run = (New-JetpackRun -accepted 0);       Expect = "FAIL";    Jet = $true },
        @{ Name = "jet: host never rose";    Run = (New-JetpackRun -hostRise 0.2 -cloneRise 0.2); Expect = "ERROR"; Jet = $true },
        @{ Name = "jet: never jumped";       Run = (New-JetpackRun -acts $false);      Expect = "TIMEOUT"; Jet = $true },
        @{ Name = "pick: gone for both";     Run = (New-PickupRun);                    Expect = "PASS";    Pick = $true },
        @{ Name = "pick: client keeps it";   Run = (New-PickupRun -clientRemoves $false); Expect = "FAIL";  Pick = $true },
        @{ Name = "pick: host take failed";  Run = (New-PickupRun -hostRemoves $false -clientRemoves $false); Expect = "ERROR"; Pick = $true },
        @{ Name = "pick: client never saw";  Run = (New-PickupRun -clientSees $false); Expect = "ERROR";   Pick = $true },
        @{ Name = "pick: no item nearby";    Run = (New-PickupRun -found $false);      Expect = "ERROR";   Pick = $true },
        @{ Name = "pick: never acted";       Run = (New-PickupRun -acts $false);       Expect = "TIMEOUT"; Pick = $true },
        @{ Name = "rc: comes back";          Run = (New-ReconnectRun);                 Expect = "PASS";    Rc = $true },
        @{ Name = "rc: never comes back";    Run = (New-ReconnectRun -backAfter -1);   Expect = "FAIL";    Rc = $true },
        @{ Name = "rc: host never sees it";  Run = (New-ReconnectRun -hostSees $false); Expect = "FAIL";   Rc = $true },
        @{ Name = "rc: never dropped";       Run = (New-ReconnectRun -drops $false);   Expect = "ERROR";   Rc = $true },
        @{ Name = "rc: never cut";           Run = (New-ReconnectRun -acts $false);    Expect = "TIMEOUT"; Rc = $true },
        @{ Name = "sep: each keeps own";     Run = (New-SeparateRun);                  Expect = "PASS";    Sep = $true },
        @{ Name = "sep: client gets host's"; Run = (New-SeparateRun -restores $false); Expect = "FAIL";    Sep = $true },
        @{ Name = "sep: client loses perk";  Run = (New-SeparateRun -keepsPerk $false); Expect = "FAIL";   Sep = $true },
        @{ Name = "sep: host lost its item"; Run = (New-SeparateRun -hostKeeps $false); Expect = "FAIL";   Sep = $true },
        @{ Name = "sep: no bridge";          Run = (New-SeparateRun -bridges $false);  Expect = "ERROR";   Sep = $true },
        @{ Name = "sep: no perk to raise";   Run = (New-SeparateRun -perk "00000000"); Expect = "ERROR";   Sep = $true },
        @{ Name = "sep: never acted";        Run = (New-SeparateRun -acts $false);     Expect = "TIMEOUT"; Sep = $true },
        @{ Name = "dmg: both directions";    Run = (New-DamageRun);                    Expect = "PASS";    Dmg = $true },
        @{ Name = "dmg: host hit not shown"; Run = (New-DamageRun -mirrors $false);    Expect = "FAIL";    Dmg = $true },
        @{ Name = "dmg: client hit lost";    Run = (New-DamageRun -forwards $false);   Expect = "FAIL";    Dmg = $true },
        @{ Name = "dmg: no NPC to hit";      Run = (New-DamageRun -found $false);      Expect = "ERROR";   Dmg = $true },
        @{ Name = "dmg: never hit";          Run = (New-DamageRun -acts $false);       Expect = "TIMEOUT"; Dmg = $true },
        @{ Name = "space: same around both"; Run = (New-SpaceRun);                     Expect = "PASS";    Space = $true },
        @{ Name = "space: station missing";  Run = (New-SpaceRun -clientRefs "FF009999:00ABCDEF:3D:3.4"); Expect = "FAIL"; Space = $true },
        @{ Name = "space: other encounter";  Run = (New-SpaceRun -clientRefs "0001F00D:0001F00E:3D:12.5,FF009999:00BEEF00:3D:3.4"); Expect = "FAIL"; Space = $true },
        @{ Name = "space: other cell";       Run = (New-SpaceRun -clientCell "0000DEAD"); Expect = "FAIL"; Space = $true },
        @{ Name = "space: no census";        Run = (New-SpaceRun -census $false);      Expect = "TIMEOUT"; Space = $true },
        @{ Name = "space: both empty";       Run = (New-SpaceRun -hostRefs "" -clientRefs ""); Expect = "ERROR"; Space = $true },
        @{ Name = "scene: client plays it";  Run = (New-SceneRun);                     Expect = "PASS";    Scene = $true },
        @{ Name = "scene: client never";     Run = (New-SceneRun -clientAfter -1);     Expect = "FAIL";    Scene = $true },
        @{ Name = "scene: client too late";  Run = (New-SceneRun -clientAfter 20000);  Expect = "FAIL";    Scene = $true },
        @{ Name = "scene: host never played"; Run = (New-SceneRun -hostPlays $false);  Expect = "ERROR";   Scene = $true },
        @{ Name = "scene: no such scene";    Run = (New-SceneRun -found $false);       Expect = "ERROR";   Scene = $true },
        @{ Name = "scene: never acted";      Run = (New-SceneRun -acts $false);        Expect = "TIMEOUT"; Scene = $true },
        @{ Name = "quest: client starts it"; Run = (New-QuestRun);                     Expect = "PASS";    Quest = $true },
        @{ Name = "quest: client never";     Run = (New-QuestRun -clientAfter -1);     Expect = "FAIL";    Quest = $true },
        @{ Name = "quest: client too late";  Run = (New-QuestRun -clientAfter 20000);  Expect = "FAIL";    Quest = $true },
        @{ Name = "quest: host not recorded"; Run = (New-QuestRun -hostRecords $false); Expect = "ERROR";  Quest = $true },
        @{ Name = "quest: none startable";   Run = (New-QuestRun -found $false);       Expect = "ERROR";   Quest = $true },
        @{ Name = "quest: never acted";      Run = (New-QuestRun -acts $false);        Expect = "TIMEOUT"; Quest = $true },
        @{ Name = "mission: client gets it"; Run = (New-MissionRun);                   Expect = "PASS";    Mission = $true },
        @{ Name = "mission: client stays without"; Run = (New-MissionRun -clientBackAfter -1); Expect = "FAIL"; Mission = $true },
        @{ Name = "mission: client old stage"; Run = (New-MissionRun -clientStage 10); Expect = "FAIL";   Mission = $true },
        @{ Name = "mission: never stopped";  Run = (New-MissionRun -stops $false);     Expect = "ERROR";   Mission = $true },
        @{ Name = "mission: stage not raised"; Run = (New-MissionRun -rises $false);   Expect = "ERROR";   Mission = $true },
        @{ Name = "mission: no mission";     Run = (New-MissionRun -found $false);     Expect = "ERROR";   Mission = $true },
        @{ Name = "mission: never acted";    Run = (New-MissionRun -acts $false);      Expect = "TIMEOUT"; Mission = $true }
    )
    $bad = 0
    foreach ($case in $cases) {
        $s = if ($case.ContainsKey("Door")) { Invoke-DoorScore -HostSamples $case.Run.H -ClientSamples $case.Run.C -ClientDecisions $case.Run.D -Final $true }
             elseif ($case.ContainsKey("Loot")) { Invoke-LootScore -HostSamples $case.Run.H -ClientSamples $case.Run.C -ClientDecisions $case.Run.D -Final $true }
             elseif ($case.ContainsKey("Mission")) { Invoke-MissionScore -HostSamples $case.Run.H -ClientSamples $case.Run.C -Final $true }
             elseif ($case.ContainsKey("Quest")) { Invoke-QuestScore -HostSamples $case.Run.H -ClientSamples $case.Run.C -Final $true }
             elseif ($case.ContainsKey("Scene")) { Invoke-SceneScore -HostSamples $case.Run.H -ClientSamples $case.Run.C -Final $true }
             elseif ($case.ContainsKey("Space")) { Invoke-SpaceScore -HostSamples $case.Run.H -ClientSamples $case.Run.C -ClientDecisions $case.Run.D -Final $true }
             elseif ($case.ContainsKey("Takeoff")) { Invoke-TakeoffScore -HostSamples $case.Run.H -ClientSamples $case.Run.C -ClientDecisions $case.Run.D -Final $true -PartnerOutside $case.ContainsKey("Outside") -F8DuringVote $case.ContainsKey("F8") }
             elseif ($case.ContainsKey("Rc")) { Invoke-ReconnectScore -HostSamples $case.Run.H -ClientSamples $case.Run.C -Final $true }
             elseif ($case.ContainsKey("Pick")) { Invoke-PickupScore -HostSamples $case.Run.H -ClientSamples $case.Run.C -Final $true }
             elseif ($case.ContainsKey("Sep")) { Invoke-SeparateScore -HostSamples $case.Run.H -ClientSamples $case.Run.C -Final $true }
             elseif ($case.ContainsKey("Dmg")) { Invoke-DamageScore -HostSamples $case.Run.H -ClientSamples $case.Run.C -Final $true }
             elseif ($case.ContainsKey("Jet")) { Invoke-JetpackScore -HostSamples $case.Run.H -ClientSamples $case.Run.C -Final $true }
             elseif ($case.ContainsKey("Aim")) { Invoke-AimScore -HostSamples $case.Run.H -ClientSamples $case.Run.C -Final $true }
             elseif ($case.ContainsKey("FF")) { Invoke-FriendlyFireScore -HostSamples $case.Run.H -ClientSamples $case.Run.C -Final $true }
             elseif ($case.ContainsKey("Reward")) { Invoke-RewardScore -HostSamples $case.Run.H -ClientSamples $case.Run.C -ClientDecisions $case.Run.D -Final $true }
             else { Invoke-CoopScore -HostSamples $case.Run.H -ClientSamples $case.Run.C -Tolerance 1.5 -MoveMetres 5 -Final $true }
        $ok = $s.Result -eq $case.Expect
        if (-not $ok) { $bad++ }
        Write-Host ("{0,-28} expected {1,-7} got {2,-7} {3}" -f $case.Name, $case.Expect, $s.Result, $(if ($ok) { "ok" } else { "MISMATCH" }))
    }
    # NPC matching check: the rotated guards of run 20261028-091213 are caught,
    # identity and runtime (0xFF) rematches are not.
    $matchLines = @(
        "[09:15:29.276] [6536 ] [I] [PULSAR] NPCSync: MATCHED hostRef=0x002146E0 base=0xFF00D13C src=0x0026FB62 hostPos=(-1935,-647,32) -> localRef=0x002146EA",
        "[09:15:29.276] [6536 ] [I] [PULSAR] NPCSync: MATCHED hostRef=0x0029D488 base=0x00005999 src=0x00000000 hostPos=(1,2,3) -> localRef=0x0029D488",
        "[09:15:29.276] [6536 ] [I] [PULSAR] NPCSync: MATCHED hostRef=0xFF00D558 base=0xFF00D13C src=0x00259DC6 hostPos=(1,2,3) -> localRef=0xFF00D5AD")
    $wrong = @(Find-WrongNpcMatches $matchLines)
    $ok = ($wrong.Count -eq 1 -and $wrong[0] -eq "002146E0->002146EA")
    if (-not $ok) { $bad++ }
    Write-Host ("{0,-28} expected {1,-7} got {2,-7} {3}" -f "npc match: rotated guard", "1 wrong", "$($wrong.Count) wrong", $(if ($ok) { "ok" } else { "MISMATCH" }))
    $cases += @{ Name = "npc match" }
    # The same sample the C++ (tests/test_join_code.cpp) and C# tests pin.
    $sample = "PSR-040T1-8D2ME-JAB9N-7N2MT-NAXCN-PQAY0-R1R2M-0250Y-C41CP-03H0Z-4240Y-B01RG-E7K1J-BA0"
    $sb = ConvertFrom-JoinCode $sample
    $ok = $sb -and $sb.Length -eq 42 -and $sb[1] -eq 1 -and $sb[2] -eq 0xA0 -and $sb[18] -eq 3 -and (ConvertTo-JoinCode ([byte[]]$sb[0..39])) -eq $sample
    $r1 = New-ReplyCodeFor $sample; $r2 = New-ReplyCodeFor $sample $true; $rb = if ($r1) { ConvertFrom-JoinCode $r1 } else { $null }
    $ok = $ok -and $rb -and $rb[1] -eq 2 -and $rb[2] -eq 0xA0 -and $rb[5] -eq 0xA3 -and $r2 -and $r2 -ne $r1 -and -not (ConvertFrom-JoinCode ($sample.Substring(0, 10) + "X" + $sample.Substring(11)))
    if (-not $ok) { $bad++ }
    Write-Host ("{0,-28} expected {1,-7} got {2,-7} {3}" -f "join code helpers", "ok", $(if ($ok) { "ok" } else { "bad" }), $(if ($ok) { "ok" } else { "MISMATCH" }))
    $cases += @{ Name = "join code" }
    # Only the count goes down the pipeline (anything else would be captured with it).
    Write-Host $(if ($bad -eq 0) { "SELFTEST: OK ($($cases.Count) cases)" } else { "SELFTEST: FAILED ($bad of $($cases.Count))" })
    return $bad
}

if ($SelfTest) { $bad = Invoke-ScorerSelfTest; exit $(if ($bad -eq 0) { 0 } else { 1 }) }

# ============================================================================
# The run.
# ============================================================================
if (-not $SaveFile -or -not (Test-Path -LiteralPath $SaveFile)) { "RESULT: ERROR (-SaveFile missing)"; exit 2 }
$Instances = (Resolve-Path $Instances).Path
$runDir = Join-Path $Instances ("runs\" + (Get-Date -Format "yyyyMMdd-HHmmss"))
New-Item -ItemType Directory -Force $runDir | Out-Null
$logFile = Join-Path $runDir "run.log"
function W([string]$s) { $line = "[{0:HH:mm:ss}] {1}" -f (Get-Date), $s; Write-Host $line; Add-Content -LiteralPath $logFile $line -Encoding utf8 }
function FreeGB { [math]::Round((Get-CimInstance Win32_OperatingSystem).FreePhysicalMemory / 1MB, 1) }

# ---- owner protection: snapshot -------------------------------------------
$ownerDocs   = [Environment]::GetFolderPath("MyDocuments")
$ownerLocal  = [Environment]::GetFolderPath("LocalApplicationData")
$ownerRoam   = [Environment]::GetFolderPath("ApplicationData")
$watchDirs   = @($ownerDocs, (Join-Path $ownerDocs "My Games"), $ownerLocal, $ownerRoam)
function Get-CloudHashes { Get-ChildItem "C:\Program Files (x86)\Steam\userdata" -Directory -ErrorAction SilentlyContinue |
    ForEach-Object { Join-Path $_.FullName "1716740" } | Where-Object { Test-Path $_ } |
    ForEach-Object { Get-ChildItem $_ -Recurse -File } | Get-FileHash | Select-Object Hash, Path }
function Get-OwnerSnapshot {
    @{
        Root  = @(Get-ChildItem -LiteralPath $GameDir -File | Get-FileHash | Select-Object Hash, Path)
        Data  = @(Get-ChildItem -LiteralPath (Join-Path $GameDir "Data") -Recurse -File | ForEach-Object { "{0}|{1}" -f $_.FullName, $_.Length })
        Cloud = @(Get-CloudHashes)
        Dirs  = @($watchDirs | Where-Object { Test-Path $_ } | ForEach-Object { Get-ChildItem -LiteralPath $_ -Force | ForEach-Object { $_.FullName } })
    }
}

# ---- slot preparation (private files only) ---------------------------------
function Write-PrivateFile([string]$path, [string]$content) {
    if (Test-Path -LiteralPath $path) {
        if (@(fsutil hardlink list $path).Count -ne 1) { throw "refusing to write $path : it has more than one hard link" }
        Remove-Item -LiteralPath $path   # delete first, so an old link is never written through
    }
    New-Item -ItemType Directory -Force (Split-Path $path) | Out-Null
    [IO.File]::WriteAllText($path, $content, (New-Object System.Text.UTF8Encoding $false))
}
function Initialize-Slot([string]$slot, [string]$role, [int]$port) {
    $root = Join-Path $Instances $slot
    $plugins = Join-Path $root "game\Data\SFSE\Plugins"
    $mg = Join-Path $root "profile\Documents\My Games\Starfield"
    $harnessIni = Join-Path $plugins "PulsarTestHarness.ini"
    if (-not (Test-Path $harnessIni)) { throw "$slot has no PulsarTestHarness.ini" }
    $hLines = @(Get-Content -LiteralPath $harnessIni | Where-Object { $_ -notmatch '^\s*MaxWorkingSetMB\s*=' })
    if ($MaxWorkingSetMB -gt 0) { $hLines += "MaxWorkingSetMB = $MaxWorkingSetMB" }
    Write-PrivateFile $harnessIni (($hLines -join "`r`n") + "`r`n")
    $state = Join-Path $root "logs\bot-state.jsonl"
    Remove-Item -LiteralPath $state -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath (Join-Path $root "logs\bot-state.json") -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath (Join-Path $root "logs\bot-trace.log") -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath (Join-Path $plugins "PULSAR_JoinCode.txt"), (Join-Path $plugins "PULSAR_ReplyCode.txt") -ErrorAction SilentlyContinue
    $mode = if ($role -eq "host") { "host" } else { "client" }
    $doorFollowValue = if ($DoorFollow) { 1 } else { 0 }
    $instancedLootValue = if ($SharedLoot) { 0 } else { 1 }
    $questRewardsValue = if ($NoQuestRewards) { 0 } else { 1 }
    $takeoffVoteValue = if ($NoTakeoffVote) { 0 } else { 1 }
    $ffGuardValue = if ($NoFriendlyFireGuard) { 0 } else { 1 }
    $aimSyncValue = if ($NoAimSync) { 0 } else { 1 }
    $jetpackSyncValue = if ($NoJetpackSync) { 0 } else { 1 }
    $sharedItemsValue = if ($NoSharedWorldItems) { 0 } else { 1 }
    $autoReconnectValue = if ($NoAutoReconnect) { 0 } else { 1 }
    $restoreOwnValue = if ($NoRestoreOwnCharacter) { 0 } else { 1 }
    $npcHealthValue = if ($NoNpcHealthSync) { 0 } else { 1 }
    $sceneSyncValue = if ($NoSceneSync) { 0 } else { 1 }
    $questStartValue = if ($NoQuestStartSync) { 0 } else { 1 }
    $missionBridgeValue = if ($NoMissionBridge) { 0 } else { 1 }
    Write-PrivateFile (Join-Path $plugins "PULSAR.ini") ("[PULSAR]`r`nMode = $mode`r`nHostIP = 127.0.0.1`r`nPort = $port`r`nPlayerName = QA-$role`r`nCoopDoorFollow = $doorFollowValue`r`nCoopInstancedEnemyLoot = $instancedLootValue`r`nCoopQuestRewards = $questRewardsValue`r`nCoopTakeoffVote = $takeoffVoteValue`r`nCoopFriendlyFireGuard = $ffGuardValue`r`nCoopAimSync = $aimSyncValue`r`nCoopJetpackSync = $jetpackSyncValue`r`nCoopSharedWorldItems = $sharedItemsValue`r`nAutoReconnect = $autoReconnectValue`r`nCoopRestoreOwnCharacter = $restoreOwnValue`r`nCoopNpcHealthSync = $npcHealthValue`r`nCoopSceneSync = $sceneSyncValue`r`nCoopQuestStartSync = $questStartValue`r`nCoopMissionBridge = $missionBridgeValue`r`n" + (($ExtraIni -split ";" | Where-Object { $_.Trim() } | ForEach-Object { $_.Trim() + "`r`n" }) -join ""))
    Write-PrivateFile (Join-Path $plugins "PulsarTestBot.ini") ("; TEST SLOT ONLY`r`nEnabled = 1`r`nStateFile = $state`r`nScenario = $Scenario`r`nMoveAfterMs = 45000`r`nMoveMetres = $MoveMetres`r`n" + (($ExtraBotIni -split ";" | Where-Object { $_.Trim() } | ForEach-Object { $_.Trim() + "`r`n" }) -join ""))
    $custom = "[Archive]`r`nbInvalidateOlderFiles=1`r`nsResourceDataDirsFinal=`r`n`r`n" +
        "[General]`r`nbPauseOnLostFocus=0`r`nbAlwaysActive=1`r`nsStartingConsoleCommand=load PulsarQA_Start`r`n"
    if (-not $NoLowMemory) {
        # Merge the memory settings (the [Archive] keys join the section above).
        $custom = $custom.Replace("sResourceDataDirsFinal=`r`n", "sResourceDataDirsFinal=`r`nsResourceArchiveMemoryCacheList=`r`nsResourceMiscArchiveMemoryCacheList=`r`n")
        # [General] memory keys go into the existing section: no master-file
        # preload into RAM (Starfield.esm is 1.4 GB), no linked-area preloads.
        $custom = $custom.Replace("sStartingConsoleCommand=load PulsarQA_Start`r`n", "sStartingConsoleCommand=load PulsarQA_Start`r`nfMasterFilePreLoadMB=0`r`nbPreloadLinkedAreas=0`r`n")
        $custom += "`r`n" + ($LowMemoryIni -replace '(?s)^\[Archive\].*?\r\n\r\n', '')
    }
    if ($PapyrusLog) { $custom += "`r`n[Papyrus]`r`nbEnableLogging=1`r`nbEnableTrace=1`r`nbLoadDebugInformation=1`r`n" }
    Write-PrivateFile (Join-Path $mg "StarfieldCustom.ini") $custom
    $saves = Join-Path $mg "Saves"
    New-Item -ItemType Directory -Force $saves | Out-Null
    $dst = Join-Path $saves "PulsarQA_Start.sfs"
    if (Test-Path -LiteralPath $dst) { Remove-Item -LiteralPath $dst }
    Copy-Item -LiteralPath $SaveFile $dst
    return $state
}

# ---- processes this run owns ------------------------------------------------
$owned = @{}
$fakeProc = $null   # -ThirdPlayer
function Start-Slot([string]$slot) {
    $game = Join-Path $Instances "$slot\game"
    $loader = Start-Process (Join-Path $game "sfse_loader.exe") -WorkingDirectory $game -PassThru
    $t0 = Get-Date; $child = $null
    while (-not $child -and ((Get-Date) - $t0).TotalSeconds -lt 60) {
        Start-Sleep -Milliseconds 500
        $child = Get-CimInstance Win32_Process -Filter "Name='Starfield.exe' AND ParentProcessId=$($loader.Id)" | Select-Object -First 1
    }
    if (-not $child) {
        # The loader is ours: stop it, or it keeps sfse_loader.txt open in the
        # owner's Documents and the owner check after the run cannot move it.
        if (-not $loader.HasExited) { Stop-Process -Id $loader.Id -Force; [void]$loader.WaitForExit(10000) }
        throw "$slot : no Starfield.exe started by the loader within 60 s (loader stopped)"
    }
    $p = Get-Process -Id $child.ProcessId
    $owned[$slot] = @{ Pid = $p.Id; Start = $p.StartTime.ToFileTimeUtc(); Proc = $p }
    [void](W "$slot : Starfield.exe pid $($p.Id) from $($child.ExecutablePath)")
    return $p
}
function Stop-Slot([string]$slot) {
    if (-not $owned.ContainsKey($slot)) { return }
    $o = $owned[$slot]
    $p = Get-Process -Id $o.Pid -ErrorAction SilentlyContinue
    if (-not $p -or $p.ProcessName -ne "Starfield" -or $p.StartTime.ToFileTimeUtc() -ne $o.Start) { W "$slot : pid $($o.Pid) gone or not ours - not touching"; return }
    [void]$p.CloseMainWindow()
    if (-not $p.WaitForExit(45000)) { W "$slot : no exit within 45 s - stopping pid $($p.Id)"; Stop-Process -Id $p.Id -Force; [void]$p.WaitForExit(15000) }
    W "$slot : closed"
}
# The bot appends one JSON line per second. Read only what is new since the last
# call (shared read, so the bot's writes never fail), complete lines only.
$readOffsets = @{}
# Lines of a log the game holds open (shared read) that contain $needle.
function Read-SharedLines([string]$path, [string]$needle) {
    if (-not (Test-Path -LiteralPath $path)) { return }
    try {
        $fs = [System.IO.File]::Open($path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite -bor [System.IO.FileShare]::Delete)
        try {
            $sr = New-Object System.IO.StreamReader($fs)
            while ($null -ne ($line = $sr.ReadLine())) { if ($line.Contains($needle)) { $line } }
        } finally { $fs.Dispose() }
    } catch { }
}
function Read-NewSamples([string]$path) {
    $out = New-Object System.Collections.Generic.List[object]
    if (-not (Test-Path -LiteralPath $path)) { return ,$out }
    $fs = [IO.File]::Open($path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete)
    try {
        $start = if ($readOffsets.ContainsKey($path)) { $readOffsets[$path] } else { 0 }
        if ($start -gt $fs.Length) { $start = 0 }
        [void]$fs.Seek($start, [IO.SeekOrigin]::Begin)
        $buf = New-Object byte[] ($fs.Length - $start)
        $n = $fs.Read($buf, 0, $buf.Length)
        $text = [Text.Encoding]::UTF8.GetString($buf, 0, $n)
        $lastNl = $text.LastIndexOf("`n")
        if ($lastNl -lt 0) { return ,$out }
        $readOffsets[$path] = $start + [Text.Encoding]::UTF8.GetByteCount($text.Substring(0, $lastNl + 1))
        foreach ($line in $text.Substring(0, $lastNl).Split("`n")) {
            if ($line.Trim()) { try { $out.Add(($line | ConvertFrom-Json)) } catch { } }
        }
    } finally { $fs.Dispose() }
    return ,$out
}
function NowWallMs { [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds() }

# ---- main --------------------------------------------------------------------
$result = "ERROR"; $why = @(); $before = $null
try {
    if (Get-Process Starfield, sfse_loader -ErrorAction SilentlyContinue) { throw "a Starfield process is already running (maybe the owner's) - not starting" }
    if (-not (Get-Process steam -ErrorAction SilentlyContinue)) { throw "Steam is not running" }
    W "run dir $runDir"
    $before = Get-OwnerSnapshot
    W ("owner snapshot: {0} install files, {1} data files, {2} cloud files" -f $before.Root.Count, $before.Data.Count, $before.Cloud.Count)

    $hostState   = Initialize-Slot $HostSlot "host" $Port
    $clientState = Initialize-Slot $ClientSlot "client" $(if ($ClientPort -gt 0) { $ClientPort } else { $Port })
    if ($ClientPort -gt 0) { W "TWIN: client port $ClientPort, host port $Port - this run must not pass" }

    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    $hp = Start-Slot $HostSlot
    W "waiting for the host bot to report in-world"
    while ($true) {
        Start-Sleep -Seconds 2
        if ($hp.HasExited) { throw "host copy exited (code $($hp.ExitCode))" }
        $new = Read-NewSamples $hostState
        $s = if ($new.Count) { $new[$new.Count - 1] } else { $null }
        if ($s -and $s.inWorld) { W "host in world, cell $($s.playerCell)"; break }
        if ((Get-Date) -gt $deadline) { $result = "TIMEOUT"; $why += "host never got into the world (sStartingConsoleCommand load?)"; throw "timeout" }
    }
    if ($MeasureOnly) {
        # GPU memory per process: "Shared Usage" is system RAM the GPU borrows.
        $peak = @{ WS = 0.0; Priv = 0.0; GpuDed = 0.0; GpuShared = 0.0 }; $freeMin = 999.0
        $end = (Get-Date).AddSeconds($MeasureSec)
        while ((Get-Date) -lt $end) {
            Start-Sleep -Seconds 5
            if ($hp.HasExited) { throw "host copy exited while measuring (code $($hp.ExitCode))" }
            $hp.Refresh()
            $peak.WS   = [math]::Max($peak.WS,   $hp.WorkingSet64 / 1GB)
            $peak.Priv = [math]::Max($peak.Priv, $hp.PrivateMemorySize64 / 1GB)
            try {
                $c = Get-Counter -Counter "\GPU Process Memory(pid_$($hp.Id)*)\Dedicated Usage", "\GPU Process Memory(pid_$($hp.Id)*)\Shared Usage" -ErrorAction Stop
                $ded = ($c.CounterSamples | Where-Object { $_.Path -match 'dedicated usage' } | Measure-Object CookedValue -Sum).Sum / 1GB
                $sha = ($c.CounterSamples | Where-Object { $_.Path -match 'shared usage' } | Measure-Object CookedValue -Sum).Sum / 1GB
                $peak.GpuDed = [math]::Max($peak.GpuDed, $ded); $peak.GpuShared = [math]::Max($peak.GpuShared, $sha)
            } catch { }
            $freeMin = [math]::Min($freeMin, (FreeGB))
        }
        $line = "MEASURE: peak working set {0:N2} GB, peak private {1:N2} GB, GPU dedicated {2:N2} GB, GPU shared {3:N2} GB, lowest free RAM {4:N1} GB" -f `
            $peak.WS, $peak.Priv, $peak.GpuDed, $peak.GpuShared, $freeMin
        W $line
        $result = "MEASURED"
        throw "measure-done"
    }

    # Let the host's memory settle first: two copies in the world sit close to
    # this PC's limit, and a client started during the host's streaming peak
    # tripped the guard. Wait >= 60 s in world and free RAM stable within 0.5 GB
    # over 15 s (max 3 min).
    $settleStart = Get-Date; $samples = New-Object System.Collections.Generic.List[double]
    while ($true) {
        Start-Sleep -Seconds 5
        if ($hp.HasExited) { throw "host copy exited (code $($hp.ExitCode))" }
        $samples.Add((FreeGB)); if ($samples.Count -gt 3) { $samples.RemoveAt(0) }
        $age = ((Get-Date) - $settleStart).TotalSeconds
        $spread = ($samples | Measure-Object -Maximum).Maximum - ($samples | Measure-Object -Minimum).Minimum
        if (($age -ge 60 -and $samples.Count -eq 3 -and $spread -le 0.5) -or $age -ge 180) { break }
    }
    $free = FreeGB
    W ("host settled after {0:N0} s, free RAM {1} GB" -f ((Get-Date) - $settleStart).TotalSeconds, $free)
    if ($free -lt 5) { throw "RAM guard: only $free GB free, not starting the client" }
    if ($JoinByCode) {
        $codeFile = Join-Path $Instances "$HostSlot\game\Data\SFSE\Plugins\PULSAR_JoinCode.txt"
        $lastFile = Join-Path $Instances "last-join-code.txt"
        $codeWait = (Get-Date).AddSeconds(30)
        while (-not (Test-Path -LiteralPath $codeFile) -and (Get-Date) -lt $codeWait) { Start-Sleep -Seconds 1 }
        if (-not (Test-Path -LiteralPath $codeFile)) { $result = "ERROR"; throw "the host never published a join code" }
        $fresh = (Get-Content -LiteralPath $codeFile -Raw).Trim()
        if ($StaleJoinCode) {
            if (-not (Test-Path -LiteralPath $lastFile)) { $result = "ERROR"; throw "no join code from an earlier run for -StaleJoinCode" }
            $code = (Get-Content -LiteralPath $lastFile -Raw).Trim()
            if ($code -eq $fresh) { $result = "ERROR"; throw "the earlier join code equals this session's" }
            W "TWIN: the client gets the previous session's join code - this run must not pass"
        } else { $code = $fresh }
        Write-PrivateFile $lastFile ($fresh + "`r`n")
        $cIni = Join-Path $Instances "$ClientSlot\game\Data\SFSE\Plugins\PULSAR.ini"
        $text = (Get-Content -LiteralPath $cIni -Raw).Replace("HostIP = 127.0.0.1", "HostIP = 192.0.2.1") + "JoinCode = $code`r`n"
        Write-PrivateFile $cIni $text
        W "client joins by code only ($($code.Length) characters, HostIP unreachable)"
    }
    $cp = Start-Slot $ClientSlot

    $hostSeen = New-Object System.Collections.Generic.List[object]; $clientSeen = New-Object System.Collections.Generic.List[object]
    if ($s) { $hostSeen.Add($s) }
    $lastLog = Get-Date; $clientStart = NowWallMs; $lowRam = 0; $replyChecked = $false; $fakeProc = $null; $fakeStartedAt = $null
    $hs = $s; $cs = $null
    while ($true) {
        Start-Sleep -Seconds 1
        if ($hp.HasExited) { throw "host copy exited during the test (code $($hp.ExitCode))" }
        if ($cp.HasExited) { throw "client copy exited during the test (code $($cp.ExitCode))" }
        # RAM guard: a sustained shortage (two samples in a row below 0.7 GB)
        # stops the run; a single short dip during a load does not.
        if ((FreeGB) -lt 0.7) { $lowRam++ } else { $lowRam = 0 }
        if ($lowRam -ge 2) { throw "RAM guard: free RAM below 0.7 GB for 2 s" }
        foreach ($x in (Read-NewSamples $hostState))   { $hostSeen.Add($x); $hs = $x }
        foreach ($x in (Read-NewSamples $clientState)) { $clientSeen.Add($x); $cs = $x }
        # Stale state is an error, never a pass (playbook: a crashed or frozen bot
        # must not look like a quiet one). 30 s of no new line while running.
        $now = NowWallMs
        if ($hs -and $now - [long]$hs.wallMs -gt 30000) { throw ("host bot state is stale ({0:N0} s old) while the game runs" -f (($now - [long]$hs.wallMs) / 1000)) }
        if ($cs -and $now - [long]$cs.wallMs -gt 30000) { throw ("client bot state is stale ({0:N0} s old) while the game runs" -f (($now - [long]$cs.wallMs) / 1000)) }
        if (-not $cs -and $now - $clientStart -gt 120000) { throw "client bot wrote nothing within 120 s" }
        $final = (Get-Date) -gt $deadline
        $clientLog = Join-Path $Instances "$ClientSlot\profile\Documents\My Games\Starfield\SFSE\Logs\PULSAR.log"
        $decisions = if ($Scenario -eq "door") { @(Read-SharedLines $clientLog 'AutoFollow: host used door') }
                     elseif ($Scenario -eq "loot") { @(Read-SharedLines $clientLog 'instanced enemy loot') }
                     elseif ($Scenario -eq "reward") { @(Read-SharedLines $clientLog '[REWARD]') }
                     elseif ($Scenario -eq "takeoff" -or $Scenario -eq "space") { @(Read-SharedLines $clientLog '[TAKEOFF-VOTE]') } else { @() }
        $score = if ($Scenario -eq "door") { Invoke-DoorScore -HostSamples $hostSeen -ClientSamples $clientSeen -ClientDecisions $decisions -Final $final }
                 elseif ($Scenario -eq "loot") { Invoke-LootScore -HostSamples $hostSeen -ClientSamples $clientSeen -ClientDecisions $decisions -Final $final }
                 elseif ($Scenario -eq "reward") { Invoke-RewardScore -HostSamples $hostSeen -ClientSamples $clientSeen -ClientDecisions $decisions -Final $final }
                 elseif ($Scenario -eq "reconnect") { Invoke-ReconnectScore -HostSamples $hostSeen -ClientSamples $clientSeen -Final $final }
                 elseif ($Scenario -eq "pickup") { Invoke-PickupScore -HostSamples $hostSeen -ClientSamples $clientSeen -Final $final }
                 elseif ($Scenario -eq "separate") { Invoke-SeparateScore -HostSamples $hostSeen -ClientSamples $clientSeen -Final $final }
                 elseif ($Scenario -eq "damage") { Invoke-DamageScore -HostSamples $hostSeen -ClientSamples $clientSeen -Final $final }
                 elseif ($Scenario -eq "jetpack") { Invoke-JetpackScore -HostSamples $hostSeen -ClientSamples $clientSeen -Final $final }
                 elseif ($Scenario -eq "aim") { Invoke-AimScore -HostSamples $hostSeen -ClientSamples $clientSeen -Final $final }
                 elseif ($Scenario -eq "friendlyfire") { Invoke-FriendlyFireScore -HostSamples $hostSeen -ClientSamples $clientSeen -Final $final }
                 elseif ($Scenario -eq "mission") { Invoke-MissionScore -HostSamples $hostSeen -ClientSamples $clientSeen -Final $final }
                 elseif ($Scenario -eq "quest") { Invoke-QuestScore -HostSamples $hostSeen -ClientSamples $clientSeen -Final $final }
                 elseif ($Scenario -eq "scene") { Invoke-SceneScore -HostSamples $hostSeen -ClientSamples $clientSeen -Final $final }
                 elseif ($Scenario -eq "space") { Invoke-SpaceScore -HostSamples $hostSeen -ClientSamples $clientSeen -ClientDecisions $decisions -Final $final -SpaceTravel $true }
                 elseif ($Scenario -eq "papyrus") { Invoke-PapyrusScore -HostSamples $hostSeen -ClientSamples $clientSeen -Final $final }
                 elseif ($Scenario -eq "takeoff") { Invoke-TakeoffScore -HostSamples $hostSeen -ClientSamples $clientSeen -ClientDecisions $decisions -Final $final -PartnerOutside $PartnerOutside.IsPresent -F8DuringVote $F8DuringVote.IsPresent }
                 else { Invoke-CoopScore -HostSamples $hostSeen -ClientSamples $clientSeen -Tolerance $Tolerance -MoveMetres $MoveMetres -Final $final }
        if (((Get-Date) - $lastLog).TotalSeconds -ge 15) {
            $lastLog = Get-Date
            W ("host: inWorld={0} connected={1} cloneVisible={2} acted={3} cell={4} | client: inWorld={5} connected={6} cloneVisible={7} cell={8} | free RAM {9} GB" -f `
                $(if ($hs) { $hs.inWorld } else { "-" }), $(if ($hs) { $hs.connected } else { "-" }), $(if ($hs) { $hs.cloneVisible } else { "-" }), $(if ($hs) { $hs.acted } else { "-" }), $(if ($hs) { $hs.playerCell } else { "-" }),
                $(if ($cs) { $cs.inWorld } else { "-" }), $(if ($cs) { $cs.connected } else { "-" }), $(if ($cs) { $cs.cloneVisible } else { "-" }), $(if ($cs) { $cs.playerCell } else { "-" }), (FreeGB))
        }
        if ($ThirdPlayer -and -not $fakeProc -and $hs -and $cs -and $hs.cloneVisible -and $cs.cloneVisible) {
            $fakeExe = Join-Path $PSScriptRoot "..\..\build\vs\tests\RelWithDebInfo\pulsar_fakepeer.exe"
            $fakeProc = Start-Process $fakeExe -ArgumentList "--host 127.0.0.1 --port $Port --mode circle --radius 2 --name QA-player3 --duration 300 --no-reconnect --vote-ready-after $ThirdPlayerVoteMs" `
                -RedirectStandardOutput (Join-Path $runDir "fakepeer.log") -RedirectStandardError (Join-Path $runDir "fakepeer-err.log") -WindowStyle Hidden -PassThru
            $fakeStartedAt = Get-Date
            W "third player (fake peer) started, pid $($fakeProc.Id)"
        }
        if ($ThirdPlayer -and $score.Result -eq "PASS") {
            # Give player 3 time to join, get its own save and see player 1's packets.
            if (-not $fakeStartedAt -or ((Get-Date) - $fakeStartedAt).TotalSeconds -lt 40) { $score.Result = "PENDING" }
            else {
                $hostLog = Join-Path $Instances "$HostSlot\profile\Documents\My Games\Starfield\SFSE\Logs\PULSAR.log"
                $fakeLog = Get-Content -LiteralPath (Join-Path $runDir "fakepeer.log") -ErrorAction SilentlyContinue
                $saves = @(Read-SharedLines $clientLog 'WorldSync: received save').Count
                if ($Scenario -eq "takeoff") { $saves = 1 }   # takeoff sends every player a fresh save
                if (-not (Read-SharedLines $hostLog 'joined (3 players in the session)')) { $score.Result = "FAIL"; $score.Reasons.Add("the host never had 3 players") }
                elseif (-not (Read-SharedLines $clientLog 'of 3 in the session')) { $score.Result = "FAIL"; $score.Reasons.Add("the client never learned of player 3 (roster)") }
                elseif (-not ($fakeLog -match 'SAVE_TRANSFER')) { $score.Result = "FAIL"; $score.Reasons.Add("player 3 never got its save") }
                elseif (-not ($fakeLog -match '\b(FROM|0xA8)=')) { $score.Result = "FAIL"; $score.Reasons.Add("player 3 never got player 1's packets forwarded") }
                elseif ($saves -ne 1) { $score.Result = "FAIL"; $score.Reasons.Add("the client got $saves saves - player 3's join made it reload") }
                elseif ($Scenario -eq "takeoff") {
                    # The vote must have asked a crew of 2 and waited for player 3's "ready".
                    $readyMatch = [regex]::Match(($fakeLog -join "`n"), 'ready sent wallMs=(\d+)')
                    $space = @($hostSeen | Sort-Object wallMs | Where-Object { $_.ship.inSpace }) | Select-Object -First 1
                    if (-not (Read-SharedLines $hostLog 'asks the crew (0/2 ready)')) { $score.Result = "FAIL"; $score.Reasons.Add("the host's vote did not ask a crew of 2") }
                    elseif (-not $readyMatch.Success) { $score.Result = "FAIL"; $score.Reasons.Add("player 3 never answered the vote") }
                    elseif (-not $space -or [long]$space.wallMs -lt [long]$readyMatch.Groups[1].Value) { $score.Result = "FAIL"; $score.Reasons.Add("the ship took off before player 3 was ready") }
                    else { W ("third player: the vote waited for player 3 (ready {0} ms before the host reached space)" -f ([long]$space.wallMs - [long]$readyMatch.Groups[1].Value)) }
                }
                else {
                    # Player 3 (ID 2) walks a 2 m circle near the host: on both copies its own
                    # figure must be there (3D loaded), near the host, and moving.
                    $figWhy = @()
                    foreach ($side in @(@("host", $hostSeen, "player"), @("client", $clientSeen, "clone"))) {
                        $recent = @($side[1] | Sort-Object wallMs | Select-Object -Last 15)
                        $pts = @(foreach ($smp in $recent) {
                            if (-not ($smp.PSObject.Properties.Name -contains 'figures')) { continue }
                            $f = @($smp.figures | Where-Object { $_.id -eq 2 -and $_.has3D }) | Select-Object -First 1
                            if ($f) { [pscustomobject]@{ x = [double]$f.pos[0]; y = [double]$f.pos[1]; rx = [double]$smp.($side[2])[0]; ry = [double]$smp.($side[2])[1] } }
                        })
                        if ($pts.Count -lt 5) { $figWhy += "$($side[0]): player 3's figure seen in $($pts.Count) of the last $($recent.Count) samples"; continue }
                        $far = @($pts | Where-Object { [math]::Sqrt(($_.x - $_.rx) * ($_.x - $_.rx) + ($_.y - $_.ry) * ($_.y - $_.ry)) -gt 8 }).Count
                        $spanX = ($pts | Measure-Object x -Maximum -Minimum); $spanY = ($pts | Measure-Object y -Maximum -Minimum)
                        $moved = [math]::Max($spanX.Maximum - $spanX.Minimum, $spanY.Maximum - $spanY.Minimum)
                        if ($far -gt 0) { $figWhy += "$($side[0]): player 3's figure more than 8 m from the host in $far samples" }
                        elseif ($moved -lt 1) { $figWhy += ("{0}: player 3's figure did not move ({1:N1} m)" -f $side[0], $moved) }
                        else { W ("third player: {0} sees player 3's figure near the host, moving {1:N1} m" -f $side[0], $moved) }
                    }
                    if ($figWhy.Count) { $score.Result = "FAIL"; foreach ($w in $figWhy) { $score.Reasons.Add($w) } }
                    else { W "third player: joined, got its own save, receives player 1's packets; the client loaded 1 save" }
                }
            }
        }
        if ($JoinByCode -and -not $StaleJoinCode -and -not $replyChecked -and $cs -and $cs.connected) {
            # The launcher's reply-code handover (PULSAR_ReplyCodeIn.txt): another session's
            # code must be refused, this session's accepted.
            $replyChecked = $true
            $inFile = Join-Path $Instances "$HostSlot\game\Data\SFSE\Plugins\PULSAR_ReplyCodeIn.txt"
            Write-PrivateFile $inFile ((New-ReplyCodeFor $code $true) + "`r`n"); Start-Sleep -Seconds 3
            Write-PrivateFile $inFile ((New-ReplyCodeFor $code) + "`r`n"); Start-Sleep -Seconds 3
        }
        if ($JoinByCode -and (Read-SharedLines $clientLog 'uses a different join code')) {
            $score.Result = "FAIL"; $score.Reasons.Add("the host did not accept the client's join code (wrong session secret)")
        }
        if ($JoinByCode -and $score.Result -eq "PASS" -and -not (Read-SharedLines $clientLog 'connected directly to the host through')) {
            $score.Result = "FAIL"; $score.Reasons.Add("the client never logged a direct connection by join code")
        }
        if ($JoinByCode -and -not $StaleJoinCode -and $score.Result -eq "PASS") {
            $hostLog = Join-Path $Instances "$HostSlot\profile\Documents\My Games\Starfield\SFSE\Logs\PULSAR.log"
            if (-not $replyChecked) { $score.Result = "ERROR"; $score.Reasons.Add("the reply-code handover was never tried") }
            elseif (-not (Read-SharedLines $hostLog 'reply code is for another session')) { $score.Result = "FAIL"; $score.Reasons.Add("the host did not refuse another session's reply code") }
            elseif (-not (Read-SharedLines $hostLog 'reply code accepted')) { $score.Result = "FAIL"; $score.Reasons.Add("the host did not accept this session's reply code from PULSAR_ReplyCodeIn.txt") }
            elseif (Test-Path -LiteralPath (Join-Path $Instances "$HostSlot\game\Data\SFSE\Plugins\PULSAR_ReplyCodeIn.txt")) { $score.Result = "FAIL"; $score.Reasons.Add("the host left PULSAR_ReplyCodeIn.txt in place") }
        }
        if ($score.Result -ne "PENDING") { $result = $score.Result; $why += $score.Reasons; if ($score.ContainsKey("Details")) { W $score.Details }; break }
    }
} catch {
    if ($_.Exception.Message -notin @("timeout", "measure-done")) { $why += $_.Exception.Message }
} finally {
    if ($fakeProc -and -not $fakeProc.HasExited) { Stop-Process -Id $fakeProc.Id -Force -ErrorAction SilentlyContinue }
    foreach ($slot in @($ClientSlot, $HostSlot)) { try { Stop-Slot $slot } catch { $why += "closing $slot : $($_.Exception.Message)" } }

    # ---- owner protection: verify ------------------------------------------
    try {
        if ($before) {
            # Known leak: SFSE writes sfse.txt / sfse_loader.txt before any plugin loads.
            $leak = Join-Path $ownerDocs "My Games\Starfield"
            if (Test-Path -LiteralPath $leak) {
                $files = @(Get-ChildItem -LiteralPath $leak -Recurse -File)
                $other = @($files | Where-Object { $_.FullName -notmatch '\\SFSE\\Logs\\sfse(_loader)?\.txt$' })
                if ($other.Count -eq 0 -and -not (@($before.Dirs) -contains $leak)) {
                    $dst = Join-Path $runDir "sfse-leak"; New-Item -ItemType Directory -Force $dst | Out-Null
                    $files | Move-Item -Destination $dst
                    foreach ($d in @((Join-Path $leak "SFSE\Logs"), (Join-Path $leak "SFSE"), $leak)) { if ((Test-Path -LiteralPath $d) -and -not (Get-ChildItem -LiteralPath $d -Force)) { Remove-Item -LiteralPath $d } }
                    W "moved the known SFSE log leak ($($files.Count) files) into the run dir"
                }
            }
            $after = Get-OwnerSnapshot
            $dRoot  = @(Compare-Object $before.Root $after.Root -Property Path, Hash)
            $dData  = @(Compare-Object $before.Data $after.Data)
            $dCloud = @(Compare-Object @($before.Cloud) @($after.Cloud) -Property Path, Hash)
            $newDirs = @(Compare-Object $before.Dirs $after.Dirs | Where-Object { $_.SideIndicator -eq "=>" } | ForEach-Object { $_.InputObject })
            W ("owner check: install {0} / data {1} / cloud {2} differences; new entries in watched profile folders: {3}" -f $dRoot.Count, $dData.Count, $dCloud.Count, $(if ($newDirs.Count) { $newDirs -join ", " } else { "none" }))
            if ($dRoot.Count -or $dData.Count -or $dCloud.Count) { $result = "FAIL"; $why += "OWNER FILES CHANGED (install/data/cloud)" }
            $suspect = @($newDirs | Where-Object { $_ -match '(?i)starfield|pulsar' })
            if ($suspect.Count) { $result = "FAIL"; $why += "new owner profile entries: $($suspect -join ', ')" }
        }
    } catch { $why += "owner check failed: $($_.Exception.Message)"; $result = "ERROR" }

    foreach ($slot in @($HostSlot, $ClientSlot)) {
        foreach ($f in @("logs\harness.log", "logs\bot-state.jsonl", "logs\bot-trace.log", "profile\Documents\My Games\Starfield\SFSE\Logs\PULSAR.log")) {
            $src = Join-Path $Instances "$slot\$f"
            if (Test-Path -LiteralPath $src) { Copy-Item -LiteralPath $src (Join-Path $runDir ("$slot-" + (Split-Path $f -Leaf))) }
        }
    }
    # Every scenario: the client must never stand one save-file NPC in for
    # another (NPCSync matcher, NPCSync/MatchPolicy.h).
    $clientLogCopy = Join-Path $runDir "$ClientSlot-PULSAR.log"
    if (Test-Path -LiteralPath $clientLogCopy) {
        $wrong = @(Find-WrongNpcMatches @(Select-String -LiteralPath $clientLogCopy -SimpleMatch "NPCSync: MATCHED" | ForEach-Object Line))
        if ($wrong.Count) {
            if ($result -eq "PASS") { $result = "FAIL" }
            $why += "client matched host save-file NPCs to other NPCs: $($wrong -join ', ')"
        } else { W "NPC matching: no save-file NPC matched to another" }
    }
    foreach ($w in $why) { W "reason: $w" }
    Add-Content -LiteralPath $logFile "RESULT: $result" -Encoding utf8
    Write-Output "RESULT: $result"
}
