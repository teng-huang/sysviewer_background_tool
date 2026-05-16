@echo off
setlocal

set "ROOT=%~dp0"
cd /d "%ROOT%"

set "GIT_BRANCH=unknown"
set "GIT_COMMIT=unknown"
set "GIT_DIRTY=0"

for /f "usebackq delims=" %%I in (`git rev-parse --abbrev-ref HEAD 2^>nul`) do set "GIT_BRANCH=%%I"
for /f "usebackq delims=" %%I in (`git rev-parse --short HEAD 2^>nul`) do set "GIT_COMMIT=%%I"

git diff --quiet --ignore-submodules -- 2>nul
if errorlevel 1 set "GIT_DIRTY=1"

git diff --cached --quiet --ignore-submodules -- 2>nul
if errorlevel 1 set "GIT_DIRTY=1"

(
  echo #pragma once
  echo #define SYSMON_GIT_BRANCH "%GIT_BRANCH%"
  echo #define SYSMON_GIT_COMMIT "%GIT_COMMIT%"
  echo #define SYSMON_GIT_DIRTY %GIT_DIRTY%
) > "%ROOT%build_version.generated.h"

endlocal
exit /b 0
