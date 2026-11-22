# Builds standalone one-click installer: installer\windows\LanVid_Setup.exe
# Links static CRT (/MT) so LanVid_Setup.exe runs on clean Windows 10/11 BEFORE VC++ Redist is installed!
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$buildDir = Join-Path $root "build"
$winDir = $PSScriptRoot
$stageDir = Join-Path $winDir "_stage"
$zipPath = Join-Path $winDir "lanvid_payload.zip"
$outExe = Join-Path $winDir "LanVid_Setup.exe"

Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $stageDir, $zipPath
New-Item -ItemType Directory -Path $stageDir -Force | Out-Null

$files = @(
    (Join-Path $buildDir "lanvid_app.exe"),
    (Join-Path $buildDir "lanvid_core.dll"),
    (Join-Path $buildDir "sender.exe"),
    (Join-Path $buildDir "receiver.exe"),
    (Join-Path $winDir "preflight.ps1")
)
foreach ($f in $files) {
    if (-not (Test-Path $f)) { throw "Missing build artifact: $f" }
    Copy-Item $f $stageDir -Force
}

Compress-Archive -Path "$stageDir\*" -DestinationPath $zipPath -CompressionLevel Optimal -Force

$cl = Get-Command cl.exe -ErrorAction SilentlyContinue
if ($cl) {
    $cmd = "cd /d `"$winDir`" && rc.exe /nologo /fo setup_payload.res setup_payload.rc && cl.exe /nologo /std:c++20 /O2 /MT /EHsc /utf-8 /DWIN32_LEAN_AND_MEAN /DNOMINMAX setup_wizard.cpp setup_payload.res /Fe:`"$outExe`" /link /SUBSYSTEM:WINDOWS"
} else {
    $candidates = @(
        "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat",
        "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat",
        "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat",
        "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
    )
    $vcvars = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $vcvars) { throw "Could not locate MSVC cl.exe or vcvars64.bat" }
    $cmd = "call `"$vcvars`" >nul && cd /d `"$winDir`" && rc.exe /nologo /fo setup_payload.res setup_payload.rc && cl.exe /nologo /std:c++20 /O2 /MT /EHsc /utf-8 /DWIN32_LEAN_AND_MEAN /DNOMINMAX setup_wizard.cpp setup_payload.res /Fe:`"$outExe`" /link /SUBSYSTEM:WINDOWS"
}
cmd /c $cmd
if ($LASTEXITCODE -ne 0) { throw "Failed to compile LanVid_Setup.exe (exit=$LASTEXITCODE)" }

Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $stageDir, $zipPath, (Join-Path $winDir "setup_payload.res"), (Join-Path $winDir "setup_wizard.obj")
$item = Get-Item $outExe
Write-Host ("Built {0} ({1:N0} bytes)" -f $item.FullName, $item.Length)
