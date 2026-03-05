#include "ui_app.h"

#include "network_server.h"

#include "sys_cpu.h"
#include "sys_gpu.h"
#include "sys_mem.h"
#include "sys_monitor.h"
#include "sys_rss.h"

#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <iphlpapi.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#pragma comment(lib, "iphlpapi.lib")

namespace sysmon {

static constexpr wchar_t kWndClassName[] = L"SysMonitorTrayWnd";
static constexpr UINT WM_TRAYICON = WM_APP + 1;
static constexpr UINT_PTR TIMER_ID_SEND = 1;

static constexpr int IDC_PORT = 1001;
static constexpr int IDC_BTN_TOGGLE = 1002;
static constexpr int IDC_STATUS = 1003;
static constexpr int IDC_IPS = 1004;

static constexpr std::uint16_t kDefaultPort = 6666;
static constexpr std::uint16_t kMinPort = 5000;
static constexpr std::uint16_t kMaxPort = 50000;

static constexpr int kWndWidth = 420;
static constexpr int kWndHeight = 320; 

static std::string narrowUtf8(const std::wstring& ws) {
	if (ws.empty()) return {};
	int len = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, nullptr, 0, nullptr, nullptr);
	if (len <= 0) return {};
	std::string s(static_cast<size_t>(len - 1), '\0');
	WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, &s[0], len, nullptr, nullptr);
	return s;
}

static std::wstring widen(const std::string& s) {
	if (s.empty()) return {};
	int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
	if (len <= 0) return {};
	std::wstring ws(static_cast<size_t>(len - 1), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &ws[0], len);
	return ws;
}

// CPU 型號不變，只讀一次以省資源
static std::wstring readCpuBrandString() {
	static std::wstring cached;
	static bool once = false;
	if (once) return cached;
	once = true;
	HKEY hKey{};
	if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
		L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
		0,
		KEY_QUERY_VALUE | KEY_WOW64_64KEY,
		&hKey) != ERROR_SUCCESS) {
		return {};
	}
	DWORD type = 0;
	DWORD size = 0;
	if (RegQueryValueExW(hKey, L"ProcessorNameString", nullptr, &type, nullptr, &size) != ERROR_SUCCESS || type != REG_SZ || size == 0) {
		RegCloseKey(hKey);
		return {};
	}
	std::wstring value(size / sizeof(wchar_t), L'\0');
	if (RegQueryValueExW(hKey, L"ProcessorNameString", nullptr, &type, reinterpret_cast<LPBYTE>(&value[0]), &size) != ERROR_SUCCESS) {
		RegCloseKey(hKey);
		return {};
	}
	RegCloseKey(hKey);
	while (!value.empty() && value.back() == L'\0') value.pop_back();
	cached = std::move(value);
	return cached;
}

struct NetId {
	std::string mac;
	std::vector<std::string> ips;
};

// 網路介面 MAC/IP 快取 15 秒，減少 GetAdaptersInfo 呼叫
static constexpr DWORD kNetIdCacheMs = 15000;
static bool getPrimaryNetId(NetId& out) {
	static NetId cached;
	static DWORD lastTick = 0;
	DWORD now = static_cast<DWORD>(GetTickCount64());
	if (now - lastTick < kNetIdCacheMs && (!cached.mac.empty() || !cached.ips.empty())) {
		out = cached;
		return true;
	}
	lastTick = now;
	out = {};
	ULONG size = 0;
	if (GetAdaptersInfo(nullptr, &size) != ERROR_BUFFER_OVERFLOW || size == 0) {
		return false;
	}
	std::vector<unsigned char> buf(size);
	auto* info = reinterpret_cast<PIP_ADAPTER_INFO>(buf.data());
	if (GetAdaptersInfo(info, &size) != NO_ERROR) {
		return false;
	}
	for (auto* a = info; a; a = a->Next) {
		if (a->Type == MIB_IF_TYPE_LOOPBACK) continue;
		if (a->AddressLength < 6) continue;
		std::ostringstream mac;
		mac << std::hex << std::setfill('0');
		for (UINT i = 0; i < a->AddressLength; ++i) {
			if (i) mac << ":";
			mac << std::setw(2) << static_cast<int>(a->Address[i]);
		}
		out.mac = mac.str();
		for (auto* ip = &a->IpAddressList; ip; ip = ip->Next) {
			if (ip->IpAddress.String[0] == '\0') continue;
			std::string s = ip->IpAddress.String;
			if (s == "0.0.0.0") continue;
			out.ips.push_back(std::move(s));
		}
		if (!out.mac.empty() || !out.ips.empty()) {
			cached = out;
			return true;
		}
	}
	return false;
}

