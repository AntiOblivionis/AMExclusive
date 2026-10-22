@echo off
setlocal
for /f "tokens=2 delims=:" %%C in ('chcp') do set "oldcp=%%C"
chcp 65001 >nul
title AMExclusive - Uninstall
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Uninstall-Mod.ps1" %*
set "rc=%ERRORLEVEL%"
echo.
if "%rc%"=="0" (
  echo 卸载成功。 / Uninstall completed successfully.
  echo 伴生任务、旧版自启动项、应用注册项、日志和安装目录已删除。
  echo The scheduled companion, compatibility Run entry, Apps entry, logs, and install directory were removed.
) else (
  echo 卸载失败，退出码 / Uninstall failed with exit code: %rc%.
)
echo.
pause
chcp %oldcp% >nul
exit /b %rc%
