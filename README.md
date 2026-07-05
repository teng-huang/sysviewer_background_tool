# SysMonitor

SysMonitor is a lightweight native Windows monitoring tool that shows CPU, GPU, memory, and network information locally, and can also start a built-in TCP server so this PC, or other devices when LAN access is enabled, can read monitoring data in real time.<br>
SysMonitor 是一個輕量級的 Windows 原生監控工具，可以在本機顯示 CPU、GPU、記憶體與網路資訊，也可以開啟內建 TCP Server，讓本機或已啟用 LAN 存取時的手機、Mac、Linux、其他電腦即時讀取監控資料。

![SysMonitor app screenshot](docs/images/screenshot.jpg)

## Key Features
主要功能

- Shows the CPU model, total CPU usage, GPU model, memory information, and network information.<br>
  顯示 CPU 型號、總使用率、GPU 型號、記憶體與網路資訊。
- Estimates the FPS of the current foreground window through ETW.<br>
  透過 ETW 估算目前前景視窗的 FPS。
- Includes a built-in LAN TCP server that pushes UTF-8 JSON Lines data once per second.<br>
  內建 LAN TCP Server，以 UTF-8 JSON Lines 格式每秒推送一次資料。
- Advertises the LAN TCP server with Bonjour / DNS-SD as `_sysviewer._tcp.local`, so iPhone apps can discover the agent automatically.<br>
  會用 Bonjour / DNS-SD 以 `_sysviewer._tcp.local` 發布這台 agent，讓 iPhone app 可以自動發現。
- Uses port `6666` by default, and the port can be changed in the UI.<br>
  預設 Port 為 `6666`，可以在 UI 中修改。
- Starts the local TCP server automatically on launch by default; this can be disabled with **Start server on launch**.<br>
  預設啟動程式後會自動開啟本機 TCP Server；可用 **Start server on launch** 取消。
- Supports background operation: minimizing or closing the window sends it to the system tray.<br>
  支援背景常駐：最小化或關閉視窗後會縮到系統工具列。
- Supports optional launch at startup, then stays in the system tray as a small utility.<br>
  支援可選的開機自動啟動，啟動後會以小工具形式常駐在工具列。
- Allows only one running instance; starting SysMonitor again brings the existing window forward instead of launching a second copy.<br>
  同時間只允許執行一個 SysMonitor；再次啟動時會叫出既有視窗，不會開第二份程式。
- Built as a native Windows x64 C++ desktop app using the Win32 API, without Electron or a browser runtime.<br>
  使用 Win32 API 製作的 Windows 原生 x64 C++ 桌面程式，不使用 Electron 或瀏覽器 runtime。

## Download and Install
下載與安裝

1. Download the latest `SysMonitor_Setup_*.exe` from GitHub Releases.<br>
   到 GitHub Releases 下載最新版的 `SysMonitor_Setup_*.exe`。
2. Run the installer and follow the on-screen steps.<br>
   執行安裝檔並依照畫面完成安裝。
3. On first launch, Windows may show a UAC permission prompt because FPS monitoring uses ETW. If you enable startup launch, SysMonitor creates an elevated Windows scheduled task.<br>
   第一次啟動時 Windows 可能會跳出 UAC 權限確認，這是因為 FPS 監控使用 ETW。如果你啟用開機自動啟動，SysMonitor 會建立提高權限的 Windows 工作排程。
4. After installation, you can launch SysMonitor from the Start menu, or uninstall it from Windows Installed apps.<br>
   安裝後可以從開始功能表啟動 SysMonitor，也可以在 Windows「已安裝的應用程式」中解除安裝。

## Verify Downloads
驗證下載檔

Each GitHub Release includes `SHA256SUMS.txt` and `BUILD_INFO.txt`. `BUILD_INFO.txt` records the GitHub Actions run and commit that produced the installer; `SHA256SUMS.txt` can be used to confirm that the installer you downloaded matches the file produced by GitHub Actions.<br>
GitHub Release 會附上 `SHA256SUMS.txt` 與 `BUILD_INFO.txt`。`BUILD_INFO.txt` 會記錄產生該安裝檔的 GitHub Actions run 與 commit；`SHA256SUMS.txt` 可以用來確認你下載到的安裝檔和 GitHub Actions 產出的檔案一致。