// 取得全機網路流量（排除 loopback），單次 GetIfTable 取樣；快取 buffer 避免每秒配置
#ifndef IF_TYPE_LOOPBACK
#define IF_TYPE_LOOPBACK 24
#endif
static bool getNetworkTraffic(std::uint64_t& outBytesSent, std::uint64_t& outBytesRecv) {
	outBytesSent = 0;
	outBytesRecv = 0;
	ULONG size = 0;
	if (GetIfTable(nullptr, &size, FALSE) != ERROR_INSUFFICIENT_BUFFER || size == 0) return false;
	static std::vector<unsigned char> buf;
	if (buf.size() < size) buf.resize(size);
	auto* table = reinterpret_cast<MIB_IFTABLE*>(buf.data());
	if (GetIfTable(table, &size, FALSE) != NO_ERROR) return false;
	for (DWORD i = 0; i < table->dwNumEntries; ++i) {
		const MIB_IFROW& row = table->table[i];
		if (row.dwType == IF_TYPE_LOOPBACK) continue;
		outBytesSent += row.dwOutOctets;
		outBytesRecv += row.dwInOctets;
	}
	return true;
}

static std::wstring formatDeviceInfo(CpuMonitor* cpuMon = nullptr, std::mutex* cpuMonMutex = nullptr, bool includePerCoreCpu = false) {
	auto mem = getMemInfo();
	auto gpu = getGpuVideoMemoryInfo();
	auto fps = getForegroundPresentFps();
	auto cpuName = readCpuBrandString();

	sysmon::OptDbl cpuPct{};
	std::vector<double> perCorePct;
	bool hasPerCore = false;
	if (cpuMon) {
		std::unique_lock<std::mutex> lk;
		if (cpuMonMutex) lk = std::unique_lock<std::mutex>(*cpuMonMutex);
		double v = 0.0;
		if (cpuMon->getCpuPercent(v)) {
			cpuPct.has = true;
			cpuPct.value = v;
		}
		if (includePerCoreCpu) {
			hasPerCore = cpuMon->getPerCoreCpuPercent(perCorePct);
		}
	}

	NetId net;
	bool hasNet = getPrimaryNetId(net);

	auto gb = [](std::uint64_t bytes) { return static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0); };
	auto mb = [](std::uint64_t bytes) { return static_cast<double>(bytes) / (1024.0 * 1024.0); };

	std::wostringstream oss;
	// === 即時監控區（每項一行，避免寬度跳動）===
	oss << L"CPU: ";
	if (cpuPct.has) {
		oss << std::fixed << std::setprecision(1) << cpuPct.value << L"%";
	} else {
		oss << L"n/a";
	}
	oss << L"\r\n";

	oss << L"GPU 使用率: ";
	if (gpu.utilizationPercent >= 0.0) {
		oss << std::fixed << std::setprecision(1) << gpu.utilizationPercent << L"%";
	} else {
		oss << L"n/a";
	}
	oss << L"\r\n";

	oss << L"GPU 記憶體: ";
	if (gpu.dedicatedUsagePercent >= 0.0 && gpu.dedicatedBytes.has && gpu.dedicatedCapacityBytes.has) {
		oss << std::fixed << std::setprecision(0) << mb(gpu.dedicatedBytes.value) << L" / " << mb(gpu.dedicatedCapacityBytes.value) << L" MB (" << std::setprecision(1) << gpu.dedicatedUsagePercent << L"%)";
	} else if (gpu.dedicatedBytes.has && gpu.dedicatedCapacityBytes.has) {
		oss << std::fixed << std::setprecision(0) << mb(gpu.dedicatedBytes.value) << L" / " << mb(gpu.dedicatedCapacityBytes.value) << L" MB";
	} else {
		oss << L"n/a";
	}
	oss << L"\r\n";

	oss << L"FPS: ";
	if (fps.ok) {
		oss << std::fixed << std::setprecision(1) << fps.fps;
	} else if (!isFpsEtwRunning()) {
		oss << L"n/a (請以管理員身分執行)";
	} else {
		oss << L"n/a";
	}
	oss << L"\r\n";

	oss << L"RAM: ";
	if (mem.ok && mem.totalPhysBytes > 0) {
		std::uint64_t used = mem.totalPhysBytes - mem.availPhysBytes;
		double pct = (static_cast<double>(used) * 100.0) / static_cast<double>(mem.totalPhysBytes);
		oss << std::fixed << std::setprecision(1) << pct << L"% (" << std::setprecision(1) << gb(used) << L" / " << gb(mem.totalPhysBytes) << L" GB)";
	} else {
		oss << L"n/a";
	}
	oss << L"\r\n";

	std::uint64_t bytesSent = 0, bytesRecv = 0;
	if (getNetworkTraffic(bytesSent, bytesRecv)) {
		static std::uint64_t prevSent = 0, prevRecv = 0;
		static bool firstNet = true;
		std::uint64_t rateUp = firstNet ? 0 : (bytesSent - prevSent);
		std::uint64_t rateDown = firstNet ? 0 : (bytesRecv - prevRecv);
		prevSent = bytesSent;
		prevRecv = bytesRecv;
		firstNet = false;
		auto kbps = [](std::uint64_t b) { return static_cast<double>(b) / 1024.0; };
		oss << L"網路: ↑" << std::fixed << std::setprecision(1) << kbps(rateUp) << L" KB/s ↓" << kbps(rateDown) << L" KB/s\r\n";
	}

	if (fps.ok && !fps.windowTitle.empty()) {
		std::wstring winTitle = fps.windowTitle;
		if (winTitle.size() > 50) winTitle = winTitle.substr(0, 47) + L"...";
		oss << L"前景: " << winTitle << L"\r\n";
	}
	oss << L"\r\n";

	// === 靜態資訊 ===
	if (includePerCoreCpu) {
		if (hasPerCore && !perCorePct.empty()) {
			for (size_t i = 0; i < perCorePct.size(); ++i) {
				oss << L"CPU" << i << L": " << std::fixed << std::setprecision(1) << perCorePct[i] << L"%\r\n";
			}
		} else {
			SYSTEM_INFO si{};
			GetSystemInfo(&si);
			DWORD n = si.dwNumberOfProcessors ? si.dwNumberOfProcessors : 1;
			for (DWORD i = 0; i < n; ++i) {
				oss << L"CPU" << i << L": n/a\r\n";
			}
		}
	}
	oss << L"CPU: " << (cpuName.empty() ? L"n/a" : cpuName) << L"\r\n";
	oss << L"GPU: " << (gpu.adapterName.empty() ? L"n/a" : gpu.adapterName) << L"\r\n";
	oss << L"MAC: " << (hasNet && !net.mac.empty() ? widen(net.mac) : L"n/a") << L"\r\n";

	auto ipOr = [&](size_t idx) -> std::wstring {
		if (!hasNet || idx >= net.ips.size()) return L"";
		return widen(net.ips[idx]);
	};
	for (int i = 0; i < 3; ++i) {
		std::wstring ip = ipOr(static_cast<size_t>(i));
		if (!ip.empty()) oss << L"IP" << (i + 1) << L": " << ip << L"\r\n";
	}

	return oss.str();
}

