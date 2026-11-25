# Builds standalone one-click Windows installer: installer\windows\LanVid_Setup.exe & LanVid_Setup_v2.6.0.exe
# Links static CRT (/MT) so LanVid_Setup.exe runs on clean Windows 10/11 BEFORE VC++ Redist is installed!
# Optional: pass -BuildInno to also compile installer\windows\lanvid.iss with Inno Setup 6 (ISCC.exe) if installed.
param(
    [switch]$BuildInno
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$buildDir = Join-Path $root "build"
$winDir = $PSScriptRoot
$stageDir = Join-Path $winDir "_stage"
$zipPath = Join-Path $winDir "lanvid_payload.zip"
$version = "2.6.0"
$outExeVersioned = Join-Path $winDir "LanVid_Setup_v$version.exe"
$outExe = Join-Path $winDir "LanVid_Setup.exe"

Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $stageDir, $zipPath
New-Item -ItemType Directory -Path $stageDir -Force | Out-Null

# 1. Stage Core Studio + Grid L2 DSP Server + Virtual ASIO + Preflight Configurator
$files = @(
    (Join-Path $buildDir "lanvid_app.exe"),
    (Join-Path $buildDir "lanvid_core.dll"),
    (Join-Path $buildDir "sender.exe"),
    (Join-Path $buildDir "receiver.exe"),
    (Join-Path $buildDir "lanvid_dsp_server.exe"),
    (Join-Path $buildDir "LanVidASIO.dll"),
    (Join-Path $winDir "preflight.ps1")
)
foreach ($f in $files) {
    if (-not (Test-Path $f)) { throw "Missing build artifact: $f" }
    Copy-Item $f $stageDir -Force
}

$readme = Join-Path $root "README.md"
if (Test-Path $readme) {
    New-Item -ItemType Directory -Path (Join-Path $stageDir "docs") -Force | Out-Null
    Copy-Item $readme (Join-Path $stageDir "docs\README.md") -Force
}

# 2. Stage optional bundled Npcap OEM / setup executable if provided in installer\windows\third_party\
$thirdPartyDir = Join-Path $winDir "third_party"
if (Test-Path $thirdPartyDir) {
    $npcapFiles = Get-ChildItem -Path $thirdPartyDir -Filter "npcap-*.exe" -ErrorAction SilentlyContinue
    if ($npcapFiles) {
        $tpStage = Join-Path $stageDir "third_party"
        New-Item -ItemType Directory -Path $tpStage -Force | Out-Null
        foreach ($nf in $npcapFiles) { Copy-Item $nf.FullName $tpStage -Force }
        Write-Host "Staged bundled Npcap installer(s): $(($npcapFiles | Select-Object -ExpandProperty Name) -join ', ')"
    }
}

# 3. Stage obs-lanvid plugin + locales (en-US.ini, uk-UA.ini) if built against OBS SDK
$obsDll = Join-Path $buildDir "plugins\obs-lanvid\obs-lanvid.dll"
$obsLocaleDir = Join-Path $root "plugins\obs-lanvid\data\locale"
$obsStage = Join-Path $stageDir "obs-lanvid"
if (Test-Path $obsLocaleDir) {
    New-Item -ItemType Directory -Path (Join-Path $obsStage "data\obs-plugins\obs-lanvid\locale") -Force | Out-Null
    Copy-Item (Join-Path $obsLocaleDir "*.ini") (Join-Path $obsStage "data\obs-plugins\obs-lanvid\locale\") -Force
}
if (Test-Path $obsDll) {
    New-Item -ItemType Directory -Path (Join-Path $obsStage "obs-plugins\64bit") -Force | Out-Null
    Copy-Item $obsDll (Join-Path $obsStage "obs-plugins\64bit\obs-lanvid.dll") -Force
    $obsSystemDir = "C:\Program Files\obs-studio\obs-plugins\64bit"
    if (Test-Path $obsSystemDir) {
        try {
            Copy-Item $obsDll (Join-Path $obsSystemDir "obs-lanvid.dll") -Force
            Write-Host "Installed obs-lanvid.dll into $obsSystemDir"
        } catch {}
    }
}

# 4. Compress payload and compile zero-dependency Win32 setup_wizard.cpp
Compress-Archive -Path "$stageDir\*" -DestinationPath $zipPath -CompressionLevel Optimal -Force

$cl = Get-Command cl.exe -ErrorAction SilentlyContinue
if ($cl) {
    $cmd = "cd /d `"$winDir`" && rc.exe /nologo /fo setup_payload.res setup_payload.rc && cl.exe /nologo /std:c++20 /O2 /MT /EHsc /utf-8 /DWIN32_LEAN_AND_MEAN /DNOMINMAX setup_wizard.cpp setup_payload.res /Fe:`"$outExe`" /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED /MANIFESTUAC:`"level='requireAdministrator' uiAccess='false'`""
} else {
    $candidates = @(
        "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat",
        "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat",
        "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat",
        "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
    )
    $vcvars = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $vcvars) { throw "Could not locate MSVC cl.exe or vcvars64.bat" }
    $cmd = "call `"$vcvars`" >nul && cd /d `"$winDir`" && rc.exe /nologo /fo setup_payload.res setup_payload.rc && cl.exe /nologo /std:c++20 /O2 /MT /EHsc /utf-8 /DWIN32_LEAN_AND_MEAN /DNOMINMAX setup_wizard.cpp setup_payload.res /Fe:`"$outExe`" /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED /MANIFESTUAC:`"level='requireAdministrator' uiAccess='false'`""
}
cmd /c $cmd
if ($LASTEXITCODE -ne 0) { throw "Failed to compile LanVid_Setup.exe (exit=$LASTEXITCODE)" }

Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $stageDir, $zipPath, (Join-Path $winDir "setup_payload.res"), (Join-Path $winDir "setup_wizard.obj")
Copy-Item -Force $outExe $outExeVersioned
$item = Get-Item $outExeVersioned
Write-Host ("Built {0} and {1} ({2:N0} bytes)" -f $item.FullName, $outExe, $item.Length)
Write-Host "  CLI modes: /S (Auto silent) | /COMPONENTS=core,dsp_server,virtual_audio,grid_l2_npcap,obs_plugin,firewall,high_perf_power | --custom | --check-only"

# 5. Optional Inno Setup 6 compilation of lanvid.iss
if ($BuildInno) {
    $iscc = Get-Command iscc.exe -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source
    if (-not $iscc -and (Test-Path "C:\Program Files (x86)\Inno Setup 6\ISCC.exe")) {
        $iscc = "C:\Program Files (x86)\Inno Setup 6\ISCC.exe"
    }
    if ($iscc) {
        Write-Host "Compiling Inno Setup script: installer\windows\lanvid.iss"
        & $iscc (Join-Path $winDir "lanvid.iss")
        if ($LASTEXITCODE -ne 0) { throw "ISCC.exe failed (exit=$LASTEXITCODE)" }
    } else {
        Write-Warning "Inno Setup 6 (ISCC.exe) not found; skipped lanvid.iss compilation."
    }
}