Check it in PowerShell with:<br>
在 PowerShell 中可以這樣檢查：

```powershell
Get-FileHash .\SysMonitor_Setup_1.0.1.exe -Algorithm SHA256
```

Compare the output SHA256 value with `SHA256SUMS.txt` in the Release.<br>
把輸出的 SHA256 值和 Release 裡的 `SHA256SUMS.txt` 對照即可。

The installer is not currently signed with an official code signing certificate. Windows Smart App Control or Microsoft Defender SmartScreen may still show warnings or block newly downloaded unsigned installers; open source code and SHA256 checks help verify the file source, but they do not replace Windows code signing trust.<br>
目前安裝檔尚未使用正式 code signing certificate 簽章。Windows Smart App Control 或 Microsoft Defender SmartScreen 仍可能對新下載、未簽章的安裝檔顯示警告或封鎖；開源和 SHA256 可以協助確認檔案來源，但不能取代 Windows 的程式碼簽章信任。

## Basic Usage
基本使用

After opening the app, the window shows the current system status and starts the LAN TCP server automatically when **Start server on launch** is checked, which is the default. **Allow LAN connections** is always checked and cannot be changed. Uncheck **Start server on launch** if you prefer to start the server manually next time.<br>
開啟程式後，視窗會顯示目前系統狀態；預設勾選 **Start server on launch**，因此會自動啟動 LAN TCP Server。**Allow LAN connections** 會永遠保持勾選且不可變更。若下次想手動啟動，取消勾選 **Start server on launch** 即可。

When the server starts successfully, SysMonitor also advertises itself on the local network with Bonjour / DNS-SD. The service type is `_sysviewer._tcp`, and the advertised instance name is `SysViewer-192.168.0.146`.<br>
Server 成功啟動後，SysMonitor 也會透過 Bonjour / DNS-SD 在區網中發布自己。Service type 是 `_sysviewer._tcp`，發布的 instance name 是 `SysViewer-192.168.0.146`。

When **Run at startup** is checked, SysMonitor creates an elevated scheduled task, automatically starts when you sign in to Windows, and minimizes to the system tray. Uncheck it to disable startup launch.<br>
勾選 **Run at startup** 後，SysMonitor 會建立提高權限的工作排程，在登入 Windows 時自動啟動並縮到系統工具列。取消勾選即可停用開機自動啟動。

When you minimize the window or close it with the top-right close button, the app does not exit directly; it keeps running in the background from the system tray. To fully close it, right-click the tray icon and choose **Stop SysMonitor**.<br>
最小化或按右上角關閉視窗時，程式不會直接結束，而是縮到系統工具列背景執行。若要完全關閉程式，請在工具列圖示上按右鍵，選擇 **Stop SysMonitor**。

If SysMonitor is already running, launching it again will bring the existing window to the foreground instead of starting another process.<br>
如果 SysMonitor 已經在執行，再次啟動會把既有視窗叫到前景，不會再開一個新的 process。

## Read Data from Another Device
從其他裝置讀取資料

SysMonitor's TCP server actively pushes data. After a client connects, it does not need to send any command; it only needs to keep reading. From another device, connect to the Windows LAN IP and selected port.<br>
SysMonitor 的 TCP Server 是主動推送模式。Client 連線後不需要送任何指令，只要持續讀取即可。若要從其他裝置連線，請連到 Windows 的區網 IP 與指定 Port。

For an iPhone app, browse Bonjour services with type `_sysviewer._tcp`. After resolving the service, connect to the resolved host and port with TCP and read UTF-8 JSON Lines from the stream.<br>
iPhone app 可以掃描 Bonjour service type `_sysviewer._tcp`。Resolve 到 service 後，用 TCP 連到解析出的 host 與 port，並從 stream 讀取 UTF-8 JSON Lines。

On macOS or Linux, use:<br>
在 macOS 或 Linux 上可以使用：

```bash
nc <Windows_IP> 6666
```

