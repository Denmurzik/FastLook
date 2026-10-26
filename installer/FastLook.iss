; FastLook installer. Build: ISCC /DAppVersion=<x.y.z> /DStageDir=<abs path to out\stage\FastLook> installer\FastLook.iss
; (scripts/package.ps1 does this.) Per-user install, no elevation.

#ifndef AppVersion
  #error Pass /DAppVersion=<x.y.z>
#endif
#ifndef StageDir
  #error Pass /DStageDir=<staging folder>
#endif

[Setup]
AppId={{6F1C2A9E-3B4D-4E8F-A1C7-5D2E9B0F4A63}
AppName=FastLook
AppVersion={#AppVersion}
AppPublisher=Denmurzik
AppPublisherURL=https://github.com/Denmurzik/FastLook
AppSupportURL=https://github.com/Denmurzik/FastLook/issues
AppUpdatesURL=https://github.com/Denmurzik/FastLook/releases
VersionInfoVersion={#AppVersion}
PrivilegesRequired=lowest
DefaultDirName={localappdata}\Programs\FastLook
; The install path is fixed by the spec; a hand-picked existing folder would be exposed to [InstallDelete].
DisableDirPage=yes
DisableProgramGroupPage=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.22621
OutputBaseFilename=FastLook-Setup-{#AppVersion}
SetupIconFile=..\assets\FastLook.ico
UninstallDisplayIcon={app}\FastLook.exe
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
CloseApplications=no
; Silent updates (the in-app updater) run this [Code] unattended; the log (%TEMP%\Setup Log ....txt) is the only diagnostics.
SetupLogging=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "russian"; MessagesFile: "compiler:Languages\Russian.isl"

[CustomMessages]
english.AutostartTask=Start FastLook with Windows
russian.AutostartTask=Запускать FastLook вместе с Windows
english.StillRunning=FastLook is still running and could not be stopped. Exit it from the tray icon (or end FastLook.exe in Task Manager) and click Retry.
russian.StillRunning=FastLook всё ещё работает, и его не удалось остановить. Закройте его через значок в трее (или завершите FastLook.exe в диспетчере задач) и нажмите «Повторить».
english.DeleteData=Delete FastLook settings and logs?
russian.DeleteData=Удалить настройки и журналы FastLook?

[Tasks]
Name: "autostart"; Description: "{cm:AutostartTask}"

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: recursesubdirs ignoreversion

[InstallDelete]
; Runtime files of the previous version that a newer stage may no longer ship (stale DLLs). Upgrades only (stale files exist only
; then). Top level only: the uninstaller (unins*.exe) and subfolders are left alone; the installed layout keeps no user data in {app}.
Type: files; Name: "{app}\*.dll"; Check: IsUpgradeInstall
Type: files; Name: "{app}\*.pri"; Check: IsUpgradeInstall
Type: files; Name: "{app}\*.xbf"; Check: IsUpgradeInstall
Type: files; Name: "{app}\*.winmd"; Check: IsUpgradeInstall
Type: files; Name: "{app}\*.txt"; Check: IsUpgradeInstall

[Icons]
Name: "{userprograms}\FastLook"; Filename: "{app}\FastLook.exe"

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "FastLook"; ValueData: """{app}\FastLook.exe"""; Tasks: autostart; Check: ShouldWriteAutostart; Flags: uninsdeletevalue

[Run]
Filename: "{app}\FastLook.exe"; Description: "{cm:LaunchProgram,FastLook}"; Flags: nowait postinstall

[Code]
const
  RunKey = 'Software\Microsoft\Windows\CurrentVersion\Run';
  UninstallKey = 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{6F1C2A9E-3B4D-4E8F-A1C7-5D2E9B0F4A63}_is1';

var
  IsUpgrade: Boolean;
  AutostartPreset: Boolean;

// Decided once at start, before Inno writes this run's own uninstall key.
function InitializeSetup(): Boolean;
begin
  IsUpgrade := RegKeyExists(HKEY_CURRENT_USER, UninstallKey);
  Result := True;
end;

function IsUpgradeInstall: Boolean;
begin
  Result := IsUpgrade;
end;

// The Run value of THIS install ({app}\FastLook.exe, quoted as the app writes it), not a portable copy's.
function RunValueIsOurs: Boolean;
var
  Data: String;
begin
  Result := RegQueryStringValue(HKEY_CURRENT_USER, RunKey, 'FastLook', Data) and
    (CompareText(Data, '"' + ExpandConstant('{app}\FastLook.exe') + '"') = 0);
end;

// Fresh install and interactive upgrade: honour the autostart task (preset to the current state on upgrades, see CurPageChanged).
// Silent upgrade (the updater): keep the user's current state, i.e. never re-create a Run value that was removed in Settings.
function ShouldWriteAutostart: Boolean;
begin
  if WizardSilent and IsUpgrade then
    Result := RegValueExists(HKEY_CURRENT_USER, RunKey, 'FastLook')
  else
    Result := True;
end;

// Interactive upgrade: show the task as the current state (UsePreviousTasks would show the previous install's choice).
procedure CurPageChanged(CurPageID: Integer);
begin
  if (CurPageID = wpSelectTasks) and IsUpgrade and not AutostartPreset then
  begin
    AutostartPreset := True;
    if RegValueExists(HKEY_CURRENT_USER, RunKey, 'FastLook') then
      WizardSelectTasks('autostart')
    else
      WizardSelectTasks('!autostart');
  end;
end;

// FastLook.exe --shutdown: exit 0 once the daemon (and its Viewer) is gone, 1 if still alive after 5 s.
function TryShutdown(const Exe: String): Integer;
var
  Code: Integer;
begin
  if Exec(Exe, '--shutdown', '', SW_HIDE, ewWaitUntilTerminated, Code) then
    Result := Code
  else
    Result := 1;
end;

// True when FastLook is gone (or was never installed here). One retry; then silent runs kill the processes of the current
// user (a per-user installer cannot touch other users' processes anyway), interactive runs offer Retry / Cancel.
function StopFastLook(const ExeDir: String; Silent: Boolean): Boolean;
var
  Exe, Dir: String;
  Code: Integer;
begin
  Result := True;
  Exe := ExeDir + '\FastLook.exe';
  if not FileExists(Exe) then
    Exit;
  while True do
  begin
    if (TryShutdown(Exe) = 0) or (TryShutdown(Exe) = 0) then
      Exit;
    if Silent then
    begin
      // Only processes started from this install folder (a portable copy elsewhere is left alone).
      Dir := ExeDir;
      StringChangeEx(Dir, '''', '''''', True);
      Exec(ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe'),
        '-NoProfile -NonInteractive -Command "$d = ''' + Dir + '\''; Get-Process | Where-Object { $_.ProcessName -like ''FastLook*'' ' +
        '-and $_.Path -and $_.Path.StartsWith($d, [StringComparison]::OrdinalIgnoreCase) } | Stop-Process -Force"',
        '', SW_HIDE, ewWaitUntilTerminated, Code);
      Sleep(1000);
      Exit;
    end;
    if MsgBox(CustomMessage('StillRunning'), mbError, MB_RETRYCANCEL) <> IDRETRY then
    begin
      Result := False;
      Exit;
    end;
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := '';
  // Abort does not stop Setup here; a non-empty result stops it on the Preparing to Install page with this text.
  if not StopFastLook(ExpandConstant('{app}'), WizardSilent) then
    Result := CustomMessage('StillRunning');
end;

function InitializeUninstall(): Boolean;
begin
  Result := StopFastLook(ExpandConstant('{app}'), UninstallSilent);
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usPostUninstall then
  begin
    // Autostart may have been enabled later from Settings; uninsdeletevalue only covers the installer's own entry.
    // Only if it points at this install: a portable copy keeps its own autostart.
    if RunValueIsOurs then
      RegDeleteValue(HKEY_CURRENT_USER, RunKey, 'FastLook');
    // Installer downloaded by the in-app updater.
    DelTree(ExpandConstant('{%TEMP}\FastLook-Update'), True, True, True);
    if not UninstallSilent then
      if MsgBox(CustomMessage('DeleteData'), mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES then
        DelTree(ExpandConstant('{localappdata}\FastLook'), True, True, True);
  end;
end;
