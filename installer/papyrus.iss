; Papyrus setup wizard (Inno Setup 6). Built by .github/workflows/release.yml:
;   iscc /DAppVersion=0.2.2 installer\papyrus.iss
#ifndef AppVersion
  #define AppVersion "dev"
#endif

[Setup]
AppId={{6F0B8E7C-52C4-4B0E-9A77-3C1D2A9E5B41}
AppName=Papyrus
AppVersion={#AppVersion}
AppVerName=Papyrus {#AppVersion}
AppPublisher=Opifex Studios
AppPublisherURL=https://github.com/arkbullet2626-droid/Papyrus
AppSupportURL=https://github.com/arkbullet2626-droid/Papyrus/issues
DefaultDirName={autopf}\Papyrus
DefaultGroupName=Papyrus
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.18362
WizardStyle=modern
Compression=lzma2
SolidCompression=yes
OutputDir=..\dist
OutputBaseFilename=Papyrus-Setup-{#AppVersion}
UninstallDisplayName=Papyrus
UninstallDisplayIcon={app}\papyrus-engine.exe
CloseApplications=yes
InfoBeforeFile=welcome.txt

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Shortcuts:"; Flags: unchecked

[Files]
Source: "..\dist\papyrus-engine.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\README.md"; DestDir: "{app}"; DestName: "README.txt"; Flags: ignoreversion
Source: "..\third_party\nv-codec-headers\LICENSE.txt"; DestDir: "{app}"; DestName: "THIRD-PARTY-LICENSES.txt"; Flags: ignoreversion

[Icons]
Name: "{group}\Papyrus"; Filename: "{app}\papyrus-engine.exe"; Parameters: "--replay --pause"; WorkingDir: "{app}"; Comment: "Start the replay buffer. Ctrl+Shift+F9 saves the last 30 seconds."
Name: "{group}\Papyrus clips folder"; Filename: "{app}\papyrus-engine.exe"; Parameters: "--open-clips"; WorkingDir: "{app}"
Name: "{group}\Papyrus self-test"; Filename: "{app}\papyrus-engine.exe"; Parameters: "--replay --seconds 45 --autosave 40 --pause"; WorkingDir: "{app}"; Comment: "Runs for 45 seconds and saves one clip by itself."
Name: "{group}\Papyrus 4K benchmark"; Filename: "{app}\papyrus-engine.exe"; Parameters: "--bench4k --pause"; WorkingDir: "{app}"
Name: "{group}\Uninstall Papyrus"; Filename: "{uninstallexe}"
Name: "{autodesktop}\Papyrus"; Filename: "{app}\papyrus-engine.exe"; Parameters: "--replay --pause"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\papyrus-engine.exe"; Parameters: "--replay --pause"; Description: "Start Papyrus now"; Flags: postinstall nowait skipifsilent
