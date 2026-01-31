@echo off
setlocal

rem Run SysMonitor.exe
rem Usage:
rem   run.bat                (runs Debug x64)
rem   run.bat debug           (runs Debug x64)
rem   run.bat release         (runs Release x64)

set "ROOT=%~dp0"
set "CONFIG=%~1"

if "%CONFIG%"=="" set "CONFIG=debug"

if /i "%CONFIG%"=="debug" (
  set "EXE=%ROOT%x64\Debug\SysMonitor.exe"
) else if /i "%CONFIG%"=="release" (
  set "EXE=%ROOT%x64\Release\SysMonitor.exe"
) else (
  echo ERROR: Unknown config "%CONFIG%". Use: debug or release
  exit /b 2
)

if not exist "%EXE%" (
  echo ERROR: Exe not found: "%EXE%"
  echo Build first: build_debug.bat or build_release.bat
  exit /b 1
)

echo Starting: "%EXE%"
start "SysMonitor" "%EXE%"
exit /b 0
