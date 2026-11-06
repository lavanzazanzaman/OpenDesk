; OpenDesk Streamer installer. Built with: makensis -DAPP_VERSION=1.0 -DSTAGE_DIR=... installer.nsi
Unicode true
!include "MUI2.nsh"
!include "x64.nsh"

!ifndef APP_VERSION
  !define APP_VERSION "1.0"
!endif
!ifndef STAGE_DIR
  !define STAGE_DIR "..\..\dist\OpenDesk"
!endif

Name "OpenDesk Streamer"
OutFile "..\..\dist\OpenDesk-Windows-x86_64-Setup.exe"
InstallDir "$PROGRAMFILES64\OpenDesk"
InstallDirRegKey HKLM "Software\OpenDesk" "InstallDir"
RequestExecutionLevel admin
SetCompressor /SOLID lzma

!define MUI_ABORTWARNING
!define MUI_ICON "${NSISDIR}\Contrib\Graphics\Icons\modern-install.ico"
!define MUI_UNICON "${NSISDIR}\Contrib\Graphics\Icons\modern-uninstall.ico"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "..\..\LICENSE"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

Section "OpenDesk Streamer" SecMain
  SetOutPath "$INSTDIR"
  File /r "${STAGE_DIR}\*.*"

  WriteRegStr HKLM "Software\OpenDesk" "InstallDir" "$INSTDIR"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\OpenDesk" "DisplayName" "OpenDesk Streamer"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\OpenDesk" "DisplayVersion" "${APP_VERSION}"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\OpenDesk" "Publisher" "OpenDesk"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\OpenDesk" "DisplayIcon" "$INSTDIR\OpenDesk.exe"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\OpenDesk" "UninstallString" "$INSTDIR\Uninstall.exe"
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\OpenDesk" "NoModify" 1
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\OpenDesk" "NoRepair" 1

  CreateDirectory "$SMPROGRAMS\OpenDesk"
  CreateShortcut "$SMPROGRAMS\OpenDesk\OpenDesk Streamer.lnk" "$INSTDIR\OpenDesk.exe"
  CreateShortcut "$SMPROGRAMS\OpenDesk\Uninstall OpenDesk.lnk" "$INSTDIR\Uninstall.exe"
  CreateShortcut "$DESKTOP\OpenDesk Streamer.lnk" "$INSTDIR\OpenDesk.exe"

  WriteUninstaller "$INSTDIR\Uninstall.exe"
SectionEnd

; OpenDesk XR as the system's OpenXR runtime: Windows keeps it in HKLM, so it is set here (elevated) rather than by the
; streamer, which would need a UAC prompt. The previous runtime is kept and restored on uninstall.
Section "Make OpenDesk XR the OpenXR runtime" SecOpenXR
  SetRegView 64
  ReadRegStr $0 HKLM "SOFTWARE\Khronos\OpenXR\1" "ActiveRuntime"
  StrCmp $0 "$INSTDIR\runtime\opendesk_xr.json" openxr_done
  WriteRegStr HKLM "Software\OpenDesk" "PreviousOpenXRRuntime" "$0"
  WriteRegStr HKLM "SOFTWARE\Khronos\OpenXR\1" "ActiveRuntime" "$INSTDIR\runtime\opendesk_xr.json"
  WriteRegDWORD HKLM "SOFTWARE\Khronos\OpenXR\1\AvailableRuntimes" "$INSTDIR\runtime\opendesk_xr.json" 0
  openxr_done:
SectionEnd

; The streamer listens on TCP 38810-38840 and discovers over UDP 38850. Adding the rules here avoids the
; Windows Firewall prompt on the first connection.
Section "Firewall rule" SecFirewall
  nsExec::ExecToLog 'netsh advfirewall firewall delete rule name="OpenDesk Streamer"'
  nsExec::ExecToLog 'netsh advfirewall firewall delete rule name="OpenDesk Streamer (UDP)"'
  nsExec::ExecToLog 'netsh advfirewall firewall add rule name="OpenDesk Streamer" dir=in action=allow protocol=TCP localport=38810-38840'
  nsExec::ExecToLog 'netsh advfirewall firewall add rule name="OpenDesk Streamer (UDP)" dir=in action=allow protocol=UDP localport=38850'
SectionEnd

Section "Uninstall"
  SetRegView 64
  ReadRegStr $0 HKLM "SOFTWARE\Khronos\OpenXR\1" "ActiveRuntime"
  StrCmp $0 "$INSTDIR\runtime\opendesk_xr.json" 0 openxr_kept
  ReadRegStr $1 HKLM "Software\OpenDesk" "PreviousOpenXRRuntime"
  StrCmp $1 "" 0 +3
  DeleteRegValue HKLM "SOFTWARE\Khronos\OpenXR\1" "ActiveRuntime"
  Goto openxr_kept
  WriteRegStr HKLM "SOFTWARE\Khronos\OpenXR\1" "ActiveRuntime" "$1"
  openxr_kept:
  DeleteRegValue HKLM "SOFTWARE\Khronos\OpenXR\1\AvailableRuntimes" "$INSTDIR\runtime\opendesk_xr.json"
  Delete "$SMPROGRAMS\OpenDesk\OpenDesk Streamer.lnk"
  Delete "$SMPROGRAMS\OpenDesk\Uninstall OpenDesk.lnk"
  RMDir "$SMPROGRAMS\OpenDesk"
  Delete "$DESKTOP\OpenDesk Streamer.lnk"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir /r "$INSTDIR"
  DeleteRegKey HKLM "Software\OpenDesk"
  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\OpenDesk"
  nsExec::ExecToLog 'netsh advfirewall firewall delete rule name="OpenDesk Streamer"'
  nsExec::ExecToLog 'netsh advfirewall firewall delete rule name="OpenDesk Streamer (UDP)"'
SectionEnd
