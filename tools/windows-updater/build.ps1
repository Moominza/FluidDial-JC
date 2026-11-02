# Builds FluidDialUpdater.exe with the C# compiler that ships with Windows (.NET Framework 4.x).
# No SDK, NuGet, PlatformIO or Python is needed to build or to run the result.
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$csc = "$env:WINDIR\Microsoft.NET\Framework64\v4.0.30319\csc.exe"
if (-not (Test-Path $csc)) { throw "csc.exe not found: $csc" }
$out = Join-Path $here "build"
New-Item -ItemType Directory -Force $out | Out-Null
$exe = Join-Path $out "FluidDialUpdater.exe"
& $csc /nologo /target:winexe /platform:x64 /optimize+ /warn:4 /warnaserror+ "/out:$exe" "/win32manifest:$here\app.manifest" `
    /r:System.dll /r:System.Drawing.dll /r:System.Windows.Forms.dll `
    "$here\src\OtaClient.cs" "$here\src\FirmwareInfo.cs" "$here\src\MainForm.cs" "$here\src\Program.cs"
if ($LASTEXITCODE -ne 0) { throw "compile failed" }
Get-Item $exe | Select-Object FullName, Length
