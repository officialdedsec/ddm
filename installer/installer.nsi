; DDM - DedSec Download Manager 4.0 setup (per user, no administrator needed). Windows 7, 8, 8.1, 10 and 11.
Unicode true
SetCompressor /SOLID lzma
!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "WinVer.nsh"

!define APP "DDM"
!define FULL "DDM - DedSec Download Manager"
!ifndef VER
  !define VER "4.3.2"
!endif
!define ORG "DedSec Cyber Security Research Institute"
!define UNINST "Software\Microsoft\Windows\CurrentVersion\Uninstall\DDM"

Name "${FULL}"
OutFile "..\out\DDM-Setup-${VER}.exe"
InstallDir "$LOCALAPPDATA\Programs\DDM"
InstallDirRegKey HKCU "Software\DedSec\DDM" "InstallDir"
RequestExecutionLevel user
ManifestDPIAware true
BrandingText "${ORG}"
VIProductVersion "${VER}.0"
VIAddVersionKey "ProductName" "${FULL}"
VIAddVersionKey "CompanyName" "${ORG}"
VIAddVersionKey "FileDescription" "${FULL} setup"
VIAddVersionKey "FileVersion" "${VER}"
VIAddVersionKey "ProductVersion" "${VER}"
VIAddVersionKey "LegalCopyright" "© 2026 ${ORG}"

!define MUI_ICON "..\res\ddm.ico"
!define MUI_UNICON "..\res\ddm.ico"
!define MUI_ABORTWARNING
!define MUI_WELCOMEPAGE_TITLE "Welcome to DDM ${VER}"
!define MUI_WELCOMEPAGE_TEXT "DDM — DedSec Download Manager: fast downloads with up to 32 connections, videos from the sites you watch, torrents and MEGA, with a check of every link and file for your safety.$\r$\n$\r$\nDDM starts with Windows in the tray and uses very little memory. After setup, it shows you how to add it to your browsers.$\r$\n$\r$\nClick Next to continue."
!define MUI_FINISHPAGE_RUN "$INSTDIR\DDM.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Start DDM"
!define MUI_FINISHPAGE_LINK "dedseec.com"
!define MUI_FINISHPAGE_LINK_LOCATION "https://dedseec.com"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Function .onInit
  ${IfNot} ${AtLeastWin7}
    MessageBox MB_ICONSTOP "DDM needs Windows 7 or later."
    Abort
  ${EndIf}
FunctionEnd

; an update started by DDM runs quietly (/S): DDM starts again by itself afterwards
Function .onInstSuccess
  IfSilent 0 +2
    Exec '"$INSTDIR\DDM.exe"'
FunctionEnd

