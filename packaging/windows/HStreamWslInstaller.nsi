Unicode True
RequestExecutionLevel user
SetCompressor /SOLID lzma
SetCompressorDictSize 32

!include "MUI2.nsh"
!include "FileFunc.nsh"
!include "LogicLib.nsh"
!include "nsDialogs.nsh"

!define PRODUCT_NAME "HStream"
!define PRODUCT_PUBLISHER "HStream"
!define PRODUCT_WEB_SITE "https://github.com/${REPOSITORY}"
!define PRODUCT_UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\HStreamWSL"
!define MUI_ICON "${ICON_FILE}"
!define MUI_UNICON "${ICON_FILE}"
!define MUI_ABORTWARNING

Name "${PRODUCT_NAME} ${VERSION_TAG}"
OutFile "${OUTPUT_FILE}"
Icon "${ICON_FILE}"
UninstallIcon "${ICON_FILE}"
InstallDir "$LOCALAPPDATA\Programs\HStream"
InstallDirRegKey HKCU "Software\HStream" "InstallDir"
BrandingText "HStream Windows/WSL Bootstrapper"
ShowInstDetails show
ShowUninstDetails show

VIProductVersion "${PACKAGE_VERSION}.0"
VIAddVersionKey /LANG=1033 "ProductName" "HStream Windows/WSL Bootstrapper"
VIAddVersionKey /LANG=1033 "CompanyName" "HStream"
VIAddVersionKey /LANG=1033 "FileDescription" "Installs HStream into a dedicated WSL 2 distribution"
VIAddVersionKey /LANG=1033 "FileVersion" "${PACKAGE_VERSION}.0"
VIAddVersionKey /LANG=1033 "ProductVersion" "${PACKAGE_VERSION}.0"
VIAddVersionKey /LANG=1033 "LegalCopyright" "HStream contributors"

Var DeepStreamDeb
Var DeepStreamInput
Var Dialog
Var ExitCode
Var GitHubToken
Var GitHubTokenInput
Var PowerShellExe

!insertmacro MUI_PAGE_WELCOME
Page custom DeepStreamPage DeepStreamPageLeave
Page custom GitHubTokenPage GitHubTokenPageLeave
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_LINK "Open the HStream release page"
!define MUI_FINISHPAGE_LINK_LOCATION "https://github.com/${REPOSITORY}/releases/tag/${VERSION_TAG}"
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

Function .onInit
  SetRegView 64
  StrCpy $PowerShellExe "$WINDIR\Sysnative\WindowsPowerShell\v1.0\powershell.exe"
  IfFileExists "$PowerShellExe" +2 0
    StrCpy $PowerShellExe "$WINDIR\System32\WindowsPowerShell\v1.0\powershell.exe"
  ${GetParameters} $0
  ${GetOptions} $0 "/DEEPSTREAM_DEB=" $DeepStreamDeb
  ReadEnvStr $GitHubToken "HSTREAM_GITHUB_TOKEN"
FunctionEnd

Function un.onInit
  SetRegView 64
  StrCpy $PowerShellExe "$WINDIR\Sysnative\WindowsPowerShell\v1.0\powershell.exe"
  IfFileExists "$PowerShellExe" +2 0
    StrCpy $PowerShellExe "$WINDIR\System32\WindowsPowerShell\v1.0\powershell.exe"
FunctionEnd

Function DeepStreamPage
  nsDialogs::Create 1018
  Pop $Dialog
  ${If} $Dialog == error
    Abort
  ${EndIf}

  ${NSD_CreateLabel} 0 0 100% 30u "Select NVIDIA DeepStream 9.1.0-1+resolute2 for AMD64. It remains a local input and is not embedded in or uploaded by this installer."
  Pop $0
  ${NSD_CreateFileRequest} 0 42u 77% 13u "$DeepStreamDeb"
  Pop $DeepStreamInput
  ${NSD_CreateBrowseButton} 80% 41u 20% 15u "Browse..."
  Pop $0
  ${NSD_OnClick} $0 SelectDeepStreamDeb
  ${NSD_CreateLabel} 0 67u 100% 52u "The bootstrapper downloads and verifies Canonical Ubuntu 24.04 and HStream ${VERSION_TAG}, creates a dedicated HStream WSL 2 distribution, and then installs this local NVIDIA package. Expect several gigabytes of downloads and installed dependencies."
  Pop $0
  nsDialogs::Show
FunctionEnd

Function SelectDeepStreamDeb
  nsDialogs::SelectFileDialog open "$DeepStreamDeb" "Debian packages (*.deb)|*.deb|All files (*.*)|*.*"
  Pop $0
  ${If} $0 != error
    StrCpy $DeepStreamDeb $0
    ${NSD_SetText} $DeepStreamInput $DeepStreamDeb
  ${EndIf}
FunctionEnd

Function DeepStreamPageLeave
  ${NSD_GetText} $DeepStreamInput $DeepStreamDeb
  ${If} $DeepStreamDeb == ""
    MessageBox MB_ICONEXCLAMATION|MB_OK "Select the required DeepStream 9.1 Debian package."
    Abort
  ${EndIf}
  IfFileExists "$DeepStreamDeb" +3 0
    MessageBox MB_ICONEXCLAMATION|MB_OK "The selected DeepStream package does not exist."
    Abort
FunctionEnd

Function GitHubTokenPage
  nsDialogs::Create 1018
  Pop $Dialog
  ${If} $Dialog == error
    Abort
  ${EndIf}

  ${NSD_CreateLabel} 0 0 100% 34u "GitHub access token (needed for private repositories):"
  Pop $0
  ${NSD_CreatePassword} 0 38u 100% 13u "$GitHubToken"
  Pop $GitHubTokenInput
  ${NSD_CreateLabel} 0 62u 100% 68u "Paste a fine-grained token with read-only Contents access to ${REPOSITORY}. The token is used only by this provisioning run, is not written to disk, and is not placed on a command line. Leave it blank only when the release repository is public."
  Pop $0
  nsDialogs::Show
