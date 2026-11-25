<#
.SYNOPSIS
  LanVid Studio + LanVid Grid (L2 · 256ch) + DSP Server Windows Configurator & Pre-Flight Engine.
  Handles:
    - Conflict process termination (lanvid_app, sender, receiver, lanvid_dsp_server)
    - UDP port 5000 probe & conflict-free default assignment
    - Visual C++ 2015-2022 x64 Redistributable verification & silent installation
    - Direct3D 11 hardware check & config persistence
    - Windows Defender Firewall rules (Domain,Private,Public: UDP 5000-5035, 5099, 5353 mDNS + TCP)
    - Policy-based QoS DSCP=46 (EF) for lanvid_app.exe & lanvid_dsp_server.exe
    - Socket buffer & MMCSS audio priority tuning + EEE/Interrupt Moderation check + High Performance power plan
    - LanVidGridDSP background service / auto-restart scheduled task registration
    - Virtual 64ch ASIO driver (LanVidASIO.dll) registration
    - Npcap Layer 2 driver detection, OEM silent / interactive installation & `sc start npcap`
    - Automatic OBS Studio detection (Registry + Program Files) & obs-lanvid.dll + locales deployment
    - Clean full uninstallation (-Uninstall)
#>
param(
    [string]$InstallDir = "$env:LOCALAPPDATA\Programs\LanVid",
    [switch]$StopProcesses,
    [switch]$InstallVCRedist,
    [switch]$ConfigureFirewall,
    [switch]$ConfigureQoS,
    [switch]$TuneNetwork,
    [switch]$EnableHighPerfPower,
    [switch]$InstallDspService,
    [switch]$RegisterAsio,
    [switch]$InstallNpcap,
    [switch]$InstallObsPlugin,
    [switch]$Uninstall,
    [switch]$Silent,
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
    $obj | ConvertTo-Json -Depth 6 | Set-Content -Path $configPath -Encoding UTF8
}

function Test-IsAdmin {
    try {
        $id = [Security.Principal.WindowsIdentity]::GetCurrent()
        $pr = New-Object Security.Principal.WindowsPrincipal($id)
        return $pr.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    } catch {
        return $false
    }
}

# =============================================================================
# FULL CLEAN UNINSTALL MODE (-Uninstall)
# =============================================================================
if ($Uninstall) {
    Write-Host "[UNINSTALL] Cleaning up LanVid ecosystem services, Firewall, QoS DSCP & ASIO..."

    # 1. Stop & remove LanVidGridDSP Scheduled Task and Windows Service (if present)
    try {
        Stop-ScheduledTask -TaskName "LanVidGridDSP" -ErrorAction SilentlyContinue
        Unregister-ScheduledTask -TaskName "LanVidGridDSP" -Confirm:$false -ErrorAction SilentlyContinue
    } catch {}
    try {
        & sc.exe stop LanVidGridDSP 2>$null | Out-Null
        & sc.exe delete LanVidGridDSP 2>$null | Out-Null
    } catch {}

    # 2. Terminate any running LanVid binaries
    foreach ($name in @("lanvid_app", "lanvid_dsp_server", "sender", "receiver")) {
        Stop-Process -Name $name -Force -ErrorAction SilentlyContinue
    }

    # 3. Unregister 64-bit LanVidASIO.dll
    $asioDll = Join-Path $InstallDir "LanVidASIO.dll"
    if (Test-Path $asioDll) {
        try {
            Start-Process -FilePath "$env:SystemRoot\System32\regsvr32.exe" `
                -ArgumentList "/u", "/s", "`"$asioDll`"" -Wait -NoNewWindow -ErrorAction SilentlyContinue
        } catch {}
    }

    # 4. Remove all LanVid Windows Defender Firewall rules
    $fwNames = @(
        "LanVid Studio (UDP In)",
        "LanVid Studio (UDP Out)",
        "LanVid Studio (TCP In)",
        "LanVid Studio (TCP Out)",
        "LanVid Grid DSP Server (UDP In)",
        "LanVid Grid DSP Server (UDP Out)",
        "LanVid Grid DSP Server (TCP In)",
        "LanVid Grid DSP Server (TCP Out)",
        "LanVid Monitor & Discovery (UDP Ports)",
        "LanVid Ecosystem (UDP 5000-5035, 5099, 5353)"
    )
    foreach ($r in $fwNames) {
        Remove-NetFirewallRule -DisplayName $r -ErrorAction SilentlyContinue
    }

    # 5. Remove Policy-based QoS DSCP entries
    foreach ($qKey in @(
        "HKLM:\SOFTWARE\Policies\Microsoft\Windows\QoS\LanVid Studio",
        "HKLM:\SOFTWARE\Policies\Microsoft\Windows\QoS\LanVid Grid DSP Server"
    )) {
        if (Test-Path $qKey) {
            Remove-Item -Path $qKey -Recurse -Force -ErrorAction SilentlyContinue
        }
    }

    Write-Host "[UNINSTALL] LanVid system cleanup complete."
    exit 0
}

