# Contributing to SysMonitor

Thanks for helping improve SysMonitor. This project is a native Windows C++ app, so changes are easiest to review when they stay close to the existing Win32/C++17 style.

## Development Setup

- Windows 10 or later.
- Visual Studio 2022 or Visual Studio Build Tools.
- MSBuild, MSVC v143 C++ toolchain, and Windows SDK.
- Inno Setup 6 if you need to build the installer.

## Build Commands

Run from the repository root:

```bat
build_debug.bat
build_release.bat
run.bat
run.bat release
```

Before release or installer changes, also run:

```bat
build_installer.bat
```

Version consistency can be checked with:

```batch
.\check_version_consistency.bat
```

## Pull Request Guidelines

- Keep changes focused on one bug fix or feature.
- Update `README.md` when user-facing behavior, TCP JSON fields, setup behavior, or security expectations change.
- Update `version_info.h` and `SysMonitor.iss` together when changing the release version.
- Do not commit build outputs such as `x64/`, `installer/`, `dist/`, `.vs/`, dumps, or generated files.
- Mention what you tested in the PR description.

## Security-Sensitive Areas

Please be careful with changes involving:

- TCP server binding, ports, or JSON contents.
- Windows Task Scheduler / startup behavior.
- ETW FPS monitoring and administrator privileges.
- Installer behavior and release artifacts.

If you found a vulnerability, please follow `SECURITY.md` instead of opening a public issue first.
