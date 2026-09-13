#ifndef PackageDir
  #error PackageDir must point to the verified release staging directory
#endif
#ifndef OutputDir
  #define OutputDir "..\dist"
#endif
#define AppVersion GetStringFileInfo(PackageDir + "\Biomes.exe", "ProductVersion")

[Setup]
; Stable across releases: upgrades retain the same per-user uninstall entry.
AppId={{CE068B3E-38EE-4A60-916A-BFB2BEA3DC96}
AppName=Biomes
AppVersion={#AppVersion}
AppPublisher=Biomes
DefaultDirName={userpf}\Biomes
PrivilegesRequired=lowest
DisableProgramGroupPage=yes
UninstallDisplayIcon={app}\Biomes.exe
SetupIconFile=..\resources\biomes.ico
LicenseFile={#PackageDir}\LICENSE
InfoBeforeFile=InstallerNotes.txt
OutputDir={#OutputDir}
OutputBaseFilename=BiomesSetup-v{#AppVersion}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
MinVersion=10.0
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
CloseApplications=yes
RestartApplications=no
VersionInfoVersion=1.0.0.0

[Tasks]
Name: "desktopicon"; Description: "Create a &Desktop shortcut"; GroupDescription: "Shortcuts:"; Flags: unchecked

[Files]
; PackageDir is generated from the allowlisted release ZIP, never raw build output.
Source: "{#PackageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#PackageDir}\prerequisites\MicrosoftEdgeWebview2Setup.exe"; Flags: dontcopy

[Icons]
Name: "{userprograms}\Biomes"; Filename: "{app}\Biomes.exe"; WorkingDir: "{app}"
Name: "{userdesktop}\Biomes"; Filename: "{app}\Biomes.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\Biomes.exe"; Description: "Launch Biomes"; Flags: nowait postinstall skipifsilent

; No wildcard uninstall deletion and no operations on {localappdata}\biomes.
; Inno removes only installed files, shortcuts, and its per-user uninstall entry.
[Code]
function RuntimeInRegistry(RootKey: Integer): Boolean;
var Version: String;
begin
  Result := RegQueryStringValue(RootKey,
    'Software\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}',
    'pv', Version) and (Version <> '') and (Version <> '0.0.0.0');
end;

function HasWebView2: Boolean;
begin
  Result := RuntimeInRegistry(HKCU32) or RuntimeInRegistry(HKLM32);
  if IsWin64 then
    Result := Result or RuntimeInRegistry(HKCU64) or RuntimeInRegistry(HKLM64);
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var ExitCode: Integer; AppDir, DataDir: String;
begin
  Result := '';
  AppDir := AddBackslash(Lowercase(ExpandConstant('{app}')));
  DataDir := AddBackslash(Lowercase(ExpandConstant('{localappdata}\biomes')));
  if Pos(DataDir, AppDir) = 1 then begin
    Result := 'Choose an application folder outside the Biomes personal-data directory.';
    Exit;
  end;
  if HasWebView2 then Exit;
  ExtractTemporaryFile('MicrosoftEdgeWebview2Setup.exe');
  if not Exec(ExpandConstant('{tmp}\MicrosoftEdgeWebview2Setup.exe'),
      '/silent /install', '', SW_HIDE, ewWaitUntilTerminated, ExitCode) then
    Result := 'Could not start Microsoft WebView2 setup. Please install WebView2 and retry.'
  else if not HasWebView2 then
    Result := 'Microsoft WebView2 could not be installed (code ' + IntToStr(ExitCode) +
      '). Check your internet connection, install WebView2, and retry.';
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var Command: String;
begin
  if CurUninstallStep = usUninstall then begin
    if RegQueryStringValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Run', 'biomes', Command) then
      if CompareText(Command, '"' + ExpandConstant('{app}\Biomes.exe') + '" --autostart') = 0 then
        RegDeleteValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Run', 'biomes');
  end;
end;
