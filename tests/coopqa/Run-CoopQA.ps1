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
    [ValidateSet("follow", "door")][string]$Scenario = "follow",
    [switch]$DoorFollow
)

# Memory-saving settings for test copies. Names are setting names found in
# Starfield.exe ("<name>:<section>" strings); values other than the defaults are
# guesses checked by -MeasureOnly runs (docs/TESTING.md).
$LowMemoryIni = "[Archive]`r`nsResourceArchiveMemoryCacheList=`r`nsResourceMiscArchiveMemoryCacheList=`r`n`r`n" +
                "[BackgroundLoad]`r`nbMinimizeCachedExteriors=1`r`nbDelayOuterGridCells=1`r`n`r`n" +
                "[Quality]`r`nuGlobalRendererQuality=0`r`n"

$ErrorActionPreference = "Stop"
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
        if ($Final) { $r.Result = "TIMEOUT"; $r.Reasons.Add("the host bot never used a door (clone never settled?)") }
        return $r
    }
    if ($act.actionInfo -notmatch 'activate ok=1') { $r.Result = "ERROR"; $r.Reasons.Add("host bot could not use a door: $($act.actionInfo)"); return $r }
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
    $r.Result = "PASS"
    return $r
}

# ---- scorer self-test (playbook: test the scorer, not only the test) -------
function New-Sample($role, $t, $player, $clone, [bool]$visible = $true, [bool]$acted = $false, [long]$actedAt = 0) {
    [pscustomobject]@{ role = $role; wallMs = [long]$t; player = $player; clone = $clone; cloneVisible = $visible; acted = $acted; actedAtWallMs = $actedAt }
}
function New-DoorRun([bool]$clientFollows = $false, [bool]$hostChanges = $true, [string]$info = "door 0001ABCD of 2 load doors within 60 m, activate ok=1", [bool]$acts = $true) {
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
    return @{ H = $hostRun; C = $clientRun }
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
        @{ Name = "door: host stays";        Run = (New-DoorRun -hostChanges $false);  Expect = "FAIL";    Door = $true },
        @{ Name = "door: no door found";     Run = (New-DoorRun -info "no load door found"); Expect = "ERROR"; Door = $true },
        @{ Name = "door: never acted";       Run = (New-DoorRun -acts $false);         Expect = "TIMEOUT"; Door = $true }
    )
    $bad = 0
    foreach ($case in $cases) {
        $s = if ($case.ContainsKey("Door")) { Invoke-DoorScore -HostSamples $case.Run.H -ClientSamples $case.Run.C -Final $true }
             else { Invoke-CoopScore -HostSamples $case.Run.H -ClientSamples $case.Run.C -Tolerance 1.5 -MoveMetres 5 -Final $true }
        $ok = $s.Result -eq $case.Expect
        if (-not $ok) { $bad++ }
        Write-Host ("{0,-24} expected {1,-7} got {2,-7} {3}" -f $case.Name, $case.Expect, $s.Result, $(if ($ok) { "ok" } else { "MISMATCH" }))
    }
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
    if (-not (Test-Path (Join-Path $plugins "PulsarTestHarness.ini"))) { throw "$slot has no PulsarTestHarness.ini" }
    $state = Join-Path $root "logs\bot-state.jsonl"
    Remove-Item -LiteralPath $state -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath (Join-Path $root "logs\bot-state.json") -ErrorAction SilentlyContinue
    $mode = if ($role -eq "host") { "host" } else { "client" }
    $doorFollowValue = if ($DoorFollow) { 1 } else { 0 }
    Write-PrivateFile (Join-Path $plugins "PULSAR.ini") "[PULSAR]`r`nMode = $mode`r`nHostIP = 127.0.0.1`r`nPort = $port`r`nPlayerName = QA-$role`r`nCoopDoorFollow = $doorFollowValue`r`n"
    Write-PrivateFile (Join-Path $plugins "PulsarTestBot.ini") "; TEST SLOT ONLY`r`nEnabled = 1`r`nStateFile = $state`r`nScenario = $Scenario`r`nMoveAfterMs = 45000`r`nMoveMetres = $MoveMetres`r`n"
    $custom = "[Archive]`r`nbInvalidateOlderFiles=1`r`nsResourceDataDirsFinal=`r`n`r`n" +
        "[General]`r`nbPauseOnLostFocus=0`r`nbAlwaysActive=1`r`nsStartingConsoleCommand=load PulsarQA_Start`r`n"
    if (-not $NoLowMemory) {
        # Merge the memory settings (the [Archive] keys join the section above).
        $custom = $custom.Replace("sResourceDataDirsFinal=`r`n", "sResourceDataDirsFinal=`r`nsResourceArchiveMemoryCacheList=`r`nsResourceMiscArchiveMemoryCacheList=`r`n")
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
        $score = if ($Scenario -eq "door") { Invoke-DoorScore -HostSamples $hostSeen -ClientSamples $clientSeen -Final $final }
                 else { Invoke-CoopScore -HostSamples $hostSeen -ClientSamples $clientSeen -Tolerance $Tolerance -MoveMetres $MoveMetres -Final $final }
        if (((Get-Date) - $lastLog).TotalSeconds -ge 15) {
            $lastLog = Get-Date
            W ("host: inWorld={0} connected={1} cloneVisible={2} acted={3} cell={4} | client: inWorld={5} connected={6} cloneVisible={7} cell={8} | free RAM {9} GB" -f `
                $(if ($hs) { $hs.inWorld } else { "-" }), $(if ($hs) { $hs.connected } else { "-" }), $(if ($hs) { $hs.cloneVisible } else { "-" }), $(if ($hs) { $hs.acted } else { "-" }), $(if ($hs) { $hs.playerCell } else { "-" }),
                $(if ($cs) { $cs.inWorld } else { "-" }), $(if ($cs) { $cs.connected } else { "-" }), $(if ($cs) { $cs.cloneVisible } else { "-" }), $(if ($cs) { $cs.playerCell } else { "-" }), (FreeGB))
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
        foreach ($f in @("logs\harness.log", "logs\bot-state.jsonl", "profile\Documents\My Games\Starfield\SFSE\Logs\PULSAR.log")) {
            $src = Join-Path $Instances "$slot\$f"
            if (Test-Path -LiteralPath $src) { Copy-Item -LiteralPath $src (Join-Path $runDir ("$slot-" + (Split-Path $f -Leaf))) }
        }
    }
    foreach ($w in $why) { W "reason: $w" }
    Add-Content -LiteralPath $logFile "RESULT: $result" -Encoding utf8
    Write-Output "RESULT: $result"
}
