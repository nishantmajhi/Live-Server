; -----------------------------------------------------------------------
; Live Server - Inno Setup installer script
;
; Fully automated installer:
;   - Lets the user pick the install location (standard wizard page)
;   - Asks for a default port on its own wizard page (no regedit needed;
;     the value is written straight into the registry command as an
;     argv[2] passed to liveserver.exe)
;   - Adds the install folder to the system PATH, so "liveserver" works
;     from any terminal
;   - Registers "Start Live Server Here" on:
;       * right-click on a folder            (Directory\shell)
;       * right-click in a folder's empty    (Directory\Background\shell)
;         space (background)
;   - Ships a clean uninstaller that removes the files and registry keys
;   - Uses app_icon.ico as the setup program's own icon. liveserver.exe
;     already has this icon baked in via liveserver.rc at build time, so
;     the context menu entries (which point at "{app}\liveserver.exe,0")
;     pick it up automatically without any extra work here.
;
; Build with Inno Setup 6 (iscc.exe), e.g.:
;   iscc.exe installer.iss
;
; Expects liveserver.exe to already exist next to this script (the
; GitHub Actions workflow builds it first).
; -----------------------------------------------------------------------

#define MyAppName "Live Server"
#ifndef MyAppVersion
  #define MyAppVersion "1.0.0"
#endif
#define MyAppPublisher "Live Server"
#define MyAppExeName "liveserver.exe"

[Setup]
AppId={{B9F1B6C2-6E1E-4C6B-9B9C-7A1C1E2B9B31}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
DisableWelcomePage=no
OutputDir=Output
OutputBaseFilename=LiveServerSetup
Compression=lzma
SolidCompression=yes
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
WizardStyle=modern
UninstallDisplayIcon={app}\{#MyAppExeName}
SetupIconFile=app_icon.ico
ChangesAssociations=no

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Files]
Source: "liveserver.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion isreadme

[Registry]
; Right-click ON a folder (Explorer passes the clicked folder as %1)
Root: HKCR; Subkey: "Directory\shell\StartLiveServer"; ValueType: string; ValueName: ""; ValueData: "Start Live Server Here"; Flags: uninsdeletekey
Root: HKCR; Subkey: "Directory\shell\StartLiveServer"; ValueType: string; ValueName: "Icon"; ValueData: "{app}\{#MyAppExeName},0"
Root: HKCR; Subkey: "Directory\shell\StartLiveServer\command"; ValueType: string; ValueName: ""; ValueData: "{code:GetServerCommand|1}"; Flags: uninsdeletekey

; Right-click INSIDE a folder's empty space (Explorer passes the open
; folder as %V)
Root: HKCR; Subkey: "Directory\Background\shell\StartLiveServer"; ValueType: string; ValueName: ""; ValueData: "Start Live Server Here"; Flags: uninsdeletekey
Root: HKCR; Subkey: "Directory\Background\shell\StartLiveServer"; ValueType: string; ValueName: "Icon"; ValueData: "{app}\{#MyAppExeName},0"
Root: HKCR; Subkey: "Directory\Background\shell\StartLiveServer\command"; ValueType: string; ValueName: ""; ValueData: "{code:GetServerCommand|V}"; Flags: uninsdeletekey

[UninstallDelete]
Type: filesandordirs; Name: "{app}"

[Code]
var
  PortPage: TInputQueryWizardPage;

procedure InitializeWizard;
begin
  PortPage := CreateInputQueryPage(wpSelectDir,
    'Default Port', 'Choose the default port Live Server should use',
    'Live Server will try this port first whenever it''s launched from the ' +
    'right-click menu (if it''s busy, it automatically tries the next few ' +
    'ports). You can change your mind later by re-running this installer.');
  PortPage.Add('Port:', False);
  PortPage.Values[0] := '5500';
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  PortNum: Integer;
begin
  Result := True;
  if CurPageID = PortPage.ID then
  begin
    PortNum := StrToIntDef(Trim(PortPage.Values[0]), -1);
    if (PortNum < 1) or (PortNum > 65535) then
    begin
      MsgBox('Please enter a valid port number between 1 and 65535.', mbError, MB_OK);
      Result := False;
    end;
  end;
end;

{ Builds: "C:\install\path\liveserver.exe" "%<Placeholder>" "<port>" }
function GetServerCommand(Placeholder: String): String;
var
  Port: String;
begin
  Port := Trim(PortPage.Values[0]);
  if Port = '' then
    Port := '5500';
  Result := '"' + ExpandConstant('{app}') + '\{#MyAppExeName}" "%' + Placeholder + '" "' + Port + '"';
end;

{ ------------------------------------------------------------------- }
{ Add/remove the install folder from the machine-wide PATH, so        }
{ "liveserver" works from any terminal without typing the full path.  }
{ ------------------------------------------------------------------- }
const
  EnvironmentKey = 'SYSTEM\CurrentControlSet\Control\Session Manager\Environment';
  MY_HWND_BROADCAST = $FFFF;
  WM_SETTINGCHANGE = $001A;
  SMTO_ABORTIFHUNG = $0002;

function SendMessageTimeoutA(hWnd: Longint; Msg: Longint; wParam: Longint;
  lParam: String; fuFlags: Longint; uTimeout: Longint; var lpdwResult: Longint): Longint;
  external 'SendMessageTimeoutA@user32.dll stdcall';

procedure BroadcastEnvironmentChange;
var
  ResultCode: Longint;
begin
  SendMessageTimeoutA(MY_HWND_BROADCAST, WM_SETTINGCHANGE, 0, 'Environment',
    SMTO_ABORTIFHUNG, 5000, ResultCode);
end;

procedure EnvAddPath(Path: string);
var
  Paths: string;
begin
  if not RegQueryStringValue(HKEY_LOCAL_MACHINE, EnvironmentKey, 'Path', Paths) then
    Paths := '';

  { Already present? Nothing to do. }
  if Pos(';' + Uppercase(Path) + ';', ';' + Uppercase(Paths) + ';') > 0 then
    exit;

  if (Length(Paths) > 0) and (Paths[Length(Paths)] <> ';') then
    Paths := Paths + ';';
  Paths := Paths + Path + ';';

  if not RegWriteExpandStringValue(HKEY_LOCAL_MACHINE, EnvironmentKey, 'Path', Paths) then
    MsgBox('Could not add ' + Path + ' to the system PATH. You can add it manually later.', mbError, MB_OK);
end;

procedure EnvRemovePath(Path: string);
var
  Paths: string;
  P: Integer;
begin
  if not RegQueryStringValue(HKEY_LOCAL_MACHINE, EnvironmentKey, 'Path', Paths) then
    exit;

  P := Pos(';' + Uppercase(Path) + ';', ';' + Uppercase(Paths) + ';');
  if P = 0 then
    exit;

  Delete(Paths, P - 1, Length(Path) + 1);
  RegWriteExpandStringValue(HKEY_LOCAL_MACHINE, EnvironmentKey, 'Path', Paths);
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
  begin
    EnvAddPath(ExpandConstant('{app}'));
    BroadcastEnvironmentChange;
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usPostUninstall then
  begin
    EnvRemovePath(ExpandConstant('{app}'));
    BroadcastEnvironmentChange;
  end;
end;
