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
- Includes a built-in TCP server that pushes UTF-8 JSON Lines data once per second; it listens on `127.0.0.1` by default, and LAN access must be enabled explicitly.<br>
  內建 TCP Server，以 UTF-8 JSON Lines 格式每秒推送一次資料；預設只監聽 `127.0.0.1`，需要明確啟用 LAN 存取才會開放給其他裝置。
- Uses port `6666` by default, and the port can be changed in the UI.<br>
  預設 Port 為 `6666`，可以在 UI 中修改。
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

After opening the app, the window shows the current system status. Press **Start** to start the TCP server. By default the server listens on `127.0.0.1:6666`; check **Allow LAN connections** before starting if you want other devices on your network to connect. Press **Stop** to stop the server.<br>
開啟程式後，視窗會顯示目前系統狀態。按下 **Start** 會啟動 TCP Server。預設 Server 只監聽 `127.0.0.1:6666`；如果要讓同網路的其他裝置連線，請在啟動前勾選 **Allow LAN connections**。按下 **Stop** 則會停止 Server。

When **Run at startup** is checked, SysMonitor creates an elevated scheduled task, automatically starts when you sign in to Windows, and minimizes to the system tray. Uncheck it to disable startup launch.<br>
勾選 **Run at startup** 後，SysMonitor 會建立提高權限的工作排程，在登入 Windows 時自動啟動並縮到系統工具列。取消勾選即可停用開機自動啟動。

When you minimize the window or close it with the top-right close button, the app does not exit directly; it keeps running in the background from the system tray. To fully close it, right-click the tray icon and choose **Stop SysMonitor**.<br>
最小化或按右上角關閉視窗時，程式不會直接結束，而是縮到系統工具列背景執行。若要完全關閉程式，請在工具列圖示上按右鍵，選擇 **Stop SysMonitor**。

If SysMonitor is already running, launching it again will bring the existing window to the foreground instead of starting another process.<br>
如果 SysMonitor 已經在執行，再次啟動會把既有視窗叫到前景，不會再開一個新的 process。

## Read Data from Another Device
從其他裝置讀取資料

SysMonitor's TCP server actively pushes data. After a client connects, it does not need to send any command; it only needs to keep reading. By default, connect from the same Windows PC with `127.0.0.1`. To connect from another device, enable **Allow LAN connections** before pressing **Start**.<br>
SysMonitor 的 TCP Server 是主動推送模式。Client 連線後不需要送任何指令，只要持續讀取即可。預設請在同一台 Windows 電腦上用 `127.0.0.1` 連線；若要從其他裝置連線，請先勾選 **Allow LAN connections** 再按 **Start**。

On macOS or Linux, after LAN access is enabled, use:<br>
在 macOS 或 Linux 上，啟用 LAN 存取後可以使用：

```bash
nc <Windows_IP> 6666
```

On the same Windows PC, use PowerShell with `127.0.0.1`; replace it with the Windows LAN IP only when LAN access is enabled:<br>
在同一台 Windows 電腦上，可以用 PowerShell 連到 `127.0.0.1`；只有啟用 LAN 存取時，才需要改成 Windows 的區網 IP：

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

- **Start** has been pressed in SysMonitor.<br>
  SysMonitor 已按下 **Start**。
- **Allow LAN connections** was enabled before starting the server if the client is on another device.<br>
  如果 client 在另一台裝置上，啟動 Server 前已勾選 **Allow LAN connections**。
- The client is using the correct Windows IP address and port.<br>
  Client 使用的是正確的 Windows IP 與 Port。
- Windows Firewall allows inbound connections for SysMonitor or the selected port.<br>
  Windows 防火牆允許 SysMonitor 或該 Port 的入站連線。
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

By default, the TCP server only accepts connections from the same PC through `127.0.0.1`. When **Allow LAN connections** is enabled, any device that can reach the selected port on this Windows PC can read the monitoring data. The data may include the MAC address, local network IP address, foreground window title, and hardware information.<br>
預設情況下，TCP Server 只接受同一台電腦透過 `127.0.0.1` 連線。啟用 **Allow LAN connections** 後，只要能連到這台 Windows 電腦指定 Port 的裝置，就可以讀取監控資料。資料中可能包含 MAC address、內網 IP、前景視窗標題與硬體資訊。

Use it only on trusted home or private networks. Avoid enabling the server on public Wi-Fi, company guest networks, or other untrusted environments.<br>
建議只在可信任的家用或私人網路中使用，不建議在公共 Wi-Fi、公司訪客網路或不受信任的環境中開啟 Server。

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

## Contributing and Security
貢獻與安全

For contribution guidelines, see `CONTRIBUTING.md`. For private vulnerability reporting, see `SECURITY.md`.<br>
貢獻流程請參考 `CONTRIBUTING.md`。如果要私下回報安全漏洞，請參考 `SECURITY.md`。

## License
授權

SysMonitor is released under the MIT License. The app icon is based on the Lucide Icons `activity` icon; see `assets/THIRD_PARTY_NOTICES.txt` for license details.<br>
SysMonitor 採用 MIT License。應用程式圖示基於 Lucide Icons 的 `activity` icon 製作，授權資訊請見 `assets/THIRD_PARTY_NOTICES.txt`。
