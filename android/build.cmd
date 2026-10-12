@echo off
REM ASCII only on purpose: cmd.exe reads .cmd files using the OEM code page
REM (936/GBK on this machine), so UTF-8 Chinese comments here become garbage
REM "commands" and the script fails. All Chinese lives in build.ps1 instead.
REM Usage: build.cmd [gradle task] ...   (default: assembleDebug)
setlocal
set "PS1=%~dp0build.ps1"
if not exist "%PS1%" (
    echo [ERROR] build.ps1 not found next to this script.
    exit /b 2
)
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%PS1%" %*
exit /b %ERRORLEVEL%
