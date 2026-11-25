# Prepares a light OBS SDK for building obs-lanvid against the OBS Studio installed on this PC:
#   headers of the matching obs-studio tag (libobs + obs-frontend-api.h + obsconfig.h) and import libraries
#   (obs.lib / obs-frontend-api.lib) generated from the installed DLLs' export tables (dumpbin + lib).
# Run from a Visual Studio developer shell (dumpbin.exe / lib.exe on PATH):
#   pwsh plugins/obs-lanvid/tools/fetch_obs_sdk.ps1 [-Out build-obs-sdk] [-ObsDir "C:\Program Files\obs-studio"]
#   cmake ... -DBUILD_OBS_PLUGIN=ON -DLANVID_OBS_SDK_DIR=<Out>
param(
    [string]$Out = (Join-Path (Resolve-Path "$PSScriptRoot\..\..\..") "build-obs-sdk"),
    [string]$ObsDir = "C:\Program Files\obs-studio"
)
$ErrorActionPreference = "Stop"
$bin = Join-Path $ObsDir "bin\64bit"
$ver = (Get-Item (Join-Path $bin "obs64.exe")).VersionInfo.ProductVersion
Write-Host "OBS Studio $ver at $ObsDir -> $Out"
$tmp = Join-Path ([IO.Path]::GetTempPath()) ("obs-sdk-" + [Guid]::NewGuid())
git clone -q --depth 1 --branch $ver --filter=blob:none --sparse https://github.com/obsproject/obs-studio.git "$tmp\obs"
git -C "$tmp\obs" sparse-checkout set libobs frontend/api UI/obs-frontend-api
if (Test-Path $Out) { Remove-Item -Recurse -Force $Out }
New-Item -ItemType Directory -Force "$Out\include", "$Out\lib" | Out-Null
$src = Join-Path $tmp "obs\libobs"
Get-ChildItem $src -Recurse -Include *.h, *.hpp | ForEach-Object {
    $rel = $_.FullName.Substring($src.Length + 1)
    $dst = Join-Path "$Out\include" $rel
    New-Item -ItemType Directory -Force (Split-Path $dst) | Out-Null
    Copy-Item $_.FullName $dst
}
Copy-Item (Get-ChildItem "$tmp\obs" -Recurse -Filter obs-frontend-api.h | Select-Object -First 1).FullName "$Out\include\"
Set-Content "$Out\include\obsconfig.h" "#pragma once`n#define OBS_RELEASE_CANDIDATE 0`n#define OBS_BETA 0`n"
foreach ($pair in @(@("obs.dll", "obs"), @("obs-frontend-api.dll", "obs-frontend-api"))) {
    $dll = Join-Path $bin $pair[0]
    $name = $pair[1]
    $exports = dumpbin /nologo /exports $dll | ForEach-Object {
        if ($_ -match '^\s+\d+\s+[0-9A-F]+\s+[0-9A-F]+\s+(\S+)') { $Matches[1] }
    }
    $def = @("LIBRARY $($pair[0])", "EXPORTS") + ($exports | ForEach-Object { "    $_" })
    Set-Content "$Out\lib\$name.def" $def
    lib /nologo /machine:x64 /def:"$Out\lib\$name.def" /out:"$Out\lib\$name.lib" | Out-Null
    Write-Host "  $name.lib: $($exports.Count) exports"
}
Set-Content "$Out\OBS_VERSION" $ver
Remove-Item -Recurse -Force $tmp
Write-Host "Light OBS SDK ready: -DLANVID_OBS_SDK_DIR=$Out"
