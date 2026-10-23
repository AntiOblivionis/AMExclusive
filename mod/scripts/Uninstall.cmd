@echo off
setlocal
if /i "%~1"=="/ame-relaunched" goto relaunched
rem Resolve this script's directory before any directory change: a relative,
rem quoted launch ("Uninstall.cmd") re-resolves %~dp0 against the new cwd.
set "AME_HERE=%~dp0"
set "AME_SELF=%~f0"
rem Re-run from a copy in %TEMP%. Launched by double-click, this console's
rem working directory is the install directory and this file is still being
rem read line by line; Uninstall-Mod.ps1 removes both, so the directory delete
rem would fail and the result message would never print.
set "AME_COPY=%TEMP%\AMExclusive-Uninstall-%RANDOM%%RANDOM%.cmd"
copy /y "%AME_SELF%" "%AME_COPY%" >nul
if not exist "%AME_COPY%" goto inplace
pushd "%TEMP%"
"%AME_COPY%" /ame-relaunched "%AME_HERE%" %*
rem Not reached: the copy replaces this batch context.
exit /b 1

:relaunched
set "AME_COPY=%~f0"
set "AME_SRC=%~2"
set "AME_ARGS="
shift
shift
:collect
if "%~1"=="" goto run
set AME_ARGS=%AME_ARGS% %1
shift
goto collect

:inplace
rem Running in place from the install directory would reproduce the original
rem failure (the directory cannot be removed while it is this console's cwd).
echo Unable to stage the uninstaller in "%TEMP%". Use Windows Settings ^> Apps ^> Installed apps ^> AMExclusive, or run AMExclusive-Setup.exe and choose 2.
pause
exit /b 1

:run
for /f "tokens=2 delims=:" %%C in ('chcp') do set "oldcp=%%C"
chcp 65001 >nul
title AMExclusive - Uninstall
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%AME_SRC%Uninstall-Mod.ps1"%AME_ARGS%
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
rem Leave the batch context, remove the temporary copy of this script, and
rem hand the exit code back without ending the console that may have run us
rem (a bare "exit" after (goto) would close an interactive Command Prompt).
(goto) 2>nul & del "%AME_COPY%" & cmd /c exit %rc%
