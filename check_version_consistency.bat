@echo off
setlocal

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0check_version_consistency.ps1"
exit /b %ERRORLEVEL%
