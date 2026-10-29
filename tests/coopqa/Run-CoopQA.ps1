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
    [ValidateSet("follow", "door", "loot", "reward", "takeoff", "papyrus")][string]$Scenario = "follow",
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
        @{ Name = "f8: never pressed";       Run = (New-TakeoffRun);                   Expect = "ERROR";   Takeoff = $true; F8 = $true }
    )
    $bad = 0
    foreach ($case in $cases) {
        $s = if ($case.ContainsKey("Door")) { Invoke-DoorScore -HostSamples $case.Run.H -ClientSamples $case.Run.C -ClientDecisions $case.Run.D -Final $true }
             elseif ($case.ContainsKey("Loot")) { Invoke-LootScore -HostSamples $case.Run.H -ClientSamples $case.Run.C -ClientDecisions $case.Run.D -Final $true }
             elseif ($case.ContainsKey("Takeoff")) { Invoke-TakeoffScore -HostSamples $case.Run.H -ClientSamples $case.Run.C -ClientDecisions $case.Run.D -Final $true -PartnerOutside $case.ContainsKey("Outside") -F8DuringVote $case.ContainsKey("F8") }
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
    Write-PrivateFile (Join-Path $plugins "PULSAR.ini") ("[PULSAR]`r`nMode = $mode`r`nHostIP = 127.0.0.1`r`nPort = $port`r`nPlayerName = QA-$role`r`nCoopDoorFollow = $doorFollowValue`r`nCoopInstancedEnemyLoot = $instancedLootValue`r`nCoopQuestRewards = $questRewardsValue`r`nCoopTakeoffVote = $takeoffVoteValue`r`n" + (($ExtraIni -split ";" | Where-Object { $_.Trim() } | ForEach-Object { $_.Trim() + "`r`n" }) -join ""))
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
    $lastLog = Get-Date; $clientStart = NowWallMs; $lowRam = 0
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
                     elseif ($Scenario -eq "takeoff") { @(Read-SharedLines $clientLog '[TAKEOFF-VOTE]') } else { @() }
        $score = if ($Scenario -eq "door") { Invoke-DoorScore -HostSamples $hostSeen -ClientSamples $clientSeen -ClientDecisions $decisions -Final $final }
                 elseif ($Scenario -eq "loot") { Invoke-LootScore -HostSamples $hostSeen -ClientSamples $clientSeen -ClientDecisions $decisions -Final $final }
                 elseif ($Scenario -eq "reward") { Invoke-RewardScore -HostSamples $hostSeen -ClientSamples $clientSeen -ClientDecisions $decisions -Final $final }
                 elseif ($Scenario -eq "papyrus") { Invoke-PapyrusScore -HostSamples $hostSeen -ClientSamples $clientSeen -Final $final }
                 elseif ($Scenario -eq "takeoff") { Invoke-TakeoffScore -HostSamples $hostSeen -ClientSamples $clientSeen -ClientDecisions $decisions -Final $final -PartnerOutside $PartnerOutside.IsPresent -F8DuringVote $F8DuringVote.IsPresent }
                 else { Invoke-CoopScore -HostSamples $hostSeen -ClientSamples $clientSeen -Tolerance $Tolerance -MoveMetres $MoveMetres -Final $final }
        if (((Get-Date) - $lastLog).TotalSeconds -ge 15) {
            $lastLog = Get-Date
            W ("host: inWorld={0} connected={1} cloneVisible={2} acted={3} cell={4} | client: inWorld={5} connected={6} cloneVisible={7} cell={8} | free RAM {9} GB" -f `
                $(if ($hs) { $hs.inWorld } else { "-" }), $(if ($hs) { $hs.connected } else { "-" }), $(if ($hs) { $hs.cloneVisible } else { "-" }), $(if ($hs) { $hs.acted } else { "-" }), $(if ($hs) { $hs.playerCell } else { "-" }),
                $(if ($cs) { $cs.inWorld } else { "-" }), $(if ($cs) { $cs.connected } else { "-" }), $(if ($cs) { $cs.cloneVisible } else { "-" }), $(if ($cs) { $cs.playerCell } else { "-" }), (FreeGB))
        }
        if ($JoinByCode -and (Read-SharedLines $clientLog 'uses a different join code')) {
            $score.Result = "FAIL"; $score.Reasons.Add("the host did not accept the client's join code (wrong session secret)")
        }
        if ($JoinByCode -and $score.Result -eq "PASS" -and -not (Read-SharedLines $clientLog 'connected directly to the host through')) {
            $score.Result = "FAIL"; $score.Reasons.Add("the client never logged a direct connection by join code")
        }
        if ($score.Result -ne "PENDING") { $result = $score.Result; $why += $score.Reasons; if ($score.ContainsKey("Details")) { W $score.Details }; break }
    }
} catch {
    if ($_.Exception.Message -notin @("timeout", "measure-done")) { $why += $_.Exception.Message }
} finally {
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
