@echo off
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "%~dp0Install-VLC-Subtitles.ps1" %*
set "vlcInstallerExit=%errorlevel%"
if not "%vlcInstallerExit%"=="0" (
    echo.
    echo Installation stopped. See the message above.
)
echo Press any key to close this window.
pause >nul
exit /b %vlcInstallerExit%
