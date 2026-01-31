# SysMonitor Agent Notes

This repo is a Visual Studio C++ (VCXPROJ/SLN) Windows app.

## Quick commands

From the repo root:

- Build Debug (x64): `build_debug.bat`
- Build Release (x64): `build_release.bat`
- Run Debug (x64): `run.bat` or `run.bat debug`
- Run Release (x64): `run.bat release`

## What the scripts do

- `build_debug.bat` / `build_release.bat`
  - Uses `vswhere.exe` to locate `MSBuild.exe` from the latest installed Visual Studio / Build Tools.
  - Runs MSBuild on `SysMonitor.sln` with `Platform=x64` and `Configuration=Debug` or `Release`.

- `run.bat`
  - Starts the built executable from `x64\Debug\SysMonitor.exe` or `x64\Release\SysMonitor.exe`.

## Requirements

- Visual Studio 2022 (any edition) or Visual Studio Build Tools installed.
- `vswhere.exe` at:
  - `%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe`

## Troubleshooting

- If you see "vswhere not found" or "MSBuild.exe not found":
  - Install Visual Studio 2022 or Build Tools, including the **MSBuild** component and the C++ toolchain.
- If `run.bat` says the EXE is missing:
  - Run `build_debug.bat` or `build_release.bat` first.
