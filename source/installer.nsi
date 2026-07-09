; installer.nsi — TapeXPlayer Windows Setup (NSIS 3, MUI2)
;
; Built by ./create_win_installer.sh (MSYS2, package mingw-w64-x86_64-nsis).
; Packages the contents of ../builds/win-bundle (exe + all DLLs after bundle_dlls.sh)
; plus extensions/ and licenses. makensis is an x86 tool, but the payload can be
; any architecture (ARM64/x64); on Windows ARM64 the installer itself runs via emulation.

!include "MUI2.nsh"

!ifndef BUILD_NUMBER
  !define BUILD_NUMBER "dev"
!endif
!ifndef ARCH
  !define ARCH "x64"
!endif
!ifndef BUNDLE_DIR
  !define BUNDLE_DIR "..\builds\win-bundle"
!endif

Name "TapeXPlayer"
OutFile "..\builds\TapeXPlayer-Setup-${ARCH}-build${BUILD_NUMBER}.exe"
Unicode true
InstallDir "$PROGRAMFILES64\TapeXPlayer"
InstallDirRegKey HKLM "Software\TapeXPlayer" "InstallDir"
RequestExecutionLevel admin

!define MUI_ICON "modules\FSTPMainModule\WSGUI\resources\TapeXPlayer.ico"
!define MUI_ABORTWARNING

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_RUN "$INSTDIR\TapeXPlayer.exe"
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

Section "TapeXPlayer (required)" SecMain
  SectionIn RO
  SetOutPath "$INSTDIR"
  File /r "${BUNDLE_DIR}\*.*"
  File /nonfatal "LICENSES_THIRD_PARTY.txt"

  ; Extensions (examples and registry) — the app looks for them next to the exe
  SetOutPath "$INSTDIR\extensions"
  File /nonfatal /r "extensions\*.*"
  SetOutPath "$INSTDIR"

  WriteRegStr HKLM "Software\TapeXPlayer" "InstallDir" "$INSTDIR"
  WriteUninstaller "$INSTDIR\Uninstall.exe"

  ; Add/Remove Programs
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\TapeXPlayer" \
      "DisplayName" "TapeXPlayer"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\TapeXPlayer" \
      "DisplayVersion" "Build ${BUILD_NUMBER}"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\TapeXPlayer" \
      "Publisher" "FFB_soffa"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\TapeXPlayer" \
      "DisplayIcon" "$INSTDIR\TapeXPlayer.exe"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\TapeXPlayer" \
      "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\TapeXPlayer" \
      "NoModify" 1
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\TapeXPlayer" \
      "NoRepair" 1

  ; Start Menu shortcuts
  CreateDirectory "$SMPROGRAMS\TapeXPlayer"
  CreateShortcut "$SMPROGRAMS\TapeXPlayer\TapeXPlayer.lnk" "$INSTDIR\TapeXPlayer.exe"
  CreateShortcut "$SMPROGRAMS\TapeXPlayer\Uninstall TapeXPlayer.lnk" "$INSTDIR\Uninstall.exe"
SectionEnd

Section "Desktop shortcut" SecDesktop
  CreateShortcut "$DESKTOP\TapeXPlayer.lnk" "$INSTDIR\TapeXPlayer.exe"
SectionEnd

Section "Uninstall"
  Delete "$SMPROGRAMS\TapeXPlayer\TapeXPlayer.lnk"
  Delete "$SMPROGRAMS\TapeXPlayer\Uninstall TapeXPlayer.lnk"
  RMDir "$SMPROGRAMS\TapeXPlayer"
  Delete "$DESKTOP\TapeXPlayer.lnk"
  RMDir /r "$INSTDIR"
  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\TapeXPlayer"
  DeleteRegKey HKLM "Software\TapeXPlayer"
SectionEnd
