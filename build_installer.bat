@echo off
setlocal

rem Build SysMonitor Release and create installer with Inno Setup
rem Prerequisite: Inno Setup 6 installed (winget install JRSoftware.InnoSetup)

set "ROOT=%~dp0"
cd /d "%ROOT%"

rem 1. Build Release if needed
if not exist "x64\Release\SysMonitor.exe" (
  echo Building Release...
  call build_release.bat
  if errorlevel 1 (
    echo ERROR: Build failed.
    exit /b 1
  )
) else (
  echo SysMonitor.exe found, skip build.
)

rem 2. Find ISCC.exe (try where, then common paths)
set "ISCC="
where iscc >nul 2>&1 && for /f "tokens=*" %%i in ('where iscc 2^>nul') do set "ISCC=%%i" & goto :iscc_found
if exist "%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe" set "ISCC=%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe"
if exist "%ProgramFiles%\Inno Setup 6\ISCC.exe" set "ISCC=%ProgramFiles%\Inno Setup 6\ISCC.exe"
if exist "%LocalAppData%\Programs\Inno Setup 6\ISCC.exe" set "ISCC=%LocalAppData%\Programs\Inno Setup 6\ISCC.exe"
if exist "%UserProfile%\AppData\Local\Programs\Inno Setup 6\ISCC.exe" set "ISCC=%UserProfile%\AppData\Local\Programs\Inno Setup 6\ISCC.exe"
:iscc_found
if "%ISCC%"=="" (
  echo ERROR: Inno Setup not found.
  echo Install: winget install JRSoftware.InnoSetup
  echo Or download: https://jrsoftware.org/isdl.php
  echo After install, open a NEW terminal and run build_installer.bat again.
  exit /b 1
)

rem 3. Create installer
echo Creating installer...
"%ISCC%" "SysMonitor.iss"
if errorlevel 1 (
  echo ERROR: Inno Setup compile failed.
  exit /b 1
)

echo.
echo Done. Installer: installer\SysMonitor_Setup_1.0.exe
exit /b 0
