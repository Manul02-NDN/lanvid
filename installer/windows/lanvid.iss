; LanVid Studio + LanVid Grid (L2 · 256ch) + DSP Server — Inno Setup 6 Full Ecosystem Installer
; Build with: iscc installer\windows\lanvid.iss
;
; Note: CMake target `lanvid_core_shared` sets OUTPUT_NAME "lanvid_core", producing build\lanvid_core.dll.

#define MyAppName "LanVid Studio"
#define MyAppVersion "2.6.0"
#define MyAppPublisher "LanVid Project"
#define MyAppExeName "lanvid_app.exe"

[Setup]
AppId={{8E74A82C-4D39-4E53-9B3C-6F82D05147B2}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName=LanVid Studio + Grid L2 {#MyAppVersion}
VersionInfoVersion=2.6.0.0
VersionInfoProductVersion=2.6.0.0
AppPublisher={#MyAppPublisher}
DefaultDirName={autopf}\LanVid
DefaultGroupName={#MyAppName}
AllowNoIcons=yes
OutputDir=.
OutputBaseFilename=LanVid_Setup_v{#MyAppVersion}
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
MinVersion=10.0.10240
CloseApplications=yes

[Languages]
Name: "ukrainian"; MessagesFile: "compiler:Languages\Ukrainian.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Types]
Name: "full"; Description: "Повне встановлення (Studio + Grid L2 + DSP Server + Virtual ASIO + OBS)"
Name: "compact"; Description: "Базове встановлення (LanVid Studio + Virtual ASIO)"
Name: "custom"; Description: "Вибіркове встановлення"; Flags: iscustom

[Components]
Name: "core"; Description: "LanVid Studio, ядро (lanvid_core.dll) та утиліти діагностики (обов'язково)"; Types: full compact custom; Flags: fixed
Name: "dsp_server"; Description: "LanVid Grid DSP Server (фонова служба LanVidGridDSP 256ch + Failover)"; Types: full
Name: "virtual_audio"; Description: "Віртуальний аудіодрайвер LanVid Virtual ASIO (64 In / 64 Out)"; Types: full compact
Name: "grid_l2_npcap"; Description: "LanVid Grid Layer 2 (перевірка та встановлення Npcap для прямого Ethernet)"; Types: full
Name: "obs_plugin"; Description: "Плагін OBS Studio (obs-lanvid.dll + локалізації, автовиявлення OBS)"; Types: full

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"
Name: "firewall"; Description: "Налаштувати Windows Defender Firewall (UDP 5000-5035, 5099, 5353 mDNS та TCP для Studio і DSP)"; GroupDescription: "Мережа та продуктивність:"; Flags: checkedonce
Name: "qos_dscp"; Description: "Увімкнути Policy-based QoS DSCP=46 (EF) та оптимізувати буфери сокетів для аудіо/відео"; GroupDescription: "Мережа та продуктивність:"; Flags: checkedonce
Name: "high_perf_power"; Description: "Увімкнути профіль живлення «Висока продуктивність» (High Performance, без засинання NIC)"; GroupDescription: "Мережа та продуктивність:"; Flags: unchecked

[Files]
; Core (Studio GUI, shared core DLL, CLI tools, preflight configurator & docs)
Source: "..\..\build\lanvid_app.exe"; DestDir: "{app}"; Flags: ignoreversion; Components: core
Source: "..\..\build\lanvid_core.dll"; DestDir: "{app}"; Flags: ignoreversion; Components: core
Source: "..\..\build\sender.exe"; DestDir: "{app}"; Flags: ignoreversion; Components: core
Source: "..\..\build\receiver.exe"; DestDir: "{app}"; Flags: ignoreversion; Components: core
Source: "preflight.ps1"; DestDir: "{app}"; Flags: ignoreversion; Components: core
Source: "..\..\README.md"; DestDir: "{app}\docs"; Flags: ignoreversion skipifsourcedoesntexist; Components: core

; Distributed 256-channel Layer 2 DSP Server
Source: "..\..\build\lanvid_dsp_server.exe"; DestDir: "{app}"; Flags: ignoreversion; Components: dsp_server

; 64-channel Virtual ASIO Driver (64-bit COM registration in HKLM\SOFTWARE\ASIO)
Source: "..\..\build\LanVidASIO.dll"; DestDir: "{app}"; Flags: ignoreversion regserver 64bit; Components: virtual_audio

; Optional bundled Npcap OEM / standard installer (if placed in installer\windows\third_party\)
Source: "third_party\npcap-*.exe"; DestDir: "{app}\third_party"; Flags: ignoreversion skipifsourcedoesntexist; Components: grid_l2_npcap

; OBS Studio plugin & locales (staged into {app}\obs-lanvid and auto-copied to detected OBS Studio)
Source: "..\..\build\plugins\obs-lanvid\obs-lanvid.dll"; DestDir: "{app}\obs-lanvid\obs-plugins\64bit"; Flags: ignoreversion skipifsourcedoesntexist; Components: obs_plugin
Source: "..\..\plugins\obs-lanvid\data\locale\en-US.ini"; DestDir: "{app}\obs-lanvid\data\obs-plugins\obs-lanvid\locale"; Flags: ignoreversion skipifsourcedoesntexist; Components: obs_plugin
Source: "..\..\plugins\obs-lanvid\data\locale\uk-UA.ini"; DestDir: "{app}\obs-lanvid\data\obs-plugins\obs-lanvid\locale"; Flags: ignoreversion skipifsourcedoesntexist; Components: obs_plugin

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\LanVid Grid DSP Server (консоль)"; Filename: "{app}\lanvid_dsp_server.exe"; Components: dsp_server
Name: "{group}\Видалити {#MyAppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
; 1. Core preflight (stop conflicting processes, VC++ Redistributable x64, D3D11 check)
Filename: "powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\preflight.ps1"" -InstallDir ""{app}"" -StopProcesses -InstallVCRedist"; StatusMsg: "Перевірка конфліктів, портів, VC++ Redistributable та Direct3D 11..."; Flags: runhidden waituntilterminated; Components: core

; 2. Windows Defender Firewall (Domain,Private,Public — UDP 5000-5035, 5099, 5353 + TCP)
Filename: "powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\preflight.ps1"" -InstallDir ""{app}"" -ConfigureFirewall"; StatusMsg: "Налаштування правил Windows Defender Firewall для Studio та DSP Server..."; Flags: runhidden waituntilterminated; Tasks: firewall

; 3. Policy-based QoS DSCP=46 (EF) & network socket / MMCSS low-latency tuning
Filename: "powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\preflight.ps1"" -InstallDir ""{app}"" -ConfigureQoS -TuneNetwork"; StatusMsg: "Увімкнення QoS DSCP=46 (EF) та оптимізація буферів сокетів..."; Flags: runhidden waituntilterminated; Tasks: qos_dscp

; 4. Optional High Performance power profile
Filename: "powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\preflight.ps1"" -InstallDir ""{app}"" -EnableHighPerfPower"; StatusMsg: "Активація профілю живлення High Performance..."; Flags: runhidden waituntilterminated; Tasks: high_perf_power

; 5. Register & start LanVidGridDSP background service / scheduled task (auto-start + auto-restart on failure)
Filename: "powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\preflight.ps1"" -InstallDir ""{app}"" -InstallDspService"; StatusMsg: "Реєстрація фонової служби LanVidGridDSP (автозапуск + Failover)..."; Flags: runhidden waituntilterminated; Components: dsp_server

; 6. Verify/install Npcap driver for LanVid Grid Layer 2 Ethernet (256ch)
Filename: "powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\preflight.ps1"" -InstallDir ""{app}"" -InstallNpcap"; StatusMsg: "Перевірка та налаштування драйвера Npcap для LanVid Grid L2..."; Flags: runhidden waituntilterminated; Components: grid_l2_npcap

; 7. Auto-detect OBS Studio and install obs-lanvid.dll + locales
Filename: "powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\preflight.ps1"" -InstallDir ""{app}"" -InstallObsPlugin"; StatusMsg: "Автоматичне встановлення плагіна obs-lanvid у OBS Studio..."; Flags: runhidden waituntilterminated; Components: obs_plugin

; 8. Launch LanVid Studio
Filename: "{app}\{#MyAppExeName}"; Description: "Запустити LanVid Studio зараз"; Flags: nowait postinstall skipifsilent

[UninstallRun]
; Cleanly stop & delete LanVidGridDSP, remove Firewall + QoS DSCP rules, and unregister LanVidASIO.dll
Filename: "powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\preflight.ps1"" -InstallDir ""{app}"" -Uninstall"; Flags: runhidden waituntilterminated; RunOnceId: "LanVidPreflightUninstall"
Filename: "sc.exe"; Parameters: "stop LanVidGridDSP"; Flags: runhidden waituntilterminated; RunOnceId: "StopLanVidDspSvc"
Filename: "sc.exe"; Parameters: "delete LanVidGridDSP"; Flags: runhidden waituntilterminated; RunOnceId: "DeleteLanVidDspSvc"
Filename: "{sys}\regsvr32.exe"; Parameters: "/u /s ""{app}\LanVidASIO.dll"""; Flags: runhidden waituntilterminated; RunOnceId: "UnregLanVidASIO"
