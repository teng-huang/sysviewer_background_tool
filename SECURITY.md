# Security Policy

## Supported Versions

Security fixes are expected to target the latest release and the current `main` branch.

## Reporting a Vulnerability

Please do not open a public issue for a suspected vulnerability.

Preferred options:

- Use GitHub private vulnerability reporting if it is enabled for this repository.
- Otherwise, email the maintainer listed in `LICENSE`.

Please include:

- A short description of the issue.
- Steps to reproduce.
- Affected version or commit.
- Whether the TCP server, installer, startup task, ETW monitoring, or release artifact is involved.

## Security Notes

- The TCP server is unauthenticated. By default it listens only on `127.0.0.1`; LAN access must be enabled explicitly in the UI.
- When LAN access is enabled, any device that can reach the selected port can read the monitoring JSON.
- The app and installer may require administrator privileges because FPS monitoring uses ETW and startup launch uses an elevated scheduled task.
- Release installers are not code-signed yet. Verify downloads with `SHA256SUMS.txt` and `BUILD_INFO.txt`.