Section "DDM" SecMain
  ; a running DDM lets go of its files (unfinished downloads carry on where they were when it starts again)
  nsExec::Exec 'taskkill /IM DDM.exe /F'
  nsExec::Exec 'taskkill /IM ddmhost.exe /F'
  Sleep 800
  ; an older DDM installed in another folder: its program files go (one DDM on this PC, one entry in Apps). Its
  ; data stays: a portable copy (portable.txt, or its own data folder) is never touched.
  ReadRegStr $0 HKCU "${UNINST}" "InstallLocation"
  ${If} $0 != ""
  ${AndIf} $0 != "$INSTDIR"
  ${AndIf} ${FileExists} "$0\DDM.exe"
  ${AndIfNot} ${FileExists} "$0\portable.txt"
  ${AndIfNot} ${FileExists} "$0\data\*.*"
    Delete "$0\DDM.exe"
    Delete "$0\DDM.exe.sig"
    Delete "$0\ddmhost.exe"
    Delete "$0\ffmpeg.exe"
    Delete "$0\sites.json"
    Delete "$0\sites.json.sig"
    Delete "$0\THIRD-PARTY-NOTICES.txt"
    Delete "$0\LICENSE.txt"
    Delete "$0\DDM.exe.config"
    Delete "$0\DDMHost.exe.config"
    Delete "$0\engine.dll"
    RMDir /r "$0\extension"
    Delete "$0\Uninstall DDM.exe"
    RMDir "$0"
  ${EndIf}
  SetOutPath "$INSTDIR"
  ; files of DDM 3.x that 4.0 doesn't use (its old signature would stop the new DDM.exe from starting)
  Delete "$INSTDIR\DDM.exe.sig"
  Delete "$INSTDIR\DDM.exe.config"
  Delete "$INSTDIR\DDMHost.exe.config"
  Delete "$INSTDIR\engine.dll"
  Delete "$INSTDIR\README.md"
  Delete "$INSTDIR\DDM-User-Guide.html"
  Delete "$INSTDIR\Fonts-License.txt"
  RMDir /r "$INSTDIR\extension"
  File "..\out\DDM.exe"
  File "..\out\ddmhost.exe"
  File "..\out\ffmpeg.exe"
  File "..\out\sites.json"
  File "..\out\sites.json.sig"
  File "THIRD-PARTY-NOTICES.txt"
  File "/oname=LICENSE.txt" "..\LICENSE"
  File /nonfatal "..\out\DDM.exe.sig"
  ; DDM's browser add-on for Chrome, Edge, Brave, Opera and Vivaldi, for "Load unpacked" (the stores have it too)
  SetOutPath "$INSTDIR\extension\chromium"
  File "..\out\extension\chromium\*.*"
  SetOutPath "$INSTDIR"
  WriteUninstaller "$INSTDIR\Uninstall DDM.exe"

  CreateShortCut "$SMPROGRAMS\DDM - DedSec Download Manager.lnk" "$INSTDIR\DDM.exe" "" "$INSTDIR\DDM.exe" 0
  CreateShortCut "$DESKTOP\DDM.lnk" "$INSTDIR\DDM.exe" "" "$INSTDIR\DDM.exe" 0
  ; starts with Windows, quietly in the tray (Options → General → Start DDM with Windows turns it off)
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "DDM" '"$INSTDIR\DDM.exe" /tray'
  WriteRegStr HKCU "Software\DedSec\DDM" "InstallDir" "$INSTDIR"
  ; .torrent files can open in DDM (Explorer's "Open with")
  WriteRegStr HKCU "Software\Classes\DDM.torrent" "" "Torrent file"
  WriteRegStr HKCU "Software\Classes\DDM.torrent\DefaultIcon" "" "$INSTDIR\DDM.exe,0"
  WriteRegStr HKCU "Software\Classes\DDM.torrent\shell\open\command" "" '"$INSTDIR\DDM.exe" "%1"'
  WriteRegStr HKCU "Software\Classes\.torrent\OpenWithProgids" "DDM.torrent" ""
  WriteRegStr HKCU "Software\DedSec\DDM\Capabilities\FileAssociations" ".torrent" "DDM.torrent"
  WriteRegStr HKCU "Software\DedSec\DDM\Capabilities" "ApplicationName" "DDM — DedSec Download Manager"
  WriteRegStr HKCU "Software\DedSec\DDM\Capabilities" "ApplicationDescription" "Downloads files, videos and torrents"
  WriteRegStr HKCU "Software\RegisteredApplications" "DDM" "Software\DedSec\DDM\Capabilities"
  ; magnet links: only when no other program has them (DDM asks once if another download manager is on the PC)
  ReadRegStr $0 HKCU "Software\Classes\magnet\shell\open\command" ""
  ${If} $0 == ""
    WriteRegStr HKCU "Software\Classes\magnet" "" "URL:Magnet link"
    WriteRegStr HKCU "Software\Classes\magnet" "URL Protocol" ""
    WriteRegStr HKCU "Software\Classes\magnet\DefaultIcon" "" '"$INSTDIR\DDM.exe",0'
    WriteRegStr HKCU "Software\Classes\magnet\shell\open\command" "" '"$INSTDIR\DDM.exe" "%1"'
  ${EndIf}

  WriteRegStr HKCU "${UNINST}" "DisplayName" "${FULL}"
  WriteRegStr HKCU "${UNINST}" "DisplayVersion" "${VER}"
  WriteRegStr HKCU "${UNINST}" "Publisher" "${ORG}"
  WriteRegStr HKCU "${UNINST}" "URLInfoAbout" "https://dedseec.com"
  WriteRegStr HKCU "${UNINST}" "HelpLink" "https://dedseec.com/contact"
  WriteRegStr HKCU "${UNINST}" "Contact" "admin@dedseec.com"
  WriteRegStr HKCU "${UNINST}" "DisplayIcon" "$INSTDIR\DDM.exe,0"
  WriteRegStr HKCU "${UNINST}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${UNINST}" "UninstallString" '"$INSTDIR\Uninstall DDM.exe"'
  WriteRegStr HKCU "${UNINST}" "QuietUninstallString" '"$INSTDIR\Uninstall DDM.exe" /S'
  WriteRegDWORD HKCU "${UNINST}" "EstimatedSize" 9000
  WriteRegDWORD HKCU "${UNINST}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINST}" "NoRepair" 1
SectionEnd

Function un.onInit
  MessageBox MB_YESNO|MB_ICONEXCLAMATION "This removes DDM - DedSec Download Manager from your PC.$\r$\n$\r$\nDownloads that are still running stop. DDM is taken out of your browsers (its helper and add-on offers), of starting with Windows and of magnet links.$\r$\n$\r$\nNext you choose what else to remove. Continue?" /SD IDYES IDYES +2
    Abort
FunctionEnd

Section "Uninstall"
  nsExec::Exec 'taskkill /IM DDM.exe /F'
  nsExec::Exec 'taskkill /IM ddmhost.exe /F'
  Sleep 800
  ; 1. what DDM downloaded (only when you say so; to the Recycle Bin, so it can still be restored)
  MessageBox MB_YESNO|MB_ICONEXCLAMATION|MB_DEFBUTTON2 "Also remove the files you downloaded with DDM?$\r$\n$\r$\nEvery file in DDM's download list (and what unfinished downloads had received) is moved to the Recycle Bin. Choose No to keep your files." /SD IDNO IDNO keepfiles
    nsExec::Exec '"$INSTDIR\DDM.exe" /purge-downloads'
  keepfiles:
  ; 2. the browser helper, the add-on offers, magnet links and start with Windows
  nsExec::Exec '"$INSTDIR\DDM.exe" /unregister'
  DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "DDM"
  DeleteRegKey HKCU "Software\Classes\DDM.torrent"
  DeleteRegKey HKCU "Software\Classes\DDM.Magnet"
  DeleteRegValue HKCU "Software\Classes\.torrent\OpenWithProgids" "DDM.torrent"
  ReadRegStr $0 HKCU "Software\Classes\magnet\shell\open\command" ""
  ${If} $0 == '"$INSTDIR\DDM.exe" "%1"'
    DeleteRegKey HKCU "Software\Classes\magnet"
  ${EndIf}
  DeleteRegValue HKCU "Software\RegisteredApplications" "DDM"
  DeleteRegKey HKCU "${UNINST}"
  DeleteRegKey HKCU "Software\DedSec\DDM"
  DeleteRegKey /ifempty HKCU "Software\DedSec"
  Delete "$SMPROGRAMS\DDM - DedSec Download Manager.lnk"
  Delete "$DESKTOP\DDM.lnk"
  Delete "$INSTDIR\DDM.exe"
  Delete "$INSTDIR\DDM.exe.sig"
  Delete "$INSTDIR\ddmhost.exe"
  Delete "$INSTDIR\ffmpeg.exe"
  Delete "$INSTDIR\sites.json"
  Delete "$INSTDIR\sites.json.sig"
  Delete "$INSTDIR\THIRD-PARTY-NOTICES.txt"
  Delete "$INSTDIR\LICENSE.txt"
  Delete "$INSTDIR\portable.txt"
  RMDir /r "$INSTDIR\extension"
  RMDir /r "$INSTDIR\data"
  Delete "$INSTDIR\Uninstall DDM.exe"
  RMDir "$INSTDIR"
  ; 3. everything DDM kept for itself (asked, with a warning about the vault)
  IfFileExists "$LOCALAPPDATA\DDM\native\vault\*.*" 0 novault
    MessageBox MB_YESNO|MB_ICONEXCLAMATION|MB_DEFBUTTON2 "Remove everything DDM saved on this PC?$\r$\n$\r$\nSettings, download list, statistics, logs, caches, helper programs and backups — and your PRIVATE VAULT. Files in the vault are deleted for good and can't be recovered. Take them out of the vault first if you want to keep them.$\r$\n$\r$\nChoose No to keep them (for example to install DDM again later)." /SD IDNO IDNO keepdata
    Goto wipe
  novault:
    MessageBox MB_YESNO|MB_ICONQUESTION "Remove everything DDM saved on this PC?$\r$\n$\r$\nSettings, download list, statistics, logs, caches, helper programs and backups.$\r$\n$\r$\nChoose No to keep them (for example to install DDM again later)." /SD IDNO IDNO keepdata
  wipe:
    RMDir /r "$LOCALAPPDATA\DDM"
    RMDir /r "$APPDATA\DDM"
  keepdata:
  MessageBox MB_OK|MB_ICONINFORMATION "DDM is removed.$\r$\n$\r$\nAn add-on you installed yourself from a browser's store stays in that browser until you remove it there: open the browser's Extensions page and choose Remove on DDM." /SD IDOK
SectionEnd
