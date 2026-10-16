; Build only through scripts/build-windows-installer.ps1, from its validated ZIP payload.
; The build wrapper verifies the pinned 6.7.3 compiler binaries and translation.
#ifndef AppVersion
  #error AppVersion is required.
#endif
#ifndef NumericVersion
  #error NumericVersion is required.
#endif
#ifndef PayloadDirectory
  #error PayloadDirectory is required.
#endif
#ifndef FileEntries
  #error FileEntries is required.
#endif
#ifndef AppArchitecture
  #error AppArchitecture is required.
#endif
#ifndef AppIcon
  #error AppIcon is required.
#endif
#if AppArchitecture == "x64"
  #define AllowedArchitecture "x64compatible"
#elif AppArchitecture == "arm64"
  #define AllowedArchitecture "arm64"
#else
  #error AppArchitecture must be x64 or arm64.
#endif

[Setup]
AppId={{6AFB2AC5-3180-45B7-BD57-1485C19F47C3}
AppName=GlideBeam
AppVersion={#AppVersion}
AppPublisher=dixtdf
AppPublisherURL=https://github.com/dixtdf/GlideBeam
AppSupportURL=https://github.com/dixtdf/GlideBeam/issues
AppUpdatesURL=https://github.com/dixtdf/GlideBeam/releases
VersionInfoVersion={#NumericVersion}
VersionInfoDescription=GlideBeam Setup ({#AppArchitecture})
DefaultDirName={localappdata}\Programs\GlideBeam
DefaultGroupName=GlideBeam
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed={#AllowedArchitecture}
ArchitecturesInstallIn64BitMode={#AllowedArchitecture}
MinVersion=10.0
OutputBaseFilename=GlideBeam-{#AppVersion}-Windows-{#AppArchitecture}-Setup
SetupIconFile={#AppIcon}
UninstallDisplayIcon={app}\GlideBeam.exe
UninstallDisplayName=GlideBeam
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UsePreviousAppDir=yes
UsePreviousGroup=yes
UsePreviousTasks=yes
CloseApplications=yes
CloseApplicationsFilter=GlideBeam.exe,remote_transport.dll,libwinpthread-1.dll,libunwind.dll
RestartApplications=no
RestartIfNeededByRun=no
UninstallLogMode=append
UninstallFilesDir={app}

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "chinesesimplified"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
#include FileEntries

; Only the other architecture's known, application-owned runtime can become stale.
; No wildcard delete is used; settings and unknown files are never removed.
[InstallDelete]
#if AppArchitecture == "x64"
Type: files; Name: "{app}\libunwind.dll"
#else
Type: files; Name: "{app}\libwinpthread-1.dll"
#endif

[Icons]
Name: "{group}\GlideBeam"; Filename: "{app}\GlideBeam.exe"; Parameters: "--connect"; WorkingDir: "{app}"
Name: "{group}\Uninstall GlideBeam"; Filename: "{uninstallexe}"
Name: "{autodesktop}\GlideBeam"; Filename: "{app}\GlideBeam.exe"; Parameters: "--connect"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\GlideBeam.exe"; Parameters: "--connect"; WorkingDir: "{app}"; Description: "{cm:LaunchGlideBeam}"; Flags: nowait postinstall skipifsilent unchecked

[CustomMessages]
english.LaunchGlideBeam=Launch GlideBeam
chinesesimplified.LaunchGlideBeam=启动光梭 GlideBeam
