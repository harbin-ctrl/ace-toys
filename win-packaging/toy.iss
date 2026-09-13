; Per-user installer for one toy, on ARM64 or x64 Windows; it installs the
; build that matches. Built by the toy's `make inno` (installer.mk), which
; defines:
;
;   Toy         program and folder name     poingo
;   Name        Start menu name             Poingo
;   AppVersion  from debian/changelog       0.1
;   Stage       holds arm64\ and x64\, each the program and its DLLs
;   Icon        the toy's .ico
;
;   %LOCALAPPDATA%\Programs\Ace\<Toy>\   the program and its runtime DLLs
;   Start menu\Programs\Ace\<Name>.lnk
;
; /DForceArch=arm64 or x64 installs that build on any machine; for testing.

#define AppExe Toy + ".exe"

[Setup]
AppId=harbin-ctrl.ace-toys.{#Toy}
AppName={#Name}
AppVersion={#AppVersion}
AppPublisher=harbin-ctrl
AppPublisherURL=https://github.com/harbin-ctrl/ace-toys
DefaultDirName={autopf}\Ace\{#Toy}
DisableDirPage=yes
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible or arm64
ArchitecturesInstallIn64BitMode=x64compatible or arm64
SetupIconFile={#Icon}
UninstallDisplayIcon={app}\{#AppExe}
UninstallDisplayName={#Name}
WizardStyle=modern
Compression=lzma2
SolidCompression=yes
; An upgrade replaces the running program.
CloseApplications=force
RestartApplications=no

[Files]
Source: "{#Stage}\arm64\*"; DestDir: "{app}"; Check: InstallArm64; Flags: ignoreversion
Source: "{#Stage}\x64\*"; DestDir: "{app}"; Check: not InstallArm64; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\Ace\{#Name}"; Filename: "{app}\{#AppExe}"

[Run]
Filename: "{app}\{#AppExe}"; Description: "Launch {#Name}"; Flags: nowait postinstall

[UninstallRun]
Filename: "{sys}\taskkill.exe"; Parameters: "/F /IM {#AppExe}"; Flags: runhidden; RunOnceId: "StopApp"

[Code]
function InstallArm64: Boolean;
begin
#ifdef ForceArch
  Result := '{#ForceArch}' = 'arm64';
#else
  Result := IsArm64;
#endif
end;
