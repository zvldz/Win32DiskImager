; Inno Setup script for Win32 Disk Imager (GUI + CLI).
; Expects a Release/ directory next to this file (produced by the build
; workflow). Static GUI build bundles Qt into the exe, so no DLLs or plugins
; are copied here. Translations stay on disk so QTranslator::load finds them.

#define MyAppSourceDir AddBackslash(SourcePath) + "Release"
#define MyAppExeName "Win32DiskImager.exe"
#define MyAppCliExeName "Win32DiskImager-cli.exe"
#define MyAppExeFile AddBackslash(MyAppSourceDir) + MyAppExeName
#define MyAppName GetStringFileInfo(MyAppExeFile,ORIGINAL_FILENAME)
#define MyAppVersion GetStringFileInfo(MyAppExeFile,PRODUCT_VERSION)
#define MyAppPublisher "ImageWriter Developers"
#define MyAppURL "https://github.com/zvldz/Win32DiskImager"


[Setup]
AppId={{3DFFA293-DF2C-4B23-92E5-3433BDC310E1}}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
DefaultDirName={autopf}\Win32DiskImager
DefaultGroupName=Win32DiskImager
LicenseFile=License.txt
OutputBaseFilename={#MyAppName}-setup-{#MyAppVersion}
SetupIconFile=src\images\setup.ico
Compression=lzma2
SolidCompression=yes
PrivilegesRequired=admin
ArchitecturesInstallIn64BitMode=x64compatible
ArchitecturesAllowed=x64compatible
ChangesEnvironment=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "addclitopath"; Description: "Add Win32DiskImager-cli to system PATH"; GroupDescription: "Command-line tool:"; Flags: unchecked

[Files]
Source: "Release\Win32DiskImager.exe";     DestDir: "{app}"; Flags: ignoreversion
Source: "Release\Win32DiskImager-cli.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "Release\translations\*.qm";       DestDir: "{app}\translations"; Flags: ignoreversion skipifsourcedoesntexist
Source: "Changelog.txt";                   DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "GPL-2";                           DestDir: "{app}"; Flags: ignoreversion
Source: "LGPL-2.1";                        DestDir: "{app}"; Flags: ignoreversion
Source: "README.txt";                      DestDir: "{app}"; Flags: ignoreversion isreadme skipifsourcedoesntexist

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\{cm:ProgramOnTheWeb,{#MyAppName}}"; Filename: "{#MyAppURL}"
Name: "{commondesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Registry]
; Append {app} to the system PATH when the user opts in. The value is written
; to the machine-wide environment so new shells pick it up; ChangesEnvironment
; above causes Inno Setup to broadcast WM_SETTINGCHANGE on completion.
Root: HKLM; Subkey: "SYSTEM\CurrentControlSet\Control\Session Manager\Environment"; \
    ValueType: expandsz; ValueName: "Path"; ValueData: "{olddata};{app}"; \
    Tasks: addclitopath; Check: NeedsAddPath('{app}')

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent runascurrentuser

[Code]
function NeedsAddPath(Param: string): boolean;
var
  OrigPath: string;
begin
  if not RegQueryStringValue(HKEY_LOCAL_MACHINE,
    'SYSTEM\CurrentControlSet\Control\Session Manager\Environment',
    'Path', OrigPath)
  then begin
    Result := True;
    exit;
  end;
  // case-insensitive substring match, surrounded by ';' to avoid false hits
  Result := Pos(';' + Lowercase(Param) + ';', ';' + Lowercase(OrigPath) + ';') = 0;
end;
