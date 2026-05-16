# SysMonitor

SysMonitor 是一個輕量級的 Windows 背景監控工具，可以在本機顯示 CPU、GPU、記憶體與網路資訊，也可以開啟內建 TCP Server，讓手機、Mac、Linux 或其他電腦即時讀取這台 Windows 電腦的監控資料。

![SysMonitor app screenshot](docs/images/screenshot.jpg)

## 主要功能

- 顯示 CPU 型號、總使用率、GPU 型號、記憶體與網路資訊。
- 透過 ETW 估算目前前景視窗的 FPS。
- 內建 TCP Server，以 UTF-8 JSON Lines 格式每秒推送一次資料。
- 預設 Port 為 `6666`，可以在 UI 中修改。
- 支援背景常駐：最小化或關閉視窗後會縮到系統工具列。
- 支援開機自動啟動，啟動後會以小工具形式常駐在工具列。
- 使用 Windows 原生 Win32 C++ 製作，不使用 Electron 或瀏覽器 runtime。

## 下載與安裝

1. 到 GitHub Releases 下載最新版的 `SysMonitor_Setup_*.exe`。
2. 執行安裝檔並依照畫面完成安裝。
3. 第一次啟動時 Windows 可能會跳出 UAC 權限確認，這是因為 FPS 監控使用 ETW，且開機自動啟動會建立 Windows 工作排程。
4. 安裝後可以從開始功能表啟動 SysMonitor，也可以在 Windows「已安裝的應用程式」中解除安裝。

## 基本使用

開啟程式後，視窗會顯示目前系統狀態。按下 **Start** 會啟動 TCP Server，狀態會顯示 `Listening on port 6666`；按下 **Stop** 則會停止 Server。

勾選 **Run at startup** 後，SysMonitor 會在登入 Windows 時自動啟動並縮到系統工具列。取消勾選即可停用開機自動啟動。

最小化或按右上角關閉視窗時，程式不會直接結束，而是縮到系統工具列背景執行。若要完全關閉程式，請在工具列圖示上按右鍵，選擇 **Stop SysMonitor**。

## 從其他裝置讀取資料

SysMonitor 的 TCP Server 是主動推送模式。Client 連線後不需要送任何指令，只要持續讀取即可。

在 macOS 或 Linux 上可以使用：

```bash
nc <Windows_IP> 6666
```

在 Windows PowerShell 上可以使用：

```powershell
$client = New-Object System.Net.Sockets.TcpClient("<Windows_IP>", 6666)
$stream = $client.GetStream()
$reader = New-Object System.IO.StreamReader($stream, [Text.Encoding]::UTF8)
while ($true) {
  $line = $reader.ReadLine()
  if ($null -eq $line) { break }
  $line
}
```

如果其他裝置連不上，請確認：

- SysMonitor 已按下 **Start**。
- Client 使用的是正確的 Windows IP 與 Port。
- Windows 防火牆允許 SysMonitor 或該 Port 的入站連線。
- 兩台裝置在可互通的網路環境中。

## 傳出資料

每一行都是一個完整 JSON object，內容包含：

- `cpu`：CPU 型號、總使用率、每核心使用率。
- `gpu`：GPU 型號、Dedicated/Shared 記憶體資訊。
- `memory`：實體記憶體總量、可用量、使用量與使用率。
- `network`：MAC address 與 IP 清單。
- `fps`：目前前景視窗 PID、視窗標題與 FPS 估算值。

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

目前同時間只服務一個 client；若需要換裝置讀取，請先中斷原本的連線。

## 隱私與安全提醒

啟動 TCP Server 後，只要能連到這台 Windows 電腦指定 Port 的裝置，就可以讀取監控資料。資料中可能包含 MAC address、內網 IP、前景視窗標題與硬體資訊。

建議只在可信任的家用或私人網路中使用，不建議在公共 Wi-Fi、公司訪客網路或不受信任的環境中開啟 Server。

## 自行編譯

需要 Visual Studio 2022 或 Visual Studio Build Tools，並安裝 MSBuild、MSVC C++ 工具鏈與 Windows SDK。

在專案根目錄執行：

```batch
build_debug.bat    REM 編譯 Debug x64
build_release.bat  REM 編譯 Release x64
run.bat            REM 執行 Debug 版
run.bat release    REM 執行 Release 版
```

## 授權

SysMonitor 採用 MIT License。應用程式圖示基於 Lucide Icons 的 `activity` icon 製作，授權資訊請見 `assets/THIRD_PARTY_NOTICES.txt`。
