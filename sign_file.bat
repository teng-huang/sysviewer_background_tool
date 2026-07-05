@echo off
setlocal EnableExtensions

if "%~1"=="" (
  echo Usage: sign_file.bat path\to\file.exe
  exit /b 2
)

set "TARGET=%~1"
if not exist "%TARGET%" (
  echo ERROR: File not found: %TARGET%
  exit /b 1
)

call :find_signtool
if "%SIGNTOOL%"=="" (
  echo ERROR: signtool.exe not found.
  echo Install the Windows SDK, or set SYSMON_SIGNTOOL to the full signtool.exe path.
  exit /b 1
)

set "TIMESTAMP_URL=%SYSMON_TIMESTAMP_URL%"
if "%TIMESTAMP_URL%"=="" set "TIMESTAMP_URL=http://timestamp.digicert.com"

set "DIGEST=%SYSMON_SIGN_DIGEST%"
if "%DIGEST%"=="" set "DIGEST=SHA256"

if not "%SYSMON_SIGN_PFX%"=="" (
  if not exist "%SYSMON_SIGN_PFX%" (
    echo ERROR: SYSMON_SIGN_PFX does not exist: %SYSMON_SIGN_PFX%
    exit /b 1
  )
  if not "%SYSMON_SIGN_PASSWORD%"=="" (
    "%SIGNTOOL%" sign /fd %DIGEST% /td %DIGEST% /tr "%TIMESTAMP_URL%" /f "%SYSMON_SIGN_PFX%" /p "%SYSMON_SIGN_PASSWORD%" "%TARGET%"
  ) else (
    "%SIGNTOOL%" sign /fd %DIGEST% /td %DIGEST% /tr "%TIMESTAMP_URL%" /f "%SYSMON_SIGN_PFX%" "%TARGET%"
  )
) else if not "%SYSMON_SIGN_THUMBPRINT%"=="" (
  "%SIGNTOOL%" sign /fd %DIGEST% /td %DIGEST% /tr "%TIMESTAMP_URL%" /sha1 "%SYSMON_SIGN_THUMBPRINT%" "%TARGET%"
) else if not "%SYSMON_SIGN_SUBJECT%"=="" (
  "%SIGNTOOL%" sign /fd %DIGEST% /td %DIGEST% /tr "%TIMESTAMP_URL%" /n "%SYSMON_SIGN_SUBJECT%" "%TARGET%"
) else if /i "%SYSMON_SIGN_AUTO%"=="1" (
  "%SIGNTOOL%" sign /fd %DIGEST% /td %DIGEST% /tr "%TIMESTAMP_URL%" /a "%TARGET%"
) else (
  echo ERROR: No signing certificate configured.
  echo Set one of:
  echo   SYSMON_SIGN_PFX=path\to\cert.pfx
  echo   SYSMON_SIGN_THUMBPRINT=certificate_thumbprint
  echo   SYSMON_SIGN_SUBJECT=certificate_subject
  echo   SYSMON_SIGN_AUTO=1
  exit /b 1
)

if errorlevel 1 (
  echo ERROR: Signing failed: %TARGET%
  exit /b 1
)

"%SIGNTOOL%" verify /pa /v "%TARGET%"
if errorlevel 1 (
  echo ERROR: Signature verification failed: %TARGET%
  exit /b 1
)

exit /b 0

:find_signtool
set "SIGNTOOL=%SYSMON_SIGNTOOL%"
if not "%SIGNTOOL%"=="" (
  if exist "%SIGNTOOL%" exit /b 0
  echo ERROR: SYSMON_SIGNTOOL does not exist: %SIGNTOOL%
  set "SIGNTOOL="
  exit /b 1
)

where signtool.exe >nul 2>&1
if not errorlevel 1 (
  for /f "delims=" %%i in ('where signtool.exe 2^>nul') do (
    set "SIGNTOOL=%%i"
    exit /b 0
  )
)

for /d %%D in ("%ProgramFiles(x86)%\Windows Kits\10\bin\*") do (
  if exist "%%~fD\x64\signtool.exe" set "SIGNTOOL=%%~fD\x64\signtool.exe"
)
if not "%SIGNTOOL%"=="" exit /b 0

for /d %%D in ("%ProgramFiles%\Windows Kits\10\bin\*") do (
  if exist "%%~fD\x64\signtool.exe" set "SIGNTOOL=%%~fD\x64\signtool.exe"
)
exit /b 0
