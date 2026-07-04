; QRTP installer script for Inno Setup (https://jrsoftware.org/isinfo.php — free).
;
; Prerequisite: build qrtp_gui.exe first (see README.md's "Building on Windows" section), using
; the vcpkg CMake toolchain with WITH_OPENCV=ON. With the default VCPKG_APPLOCAL_DEPS=ON, vcpkg
; automatically copies every DLL qrtp_gui.exe depends on (OpenCV's, etc.) into the same build
; output folder — so that folder, as-is, already contains everything this installer needs to ship.
; qrencode.dll is loaded at runtime (not linked at build time), so make sure it's in that same
; folder too before compiling this script (copy it from vcpkg's installed\x64-windows\bin, or
; wherever you built/obtained it).
;
; To build the installer: open this file in the Inno Setup Compiler (Ctrl+F9), after setting
; SourceDir below to your actual build output folder (e.g. build\Release).

#define SourceDir "..\build\Release"

[Setup]
AppName=QRTP
AppVersion=1.0
AppPublisher=QRTP Project
DefaultDirName={autopf}\QRTP
DefaultGroupName=QRTP
DisableProgramGroupPage=yes
OutputDir=.
OutputBaseFilename=QRTP-Setup
Compression=lzma
SolidCompression=yes
ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64
WizardStyle=modern

[Languages]
Name: "italian"; MessagesFile: "compiler:Languages\Italian.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Crea un'icona sul desktop"; GroupDescription: "Icone aggiuntive:"

[Files]
Source: "{#SourceDir}\qrtp_gui.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\*.dll"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist

[Icons]
Name: "{group}\QRTP"; Filename: "{app}\qrtp_gui.exe"
Name: "{autodesktop}\QRTP"; Filename: "{app}\qrtp_gui.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\qrtp_gui.exe"; Description: "Avvia QRTP"; Flags: nowait postinstall skipifsilent