static void jsonAppendEscaped(std::string& out, const std::string& s) {
	for (unsigned char c : s) {
		switch (c) {
		case '"': out += "\\\""; break;
		case '\\': out += "\\\\"; break;
		case '\b': out += "\\b"; break;
		case '\f': out += "\\f"; break;
		case '\n': out += "\\n"; break;
		case '\r': out += "\\r"; break;
		case '\t': out += "\\t"; break;
		default:
			if (c < 0x20) {
				char buf[7];
				snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned int>(c));
				out += buf;
			} else {
				out.push_back(static_cast<char>(c));
			}
			break;
		}
	}
}

static void jsonAppendQuoted(std::string& out, const std::string& s) {
	out.push_back('"');
	jsonAppendEscaped(out, s);
	out.push_back('"');
}

static void jsonAppendKey(std::string& out, const char* key) {
	jsonAppendQuoted(out, key);
	out.push_back(':');
}

static void jsonAppendOptU64(std::string& out, const OptU64& v) {
	if (!v.has) {
		out += "null";
		return;
	}
	out += std::to_string(static_cast<unsigned long long>(v.value));
}

static std::string formatDeviceInfoJson(CpuMonitor* cpuMon, std::mutex* cpuMonMutex) {
	auto mem = getMemInfo();
	auto gpu = getGpuVideoMemoryInfo();
	auto fps = getForegroundPresentFps();
	auto cpuNameW = readCpuBrandString();
	std::string cpuName = narrowUtf8(cpuNameW);
	std::string gpuName = narrowUtf8(gpu.adapterName);

	OptDbl cpuTotal{};
	std::vector<double> perCore;
	bool perCoreOk = false;
	if (cpuMon) {
		std::unique_lock<std::mutex> lk;
		if (cpuMonMutex) lk = std::unique_lock<std::mutex>(*cpuMonMutex);
		double v = 0.0;
		if (cpuMon->getCpuPercent(v)) {
			cpuTotal.has = true;
			cpuTotal.value = v;
		}
		perCoreOk = cpuMon->getPerCoreCpuPercent(perCore);
	}

	NetId net;
	bool hasNet = getPrimaryNetId(net);

	std::uint64_t memUsedBytes = 0;
	double memUsedPct = 0.0;
	bool memPctOk = false;
	if (mem.ok && mem.totalPhysBytes > 0) {
		memUsedBytes = mem.totalPhysBytes - mem.availPhysBytes;
		memUsedPct = (static_cast<double>(memUsedBytes) * 100.0) / static_cast<double>(mem.totalPhysBytes);
		memPctOk = true;
	}

	// Unix timestamp (ms) - standard for JSON Lines / time-series
	auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::system_clock::now().time_since_epoch()).count();

	std::string out;
	out.reserve(1536);
	out.push_back('{');

	// ts - 時間戳（Unix ms）
	jsonAppendKey(out, "ts");
	out += std::to_string(static_cast<long long>(ts));
	out.push_back(',');

	// cpu
	jsonAppendKey(out, "cpu");
	out.push_back('{');
	jsonAppendKey(out, "name");
	jsonAppendQuoted(out, cpuName.empty() ? std::string("") : cpuName);
	out.push_back(',');
	jsonAppendKey(out, "usage_percent");
	if (cpuTotal.has) {
		std::ostringstream ss;
		ss.setf(std::ios::fixed);
		ss << std::setprecision(1) << cpuTotal.value;
		out += ss.str();
	} else {
		out += "null";
	}
	out.push_back(',');
	jsonAppendKey(out, "per_core");
	out.push_back('[');
	for (size_t i = 0; i < perCore.size(); ++i) {
		if (i) out.push_back(',');
		std::ostringstream ss;
		ss.setf(std::ios::fixed);
		ss << std::setprecision(1) << perCore[i];
		out += ss.str();
	}
	out.push_back(']');
	out.push_back('}');
	out.push_back(',');

	// gpu
	jsonAppendKey(out, "gpu");
	out.push_back('{');
	jsonAppendKey(out, "name");
	jsonAppendQuoted(out, gpuName.empty() ? std::string("") : gpuName);
	out.push_back(',');
	jsonAppendKey(out, "memory_used_bytes");
	jsonAppendOptU64(out, gpu.dedicatedBytes);
	out.push_back(',');
	jsonAppendKey(out, "memory_shared_bytes");
	jsonAppendOptU64(out, gpu.sharedBytes);
	out.push_back(',');
	jsonAppendKey(out, "memory_capacity_bytes");
	jsonAppendOptU64(out, gpu.dedicatedCapacityBytes);
	out.push_back(',');
	jsonAppendKey(out, "memory_shared_capacity_bytes");
	jsonAppendOptU64(out, gpu.sharedCapacityBytes);
	out.push_back(',');
	jsonAppendKey(out, "memory_usage_percent");
	if (gpu.dedicatedUsagePercent >= 0.0) {
		std::ostringstream ss;
		ss.setf(std::ios::fixed);
		ss << std::setprecision(1) << gpu.dedicatedUsagePercent;
		out += ss.str();
	} else {
		out += "null";
	}
	out.push_back(',');
	jsonAppendKey(out, "utilization_percent");
	if (gpu.utilizationPercent >= 0.0) {
		std::ostringstream ss;
		ss.setf(std::ios::fixed);
		ss << std::setprecision(1) << gpu.utilizationPercent;
		out += ss.str();
	} else {
		out += "null";
	}
	out.push_back('}');
	out.push_back(',');

	// memory
	jsonAppendKey(out, "memory");
	out.push_back('{');
	jsonAppendKey(out, "total_bytes");
	out += mem.ok ? std::to_string(static_cast<unsigned long long>(mem.totalPhysBytes)) : std::string("null");
	out.push_back(',');
	jsonAppendKey(out, "available_bytes");
	out += mem.ok ? std::to_string(static_cast<unsigned long long>(mem.availPhysBytes)) : std::string("null");
	out.push_back(',');
	jsonAppendKey(out, "used_bytes");
	out += (mem.ok ? std::to_string(static_cast<unsigned long long>(memUsedBytes)) : std::string("null"));
	out.push_back(',');
	jsonAppendKey(out, "used_percent");
	if (memPctOk) {
		std::ostringstream ss;
		ss.setf(std::ios::fixed);
		ss << std::setprecision(1) << memUsedPct;
		out += ss.str();
	} else {
		out += "null";
	}
	out.push_back('}');
	out.push_back(',');

	// network
	jsonAppendKey(out, "network");
	out.push_back('{');
	jsonAppendKey(out, "mac");
	jsonAppendQuoted(out, (hasNet && !net.mac.empty()) ? net.mac : std::string(""));
	out.push_back(',');
	jsonAppendKey(out, "ips");
	out.push_back('[');
	if (hasNet) {
		for (size_t i = 0; i < net.ips.size(); ++i) {
			if (i) out.push_back(',');
			jsonAppendQuoted(out, net.ips[i]);
		}
	}
	out.push_back(']');
	std::uint64_t bytesSent = 0, bytesRecv = 0;
	const bool trafficOk = getNetworkTraffic(bytesSent, bytesRecv);
	if (trafficOk) {
		out.push_back(',');
		jsonAppendKey(out, "bytes_sent");
		out += std::to_string(static_cast<unsigned long long>(bytesSent));
		out.push_back(',');
		jsonAppendKey(out, "bytes_recv");
		out += std::to_string(static_cast<unsigned long long>(bytesRecv));
		// 每秒流量（與 TCP 推送間隔 1 秒一致，無額外取樣）
		static std::uint64_t prevSent = 0, prevRecv = 0;
		static bool firstTraffic = true;
		std::uint64_t rateSent = firstTraffic ? 0 : (bytesSent - prevSent);
		std::uint64_t rateRecv = firstTraffic ? 0 : (bytesRecv - prevRecv);
		prevSent = bytesSent;
		prevRecv = bytesRecv;
		firstTraffic = false;
		out.push_back(',');
		jsonAppendKey(out, "bytes_sent_per_sec");
		out += std::to_string(static_cast<unsigned long long>(rateSent));
		out.push_back(',');
		jsonAppendKey(out, "bytes_recv_per_sec");
		out += std::to_string(static_cast<unsigned long long>(rateRecv));
	}
	out.push_back('}');
	out.push_back(',');

	// fps
	jsonAppendKey(out, "fps");
	out.push_back('{');
	jsonAppendKey(out, "value");
	if (fps.ok) {
		std::ostringstream ss;
		ss.setf(std::ios::fixed);
		ss << std::setprecision(1) << fps.fps;
		out += ss.str();
	} else {
		out += "null";
	}
	out.push_back(',');
	jsonAppendKey(out, "pid");
	out += std::to_string(static_cast<unsigned long long>(fps.pid));
	out.push_back(',');
	jsonAppendKey(out, "window_title");
	{
		std::string title = narrowUtf8(fps.windowTitle);
		jsonAppendQuoted(out, title.empty() ? std::string("") : title);
	}
	out.push_back(',');
	jsonAppendKey(out, "source");
	jsonAppendQuoted(out, "etw_present");
	out.push_back('}');

	out.push_back('}');
	return out;
}

