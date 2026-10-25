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

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "russian"; MessagesFile: "compiler:Languages\Russian.isl"

[CustomMessages]
english.AutostartTask=Start FastLook with Windows
russian.AutostartTask=Запускать FastLook вместе с Windows
english.StillRunning=FastLook is still running and could not be stopped. Exit it from the tray icon and run Setup again.
russian.StillRunning=FastLook всё ещё работает и не удалось его остановить. Закройте его через значок в трее и запустите установку снова.
english.DeleteData=Delete FastLook settings and logs?
russian.DeleteData=Удалить настройки и журналы FastLook?

[Tasks]
Name: "autostart"; Description: "{cm:AutostartTask}"

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: recursesubdirs ignoreversion

[Icons]
Name: "{userprograms}\FastLook"; Filename: "{app}\FastLook.exe"

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "FastLook"; ValueData: """{app}\FastLook.exe"""; Tasks: autostart; Flags: uninsdeletevalue

[Run]
Filename: "{app}\FastLook.exe"; Description: "{cm:LaunchProgram,FastLook}"; Flags: nowait postinstall

[Code]
// FastLook.exe --shutdown: exit 0 once the daemon (and its Viewer) is gone, 1 if still alive after 5 s.
function StopFastLook(const ExeDir: String): Integer;
var
  Exe: String;
  Code: Integer;
begin
  Result := 0;
  Exe := ExeDir + '\FastLook.exe';
  if FileExists(Exe) then
  begin
    if Exec(Exe, '--shutdown', '', SW_HIDE, ewWaitUntilTerminated, Code) then
      Result := Code
    else
      Result := 1;
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := '';
  if StopFastLook(ExpandConstant('{app}')) <> 0 then
    Result := CustomMessage('StillRunning');
end;

function InitializeUninstall(): Boolean;
begin
  Result := True;
  if StopFastLook(ExpandConstant('{app}')) <> 0 then
  begin
    if not UninstallSilent then
      MsgBox(CustomMessage('StillRunning'), mbError, MB_OK);
    Result := False;
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usPostUninstall then
  begin
    // Autostart may have been enabled later from Settings; uninsdeletevalue only covers the installer's own entry.
    RegDeleteValue(HKEY_CURRENT_USER, 'Software\Microsoft\Windows\CurrentVersion\Run', 'FastLook');
    if not UninstallSilent then
      if MsgBox(CustomMessage('DeleteData'), mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES then
        DelTree(ExpandConstant('{localappdata}\FastLook'), True, True, True);
  end;
end;
