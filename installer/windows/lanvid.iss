; LanVid Studio — Inno Setup 6 One-Click Installer Script
; Build with: iscc installer\windows\lanvid.iss

#define MyAppName "LanVid Studio"
#define MyAppVersion "2.6.0"
#define MyAppPublisher "LanVid Project"
#define MyAppExeName "lanvid_app.exe"

[Setup]
AppId={{8E74A82C-4D39-4E53-9B3C-6F82D05147B2}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName=LanVid Studio {#MyAppVersion}
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

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"
Name: "firewall"; Description: "Додати дозволи у Windows Defender Firewall (UDP для локальної мережі)"; GroupDescription: "Мережа:"; Flags: checkedonce

[Files]
Source: "..\..\build\lanvid_app.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\..\build\lanvid_core.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\..\build\sender.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\..\build\receiver.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\..\build\LanVidASIO.dll"; DestDir: "{app}"; Flags: ignoreversion regserver 64bit
Source: "preflight.ps1"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\Видалити {#MyAppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\preflight.ps1"" -InstallDir ""{app}"" -StopProcesses -InstallVCRedist -ConfigureFirewall"; StatusMsg: "Перевірка конфліктів, портів, VC++ Redistributable, Direct3D 11 та Firewall..."; Flags: runhidden waituntilterminated
Filename: "{app}\{#MyAppExeName}"; Description: "Запустити LanVid зараз"; Flags: nowait postinstall skipifsilent

[UninstallRun]
Filename: "powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -Command ""Remove-NetFirewallRule -DisplayName 'LanVid Studio (UDP In)' -ErrorAction SilentlyContinue; Remove-NetFirewallRule -DisplayName 'LanVid Studio (UDP Out)' -ErrorAction SilentlyContinue"""; Flags: runhidden waituntilterminated; RunOnceId: "RemoveLanVidFW"
