@echo off
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "%~dp0Installer-VLC-Undertekster.ps1" %*
set "vlcInstallerExit=%errorlevel%"
if not "%vlcInstallerExit%"=="0" (
    echo.
    echo Installasjonen stoppet. Se meldingen ovenfor.
)
pause
exit /b %vlcInstallerExit%
