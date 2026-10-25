; CapView for Windows installer (NSIS 3, Unicode). Built by build.sh:
;   makensis -DVERSION=1.0.0 -DEXE=<path to CapView.exe> -DOUTFILE=<setup.exe> installer.nsi
; Installs per user (no administrator prompt) into %LOCALAPPDATA%\Programs\CapView.

Unicode true
!include "MUI2.nsh"

!ifndef VERSION
  !define VERSION "0.0.0"
!endif

Name "CapView"
OutFile "${OUTFILE}"
InstallDir "$LOCALAPPDATA\Programs\CapView"
InstallDirRegKey HKCU "Software\CapView\Install" "Path"
RequestExecutionLevel user
SetCompressor /SOLID lzma
BrandingText "CapView ${VERSION}"

!define MUI_ICON "res\CapView.ico"
!define MUI_UNICON "res\CapView.ico"
!define MUI_ABORTWARNING
!define MUI_FINISHPAGE_RUN "$INSTDIR\CapView.exe"
!define MUI_FINISHPAGE_RUN_TEXT "CapView 실행"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "Korean"

!define UNINSTALL_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\CapView"

Section "CapView"
  ; Quit a running copy so its exe can be replaced.
  nsExec::Exec 'taskkill /IM CapView.exe /F'
  SetOutPath "$INSTDIR"
  File "${EXE}"
  WriteUninstaller "$INSTDIR\Uninstall.exe"

  CreateShortcut "$SMPROGRAMS\CapView.lnk" "$INSTDIR\CapView.exe"
  CreateShortcut "$DESKTOP\CapView.lnk" "$INSTDIR\CapView.exe"

  WriteRegStr HKCU "Software\CapView\Install" "Path" "$INSTDIR"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayName" "CapView"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "Publisher" "CapView"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayIcon" "$INSTDIR\CapView.exe"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegStr HKCU "${UNINSTALL_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegDWORD HKCU "${UNINSTALL_KEY}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINSTALL_KEY}" "NoRepair" 1
SectionEnd

Section "Uninstall"
  nsExec::Exec 'taskkill /IM CapView.exe /F'
  Delete "$INSTDIR\CapView.exe"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"
  Delete "$SMPROGRAMS\CapView.lnk"
  Delete "$DESKTOP\CapView.lnk"
  DeleteRegKey HKCU "${UNINSTALL_KEY}"
  DeleteRegKey HKCU "Software\CapView\Install"
  ; Settings in HKCU\Software\CapView are kept so a reinstall remembers them.
SectionEnd
