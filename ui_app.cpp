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
#include <iomanip>
#include <iostream>
#include <limits>
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

// Adjust window height to accommodate extra IP lines.
static constexpr int kWndWidth = 420;
static constexpr int kWndHeight = 320; // Increased from 290 to fit 3 IP lines comfortably

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

static std::wstring readCpuBrandString() {
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
	return value;
}

struct NetId {
	std::string mac;
	std::vector<std::string> ips;
};

static bool getPrimaryNetId(NetId& out) {
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

		if (!out.mac.empty() || !out.ips.empty()) return true;
	}

	return false;
}

static std::wstring formatDeviceInfo() {
	auto mem = getMemInfo();
	auto gpu = getGpuVideoMemoryInfo();
	auto cpuName = readCpuBrandString();

	NetId net;
	bool hasNet = getPrimaryNetId(net);

	auto gb = [](std::uint64_t bytes) { return static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0); };

	std::wostringstream oss;
	oss << L"CPU: " << (cpuName.empty() ? L"n/a" : cpuName) << L"\r\n";
	oss << L"GPU: " << (gpu.adapterName.empty() ? L"n/a" : gpu.adapterName) << L"\r\n";

	oss << L"Total RAM: ";
	if (mem.ok) {
		oss << std::fixed << std::setprecision(1) << gb(mem.totalPhysBytes) << L" GB";
	} else {
		oss << L"n/a";
	}
	oss << L"\r\n";

	oss << L"MAC: " << (hasNet && !net.mac.empty() ? widen(net.mac) : L"n/a") << L"\r\n";

	auto ipOr = [&](size_t idx) -> std::wstring {
		if (!hasNet || idx >= net.ips.size()) return L""; // Empty string if not present
		return widen(net.ips[idx]);
	};

	// Three distinct lines for IPs
	oss << L"IP1: " << (ipOr(0).empty() ? L"n/a" : ipOr(0)) << L"\r\n";
	oss << L"IP2: " << (ipOr(1).empty() ? L"n/a" : ipOr(1)) << L"\r\n";
	oss << L"IP3: " << (ipOr(2).empty() ? L"n/a" : ipOr(2)) << L"\r\n";

	return oss.str();
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
	std::atomic<bool> running{};
	std::uint16_t port{};
	NetworkServer* server{};
	HANDLE serverThread{};
};

static void updateUi(AppState& st) {
	SetWindowTextW(st.hIps, formatDeviceInfo().c_str());
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
		// Reuse the same info shown in UI, but send it as UTF-8 over TCP.
		return narrowUtf8(formatDeviceInfo());
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
		st->hIps = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | SS_LEFT | SS_NOPREFIX, 10, 78, 380, 180, hwnd, (HMENU)IDC_IPS, st->hInst, nullptr);

		addTrayIcon(*st);
		updateUi(*st);
		SetTimer(hwnd, TIMER_ID_SEND, 15000, nullptr); // Refresh every 15s
		return 0;
	}
	case WM_TIMER:
		if (wParam == TIMER_ID_SEND && st) {
			SetWindowTextW(st->hIps, formatDeviceInfo().c_str());
			RedrawWindow(st->hIps, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
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
		CW_USEDEFAULT, CW_USEDEFAULT, 420, 320, nullptr, nullptr, hInstance, &st); // Increased height here too
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
