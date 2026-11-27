<#
.SYNOPSIS
  Interactive & CLI Selectable SDK Installer for Windows (PowerShell)
.EXAMPLE
  .\scripts\install_sdk.ps1
  .\scripts\install_sdk.ps1 -Mode video -Prefix C:\LanVidSDK
  .\scripts\install_sdk.ps1 -Mode audio -Prefix C:\CrossLinkSDK
  .\scripts\install_sdk.ps1 -Mode all   -Prefix C:\LanVidCrossLinkSDK
#>
param(
  [ValidateSet("", "all", "video", "audio")]
  [string]$Mode = "",
  [string]$Prefix = "$env:LOCALAPPDATA\LanVidCrossLinkSDK"
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)

if (-not $Mode) {
  Write-Host "================================================================"
  Write-Host "  LanVid Video & CrossLink Audio SDK - Selectable Installer"
  Write-Host "================================================================"
  Write-Host "  1) Install ALL together (LanVid Video v3.1.0 + CrossLink Audio v1.0.0)"
  Write-Host "  2) Install ONLY LanVid Video SDK (Video + 2ch Stereo Audio)"
  Write-Host "  3) Install ONLY CrossLink Audio SDK (64ch AoIP + MIDI 2.0 + Grid)"
  Write-Host "----------------------------------------------------------------"
  $choice = Read-Host "Choose [1-3] (default 1)"
  switch ($choice) {
    ""  { $Mode = "all" }
    "1" { $Mode = "all" }
    "2" { $Mode = "video" }
    "3" { $Mode = "audio" }
    default { throw "Invalid selection: $choice" }
  }
}

$Comp = $Mode.ToUpper()
$BuildDir = Join-Path $Root "build-install-$Mode"

Write-Host "[INFO] Configuring SDK (LANVID_SDK_COMPONENTS=$Comp, Prefix=$Prefix)..."
cmake -S $Root -B $BuildDir `
  -DCMAKE_BUILD_TYPE=Release `
  "-DLANVID_SDK_COMPONENTS=$Comp" `
  -DLANVID_SDK_CORE_ONLY=ON `
  -DLANVID_BUILD_SHARED=ON `
  -DLANVID_BUILD_TESTS=OFF

Write-Host "[INFO] Building SDK..."
cmake --build $BuildDir --config Release --parallel

Write-Host "[INFO] Installing SDK to $Prefix..."
cmake --install $BuildDir --config Release --prefix $Prefix

Remove-Item -Recurse -Force $BuildDir -ErrorAction SilentlyContinue
Write-Host "[OK] Installation complete! Mode=$Comp, Prefix=$Prefix"
