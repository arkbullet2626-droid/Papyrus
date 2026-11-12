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
AppMutex=PapyrusControllerMutex,PapyrusAppMutex
InfoBeforeFile=welcome.txt

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Shortcuts:"; Flags: unchecked

[Files]
Source: "..\dist\papyrus-engine.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\dist\app\*"; DestDir: "{app}\app"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "..\README.md"; DestDir: "{app}"; DestName: "README.txt"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion
Source: "..\third_party\nv-codec-headers\LICENSE.txt"; DestDir: "{app}"; DestName: "THIRD-PARTY-LICENSES.txt"; Flags: ignoreversion

[Icons]
Name: "{group}\Papyrus"; Filename: "{app}\app\Papyrus.exe"; WorkingDir: "{app}\app"; Comment: "Your clips, the editor and the recorder settings."
Name: "{group}\Papyrus clips folder"; Filename: "{app}\papyrus-engine.exe"; Parameters: "--open-clips"; WorkingDir: "{app}"
Name: "{group}\Papyrus self-test"; Filename: "{app}\papyrus-engine.exe"; Parameters: "--replay --seconds 45 --autosave 40 --pause"; WorkingDir: "{app}"; Comment: "Runs for 45 seconds and saves one clip by itself."
Name: "{group}\Papyrus 4K benchmark"; Filename: "{app}\papyrus-engine.exe"; Parameters: "--bench4k --pause"; WorkingDir: "{app}"
Name: "{group}\Uninstall Papyrus"; Filename: "{uninstallexe}"
Name: "{autodesktop}\Papyrus"; Filename: "{app}\app\Papyrus.exe"; WorkingDir: "{app}\app"; Tasks: desktopicon

[InstallDelete]
; Remove the previous app files so an update never leaves stale ones behind.
Type: filesandordirs; Name: "{app}\studio"
Type: filesandordirs; Name: "{app}\app"

[Registry]
; "Always on" adds this value from inside Papyrus; make sure uninstalling removes it.
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: none; ValueName: "Papyrus"; Flags: dontcreatekey uninsdeletevalue

[Run]
Filename: "{app}\app\Papyrus.exe"; Description: "Open Papyrus now"; Flags: postinstall nowait skipifsilent