FunctionEnd

Function GitHubTokenPageLeave
  ${NSD_GetText} $GitHubTokenInput $GitHubToken
FunctionEnd

Section "HStream WSL environment" SEC_MAIN
  SetOutPath "$INSTDIR"
  File /oname=hstream-wsl.ps1 "${POWERSHELL_SOURCE}"
  File /oname=install-hstream-deb "${LINUX_INSTALLER_SOURCE}"
  File /oname=hstream.ico "${ICON_FILE}"
  WriteUninstaller "$INSTDIR\Uninstall.exe"

  DetailPrint "Provisioning the dedicated HStream WSL 2 distribution..."
  System::Call 'Kernel32::SetEnvironmentVariableW(w "HSTREAM_GITHUB_TOKEN", w "$GitHubToken") i .r0'
  nsExec::ExecToLog '"$PowerShellExe" -NoLogo -NoProfile -ExecutionPolicy Bypass -File "$INSTDIR\hstream-wsl.ps1" -Action Install -VersionTag "${VERSION_TAG}" -Repository "${REPOSITORY}" -DistroName "HStream" -DeepStreamDeb "$DeepStreamDeb"'
  Pop $ExitCode
  System::Call 'Kernel32::SetEnvironmentVariableW(w "HSTREAM_GITHUB_TOKEN", w "") i .r0'
  ${If} $ExitCode == "3010"
    SetRebootFlag true
    MessageBox MB_ICONINFORMATION|MB_OK "Windows enabled WSL components. Restart Windows, then run this installer again to finish installing HStream."
  ${ElseIf} $ExitCode != "0"
    SetErrorLevel 1
    MessageBox MB_ICONSTOP|MB_OK "HStream provisioning failed with exit code $ExitCode. Review the installation details above and %LOCALAPPDATA%\HStream\installer.log."
    Abort
  ${Else}
    CreateDirectory "$SMPROGRAMS\HStream"
    CreateShortcut "$SMPROGRAMS\HStream\HStream UI.lnk" "$WINDIR\System32\WindowsPowerShell\v1.0\powershell.exe" '-NoLogo -NoProfile -ExecutionPolicy Bypass -File "$INSTDIR\hstream-wsl.ps1" -Action Launch -DistroName "HStream"' "$INSTDIR\hstream.ico"
    CreateShortcut "$SMPROGRAMS\HStream\HStream Shell.lnk" "$WINDIR\System32\wsl.exe" '-d HStream --cd /home/hstream' "$INSTDIR\hstream.ico"
    CreateShortcut "$SMPROGRAMS\HStream\Games.lnk" "$WINDIR\explorer.exe" '"\\wsl.localhost\HStream\home\hstream\Videos"' "$INSTDIR\hstream.ico"
    CreateShortcut "$SMPROGRAMS\HStream\Output.lnk" "$WINDIR\explorer.exe" '"\\wsl.localhost\HStream\home\hstream\hstream_output"' "$INSTDIR\hstream.ico"
  ${EndIf}

  WriteRegStr HKCU "Software\HStream" "InstallDir" "$INSTDIR"
  WriteRegStr HKCU "${PRODUCT_UNINST_KEY}" "DisplayName" "HStream ${VERSION_TAG} (WSL)"
  WriteRegStr HKCU "${PRODUCT_UNINST_KEY}" "DisplayIcon" "$INSTDIR\hstream.ico"
  WriteRegStr HKCU "${PRODUCT_UNINST_KEY}" "DisplayVersion" "${PACKAGE_VERSION}"
  WriteRegStr HKCU "${PRODUCT_UNINST_KEY}" "Publisher" "${PRODUCT_PUBLISHER}"
  WriteRegStr HKCU "${PRODUCT_UNINST_KEY}" "URLInfoAbout" "${PRODUCT_WEB_SITE}"
  WriteRegStr HKCU "${PRODUCT_UNINST_KEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegDWORD HKCU "${PRODUCT_UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKCU "${PRODUCT_UNINST_KEY}" "NoRepair" 1
SectionEnd

Section "Uninstall"
  MessageBox MB_ICONQUESTION|MB_YESNO|MB_DEFBUTTON2 "Also unregister the HStream WSL distribution? This permanently deletes its Linux filesystem, games, configuration, and output. Choose No to preserve all WSL data." IDNO PreserveDistro
  nsExec::ExecToLog '"$PowerShellExe" -NoLogo -NoProfile -ExecutionPolicy Bypass -File "$INSTDIR\hstream-wsl.ps1" -Action Unregister -DistroName "HStream"'
  Pop $ExitCode
  ${If} $ExitCode != "0"
    MessageBox MB_ICONEXCLAMATION|MB_OK "The Windows launcher will be removed, but unregistering the HStream WSL distribution failed with exit code $ExitCode."
  ${EndIf}

PreserveDistro:
  Delete "$SMPROGRAMS\HStream\HStream UI.lnk"
  Delete "$SMPROGRAMS\HStream\HStream Shell.lnk"
  Delete "$SMPROGRAMS\HStream\Games.lnk"
  Delete "$SMPROGRAMS\HStream\Output.lnk"
  RMDir "$SMPROGRAMS\HStream"
  DeleteRegKey HKCU "${PRODUCT_UNINST_KEY}"
  DeleteRegKey HKCU "Software\HStream"
  Delete "$INSTDIR\hstream-wsl.ps1"
  Delete "$INSTDIR\install-hstream-deb"
  Delete "$INSTDIR\hstream.ico"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"
SectionEnd
