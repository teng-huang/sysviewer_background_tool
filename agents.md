# SysMonitor Agent 說明（中文）

此專案是 Windows 原生 C++（Visual Studio `.sln`/`.vcxproj`）應用程式，提供：

- 系統資訊顯示（CPU/GPU/RAM/網路）
- 內建 TCP Server：讓其他裝置從網路上即時讀取監控資料（JSON Lines / UTF-8）

## 快速操作（在專案根目錄）

- 編譯 Debug (x64)：`build_debug.bat`
- 編譯 Release (x64)：`build_release.bat`
- 執行 Debug (x64)：`run.bat` 或 `run.bat debug`
- 執行 Release (x64)：`run.bat release`

## 這些腳本做了什麼

- `build_debug.bat` / `build_release.bat`
  - 透過 `vswhere.exe` 自動找到最新安裝的 Visual Studio / Build Tools 裡的 `MSBuild.exe`
  - 以 `Platform=x64`、`Configuration=Debug|Release` 編譯 `SysMonitor.sln`

- `run.bat`
  - 直接啟動編譯產物：`x64\Debug\SysMonitor.exe` 或 `x64\Release\SysMonitor.exe`

## TCP Server：如何連線取得資料

### 1) 在 Windows 端啟動服務

1. 先執行 `SysMonitor.exe`
2. 在 UI 裡按 **Start** 啟動 TCP Server（狀態會顯示 Listening on port ...）
3. 預設埠號：`6666`（UI 也可改）
4. 請確認 Windows 防火牆允許該程式或允許該埠號的「入站連線」

補充：此 TCP Server 為「單一客戶端」設計，同時間只會服務一個連線。

**安全提醒**：啟動後，同網段內任何裝置皆可連線取得監控資料（CPU/GPU/記憶體、IP、前景視窗標題等）。請勿在不受信任的網路環境使用。

### 2) 從其他電腦/裝置連線（最簡單：netcat）

在 macOS / Linux：

```bash
nc <Windows_IP> 6666
```

連上後會每秒收到 **一行 JSON**（UTF-8），每行以 CRLF 結尾（JSON Lines / NDJSON）。每一行是一個完整的 JSON object。

### TCP 推送資料格式（JSON）

採用常見的監控／時間序列格式：頂層含 `ts`（Unix 毫秒）、各裝置區塊，數值缺失時為 `null`，字串缺失時為 `""`。

頂層欄位：

- `ts`: integer（Unix 時間戳，毫秒）
- `cpu`
  - `name`: string（CPU 型號，空則 `""`）
  - `usage_percent`: number | null（總 CPU 使用率，0~100）
  - `per_core`: number[]（每個 logical core 的使用率 0~100，無資料則 `[]`）
- `gpu`
  - `name`: string（GPU 型號，空則 `""`）
  - `memory_used_bytes`: integer | null（Dedicated VRAM 使用量）
  - `memory_shared_bytes`: integer | null（Shared 使用量）
  - `memory_capacity_bytes`: integer | null（Dedicated 容量）
  - `memory_shared_capacity_bytes`: integer | null（Shared 容量）
  - `memory_usage_percent`: number | null（VRAM 使用率 0~100）
  - `utilization_percent`: number | null（GPU 運算使用率 0~100）
- `memory`
  - `total_bytes`: integer | null
  - `available_bytes`: integer | null
  - `used_bytes`: integer | null
  - `used_percent`: number | null（0~100）
- `network`
  - `mac`: string（無則 `""`）
  - `ips`: string[]（可能為空陣列）
- `fps`
  - `value`: number | null（FPS；取不到則 `null`）
  - `pid`: integer（前景視窗所屬 process id）
  - `window_title`: string（前景視窗標題；無則 `""`）
  - `source`: string（目前固定為 `"etw_present"`）

#### 範例（示意）

```json
{
  "ts": 1738226400123,
  "cpu": {
    "name": "Intel(R) Core(TM) i7-10700 @ 2.90GHz",
    "usage_percent": 12.3,
    "per_core": [10.1, 14.5, 8.2, 16.4, 11.0, 13.2, 9.8, 15.1]
  },
  "gpu": {
    "name": "NVIDIA GeForce RTX 3070",
    "memory_used_bytes": 1234567890,
    "memory_shared_bytes": 0,
    "memory_capacity_bytes": 8589934592,
    "memory_shared_capacity_bytes": 17179869184,
    "memory_usage_percent": 14.4,
    "utilization_percent": 28.5
  },
  "memory": {
    "total_bytes": 34359738368,
    "available_bytes": 21474836480,
    "used_bytes": 12884901888,
    "used_percent": 37.5
  },
  "network": {
    "mac": "aa:bb:cc:dd:ee:ff",
    "ips": ["192.168.1.10"]
  },
  "fps": {
    "value": 144.2,
    "pid": 12345,
    "window_title": "MyGame",
    "source": "etw_present"
  }
}
```

注意：伺服器是「主動推送」模式，客戶端不需要送任何指令，只要連線後持續讀取即可。

### 3) 在 Windows 用 PowerShell 直接讀取（UTF-8）

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

如果要直接解析 JSON：

```powershell
$client = New-Object System.Net.Sockets.TcpClient("<Windows_IP>", 6666)
$stream = $client.GetStream()
$reader = New-Object System.IO.StreamReader($stream, [Text.Encoding]::UTF8)
while ($true) {
  $line = $reader.ReadLine()
  if ($null -eq $line) { break }
  $obj = $line | ConvertFrom-Json
  $obj.cpu.usage_percent
}
```

如果你是在同一台 Windows 上測試，把 `<Windows_IP>` 改成 `127.0.0.1` 即可。

### 4) 連線測試（只測是否可連、不是讀資料）

在 Windows PowerShell：

```powershell
Test-NetConnection <Windows_IP> -Port 6666
```

## 環境需求

- Visual Studio 2022（任一版本）或 Visual Studio Build Tools
- `vswhere.exe` 位置：`%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe`

## 常見問題排查

- 看到 `vswhere not found` 或 `MSBuild.exe not found`：
  - 安裝 Visual Studio / Build Tools，並包含 **MSBuild** 與 C++ 工具鏈（MSVC、Windows SDK）
- `run.bat` 提示找不到 `SysMonitor.exe`：
  - 先跑 `build_debug.bat` 或 `build_release.bat`
- 其他機器連不上：
  - 確認 UI 已 Start、IP/Port 正確、防火牆入站規則允許、同網段可互通
