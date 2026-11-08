<#
  package_single_exe.ps1 - one file for a friend: PULSAR-Revived-Setup-<Version>.exe.

  The exe is the PULSAR tool with the mod (hardened DLL, master-flagged ESP, scripts, BA2) and
  PulsarCompanion.exe packed inside (Services/EmbeddedPayload.cs unpacks them to
  %LOCALAPPDATA%\PULSAR\payload\ on first start). On first start the tool finds Starfield,
  installs the mod and the game settings by itself, and shows Setup for the two things it may
  not fetch: SFSE and the Address Library (Nexus downloads it then installs from Downloads).

  Steps: publish the companion and the tool into lobby\_dist\Pulsar, run package_release.ps1
  (hardened DLL + staging + checks), zip the staged mod + companion as the payload, publish
  the tool once more as a single file with the payload embedded.

  Usage:  .\tools\package_single_exe.ps1 -Version 0.7.5
#>
param(
    [string]$Version = "0.7.5",
    [string]$Dotnet = "$env:ProgramFiles\dotnet\dotnet.exe",
    # passed on to package_release.ps1 (cmake on PATH or the VS 2022 one otherwise)
    [string]$CMake = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$dist = Join-Path $root "lobby\_dist\Pulsar"
$out  = Join-Path $root "dist"
function Fail($m) { Write-Host "FAIL: $m" -ForegroundColor Red; exit 1 }

$publish = @("-c", "Release", "-r", "win-x64", "--self-contained",
             "-p:PublishSingleFile=true", "-p:IncludeNativeLibrariesForSelfExtract=true",
             "-p:Version=$Version")

Write-Host "[1/4] Publishing the companion and the tool ..." -ForegroundColor Cyan
& $Dotnet publish (Join-Path $root "lobby\companion\Companion.csproj") @publish -o $dist | Out-Host
if ($LASTEXITCODE -ne 0) { Fail "companion publish failed" }
& $Dotnet publish (Join-Path $root "lobby\pulsar\Pulsar.csproj") @publish -o $dist | Out-Host
if ($LASTEXITCODE -ne 0) { Fail "tool publish failed" }

Write-Host "[2/4] Building and staging the hardened mod (package_release.ps1) ..." -ForegroundColor Cyan
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "package_release.ps1") -Version $Version -CMake $CMake
if ($LASTEXITCODE -ne 0) { Fail "package_release.ps1 failed" }
$stage = Join-Path $out "PULSAR-Revived-$Version"

Write-Host "[3/4] Packing the payload ..." -ForegroundColor Cyan
$payloadDir = Join-Path $out "payload-$Version"
if (Test-Path $payloadDir) { Remove-Item $payloadDir -Recurse -Force }
New-Item -ItemType Directory -Force -Path $payloadDir | Out-Null
Copy-Item (Join-Path $stage "mod") $payloadDir -Recurse
Copy-Item (Join-Path $stage "PulsarCompanion.exe") $payloadDir
$payloadZip = Join-Path $out "payload-$Version.zip"
if (Test-Path $payloadZip) { Remove-Item $payloadZip -Force }
Compress-Archive -Path (Join-Path $payloadDir "*") -DestinationPath $payloadZip
Remove-Item $payloadDir -Recurse -Force

Write-Host "[4/4] Publishing the one-file setup ..." -ForegroundColor Cyan
$singleDir = Join-Path $out "single-$Version"
& $Dotnet publish (Join-Path $root "lobby\pulsar\Pulsar.csproj") @publish "-p:PulsarPayloadZip=$payloadZip" -o $singleDir | Out-Host
if ($LASTEXITCODE -ne 0) { Fail "one-file publish failed" }
$exe = Join-Path $out "PULSAR-Revived-Setup-$Version.exe"
Copy-Item (Join-Path $singleDir "Pulsar.exe") $exe -Force
Remove-Item $singleDir -Recurse -Force
Remove-Item $payloadZip -Force

$mb = [math]::Round((Get-Item $exe).Length / 1MB, 1)
Write-Host "DONE: $exe ($mb MB)" -ForegroundColor Green