struct AppState {
	HINSTANCE hInst{};
	HWND hwnd{};
	HWND hPort{};
	HWND hToggle{};
	HWND hStatus{};
	HWND hIps{};
	NOTIFYICONDATAW nid{};

	CpuMonitor cpuMon;
	std::mutex cpuMonMutex;
	std::atomic<bool> running{};
	std::uint16_t port{};
	NetworkServer* server{};
	HANDLE serverThread{};
};

static void updateUi(AppState& st) {
	SetWindowTextW(st.hIps, formatDeviceInfo(&st.cpuMon, &st.cpuMonMutex, false).c_str());
	RedrawWindow(st.hIps, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);

	if (st.running.load()) {
		SetWindowTextW(st.hStatus, (L"Listening on port " + std::to_wstring(st.port)).c_str());
		SetWindowTextW(st.hToggle, L"Stop");
	} else {
		SetWindowTextW(st.hStatus, L"Stopped");
		SetWindowTextW(st.hToggle, L"Start");
	}

	RECT rc;
	GetWindowRect(st.hStatus, &rc);
	MapWindowPoints(nullptr, st.hwnd, reinterpret_cast<POINT*>(&rc), 2);
	InflateRect(&rc, 2, 2);
	RedrawWindow(st.hwnd, &rc, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
}

static bool parsePortFromEdit(HWND hEdit, std::uint16_t& outPort) {
	wchar_t buf[32] = {};
	GetWindowTextW(hEdit, buf, 31);
	wchar_t* end = nullptr;
	unsigned long v = wcstoul(buf, &end, 10);
	if (end == buf || *end != L'\0') return false;
	if (v < kMinPort || v > kMaxPort) return false;
	outPort = static_cast<std::uint16_t>(v);
	return true;
}

static void stopServer(AppState& st) {
	if (!st.running.exchange(false)) return;

	if (st.server) {
		st.server->stop();
	}

	if (st.serverThread) {
		WaitForSingleObject(st.serverThread, INFINITE);
		CloseHandle(st.serverThread);
		st.serverThread = nullptr;
	}

	if (st.server) {
		delete st.server;
		st.server = nullptr;
	}
}

static void startServer(AppState& st, std::uint16_t port) {
	stopServer(st);
	if (port < kMinPort || port > kMaxPort) port = kDefaultPort;
	st.port = port;
	st.running = true;

	st.server = new NetworkServer(port, [&st]() {
		// Send JSON as UTF-8 over TCP (one JSON object per line).
		return formatDeviceInfoJson(&st.cpuMon, &st.cpuMonMutex);
	});

	st.serverThread = CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
		auto* stp = reinterpret_cast<AppState*>(p);
		NetworkServer* srv = stp->server;
		if (srv) srv->run();
		return 0;
	}, &st, 0, nullptr);

	if (!st.serverThread) {
		st.running = false;
		delete st.server;
		st.server = nullptr;
	}
}

