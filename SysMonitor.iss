; SysMonitor Inno Setup Script
#define MyAppName "SysMonitor"
#define MyAppVersion "1.0.1"
#define MyAppPublisher "Hua-Teng Huang"
#define MyAppURL "https://github.com"
#define MyAppExeName "SysMonitor.exe"
#define MyAppTaskName "SysMonitor"
#define MyAppId "A1B2C3D4-E5F6-7890-ABCD-EF1234567890"

[Setup]
AppId={{A1B2C3D4-E5F6-7890-ABCD-EF1234567890}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
AllowNoIcons=yes
DisableProgramGroupPage=yes
OutputDir=installer
OutputBaseFilename=SysMonitor_Setup_{#MyAppVersion}
SetupIconFile=assets\sysmonitor.ico
UninstallDisplayIcon={app}\{#MyAppExeName}
CloseApplications=yes
CloseApplicationsFilter={#MyAppExeName}
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
; FPS (ETW) 需要管理員權限

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "x64\Release\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion
Source: "assets\THIRD_PARTY_NOTICES.txt"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{sys}\schtasks.exe"; Parameters: "/Create /TN ""{#MyAppTaskName}"" /SC ONLOGON /TR ""\""{app}\{#MyAppExeName}\"" --tray"" /RL HIGHEST /F"; Flags: runhidden waituntilterminated
Filename: "{sys}\schtasks.exe"; Parameters: "/Run /TN ""{#MyAppTaskName}"""; Description: "即將啟動 SysMonitor（背景常駐工具列）"; StatusMsg: "正在啟動 SysMonitor..."; Flags: runhidden waituntilterminated postinstall skipifsilent

[UninstallRun]
Filename: "{sys}\schtasks.exe"; Parameters: "/Delete /TN ""{#MyAppTaskName}"" /F"; Flags: runhidden waituntilterminated; RunOnceId: "DeleteStartupTask"
Filename: "{sys}\taskkill.exe"; Parameters: "/IM ""{#MyAppExeName}"" /F"; Flags: runhidden waituntilterminated; RunOnceId: "StopRunningApp"

[UninstallDelete]
Type: files; Name: "{app}\THIRD_PARTY_NOTICES.txt"

[Code]
function TryReadInstalledVersion(RootKey: Integer; var Version: String): Boolean;
var
  Key: String;
begin
  Key := 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{' + '{#MyAppId}' + '}_is1';
  Result := RegQueryStringValue(RootKey, Key, 'DisplayVersion', Version);
end;

function GetInstalledVersion(var Version: String): Boolean;
begin
  Result := False;
  if IsWin64 then
    Result := TryReadInstalledVersion(HKLM64, Version);
  if not Result then
    Result := TryReadInstalledVersion(HKLM, Version);
  if not Result then
    Result := TryReadInstalledVersion(HKCU, Version);
end;

function InitializeSetup(): Boolean;
var
  Version: String;
begin
  Result := True;
  if GetInstalledVersion(Version) then begin
    MsgBox(
      '偵測到已安裝的 SysMonitor ' + Version + '.' + #13#10#13#10 +
      '接下來會升級到 SysMonitor {#MyAppVersion}。' + #13#10 +
      '安裝完成後會啟動背景工具列程式。',
      mbInformation,
      MB_OK);
  end;
end;
