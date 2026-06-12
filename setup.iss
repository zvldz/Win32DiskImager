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
Source: "CHANGELOG.md";                    DestDir: "{app}"; Flags: ignoreversion
Source: "GPL-2";                           DestDir: "{app}"; Flags: ignoreversion
Source: "LGPL-2.1";                        DestDir: "{app}"; Flags: ignoreversion
Source: "THIRD_PARTY_LICENSES.md";         DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist

; Translations are now baked into Win32DiskImager.exe via Qt's resource
; system; the old install layout had a {app}\translations\ folder with
; loose .qm files. Sweep it out when upgrading over a pre-2.3.0 install
; so the install dir matches what we ship today.
[InstallDelete]
Type: filesandordirs; Name: "{app}\translations"

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\{cm:ProgramOnTheWeb,{#MyAppName}}"; Filename: "{#MyAppURL}"
Name: "{commondesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Registry]
; Append {app} to the system PATH when the user opts in. BuildCleanPathPlus
; rebuilds the machine PATH dropping any prior occurrences of {app} (older
; releases used a substring check that mis-fired on REG_EXPAND_SZ Path values
; with unexpanded %ProgramFiles%, accreting dozens of duplicate entries
; across reinstalls), then appends a single trailing copy. ChangesEnvironment
; above causes Inno Setup to broadcast WM_SETTINGCHANGE on completion.
Root: HKLM; Subkey: "SYSTEM\CurrentControlSet\Control\Session Manager\Environment"; \
    ValueType: expandsz; ValueName: "Path"; ValueData: "{code:BuildCleanPathPlus|{app}}"; \
    Tasks: addclitopath

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent runascurrentuser

[Code]
// Read the current machine PATH, drop empty segments and every case-insensitive
// match of `Param` (with whitespace trimmed), then append a single `Param` at
// the end. Idempotent across reinstalls.
function BuildCleanPathPlus(Param: string): string;
var
  OrigPath, Segment, Joined, NeedleLower: string;
  i: Integer;
begin
  if not RegQueryStringValue(HKEY_LOCAL_MACHINE,
    'SYSTEM\CurrentControlSet\Control\Session Manager\Environment',
    'Path', OrigPath)
  then begin
    Result := Param;
    exit;
  end;

  NeedleLower := Lowercase(Trim(Param));
  Joined := '';

  // Manual split on ';' — Inno Pascal has no string.Split.
  while Length(OrigPath) > 0 do begin
    i := Pos(';', OrigPath);
    if i = 0 then begin
      Segment := OrigPath;
      OrigPath := '';
    end else begin
      Segment := Copy(OrigPath, 1, i - 1);
      OrigPath := Copy(OrigPath, i + 1, Length(OrigPath));
    end;
    if (Length(Trim(Segment)) > 0)
       and (Lowercase(Trim(Segment)) <> NeedleLower)
    then begin
      if Length(Joined) > 0 then Joined := Joined + ';';
      Joined := Joined + Segment;
    end;
  end;

  if Length(Joined) > 0 then
    Result := Joined + ';' + Param
  else
    Result := Param;
end;