static void addTrayIcon(AppState& st) {
	st.nid.cbSize = sizeof(st.nid);
	st.nid.hWnd = st.hwnd;
	st.nid.uID = 1;
	st.nid.uFlags = NIF_MESSAGE | NIF_TIP | NIF_ICON;
	st.nid.uCallbackMessage = WM_TRAYICON;
	st.nid.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
	wcscpy_s(st.nid.szTip, _countof(st.nid.szTip), L"SysMonitor");
	Shell_NotifyIconW(NIM_ADD, &st.nid);
}

static void removeTrayIcon(AppState& st) {
	if (st.nid.cbSize) Shell_NotifyIconW(NIM_DELETE, &st.nid);
}

static void showTrayMenu(AppState& st) {
	HMENU menu = CreatePopupMenu();
	AppendMenuW(menu, MF_STRING, 1, L"Show");
	AppendMenuW(menu, MF_STRING, 2, st.running.load() ? L"Stop" : L"Start");
	AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
	AppendMenuW(menu, MF_STRING, 3, L"Exit");

	POINT p;
	GetCursorPos(&p);
	SetForegroundWindow(st.hwnd);
	UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, p.x, p.y, 0, st.hwnd, nullptr);
	DestroyMenu(menu);

	switch (cmd) {
	case 1:
		ShowWindow(st.hwnd, SW_SHOWNORMAL);
		SetForegroundWindow(st.hwnd);
		break;
	case 2: {
		if (st.running.load()) {
			stopServer(st);
		} else {
			std::uint16_t port = st.port;
			if (!parsePortFromEdit(st.hPort, port)) port = st.port;
			startServer(st, port);
		}
		updateUi(st);
		break;
	}
	case 3:
		PostMessageW(st.hwnd, WM_CLOSE, 0, 0);
		break;
	default:
		break;
	}
}

