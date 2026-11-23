<#
.SYNOPSIS
  LanVid Studio Windows Pre-Flight Check, Conflict Resolver & Environment Configurator.
  Checks running processes, UDP port 5000 availability, VC++ 2015-2022 x64 Redistributable,
  Direct3D 11 hardware support, and Windows Defender Firewall rules.
#>
param(
    [string]$InstallDir = "$env:LOCALAPPDATA\Programs\LanVid",
    [switch]$StopProcesses,
    [switch]$InstallVCRedist,
    [switch]$ConfigureFirewall,
    [switch]$CheckOnly
)

$ErrorActionPreference = "Continue"
$configDir = Join-Path $env:APPDATA "LanVid"
$configPath = Join-Path $configDir "config.json"

function Load-OrCreateConfig {
    if (Test-Path $configPath) {
        try {
            return (Get-Content $configPath -Raw | ConvertFrom-Json)
        } catch {}
    }
    return [PSCustomObject]@{
        dest_ip          = "127.0.0.1"
        dest_port        = 5000
        listen_port      = 5000
        preview_backend  = "auto"
        quality          = 80
        fps              = 60
        volume           = 1.0
    }
}

function Save-Config($obj) {
    if (-not (Test-Path $configDir)) { New-Item -ItemType Directory -Path $configDir -Force | Out-Null }
    $obj | ConvertTo-Json -Depth 4 | Set-Content -Path $configPath -Encoding UTF8
}

$cfg = Load-OrCreateConfig
$report = [ordered]@{
    ProcessesClosed = @()
    ListenPort      = 5000
    PortConflict    = $false
    VCRedistOk      = $false
    D3D11Ok         = $false
    PreviewBackend  = "d3d11"
    FirewallOk      = $false
}

# 1. Check running LanVid processes
$targets = @("lanvid_app", "sender", "receiver")
$running = Get-Process -Name $targets -ErrorAction SilentlyContinue
if ($running) {
    Write-Host "[PREFLIGHT] Found running LanVid processes: $(($running | Select-Object -ExpandProperty ProcessName) -join ', ')"
    if ($StopProcesses -and -not $CheckOnly) {
        foreach ($p in $running) {
            try {
                $p.CloseMainWindow() | Out-Null
                if (-not $p.WaitForExit(1500)) { $p.Kill() }
                $report.ProcessesClosed += $p.ProcessName
            } catch {}
        }
        Write-Host "[PREFLIGHT] Stopped running instances cleanly."
    }
}

# 2. Check UDP port 5000 conflict (Get-NetUDPEndpoint)
$chosenPort = 5000
$conflict = Get-NetUDPEndpoint -LocalPort 5000 -ErrorAction SilentlyContinue
if ($conflict) {
    $report.PortConflict = $true
    $ownerPid = ($conflict | Select-Object -First 1).OwningProcess
    $ownerName = (Get-Process -Id $ownerPid -ErrorAction SilentlyContinue).ProcessName
    Write-Host "[PREFLIGHT] WARNING: UDP port 5000 is occupied by '$ownerName' (PID=$ownerPid)."
    for ($cand = 5002; $cand -le 5100; $cand += 2) {
        if (-not (Get-NetUDPEndpoint -LocalPort $cand -ErrorAction SilentlyContinue)) {
            $chosenPort = $cand
            break
        }
    }
    Write-Host "[PREFLIGHT] Selected free default UDP port: $chosenPort"
}
$report.ListenPort = $chosenPort
if (-not $CheckOnly) {
    $cfg | Add-Member -NotePropertyName "listen_port" -NotePropertyValue $chosenPort -Force
}