$isAdmin = Test-IsAdmin
$cfg = Load-OrCreateConfig
$report = [ordered]@{
    IsAdmin          = $isAdmin
    ProcessesClosed  = @()
    ListenPort       = 5000
    PortConflict     = $false
    VCRedistOk       = $false
    D3D11Ok          = $false
    PreviewBackend   = "d3d11"
    FirewallOk       = $false
    QosDscpOk        = $false
    NetworkTuned     = $false
    HighPerfPowerOk  = $false
    DspServiceOk     = $false
    AsioOk           = $false
    NpcapOk          = $false
    ObsPluginOk      = $false
    ObsPath          = ""
}

# =============================================================================
# 1. Stop running LanVid processes & LanVidGridDSP before upgrade
# =============================================================================
$targets = @("lanvid_app", "lanvid_dsp_server", "sender", "receiver")
$running = Get-Process -Name $targets -ErrorAction SilentlyContinue
if ($running) {
    Write-Host "[PREFLIGHT] Found running LanVid processes: $(($running | Select-Object -ExpandProperty ProcessName) -join ', ')"
    if ($StopProcesses -and -not $CheckOnly) {
        try { Stop-ScheduledTask -TaskName "LanVidGridDSP" -ErrorAction SilentlyContinue } catch {}
        try { & sc.exe stop LanVidGridDSP 2>$null | Out-Null } catch {}
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

# =============================================================================
# 2. Check UDP port 5000 availability
# =============================================================================
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

# =============================================================================
# 3. Check & auto-install Visual C++ 2015-2022 Redistributable (x64)
# =============================================================================
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

# =============================================================================
# 4. Direct3D 11 Hardware Probe
# =============================================================================
$osVer = [System.Environment]::OSVersion.Version
$d3d11Ok = $false
try {
    if (-not ("LvD3D11Probe" -as [type])) {
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
    }
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

# =============================================================================
# 5. Windows Defender Firewall (Domain,Private,Public — UDP 5000-5035, 5099, 5353 + TCP)
# =============================================================================
if ($ConfigureFirewall -and -not $CheckOnly) {
    $appExe = Join-Path $InstallDir "lanvid_app.exe"
    $dspExe = Join-Path $InstallDir "lanvid_dsp_server.exe"
    try {
        $oldRules = @(
            "LanVid Studio (UDP In)", "LanVid Studio (UDP Out)",
            "LanVid Studio (TCP In)", "LanVid Studio (TCP Out)",
            "LanVid Grid DSP Server (UDP In)", "LanVid Grid DSP Server (UDP Out)",
            "LanVid Grid DSP Server (TCP In)", "LanVid Grid DSP Server (TCP Out)",
            "LanVid Monitor & Discovery (UDP Ports)",
            "LanVid Ecosystem (UDP 5000-5035, 5099, 5353)"
        )
        foreach ($r in $oldRules) {
            Remove-NetFirewallRule -DisplayName $r -ErrorAction SilentlyContinue
        }

        if (Test-Path $appExe) {
            New-NetFirewallRule -DisplayName "LanVid Studio (UDP In)" -Direction Inbound -Action Allow `
                -Protocol UDP -Program $appExe -Profile Domain,Private,Public -ErrorAction Stop | Out-Null
            New-NetFirewallRule -DisplayName "LanVid Studio (UDP Out)" -Direction Outbound -Action Allow `
                -Protocol UDP -Program $appExe -Profile Domain,Private,Public -ErrorAction Stop | Out-Null
            New-NetFirewallRule -DisplayName "LanVid Studio (TCP In)" -Direction Inbound -Action Allow `
                -Protocol TCP -Program $appExe -Profile Domain,Private,Public -ErrorAction Stop | Out-Null
            New-NetFirewallRule -DisplayName "LanVid Studio (TCP Out)" -Direction Outbound -Action Allow `
                -Protocol TCP -Program $appExe -Profile Domain,Private,Public -ErrorAction Stop | Out-Null
        }

        if (Test-Path $dspExe) {
            New-NetFirewallRule -DisplayName "LanVid Grid DSP Server (UDP In)" -Direction Inbound -Action Allow `
                -Protocol UDP -Program $dspExe -Profile Domain,Private,Public -ErrorAction Stop | Out-Null
            New-NetFirewallRule -DisplayName "LanVid Grid DSP Server (UDP Out)" -Direction Outbound -Action Allow `
                -Protocol UDP -Program $dspExe -Profile Domain,Private,Public -ErrorAction Stop | Out-Null
            New-NetFirewallRule -DisplayName "LanVid Grid DSP Server (TCP In)" -Direction Inbound -Action Allow `
                -Protocol TCP -Program $dspExe -Profile Domain,Private,Public -ErrorAction Stop | Out-Null
            New-NetFirewallRule -DisplayName "LanVid Grid DSP Server (TCP Out)" -Direction Outbound -Action Allow `
                -Protocol TCP -Program $dspExe -Profile Domain,Private,Public -ErrorAction Stop | Out-Null
        }

        New-NetFirewallRule -DisplayName "LanVid Ecosystem (UDP 5000-5035, 5099, 5353)" -Direction Inbound -Action Allow `
            -Protocol UDP -LocalPort @("5000-5035", "5099", "5353") -Profile Domain,Private,Public -ErrorAction Stop | Out-Null

        $report.FirewallOk = $true
        Write-Host "[PREFLIGHT] Windows Defender Firewall rules configured (Domain/Private/Public, Studio + DSP Server, UDP 5000-5035, 5099, 5353 mDNS + TCP)."
    } catch {
        Write-Host "[PREFLIGHT] Firewall configuration requires Administrator privileges: $_"
    }
}

# =============================================================================
# 6. Policy-based QoS DSCP=46 (EF — Expedited Forwarding)
# =============================================================================
if ($ConfigureQoS -and -not $CheckOnly) {
    try {
        $qosRoot = "HKLM:\SOFTWARE\Policies\Microsoft\Windows\QoS"
        if (-not (Test-Path $qosRoot)) {
            New-Item -Path $qosRoot -Force -ErrorAction Stop | Out-Null
        }
        $policies = @(
            @{ Name = "LanVid Studio"; App = "lanvid_app.exe" },
            @{ Name = "LanVid Grid DSP Server"; App = "lanvid_dsp_server.exe" }
        )
        foreach ($pol in $policies) {
            $pPath = Join-Path $qosRoot $pol.Name
            New-Item -Path $pPath -Force -ErrorAction Stop | Out-Null
            Set-ItemProperty -Path $pPath -Name "Version" -Value "1.0" -Type String
            Set-ItemProperty -Path $pPath -Name "Application Name" -Value $pol.App -Type String
            Set-ItemProperty -Path $pPath -Name "Protocol" -Value "*" -Type String
            Set-ItemProperty -Path $pPath -Name "Local Port" -Value "*" -Type String
            Set-ItemProperty -Path $pPath -Name "Local IP" -Value "*" -Type String
            Set-ItemProperty -Path $pPath -Name "Local IP Prefix Length" -Value "*" -Type String
            Set-ItemProperty -Path $pPath -Name "Remote Port" -Value "*" -Type String
            Set-ItemProperty -Path $pPath -Name "Remote IP" -Value "*" -Type String
            Set-ItemProperty -Path $pPath -Name "Remote IP Prefix Length" -Value "*" -Type String
            Set-ItemProperty -Path $pPath -Name "DSCP Value" -Value "46" -Type String
            Set-ItemProperty -Path $pPath -Name "Throttle Rate" -Value "-1" -Type String
        }
        $report.QosDscpOk = $true
        Write-Host "[PREFLIGHT] Policy-based QoS DSCP=46 (EF) configured for lanvid_app.exe and lanvid_dsp_server.exe."
    } catch {
        Write-Host "[PREFLIGHT] QoS DSCP registry configuration skipped (requires Admin): $_"
    }
}

# =============================================================================
# 7. Socket Buffers, MMCSS Audio Priority, NIC EEE Check & Optional High Performance Power
# =============================================================================
if ($TuneNetwork -and -not $CheckOnly) {
    try {
        # Increase Winsock / AFD default UDP buffers & fast datagram threshold
        $afdKey = "HKLM:\SYSTEM\CurrentControlSet\Services\AFD\Parameters"
        if (Test-Path $afdKey) {
            Set-ItemProperty -Path $afdKey -Name "DefaultReceiveWindow" -Value 1048576 -Type DWord -ErrorAction Stop
            Set-ItemProperty -Path $afdKey -Name "DefaultSendWindow" -Value 1048576 -Type DWord -ErrorAction Stop
            Set-ItemProperty -Path $afdKey -Name "FastSendDatagramThreshold" -Value 1500 -Type DWord -ErrorAction Stop
        }
        # Disable Windows multimedia network throttling for deterministic sub-ms audio
        $mmcssKey = "HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Multimedia\SystemProfile"
        if (Test-Path $mmcssKey) {
            Set-ItemProperty -Path $mmcssKey -Name "NetworkThrottlingIndex" -Value 0xFFFFFFFF -Type DWord -ErrorAction SilentlyContinue
            Set-ItemProperty -Path $mmcssKey -Name "SystemResponsiveness" -Value 0 -Type DWord -ErrorAction SilentlyContinue
        }
        # Diagnose Ethernet EEE / Interrupt Moderation (non-destructive tip so user NIC never drops active RDP/link)
        try {
            $eeeProps = Get-NetAdapterAdvancedProperty -ErrorAction SilentlyContinue |
                Where-Object { $_.RegistryKeyword -match 'EEE|EnergyEfficient|InterruptModeration' -and $_.DisplayValue -match 'Enabled|On' }
            if ($eeeProps) {
                $adapters = ($eeeProps | Select-Object -ExpandProperty Name -Unique) -join ", "
                Write-Host "[PREFLIGHT] TIP: Adapters [$adapters] have EEE or Interrupt Moderation enabled; disable them in Device Manager for < 0.5 ms Grid L2 jitter."
            }
        } catch {}
        $report.NetworkTuned = $true
        Write-Host "[PREFLIGHT] Tuned AFD socket buffers (1 MB) & MMCSS NetworkThrottlingIndex=0xFFFFFFFF."
    } catch {
        Write-Host "[PREFLIGHT] Network buffer tuning skipped (requires Admin): $_"
    }
}

if ($EnableHighPerfPower -and -not $CheckOnly) {
    try {
        & powercfg.exe /setactive 8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c 2>$null | Out-Null
        if ($LASTEXITCODE -eq 0) {
            $report.HighPerfPowerOk = $true
            Write-Host "[PREFLIGHT] Activated Windows High Performance power profile."
        }
    } catch {}
}

# =============================================================================
# 8. Virtual 64-channel ASIO Driver Registration (LanVidASIO.dll)
# =============================================================================
if ($RegisterAsio -and -not $CheckOnly) {
    $asioDll = Join-Path $InstallDir "LanVidASIO.dll"
    if (Test-Path $asioDll) {
        try {
            $proc = Start-Process -FilePath "$env:SystemRoot\System32\regsvr32.exe" `
                -ArgumentList "/s", "`"$asioDll`"" -Wait -PassThru -NoNewWindow
            if ($proc.ExitCode -eq 0) {
                $report.AsioOk = $true
                Write-Host "[PREFLIGHT] Registered 64-channel LanVid Virtual ASIO driver ($asioDll)."
            }
        } catch {
            Write-Host "[PREFLIGHT] ASIO registration failed: $_"
        }
    }
}

# =============================================================================
# 9. Register & Start LanVidGridDSP Background Service / Auto-Restart Task
# =============================================================================
if ($InstallDspService -and -not $CheckOnly) {
    $dspExe = Join-Path $InstallDir "lanvid_dsp_server.exe"
    if (Test-Path $dspExe) {
        try {
            # Remove any stale sc.exe service definition first so Task Scheduler owns LanVidGridDSP cleanly
            & sc.exe stop LanVidGridDSP 2>$null | Out-Null
            & sc.exe delete LanVidGridDSP 2>$null | Out-Null

            $action = New-ScheduledTaskAction -Execute $dspExe -Argument '--name "LanVid Grid DSP"' -WorkingDirectory $InstallDir
            $trigger = New-ScheduledTaskTrigger -AtStartup
            $principal = New-ScheduledTaskPrincipal -UserId "SYSTEM" -LogonType ServiceAccount -RunLevel Highest
            $settings = New-ScheduledTaskSettingsSet `
                -AllowStartIfOnBatteries `
                -DontStopIfGoingOnBatteries `
                -StartWhenAvailable `
                -RestartCount 999 `
                -RestartInterval (New-TimeSpan -Minutes 1) `
                -ExecutionTimeLimit ([TimeSpan]::Zero) `
                -MultipleInstances IgnoreNew

            Register-ScheduledTask -TaskName "LanVidGridDSP" `
                -Description "LanVid Grid 256-channel Layer 2 Distributed DSP Offload & Failover Server" `
                -Action $action -Trigger $trigger -Principal $principal -Settings $settings -Force -ErrorAction Stop | Out-Null

            # Start immediately if Npcap is already present
            if (Test-Path "$env:SystemRoot\System32\Npcap\wpcap.dll") {
                Start-ScheduledTask -TaskName "LanVidGridDSP" -ErrorAction SilentlyContinue
            }
            $report.DspServiceOk = $true
            Write-Host "[PREFLIGHT] Registered LanVidGridDSP background auto-start service (SYSTEM, auto-restart on failure)."
        } catch {
            Write-Host "[PREFLIGHT] LanVidGridDSP registration requires Administrator privileges: $_"
        }
    } else {
        Write-Host "[PREFLIGHT] lanvid_dsp_server.exe not found in $InstallDir; skipping LanVidGridDSP."
    }
}

# =============================================================================
# 10. LanVid Grid Layer 2 Npcap Verification & Auto-Setup
# =============================================================================
$npcapDll = Join-Path $env:SystemRoot "System32\Npcap\wpcap.dll"
$npcapOk = Test-Path $npcapDll
if (-not $npcapOk -and $InstallNpcap -and -not $CheckOnly) {
    Write-Host "[PREFLIGHT] Npcap ($npcapDll) not detected — preparing installation for LanVid Grid L2..."
    $oemCandidates = @(
        (Join-Path $InstallDir "third_party\npcap-oem.exe"),
        (Join-Path $PSScriptRoot "third_party\npcap-oem.exe")
    )
    $oemExe = $oemCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1

    if ($oemExe) {
        Write-Host "[PREFLIGHT] Running silent Npcap OEM installer: $oemExe /winpcap_mode=yes /S"
        $p = Start-Process -FilePath $oemExe -ArgumentList "/winpcap_mode=yes /S" -Wait -PassThru
        if ($p.ExitCode -eq 0) { $npcapOk = Test-Path $npcapDll }
    } elseif (-not $Silent -and [string]::IsNullOrEmpty($env:CI)) {
        # Look for bundled non-OEM Npcap setup or download latest official installer and launch wizard
        $stdExe = Get-ChildItem -Path (Join-Path $InstallDir "third_party"), (Join-Path $PSScriptRoot "third_party") `
            -Filter "npcap-*.exe" -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty FullName
        if (-not $stdExe) {
            $dlPath = Join-Path $env:TEMP "npcap-setup.exe"
            try {
                Invoke-WebRequest -Uri "https://npcap.com/dist/npcap-1.80.exe" -OutFile $dlPath -UseBasicParsing -TimeoutSec 45
                if (Test-Path $dlPath) { $stdExe = $dlPath }
            } catch {
                Write-Host "[PREFLIGHT] Offline or Npcap download skipped (install manually from https://npcap.com for Grid L2): $_"
            }
        }
        if ($stdExe -and (Test-Path $stdExe)) {
            Write-Host "[PREFLIGHT] Launching interactive Npcap Setup Wizard ($stdExe)..."
            Start-Process -FilePath $stdExe -Wait
            $npcapOk = Test-Path $npcapDll
        }
    } else {
        Write-Host "[PREFLIGHT] Silent/CI mode without OEM Npcap — skipping interactive Npcap wizard (Grid L2 requires npcap.com)."
    }
}
if ($npcapOk -and -not $CheckOnly) {
    try {
        & sc.exe start npcap 2>$null | Out-Null
    } catch {}
    Write-Host "[PREFLIGHT] Npcap Layer 2 driver OK ($npcapDll) & npcap service verified."
}
$report.NpcapOk = $npcapOk

# =============================================================================
# 11. Auto-Detect OBS Studio & Install obs-lanvid.dll + Locales (en-US.ini, uk-UA.ini)
# =============================================================================
if ($InstallObsPlugin -and -not $CheckOnly) {
    $obsCandidates = @()
    foreach ($regPath in @("HKLM:\SOFTWARE\OBS Studio", "HKLM:\SOFTWARE\WOW6432Node\OBS Studio")) {
        if (Test-Path $regPath) {
            $val = (Get-ItemProperty -Path $regPath -ErrorAction SilentlyContinue).'(default)'
            if ($val -and (Test-Path $val)) { $obsCandidates += $val }
        }
    }
    foreach ($dir in @("C:\Program Files\obs-studio", "C:\Program Files (x86)\obs-studio")) {
        if (Test-Path $dir) { $obsCandidates += $dir }
    }
    $obsDir = $obsCandidates | Select-Object -Unique | Select-Object -First 1
    if ($obsDir) {
        $report.ObsPath = $obsDir
        $srcDll = @(
            (Join-Path $InstallDir "obs-lanvid\obs-plugins\64bit\obs-lanvid.dll"),
            (Join-Path $InstallDir "obs-lanvid.dll")
        ) | Where-Object { Test-Path $_ } | Select-Object -First 1
        $srcLocale = @(
            (Join-Path $InstallDir "obs-lanvid\data\obs-plugins\obs-lanvid\locale"),
            (Join-Path $PSScriptRoot "..\..\plugins\obs-lanvid\data\locale")
        ) | Where-Object { Test-Path $_ } | Select-Object -First 1

        $dstPluginDir = Join-Path $obsDir "obs-plugins\64bit"
        $dstLocaleDir = Join-Path $obsDir "data\obs-plugins\obs-lanvid\locale"
        try {
            if ($srcDll) {
                New-Item -ItemType Directory -Path $dstPluginDir -Force | Out-Null
                Copy-Item -Path $srcDll -Destination (Join-Path $dstPluginDir "obs-lanvid.dll") -Force
                $report.ObsPluginOk = $true
            }
            if ($srcLocale) {
                New-Item -ItemType Directory -Path $dstLocaleDir -Force | Out-Null
                foreach ($ini in @("en-US.ini", "uk-UA.ini")) {
                    $iniSrc = Join-Path $srcLocale $ini
                    if (Test-Path $iniSrc) {
                        Copy-Item -Path $iniSrc -Destination (Join-Path $dstLocaleDir $ini) -Force
                    }
                }
            }
            Write-Host "[PREFLIGHT] OBS Studio detected at '$obsDir' (obs-lanvid plugin installed=$($report.ObsPluginOk))."
        } catch {
            Write-Host "[PREFLIGHT] Failed to install OBS plugin into '$obsDir': $_"
        }
    } else {
        Write-Host "[PREFLIGHT] OBS Studio not detected on this system (plugin files kept in $InstallDir\obs-lanvid)."
    }
}

$report | ConvertTo-Json -Compress
