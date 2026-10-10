Unicode true
!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "FileFunc.nsh"
!include "x64.nsh"
!include "WinVer.nsh"
!include "nsDialogs.nsh"

Name "Eclipse"
OutFile "${OUTPUT_FILE}"
InstallDir "$LOCALAPPDATA\Programs\Eclipse"
InstallDirRegKey HKCU "Software\EclipseClient" "InstallDir"
RequestExecutionLevel user
SetCompressor /SOLID lzma
ShowInstDetails show
ShowUninstDetails show
VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "Eclipse"
VIAddVersionKey "FileDescription" "Eclipse per-user installer"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "LegalCopyright" "Eclipse contributors; see bundled COPYRIGHT"

!define MUI_ABORTWARNING
!define MUI_DIRECTORYPAGE_TEXT_TOP "Install Eclipse for your Windows account. No administrator access or background service is needed."
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
UninstPage custom un.Options
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_UNPAGE_FINISH
!insertmacro MUI_LANGUAGE "English"
Var KeepSettings
Var KeepSettingsBox

Function .onInit
    SetShellVarContext current
    ${IfNot} ${RunningX64}
        MessageBox MB_OK|MB_ICONSTOP "This Eclipse package requires 64-bit Windows."
        Abort
    ${EndIf}
    ${IfNot} ${AtLeastWin10}
    ${OrIfNot} ${AtLeastBuild} 17134
        MessageBox MB_OK|MB_ICONSTOP "Eclipse requires Windows 10 version 1803 or later."
        Abort
    ${EndIf}
FunctionEnd

Function un.onInit
    SetShellVarContext current
    StrCpy $KeepSettings 0
    ${GetParameters} $0
    ClearErrors
    ${GetOptions} $0 "/KEEPSETTINGS" $1
    ${IfNot} ${Errors}
        StrCpy $KeepSettings 1
    ${EndIf}
FunctionEnd

Function un.Options
    nsDialogs::Create 1018
    Pop $0
    ${NSD_CreateLabel} 0 0 100% 38u "Remove Eclipse, its shortcuts, settings, pairings, caches and logs for this account. Eclipse will be closed first. Other installations' running processes are not stopped."
    Pop $0
    ${NSD_CreateCheckbox} 0 48u 100% 14u "Keep settings and pairings for a future reinstall"
    Pop $KeepSettingsBox
    ${If} $KeepSettings == 1
        ${NSD_Check} $KeepSettingsBox
    ${EndIf}
    ${NSD_OnClick} $KeepSettingsBox un.SettingsChanged
    nsDialogs::Show
FunctionEnd

Function un.SettingsChanged
    Pop $0
    ${NSD_GetState} $KeepSettingsBox $KeepSettings
FunctionEnd

Section "Eclipse"
    InitPluginsDir
    File /oname=$PLUGINSDIR\manage-install.ps1 "${HELPER_FILE}"
    nsExec::ExecToLog '"$SYSDIR\WindowsPowerShell\v1.0\powershell.exe" -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "$PLUGINSDIR\manage-install.ps1" -Action Prepare -InstallDir "$INSTDIR"'
    Pop $0
    ${If} $0 != 0
        SetErrorLevel 1
        MessageBox MB_OK|MB_ICONSTOP "Cannot prepare the install folder. Close Eclipse and choose an empty folder or an existing Eclipse installation." /SD IDOK
        Abort
    ${EndIf}
    SetOutPath "$INSTDIR"
    File /r "${PAYLOAD_DIR}\*"
    WriteUninstaller "$INSTDIR\Uninstall.exe"
    CreateDirectory "$SMPROGRAMS\Eclipse"
    CreateShortcut "$SMPROGRAMS\Eclipse\Eclipse.lnk" "$INSTDIR\moonlight-tv.exe"
    CreateShortcut "$SMPROGRAMS\Eclipse\Uninstall Eclipse.lnk" "$INSTDIR\Uninstall.exe"
    WriteRegStr HKCU "Software\EclipseClient" "InstallDir" "$INSTDIR"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\EclipseClient" "DisplayName" "Eclipse"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\EclipseClient" "DisplayVersion" "${VERSION}"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\EclipseClient" "Publisher" "Eclipse contributors"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\EclipseClient" "InstallLocation" "$INSTDIR"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\EclipseClient" "DisplayIcon" "$INSTDIR\moonlight-tv.exe"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\EclipseClient" "UninstallString" '$\"$INSTDIR\Uninstall.exe$\"'
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\EclipseClient" "QuietUninstallString" '$\"$INSTDIR\Uninstall.exe$\" /S'
    WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\EclipseClient" "NoModify" 1
    WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\EclipseClient" "NoRepair" 1
SectionEnd

Section "Uninstall"
    InitPluginsDir
    File /oname=$PLUGINSDIR\manage-install.ps1 "${HELPER_FILE}"
    StrCpy $1 ""
    ${If} $KeepSettings == 1
        StrCpy $1 "-KeepSettings"
    ${EndIf}
    nsExec::ExecToLog '"$SYSDIR\WindowsPowerShell\v1.0\powershell.exe" -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "$PLUGINSDIR\manage-install.ps1" -Action Remove -InstallDir "$INSTDIR" $1'
    Pop $0
    ${If} $0 != 0
        SetErrorLevel 1
        MessageBox MB_OK|MB_ICONSTOP "Eclipse could not be completely removed. See the details, close Eclipse, then retry." /SD IDOK
        Abort
    ${EndIf}
    Delete "$SMPROGRAMS\Eclipse\Eclipse.lnk"
    Delete "$SMPROGRAMS\Eclipse\Uninstall Eclipse.lnk"
    RMDir "$SMPROGRAMS\Eclipse"
    DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\EclipseClient"
    DeleteRegKey HKCU "Software\EclipseClient"
    Delete "$INSTDIR\Uninstall.exe"
    RMDir "$INSTDIR"
SectionEnd
