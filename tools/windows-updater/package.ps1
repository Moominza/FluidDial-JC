# Builds the portable package FluidDial-Updater-v<version>-win-x64.zip from the exe and the release firmware.
param(
    [string]$Version = "1.5.2",
    [Parameter(Mandatory = $true)][string]$SourceCommit,
    [string]$ReleaseDir = ""
)
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
if ($ReleaseDir -eq "") { $ReleaseDir = Join-Path (Split-Path -Parent (Split-Path -Parent $here)) "release" }
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $here "build.ps1") | Out-Null
if ($LASTEXITCODE -ne 0) { throw "build failed" }

$name = "FluidDial-Updater-v$Version-win-x64"
$dist = Join-Path $here "dist"
$stage = Join-Path $dist $name
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force (Join-Path $stage "firmware") | Out-Null

Copy-Item (Join-Path $here "build\FluidDialUpdater.exe") $stage
Copy-Item (Join-Path $here "README.txt") $stage

$full = "fluiddial-jc-$Version-fullcontrol.bin"
$diag = "fluiddial-jc-$Version-diagnostic-motion-disabled.bin"
Copy-Item (Join-Path $ReleaseDir $full) (Join-Path $stage "firmware")
Copy-Item (Join-Path $ReleaseDir $diag) (Join-Path $stage "firmware")

function Sha($p) { (Get-FileHash $p -Algorithm SHA256).Hash.ToLower() }
$m = @(
    "# FluidDial-JC firmware manifest. Format: SHA256  file  kind=fullcontrol|diagnostic  version=  commit=",
    "# These are application images for the Guition JC8048W550C_I. They are not complete flash images.",
    ("{0}  {1}  kind=fullcontrol  version={2}  commit={3}" -f (Sha (Join-Path $stage "firmware\$full")), $full, $Version, $SourceCommit),
    ("{0}  {1}  kind=diagnostic  version={2}  commit={3}" -f (Sha (Join-Path $stage "firmware\$diag")), $diag, $Version, $SourceCommit)
)
Set-Content -Path (Join-Path $stage "firmware\MANIFEST.txt") -Value $m -Encoding ascii

$v = @(
    "FluidDial Updater $Version",
    "Firmware source commit: $SourceCommit",
    "Protocol: stock Arduino ESP32 OTA (espota) over UDP/TCP 3232, password challenge. Not encrypted, not signed.",
    "Runs on: Windows 10/11 x64 with the .NET Framework 4.x that ships with Windows (no installation).",
    "Built with: the C# compiler included in Windows (no SDK).",
    "See README.txt."
)
Set-Content -Path (Join-Path $stage "VERSION.txt") -Value $v -Encoding ascii

$sums = Get-ChildItem $stage -Recurse -File | Where-Object { $_.Name -ne "SHA256SUMS.txt" } | ForEach-Object {
    $rel = $_.FullName.Substring($stage.Length + 1)
    "{0}  {1}" -f (Sha $_.FullName), $rel
}
Set-Content -Path (Join-Path $stage "SHA256SUMS.txt") -Value $sums -Encoding ascii

$zip = Join-Path $dist "$name.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path $stage -DestinationPath $zip -CompressionLevel Optimal
Get-Item $zip | Select-Object FullName, Length
"sha256 " + (Sha $zip)