static COLORREF lerpColor(COLORREF a, COLORREF b, int t, int tmax) {
	auto la = [&](int c) { return GetRValue(c); };
	auto ga = [&](int c) { return GetGValue(c); };
	auto ba = [&](int c) { return GetBValue(c); };

	int ar = la(a), ag = ga(a), ab = ba(a);
	int br = la(b), bg = ga(b), bb = ba(b);
	int r = ar + ((br - ar) * t) / tmax;
	int g = ag + ((bg - ag) * t) / tmax;
	int bl = ab + ((bb - ab) * t) / tmax;
	return RGB(r, g, bl);
}

static void paintGradientBackground(HDC hdc, const RECT& rc) {
	const COLORREF top = RGB(255, 255, 255);
	const COLORREF bottom = RGB(200, 235, 200);

	int h = rc.bottom - rc.top;
	if (h <= 0) return;

	for (int y = 0; y < h; ++y) {
		COLORREF c = lerpColor(top, bottom, y, h);
		HPEN pen = CreatePen(PS_SOLID, 1, c);
		HGDIOBJ oldPen = SelectObject(hdc, pen);
		MoveToEx(hdc, rc.left, rc.top + y, nullptr);
		LineTo(hdc, rc.right, rc.top + y);
		SelectObject(hdc, oldPen);
		DeleteObject(pen);
	}
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	auto* st = reinterpret_cast<AppState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

	switch (msg) {
	case WM_ERASEBKGND: {
		HDC hdc = reinterpret_cast<HDC>(wParam);
		RECT rc{};
		GetClientRect(hwnd, &rc);
		paintGradientBackground(hdc, rc);
		return 1;
	}
	case WM_CTLCOLOREDIT: {
		HDC hdc = reinterpret_cast<HDC>(wParam);
		SetBkMode(hdc, OPAQUE);
		SetBkColor(hdc, RGB(220, 245, 220));
		static HBRUSH s_editBrush = CreateSolidBrush(RGB(220, 245, 220));
		return reinterpret_cast<INT_PTR>(s_editBrush);
	}
	case WM_CTLCOLORSTATIC: {
		HDC hdc = reinterpret_cast<HDC>(wParam);
		SetBkMode(hdc, OPAQUE);
		SetBkColor(hdc, RGB(220, 245, 220));
		static HBRUSH s_staticBrush = CreateSolidBrush(RGB(220, 245, 220));
		return reinterpret_cast<INT_PTR>(s_staticBrush);
	}
	case WM_CREATE: {
		CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
		st = reinterpret_cast<AppState*>(cs->lpCreateParams);
		st->hwnd = hwnd;
		SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));

		// Port label + edit
		CreateWindowW(L"STATIC", L"Port (5000-50000):", WS_CHILD | WS_VISIBLE, 10, 12, 130, 18, hwnd, nullptr, st->hInst, nullptr);
		st->hPort = CreateWindowW(L"EDIT", std::to_wstring(kDefaultPort).c_str(), WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER, 150, 10, 90, 22, hwnd, (HMENU)IDC_PORT, st->hInst, nullptr);

		// Move Start/Stop button right
		st->hToggle = CreateWindowW(L"BUTTON", L"Start", WS_CHILD | WS_VISIBLE, 260, 10, 80, 22, hwnd, (HMENU)IDC_BTN_TOGGLE, st->hInst, nullptr);

		st->hStatus = CreateWindowW(L"STATIC", L"Stopped", WS_CHILD | WS_VISIBLE, 10, 40, 380, 28, hwnd, (HMENU)IDC_STATUS, st->hInst, nullptr);

		// Use STATIC instead of EDIT to avoid repaint artifacts; keep multiline display.
		// Increase height of the info box to fit extra lines
		st->hIps = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | SS_LEFT | SS_NOPREFIX, 10, 78, 380, 220, hwnd, (HMENU)IDC_IPS, st->hInst, nullptr);

		addTrayIcon(*st);
		// Auto-start server on launch
		startServer(*st, st->port);
		updateUi(*st);
		SetTimer(hwnd, TIMER_ID_SEND, 1000, nullptr); // 每秒更新即時資訊
		return 0;
	}
	case WM_TIMER:
		if (wParam == TIMER_ID_SEND && st) {
			updateUi(*st);
		}
		return 0;
	case WM_COMMAND:
		if (!st) return 0;
		if (LOWORD(wParam) == IDC_BTN_TOGGLE) {
			if (st->running.load()) {
				stopServer(*st);
			} else {
				std::uint16_t port = st->port;
				if (!parsePortFromEdit(st->hPort, port)) port = st->port;
				startServer(*st, port);
			}
			updateUi(*st);
			return 0;
		}
		return 0;
	case WM_TRAYICON:
		if (!st) return 0;
		if (lParam == WM_RBUTTONUP) {
			showTrayMenu(*st);
			return 0;
		}
		if (lParam == WM_LBUTTONDBLCLK) {
			ShowWindow(hwnd, SW_SHOWNORMAL);
			SetForegroundWindow(hwnd);
			return 0;
		}
		return 0;
	case WM_CLOSE:
		DestroyWindow(hwnd);
		return 0;
	case WM_DESTROY:
		if (st) {
			stopServer(*st);
			removeTrayIcon(*st);
		}
		PostQuitMessage(0);
		return 0;
	default:
		return DefWindowProcW(hwnd, msg, wParam, lParam);
	}
}

int RunTrayApp(HINSTANCE hInstance, const UiAppConfig& cfg) {
	AppState st;
	st.hInst = hInstance;
	st.cpuMon.init();
	st.port = cfg.defaultPort;
	if (st.port < kMinPort || st.port > kMaxPort) st.port = kDefaultPort;

	WNDCLASSW wc{};
	wc.lpfnWndProc = WndProc;
	wc.hInstance = hInstance;
	wc.lpszClassName = kWndClassName;
	wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
	wc.hCursor = LoadCursor(nullptr, IDC_ARROW);

	if (!RegisterClassW(&wc)) return 1;

	HWND hwnd = CreateWindowW(kWndClassName, L"SysMonitor", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
		CW_USEDEFAULT, CW_USEDEFAULT, 420, 360, nullptr, nullptr, hInstance, &st); 
	if (!hwnd) return 1;

	ShowWindow(hwnd, SW_SHOWNORMAL);
	UpdateWindow(hwnd);

	MSG msg;
	while (GetMessageW(&msg, nullptr, 0, 0)) {
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}
	return static_cast<int>(msg.wParam);
}

} // namespace sysmon
