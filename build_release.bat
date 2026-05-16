@echo off
setlocal

rem Build SysMonitor.sln (x64 Release)

set "ROOT=%~dp0"
set "SLN=%ROOT%SysMonitor.sln"

if not exist "%SLN%" (
  echo ERROR: Solution not found: "%SLN%"
  exit /b 1
)

call "%ROOT%generate_build_version.bat"
if errorlevel 1 exit /b 1

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo ERROR: vswhere not found: "%VSWHERE%"
  echo Install Visual Studio 2022 or Build Tools.
  exit /b 1
)

set "TMPFILE=%TEMP%\sysmonitor_msbuild_path.txt"
if exist "%TMPFILE%" del /q "%TMPFILE%" >nul 2>&1

"%VSWHERE%" -latest -products * -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe > "%TMPFILE%"

for /f "usebackq delims=" %%I in ("%TMPFILE%") do (
  set "MSBUILD=%%I"
  goto :msbuild_found
)

echo ERROR: MSBuild.exe not found via vswhere.
echo Install MSBuild (Visual Studio / Build Tools).
exit /b 1

:msbuild_found
if exist "%TMPFILE%" del /q "%TMPFILE%" >nul 2>&1
echo Using MSBuild: "%MSBUILD%"

"%MSBUILD%" "%SLN%" /m /p:Configuration=Release /p:Platform=x64
exit /b %ERRORLEVEL%