On the same Windows PC, use PowerShell with `127.0.0.1`; from another device, use the Windows LAN IP instead:<br>
在同一台 Windows 電腦上，可以用 PowerShell 連到 `127.0.0.1`；從其他裝置連線時，請改用 Windows 的區網 IP：

```powershell
$client = New-Object System.Net.Sockets.TcpClient("127.0.0.1", 6666)
$stream = $client.GetStream()
$reader = New-Object System.IO.StreamReader($stream, [Text.Encoding]::UTF8)
while ($true) {
  $line = $reader.ReadLine()
  if ($null -eq $line) { break }
  $line
}
```

If another device cannot connect, check that:<br>
如果其他裝置連不上，請確認：

- The TCP server is running in SysMonitor, either from **Start server on launch** or by pressing **Start** manually.<br>
  SysMonitor 的 TCP Server 正在執行，可由 **Start server on launch** 自動啟動，或手動按下 **Start**。
- **Allow LAN connections** is checked and disabled in the UI.<br>
  **Allow LAN connections** 在 UI 中保持勾選且不可選。
- The client is using the correct Windows IP address and port.<br>
  Client 使用的是正確的 Windows IP 與 Port。
- Windows Firewall allows inbound connections for SysMonitor or the selected port.<br>
  Windows 防火牆允許 SysMonitor 或該 Port 的入站連線。
- Bonjour discovery also requires local-network multicast DNS traffic on UDP port `5353` to be allowed by the network and firewall.<br>
  Bonjour 自動發現也需要網路與防火牆允許 UDP `5353` 的 local-network multicast DNS 流量。
- Both devices are on a network where they can reach each other.<br>
  兩台裝置在可互通的網路環境中。

## Output Data
傳出資料

Each line is a complete JSON object containing:<br>
每一行都是一個完整 JSON object，內容包含：

- `cpu`: CPU model, total CPU usage, and per-core usage.<br>
  `cpu`：CPU 型號、總使用率、每核心使用率。
- `gpu`: GPU model and Dedicated/Shared memory information.<br>
  `gpu`：GPU 型號、Dedicated/Shared 記憶體資訊。
- `memory`: Total physical memory, available memory, used memory, and usage percentage.<br>
  `memory`：實體記憶體總量、可用量、使用量與使用率。
- `network`: MAC address and IP list.<br>
  `network`：MAC address 與 IP 清單。
- `fps`: Current foreground window PID, window title, and estimated FPS.<br>
  `fps`：目前前景視窗 PID、視窗標題與 FPS 估算值。

Simplified example:<br>
簡化範例：

```json
{
  "cpu": { "name": "Intel(R) ...", "usage_total_percent": 12.3 },
  "gpu": { "name": "NVIDIA ...", "dedicated_bytes": 123456789 },
  "memory": { "used_phys_percent": 37.5 },
  "network": { "mac": "aa:bb:cc:dd:ee:ff", "ips": ["192.168.0.146"] },
  "fps": { "ok": true, "pid": 12345, "window_title": "MyGame", "value": 144.2 }
}
```

Only one client is served at a time. To read from another device, disconnect the existing client first.<br>
目前同時間只服務一個 client；若需要換裝置讀取，請先中斷原本的連線。

## Privacy and Security
隱私與安全提醒

The TCP server always allows LAN connections and SysMonitor advertises itself with Bonjour. Any device that can reach the selected port on this Windows PC can read the monitoring data. The data may include the MAC address, local network IP address, foreground window title, and hardware information.<br>
TCP Server 永遠允許 LAN 連線，且 SysMonitor 會透過 Bonjour 發布自己。只要能連到這台 Windows 電腦指定 Port 的裝置，就可以讀取監控資料。資料中可能包含 MAC address、內網 IP、前景視窗標題與硬體資訊。

Use it only on trusted home or private networks. Avoid enabling the server on public Wi-Fi, company guest networks, or other untrusted environments.<br>
建議只在可信任的家用或私人網路中使用，不建議在公共 Wi-Fi、公司訪客網路或不受信任的環境中開啟 Server。

## Maintainer Status and Maintenance Policy
維護者狀態與維護規範