# 3. Check Visual C++ 2015-2022 Redistributable (x64)
$vcKey = "HKLM:\SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64"
$vcOk = $false
if (Test-Path $vcKey) {
    $vc = Get-ItemProperty $vcKey -ErrorAction SilentlyContinue
    if ($vc -and $vc.Installed -eq 1 -and $vc.Major -ge 14 -and $vc.Minor -ge 30) {
        $vcOk = $true
        Write-Host "[PREFLIGHT] VC++ Redistributable x64 OK (v$($vc.Major).$($vc.Minor).$($vc.Bld))."
    }
}
if (-not $vcOk -and $InstallVCRedist -and -not $CheckOnly) {
    Write-Host "[PREFLIGHT] Installing Visual C++ 2015-2022 Redistributable (x64)..."
    $bundled = Join-Path $PSScriptRoot "vc_redist.x64.exe"
    $vcExe = $bundled
    if (-not (Test-Path $vcExe)) {
        $vcExe = Join-Path $env:TEMP "vc_redist.x64.exe"
        try {
            Invoke-WebRequest -Uri "https://aka.ms/vs/17/release/vc_redist.x64.exe" -OutFile $vcExe -UseBasicParsing -TimeoutSec 45
        } catch {
            Write-Host "[PREFLIGHT] Could not download VC++ Redistributable: $_"
        }
    }
    if (Test-Path $vcExe) {
        $proc = Start-Process -FilePath $vcExe -ArgumentList "/install /quiet /norestart" -Wait -PassThru
        if ($proc.ExitCode -eq 0 -or $proc.ExitCode -eq 3010) { $vcOk = $true }
    }
}
$report.VCRedistOk = $vcOk

# 4. Check Windows version & Direct3D 11 hardware support
$osVer = [System.Environment]::OSVersion.Version
$d3d11Ok = $false
try {
    Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class LvD3D11Probe {
    [DllImport("d3d11.dll")]
    private static extern int D3D11CreateDevice(
        IntPtr pAdapter, int DriverType, IntPtr Software, uint Flags,
        IntPtr pFeatureLevels, uint FeatureLevels, uint SDKVersion,
        out IntPtr ppDevice, out int pFeatureLevel, out IntPtr ppImmediateContext);
    public static bool HasHardwareD3D11() {
        IntPtr dev = IntPtr.Zero, ctx = IntPtr.Zero;
        int fl = 0;
        int hr = D3D11CreateDevice(IntPtr.Zero, 1, IntPtr.Zero, 0x20, IntPtr.Zero, 0, 7, out dev, out fl, out ctx);
        if (ctx != IntPtr.Zero) Marshal.Release(ctx);
        if (dev != IntPtr.Zero) Marshal.Release(dev);
        return hr >= 0;
    }
}
"@ -ErrorAction SilentlyContinue
    $d3d11Ok = [LvD3D11Probe]::HasHardwareD3D11()
} catch {
    $d3d11Ok = $false
}
$report.D3D11Ok = $d3d11Ok
$report.PreviewBackend = if ($d3d11Ok) { "d3d11" } else { "gdi" }
Write-Host "[PREFLIGHT] Windows $($osVer.Major).$($osVer.Minor) (build $($osVer.Build)) | D3D11 Hardware: $d3d11Ok -> backend=$($report.PreviewBackend)"
if (-not $CheckOnly) {
    $cfg | Add-Member -NotePropertyName "preview_backend" -NotePropertyValue $report.PreviewBackend -Force
    Save-Config $cfg
}

# 5. Windows Defender Firewall rules for LanVid
if ($ConfigureFirewall -and -not $CheckOnly) {
    $appExe = Join-Path $InstallDir "lanvid_app.exe"
    try {
        Remove-NetFirewallRule -DisplayName "LanVid Studio (UDP In)" -ErrorAction SilentlyContinue
        Remove-NetFirewallRule -DisplayName "LanVid Studio (UDP Out)" -ErrorAction SilentlyContinue
        Remove-NetFirewallRule -DisplayName "LanVid Monitor & Discovery (UDP Ports)" -ErrorAction SilentlyContinue
        New-NetFirewallRule -DisplayName "LanVid Studio (UDP In)" -Direction Inbound -Action Allow `
            -Protocol UDP -Program $appExe -Profile Domain,Private,Public -ErrorAction Stop | Out-Null
        New-NetFirewallRule -DisplayName "LanVid Studio (UDP Out)" -Direction Outbound -Action Allow `
            -Protocol UDP -Program $appExe -Profile Domain,Private,Public -ErrorAction Stop | Out-Null
        New-NetFirewallRule -DisplayName "LanVid Monitor & Discovery (UDP Ports)" -Direction Inbound -Action Allow `
            -Protocol UDP -LocalPort 5000,5020,5021,5022,5099,5353 -Profile Domain,Private,Public -ErrorAction Stop | Out-Null
        $report.FirewallOk = $true
        Write-Host "[PREFLIGHT] Windows Defender Firewall UDP rules configured (Domain/Private/Public + ports 5020/5021/5022/5353)."
    } catch {
        Write-Host "[PREFLIGHT] Firewall rule requires Administrator privileges (skipped in per-user mode)."
    }
}

$report | ConvertTo-Json -Compress
