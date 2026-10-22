; SPAZZbs Windows installer (Inno Setup 6)
; Built by .github/workflows/spazzbs-windows.yaml from the packaged build.
; Pass /DSourceDir=<unzipped build> /DOutputDir=<where to write> on the ISCC command line.

#define AppName "SPAZZbs"
#define AppExe "SPAZZbs.exe"

#ifndef SourceDir
  #error SourceDir must be defined
#endif
#ifndef OutputDir
  #define OutputDir "."
#endif
#ifndef AppVersion
  #define AppVersion "1.0"
#endif

[Setup]
AppId={{6E2D3A9C-5B1F-4C7E-9A0D-2F8B7C4E1A55}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher=SPAZZbs
AppComments=SPAZZbs is a fork of OBS Studio (GPLv2).
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
; Installs just for you by default, so no admin prompt is needed
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir={#OutputDir}
OutputBaseFilename=SPAZZbs-Setup
SetupIconFile=..\..\frontend\cmake\windows\spazzbs.ico
UninstallDisplayIcon={app}\bin\64bit\{#AppExe}
LicenseFile=..\..\COPYING
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
CloseApplications=yes
; SPAZZbs holds this while running (see CheckIfAlreadyRunning), so setup can
; ask you to close it first
AppMutex=SPAZZbsCore

[Tasks]
Name: "desktopicon"; Description: "Put a SPAZZbs icon on my desktop"; GroupDescription: "Shortcuts:"

[InstallDelete]
; Clear the previous version's program files before copying the new ones, so
; a plugin or library dropped in a newer build can't linger and crash it.
; Your settings, scenes and playlist live in %APPDATA%\spazzbs and are kept.
; Only folders SPAZZbs ships itself: plugins you added under obs-plugins or
; data\obs-plugins are left alone.
Type: filesandordirs; Name: "{app}\bin"
Type: filesandordirs; Name: "{app}\core"
Type: filesandordirs; Name: "{app}\data\libobs"
Type: filesandordirs; Name: "{app}\data\obs-scripting"
Type: filesandordirs; Name: "{app}\data\obs-studio"

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
; SPAZZbs must start from its bin\64bit folder to find its data files
Name: "{group}\{#AppName}"; Filename: "{app}\bin\64bit\{#AppExe}"; WorkingDir: "{app}\bin\64bit"
Name: "{group}\Uninstall {#AppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\bin\64bit\{#AppExe}"; WorkingDir: "{app}\bin\64bit"; Tasks: desktopicon

[Run]
Filename: "{app}\bin\64bit\{#AppExe}"; WorkingDir: "{app}\bin\64bit"; Description: "Open SPAZZbs now"; Flags: nowait postinstall skipifsilent
