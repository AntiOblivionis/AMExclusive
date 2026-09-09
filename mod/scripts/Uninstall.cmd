@echo off
setlocal
title AMExclusive - Uninstall
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Uninstall-Mod.ps1" %*
set "rc=%ERRORLEVEL%"
echo.
if "%rc%"=="0" (
  echo Uninstall completed successfully.
  echo The scheduled companion, compatibility Run entry, Apps entry, logs, and install directory were removed.
) else (
  echo Uninstall failed with exit code %rc%.
)
echo.
pause
exit /b %rc%
