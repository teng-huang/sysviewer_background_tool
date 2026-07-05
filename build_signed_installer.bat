@echo off
setlocal EnableExtensions

rem Build Release, sign SysMonitor.exe, create the installer, and sign the installer.
rem Configure a trusted code-signing certificate before running this script:
rem   set SYSMON_SIGN_PFX=C:\path\to\certificate.pfx
rem   set SYSMON_SIGN_PASSWORD=your_pfx_password
rem or:
rem   set SYSMON_SIGN_THUMBPRINT=certificate_thumbprint

set "ROOT=%~dp0"
cd /d "%ROOT%"

set "APP_VERSION="
for /f "tokens=3" %%v in ('findstr /b /c:"#define MyAppVersion" SysMonitor.iss') do set "APP_VERSION=%%~v"
if "%APP_VERSION%"=="" (
  echo ERROR: Could not read MyAppVersion from SysMonitor.iss.
  exit /b 1
)

echo Building Release...
call build_release.bat
if errorlevel 1 (
  echo ERROR: Build failed.
  exit /b 1
)

echo Signing Release executable...
call sign_file.bat "x64\Release\SysMonitor.exe"
if errorlevel 1 exit /b 1

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
  exit /b 1
)

echo Creating and signing installer...
"%ISCC%" /DSIGNED_BUILD "/Ssigntool=cmd /c ""%ROOT%sign_file.bat"" $f" "SysMonitor.iss"
if errorlevel 1 (
  echo ERROR: Inno Setup compile failed.
  exit /b 1
)

set "INSTALLER=installer\SysMonitor_Setup_%APP_VERSION%.exe"
if not exist "%INSTALLER%" (
  echo ERROR: Installer not found: %INSTALLER%
  exit /b 1
)

echo Verifying installer signature...
powershell -NoProfile -ExecutionPolicy Bypass -Command "$sig = Get-AuthenticodeSignature -LiteralPath '%INSTALLER%'; $sig | Format-List Path,Status,SignerCertificate; if ($sig.Status -ne 'Valid') { exit 1 }"
if errorlevel 1 (
  echo ERROR: Installer signature is not valid: %INSTALLER%
  exit /b 1
)

echo.
echo Signed installer:
dir "%INSTALLER%"
exit /b 0
