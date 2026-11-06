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
; start the streamer as the signed-in user (setup runs elevated; explorer.exe hands it to the user's session)
!define MUI_FINISHPAGE_RUN
!define MUI_FINISHPAGE_RUN_TEXT "Start OpenDesk Streamer"
!define MUI_FINISHPAGE_RUN_FUNCTION StartStreamer
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

Function StartStreamer
  Exec '"$WINDIR\explorer.exe" "$INSTDIR\OpenDesk.exe"'
FunctionEnd

; Updating over a running OpenDesk: Windows locks the files of running programs, so before any file is replaced
;   - the streamer is closed (asked first; a silent setup closes it),
;   - OpenDesk Service is stopped (restarted by its section below),
;   - a VR game still holding OpenDesk XR / xrizer has to be closed (asked; a silent setup fails with exit code 2).
; "In use" = the file can't be opened for writing (no process-list plugin needed).
!macro OPENDESK_CLOSE_FUNCTIONS UN
Function ${UN}FileBusy ; $R0 = file -> $R1 = 1 when a running program holds it
  StrCpy $R1 0
  IfFileExists $R0 0 busy_done
  ClearErrors
  FileOpen $R2 $R0 a
  IfErrors 0 busy_free
  StrCpy $R1 1
  Goto busy_done
  busy_free:
  FileClose $R2
  busy_done:
FunctionEnd

Function ${UN}StopService
  StrCpy $R0 "$INSTDIR\opendesk-service.exe"
  Call ${UN}FileBusy
  StrCmp $R1 0 service_done
  DetailPrint "Stopping OpenDesk Service to update it..."
  nsExec::ExecToLog 'sc stop OpenDeskService'
  StrCpy $R3 0
  service_wait:
  Sleep 500
  Call ${UN}FileBusy
  StrCmp $R1 0 service_done
  IntOp $R3 $R3 + 1
  IntCmp $R3 20 0 service_wait
  nsExec::ExecToLog 'taskkill /F /IM opendesk-service.exe'
  Sleep 1000
  service_done:
FunctionEnd

Function ${UN}CloseStreamer
  StrCpy $R0 "$INSTDIR\OpenDesk.exe"
  Call ${UN}FileBusy
  StrCmp $R1 0 streamer_done
  IfSilent streamer_close
  MessageBox MB_OKCANCEL|MB_ICONEXCLAMATION "OpenDesk Streamer is running and has to be closed to finish.$\r$\n$\r$\nOK closes it now (a headset streaming from this PC disconnects; you can start it again at the end).$\r$\nCancel stops the setup." IDOK streamer_close
  Abort
  streamer_close:
  DetailPrint "Closing OpenDesk Streamer..."
  nsExec::ExecToLog 'taskkill /IM OpenDesk.exe'
  StrCpy $R3 0
  streamer_wait:
  Sleep 500
  Call ${UN}FileBusy
  StrCmp $R1 0 streamer_done
  IntOp $R3 $R3 + 1
  IntCmp $R3 6 0 streamer_wait ; 3 s to close by itself, then it is ended
  StrCmp $R3 6 0 +2
  nsExec::ExecToLog 'taskkill /F /IM OpenDesk.exe'
  IntCmp $R3 20 0 streamer_wait
  IfSilent +3
  MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION "OpenDesk Streamer did not close. Close it (tray icon > Quit), then click Retry." IDRETRY streamer_close
  Abort
  SetErrorLevel 2
  Abort
  streamer_done:
FunctionEnd

Function ${UN}CheckGames
  games_check:
  StrCpy $R0 "$INSTDIR\runtime\opendesk-openxr.dll"
  Call ${UN}FileBusy
  StrCmp $R1 1 games_busy
  StrCpy $R0 "$INSTDIR\runtime\ovr-vd.dll"
  Call ${UN}FileBusy
  StrCmp $R1 1 games_busy
  StrCpy $R0 "$INSTDIR\xrizer\bin\win64\vrclient_x64.dll"
  Call ${UN}FileBusy
  StrCmp $R1 1 games_busy
  Goto games_done
  games_busy:
  DetailPrint "In use by a running VR game: $R0"
  IfSilent games_silent
  MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION "A VR game is still running with OpenDesk XR or xrizer:$\r$\n$R0 is in use.$\r$\n$\r$\nClose the game (and SteamVR, if it runs), then click Retry." IDRETRY games_check
  Abort
  games_silent:
  SetErrorLevel 2
  Abort
  games_done:
FunctionEnd

Function ${UN}PrepareFiles
  Call ${UN}CloseStreamer
  Call ${UN}StopService
  Call ${UN}CheckGames
FunctionEnd
!macroend
!insertmacro OPENDESK_CLOSE_FUNCTIONS ""
!insertmacro OPENDESK_CLOSE_FUNCTIONS "un."

Section "OpenDesk Streamer" SecMain
  Call PrepareFiles
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

; OpenDesk Service (LocalSystem): switches the OpenXR runtime to OpenDesk XR while the streamer runs and back
; afterwards, without UAC prompts (streamer/service/runtime_service.cpp)
Section "OpenDesk Service" SecService
  nsExec::ExecToLog 'sc stop OpenDeskService'
  nsExec::ExecToLog 'sc delete OpenDeskService'
  nsExec::ExecToLog 'sc create OpenDeskService binPath= "\"$INSTDIR\opendesk-service.exe\"" start= auto DisplayName= "OpenDesk Service"'
  nsExec::ExecToLog 'sc description OpenDeskService "Switches the OpenXR runtime to OpenDesk XR while OpenDesk Streamer runs"'
  nsExec::ExecToLog 'sc start OpenDeskService'
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
  Call un.PrepareFiles
  nsExec::ExecToLog 'sc stop OpenDeskService'
  nsExec::ExecToLog 'sc delete OpenDeskService'
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