SysMonitor is an open-source project maintained as a practical Windows utility for real-time local hardware telemetry and LAN sharing scenarios.
It is currently published under a public repository with regular release-based maintenance, and the latest stable release is `v1.0.1` with published installer artifacts and reproducible build guidance.
<br>
SysMonitor 為輕量 Windows 原生監控工具，採公開倉庫維護，持續以發佈版本為節點進行維護，目前主要穩定版本為 `v1.0.1`，並提供可驗證的發布檔與建置流程。

Core maintainer workflow:

- Monitor project health by release cadence (build and publish checks).
- Keep release binaries and metadata consistent (`BUILD_INFO.txt`, `SHA256SUMS.txt`, version headers).
- Review issues/requests, improve reliability, and document breaking behavior changes in README and release notes.
- Prioritize safety in network exposure, startup scheduling, and privilege-required features.

核心維護作法：

- 以發佈版本節奏持續維護，確保版本資訊一致。
- 保持發佈產物對照完整（`BUILD_INFO.txt`、`SHA256SUMS.txt`、版本資訊）。
- 回應需求回報，優先修正穩定性與安全性問題，並同步更新 README。
- 對外網路連線、開機啟動與權限敏感功能保有安全優先原則。

## Adoption and Real-World Use Cases
採用情境

The project is designed for practical use cases where lightweight desktop monitoring is needed without heavy runtime dependencies:

- Lightweight desktop monitoring for Windows PCs in home/office setups.
- Real-time telemetry for iOS/Android/macOS clients in the same LAN.
- Baseline telemetry data source for simple maintenance scripts or future automation hooks.

專案設計目標是「小而穩的桌面監控」：

- 在家用/小型辦公環境替代複雜監控工具，提供即時硬體與網路狀態。
- 作為同網段手機、平板、其他電腦的即時監控資料來源。
- 作為後續加入維護自動化、告警流程與簡易排查腳本的資料基礎。

## Build from Source
自行編譯

Visual Studio 2022 or Visual Studio Build Tools is required, with MSBuild, the MSVC C++ toolchain, and the Windows SDK installed.<br>
需要 Visual Studio 2022 或 Visual Studio Build Tools，並安裝 MSBuild、MSVC C++ 工具鏈與 Windows SDK。

Run these commands from the project root:<br>
在專案根目錄執行：

```batch
build_debug.bat    REM Build Debug x64 / 編譯 Debug x64
build_release.bat  REM Build Release x64 / 編譯 Release x64
run.bat            REM Run Debug build / 執行 Debug 版
run.bat release    REM Run Release build / 執行 Release 版
```

Before release or installer changes, check version consistency and build the installer:<br>
在修改釋出或安裝檔相關內容前，請檢查版本一致性並建立安裝檔：

```batch
.\check_version_consistency.bat
```

```batch
build_installer.bat
```

To build a signed installer for Smart App Control / SmartScreen, configure a trusted code-signing certificate first, then run:<br>
若要建立可通過 Smart App Control / SmartScreen 信任檢查的簽章安裝檔，請先設定受信任的 code-signing certificate，再執行：

```batch
set SYSMON_SIGN_PFX=C:\path\to\certificate.pfx
set SYSMON_SIGN_PASSWORD=your_pfx_password
build_signed_installer.bat
```

Or sign with a certificate already installed in the Windows certificate store:<br>
也可以使用已安裝在 Windows certificate store 的憑證簽章：

```batch
set SYSMON_SIGN_THUMBPRINT=certificate_thumbprint
build_signed_installer.bat
```

## Contributing and Security
貢獻與安全

For contribution guidelines, see `CONTRIBUTING.md`. For private vulnerability reporting, see `SECURITY.md`.<br>
貢獻流程請參考 `CONTRIBUTING.md`。如果要私下回報安全漏洞，請參考 `SECURITY.md`。

## License
授權

SysMonitor is released under the MIT License. The app icon is based on the Lucide Icons `activity` icon; see `assets/THIRD_PARTY_NOTICES.txt` for license details.<br>
SysMonitor 採用 MIT License。應用程式圖示基於 Lucide Icons 的 `activity` icon 製作，授權資訊請見 `assets/THIRD_PARTY_NOTICES.txt`。
