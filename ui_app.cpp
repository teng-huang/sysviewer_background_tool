#include "ui_app.h"

#include "network_server.h"

#include "sys_cpu.h"
#include "sys_gpu.h"
#include "sys_mem.h"
#include "sys_monitor.h"
#include "sys_rss.h"

#include "resource.h"
#include "version_info.h"

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <commctrl.h>
#include <iphlpapi.h>
#include <taskschd.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <wrl/client.h>

#include <atomic>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "taskschd.lib")

namespace sysmon {

const wchar_t* UiWindowClassName() noexcept {
	return L"SysMonitorTrayWnd";
}

static constexpr UINT WM_TRAYICON = WM_APP + 1;
static constexpr UINT WM_TRAY_EXIT = WM_APP + 2;
static constexpr UINT_PTR TIMER_ID_SEND = 1;
static constexpr UINT_PTR TIMER_ID_TRAY_RETRY = 2;
static constexpr UINT kUiRefreshMs = 30000;
static constexpr UINT kTrayRetryMs = 2000;

static constexpr int IDC_PORT = 1001;
static constexpr int IDC_BTN_TOGGLE = 1002;
static constexpr int IDC_STATUS = 1003;
static constexpr int IDC_IPS = 1004;
static constexpr int IDC_AUTOSTART = 1005;
static constexpr int IDC_TITLE = 1006;
static constexpr int IDC_SUBTITLE = 1007;
static constexpr int IDC_PORT_LABEL = 1008;
static constexpr int IDC_INFO_LABEL = 1009;
static constexpr int IDC_OPTIONS_LABEL = 1010;
static constexpr int IDC_VERSION_LABEL = 1011;
static constexpr int IDC_ALLOW_LAN = 1012;
static constexpr int IDC_START_ON_LAUNCH = 1013;

static constexpr std::uint16_t kDefaultPort = 6666;
static constexpr std::uint16_t kMinPort = 5000;
static constexpr std::uint16_t kMaxPort = 50000;

static constexpr int kWndWidth = 480;
static constexpr int kWndHeight = 460;
static constexpr ULONGLONG kNetIdCacheMs = 60000;
static constexpr wchar_t kAutoStartTaskName[] = L"SysMonitor";
static constexpr wchar_t kSettingsRegPath[] = L"Software\\SysMonitor";
static constexpr wchar_t kAutoStartPrefValue[] = L"AutoStartEnabled";
static constexpr wchar_t kStartOnLaunchPrefValue[] = L"StartServerOnLaunchEnabled";

static constexpr COLORREF kColorBg = RGB(245, 247, 250);
static constexpr COLORREF kColorSurface = RGB(255, 255, 255);
static constexpr COLORREF kColorBorder = RGB(220, 226, 235);
static constexpr COLORREF kColorText = RGB(31, 41, 55);
static constexpr COLORREF kColorMuted = RGB(100, 116, 139);
static constexpr COLORREF kColorRunning = RGB(22, 101, 52);
static constexpr COLORREF kColorStopped = RGB(148, 43, 43);

static HMENU controlIdMenu(int id) {
	return reinterpret_cast<HMENU>(static_cast<INT_PTR>(id));
}

class BStr {
public:
	explicit BStr(const wchar_t* value) : _value(SysAllocString(value)) {}
	~BStr() {
		if (_value) SysFreeString(_value);
	}

	BStr(const BStr&) = delete;
	BStr& operator=(const BStr&) = delete;

	operator BSTR() const { return _value; }
	bool ok() const { return _value != nullptr; }

private:
	BSTR _value{};
};

class ComApartment {
public:
	ComApartment() {
		_hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
		_uninitialize = SUCCEEDED(_hr);
		if (_hr == RPC_E_CHANGED_MODE) _hr = S_OK;
		if (SUCCEEDED(_hr)) {
			HRESULT secHr = CoInitializeSecurity(nullptr, -1, nullptr, nullptr,
				RPC_C_AUTHN_LEVEL_PKT_PRIVACY, RPC_C_IMP_LEVEL_IMPERSONATE,
				nullptr, 0, nullptr);
			if (FAILED(secHr) && secHr != RPC_E_TOO_LATE) _hr = secHr;
		}
	}

	~ComApartment() {
		if (_uninitialize) CoUninitialize();
	}

	bool ok() const { return SUCCEEDED(_hr); }

private:
	HRESULT _hr{};
	bool _uninitialize{};
};

static bool getModulePath(std::wstring& outPath) {
	std::wstring buf(MAX_PATH, L'\0');
	for (;;) {
		DWORD len = GetModuleFileNameW(nullptr, &buf[0], static_cast<DWORD>(buf.size()));
		if (len == 0) return false;
		if (len < buf.size() - 1) {
			buf.resize(len);
			outPath = std::move(buf);
			return true;
		}
		if (buf.size() >= 32768) return false;
		buf.assign(buf.size() * 2, L'\0');
	}
}

static std::wstring getParentDirectory(const std::wstring& path) {
	size_t pos = path.find_last_of(L"\\/");
	if (pos == std::wstring::npos) return {};
	return path.substr(0, pos);
}

static bool getTaskFolder(Microsoft::WRL::ComPtr<ITaskFolder>& rootFolder) {
	Microsoft::WRL::ComPtr<ITaskService> service;
	HRESULT hr = CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
		IID_ITaskService, reinterpret_cast<void**>(service.GetAddressOf()));
	if (FAILED(hr)) return false;

	VARIANT empty;
	VariantInit(&empty);
	hr = service->Connect(empty, empty, empty, empty);
	if (FAILED(hr)) return false;

	BStr rootPath(L"\\");
	if (!rootPath.ok()) return false;
	hr = service->GetFolder(rootPath, rootFolder.GetAddressOf());
	return SUCCEEDED(hr);
}

static bool isAutoStartEnabled() {
	ComApartment com;
	if (!com.ok()) return false;

	Microsoft::WRL::ComPtr<ITaskFolder> rootFolder;
	if (!getTaskFolder(rootFolder)) return false;

	BStr taskName(kAutoStartTaskName);
	if (!taskName.ok()) return false;

	Microsoft::WRL::ComPtr<IRegisteredTask> task;
	if (FAILED(rootFolder->GetTask(taskName, task.GetAddressOf()))) return false;

	VARIANT_BOOL enabled = VARIANT_FALSE;
	if (FAILED(task->get_Enabled(&enabled))) return false;
	return enabled == VARIANT_TRUE;
}

static bool readBoolPreference(const wchar_t* valueName, bool& enabled) {
	HKEY key{};
	if (RegOpenKeyExW(HKEY_CURRENT_USER, kSettingsRegPath, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
		return false;
	}

	DWORD type = 0;
	DWORD value = 0;
	DWORD size = sizeof(value);
	LSTATUS st = RegQueryValueExW(key, valueName, nullptr, &type, reinterpret_cast<LPBYTE>(&value), &size);
	RegCloseKey(key);
	if (st != ERROR_SUCCESS || type != REG_DWORD || size != sizeof(value)) return false;
	enabled = value != 0;
	return true;
}

static bool writeBoolPreference(const wchar_t* valueName, bool enabled) {
	HKEY key{};
	DWORD disposition = 0;
	if (RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsRegPath, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, &disposition) != ERROR_SUCCESS) {
		return false;
	}

	DWORD value = enabled ? 1 : 0;
	LSTATUS st = RegSetValueExW(key, valueName, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
	RegCloseKey(key);
	return st == ERROR_SUCCESS;
}

static bool readAutoStartPreference(bool& enabled) {
	return readBoolPreference(kAutoStartPrefValue, enabled);
}

static bool writeAutoStartPreference(bool enabled) {
	return writeBoolPreference(kAutoStartPrefValue, enabled);
}

static bool loadStartOnLaunchPreference() {
	bool enabled = true;
	if (!readBoolPreference(kStartOnLaunchPrefValue, enabled)) {
		writeBoolPreference(kStartOnLaunchPrefValue, enabled);
	}
	return enabled;
}

static bool writeStartOnLaunchPreference(bool enabled) {
	return writeBoolPreference(kStartOnLaunchPrefValue, enabled);
}

static bool setAutoStartEnabled(bool enabled) {
	ComApartment com;
	if (!com.ok()) return false;

	Microsoft::WRL::ComPtr<ITaskFolder> rootFolder;
	if (!getTaskFolder(rootFolder)) return false;

	BStr taskName(kAutoStartTaskName);
	if (!taskName.ok()) return false;

	if (!enabled) {
		HRESULT hr = rootFolder->DeleteTask(taskName, 0);
		return SUCCEEDED(hr) || HRESULT_CODE(hr) == ERROR_FILE_NOT_FOUND;
	}

	std::wstring exePath;
	if (!getModulePath(exePath)) return false;
	std::wstring workDir = getParentDirectory(exePath);

	Microsoft::WRL::ComPtr<ITaskService> service;
	HRESULT hr = CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
		IID_ITaskService, reinterpret_cast<void**>(service.GetAddressOf()));
	if (FAILED(hr)) return false;

	VARIANT empty;
	VariantInit(&empty);
	hr = service->Connect(empty, empty, empty, empty);
	if (FAILED(hr)) return false;

	Microsoft::WRL::ComPtr<ITaskDefinition> task;
	hr = service->NewTask(0, task.GetAddressOf());
	if (FAILED(hr)) return false;

	Microsoft::WRL::ComPtr<IRegistrationInfo> regInfo;
	if (SUCCEEDED(task->get_RegistrationInfo(regInfo.GetAddressOf()))) {
		BStr author(L"SysMonitor");
		if (author.ok()) regInfo->put_Author(author);
	}

	Microsoft::WRL::ComPtr<IPrincipal> principal;
	if (SUCCEEDED(task->get_Principal(principal.GetAddressOf()))) {
		principal->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN);
		principal->put_RunLevel(TASK_RUNLEVEL_HIGHEST);
	}

	Microsoft::WRL::ComPtr<ITaskSettings> settings;
	if (SUCCEEDED(task->get_Settings(settings.GetAddressOf()))) {
		settings->put_StartWhenAvailable(VARIANT_TRUE);
		settings->put_DisallowStartIfOnBatteries(VARIANT_FALSE);
		settings->put_StopIfGoingOnBatteries(VARIANT_FALSE);
		settings->put_MultipleInstances(TASK_INSTANCES_IGNORE_NEW);
		BStr noLimit(L"PT0S");
		if (noLimit.ok()) settings->put_ExecutionTimeLimit(noLimit);
	}

	Microsoft::WRL::ComPtr<ITriggerCollection> triggers;
	hr = task->get_Triggers(triggers.GetAddressOf());
	if (FAILED(hr)) return false;
	Microsoft::WRL::ComPtr<ITrigger> trigger;
	hr = triggers->Create(TASK_TRIGGER_LOGON, trigger.GetAddressOf());
	if (FAILED(hr)) return false;

	Microsoft::WRL::ComPtr<IActionCollection> actions;
	hr = task->get_Actions(actions.GetAddressOf());
	if (FAILED(hr)) return false;
	Microsoft::WRL::ComPtr<IAction> action;
	hr = actions->Create(TASK_ACTION_EXEC, action.GetAddressOf());
	if (FAILED(hr)) return false;

	Microsoft::WRL::ComPtr<IExecAction> execAction;
	hr = action.As(&execAction);
	if (FAILED(hr)) return false;

	BStr exeBstr(exePath.c_str());
	if (!exeBstr.ok()) return false;
	hr = execAction->put_Path(exeBstr);
	if (FAILED(hr)) return false;

	BStr argsBstr(L"--tray");
	if (argsBstr.ok()) {
		hr = execAction->put_Arguments(argsBstr);
		if (FAILED(hr)) return false;
	}

	if (!workDir.empty()) {
		BStr workDirBstr(workDir.c_str());
		if (workDirBstr.ok()) execAction->put_WorkingDirectory(workDirBstr);
	}

	Microsoft::WRL::ComPtr<IRegisteredTask> registeredTask;
	hr = rootFolder->RegisterTaskDefinition(taskName, task.Get(), TASK_CREATE_OR_UPDATE,
		empty, empty, TASK_LOGON_INTERACTIVE_TOKEN, empty, registeredTask.GetAddressOf());
	return SUCCEEDED(hr);
}

static void syncDefaultAutoStart() {
	bool enabled = false;
	if (!readAutoStartPreference(enabled)) {
		writeAutoStartPreference(isAutoStartEnabled());
		return;
	}

	if (setAutoStartEnabled(enabled)) {
		writeAutoStartPreference(enabled);
	}
}

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

static std::wstring appVersionText() {
	std::wstring text = L"v" + widen(SYSMON_APP_VERSION);
	text += L" - ";
	text += widen(SYSMON_GIT_BRANCH);

	std::string commit = SYSMON_GIT_COMMIT;
	if (!commit.empty() && commit != "unknown") {
		text += L" ";
		text += widen(commit);
	}

	if (SYSMON_GIT_DIRTY) {
		text += L" dirty";
	}
	return text;
}

static std::wstring readCpuBrandString() {
	static const std::wstring cached = []() {
		HKEY hKey{};
		if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
			L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
			0,
			KEY_QUERY_VALUE | KEY_WOW64_64KEY,
			&hKey) != ERROR_SUCCESS) {
			return std::wstring{};
		}

		DWORD type = 0;
		DWORD size = 0;
		if (RegQueryValueExW(hKey, L"ProcessorNameString", nullptr, &type, nullptr, &size) != ERROR_SUCCESS || type != REG_SZ || size == 0) {
			RegCloseKey(hKey);
			return std::wstring{};
		}

		std::wstring value(size / sizeof(wchar_t), L'\0');
		if (RegQueryValueExW(hKey, L"ProcessorNameString", nullptr, &type, reinterpret_cast<LPBYTE>(&value[0]), &size) != ERROR_SUCCESS) {
			RegCloseKey(hKey);
			return std::wstring{};
		}
		RegCloseKey(hKey);

		while (!value.empty() && value.back() == L'\0') value.pop_back();
		return value;
	}();
	return cached;
}

struct NetId {
	std::string mac;
	std::vector<std::string> ips;
};

static bool queryPrimaryNetId(NetId& out) {
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

static bool getPrimaryNetId(NetId& out) {
	static std::mutex cacheMutex;
	static NetId cached;
	static bool cachedOk = false;
	static ULONGLONG lastRefresh = 0;

	std::lock_guard<std::mutex> g(cacheMutex);
	ULONGLONG now = GetTickCount64();
	if (lastRefresh != 0 && (now - lastRefresh) < kNetIdCacheMs) {
		out = cached;
		return cachedOk;
	}

	NetId fresh;
	bool ok = queryPrimaryNetId(fresh);
	cached = std::move(fresh);
	cachedOk = ok;
	lastRefresh = now;
	out = cached;
	return cachedOk;
}

static HFONT createUiFont(int pointSize, int weight = FW_NORMAL, const wchar_t* faceName = L"Segoe UI") {
	HDC hdc = GetDC(nullptr);
	int dpiY = hdc ? GetDeviceCaps(hdc, LOGPIXELSY) : 96;
	if (hdc) ReleaseDC(nullptr, hdc);
	int height = -MulDiv(pointSize, dpiY, 72);
	return CreateFontW(height, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
		OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
		DEFAULT_PITCH | FF_SWISS, faceName);
}

static void setControlFont(HWND hwnd, HFONT font) {
	if (hwnd && font) SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

static std::wstring formatDeviceInfo(CpuMonitor* cpuMon = nullptr, std::mutex* cpuMonMutex = nullptr, bool includePerCoreCpu = false) {
	auto mem = getMemInfo();
	auto gpu = getGpuVideoMemoryInfo();
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
	auto clip = [](std::wstring value, size_t maxChars) {
		if (value.size() <= maxChars) return value;
		if (maxChars <= 3) return value.substr(0, maxChars);
		return value.substr(0, maxChars - 3) + L"...";
	};

	std::wostringstream oss;
	oss << L"CPU Usage: ";
	if (cpuPct.has) {
		oss << std::fixed << std::setprecision(1) << cpuPct.value << L"%";
	} else {
		oss << L"n/a";
	}
	oss << L"\r\n";

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
	oss << L"CPU: " << (cpuName.empty() ? L"n/a" : clip(cpuName, 48)) << L"\r\n";
	oss << L"GPU: " << (gpu.adapterName.empty() ? L"n/a" : clip(gpu.adapterName, 48)) << L"\r\n";

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

	std::string out;
	out.reserve(1024);
	out.push_back('{');

	// cpu
	jsonAppendKey(out, "cpu");
	out.push_back('{');
	jsonAppendKey(out, "name");
	jsonAppendQuoted(out, cpuName.empty() ? std::string("n/a") : cpuName);
	out.push_back(',');
	jsonAppendKey(out, "usage_total_percent");
	if (cpuTotal.has) {
		std::ostringstream ss;
		ss.setf(std::ios::fixed);
		ss << std::setprecision(1) << cpuTotal.value;
		out += ss.str();
	} else {
		out += "null";
	}
	out.push_back(',');
	jsonAppendKey(out, "per_core_ok");
	out += (perCoreOk ? "true" : "false");
	out.push_back(',');
	jsonAppendKey(out, "usage_per_core_percent");
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
	jsonAppendQuoted(out, gpuName.empty() ? std::string("n/a") : gpuName);
	out.push_back(',');
	jsonAppendKey(out, "dedicated_bytes");
	jsonAppendOptU64(out, gpu.dedicatedBytes);
	out.push_back(',');
	jsonAppendKey(out, "shared_bytes");
	jsonAppendOptU64(out, gpu.sharedBytes);
	out.push_back(',');
	jsonAppendKey(out, "dedicated_capacity_bytes");
	jsonAppendOptU64(out, gpu.dedicatedCapacityBytes);
	out.push_back(',');
	jsonAppendKey(out, "shared_capacity_bytes");
	jsonAppendOptU64(out, gpu.sharedCapacityBytes);
	out.push_back(',');
	jsonAppendKey(out, "is_usage");
	out += (gpu.isUsage ? "true" : "false");
	out.push_back('}');
	out.push_back(',');

	// memory
	jsonAppendKey(out, "memory");
	out.push_back('{');
	jsonAppendKey(out, "ok");
	out += (mem.ok ? "true" : "false");
	out.push_back(',');
	jsonAppendKey(out, "total_phys_bytes");
	out += mem.ok ? std::to_string(static_cast<unsigned long long>(mem.totalPhysBytes)) : std::string("null");
	out.push_back(',');
	jsonAppendKey(out, "avail_phys_bytes");
	out += mem.ok ? std::to_string(static_cast<unsigned long long>(mem.availPhysBytes)) : std::string("null");
	out.push_back(',');
	jsonAppendKey(out, "used_phys_bytes");
	out += (mem.ok ? std::to_string(static_cast<unsigned long long>(memUsedBytes)) : std::string("null"));
	out.push_back(',');
	jsonAppendKey(out, "used_phys_percent");
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
	jsonAppendQuoted(out, (hasNet && !net.mac.empty()) ? net.mac : std::string("n/a"));
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
	out.push_back('}');
	out.push_back(',');

	// fps (ETW Present-based)
	jsonAppendKey(out, "fps");
	out.push_back('{');
	jsonAppendKey(out, "ok");
	out += (fps.ok ? "true" : "false");
	out.push_back(',');
	jsonAppendKey(out, "pid");
	out += std::to_string(static_cast<unsigned long long>(fps.pid));
	out.push_back(',');
	jsonAppendKey(out, "window_title");
	{
		std::string title = narrowUtf8(fps.windowTitle);
		jsonAppendQuoted(out, title.empty() ? std::string("n/a") : title);
	}
	out.push_back(',');
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
	jsonAppendKey(out, "source");
	jsonAppendQuoted(out, "etw_present");
	out.push_back('}');

	out.push_back('}');
	return out;
}

struct AppState {
	HINSTANCE hInst{};
	HWND hwnd{};
	HWND hTitle{};
	HWND hSubtitle{};
	HWND hVersion{};
	HWND hPortLabel{};
	HWND hPort{};
	HWND hToggle{};
	HWND hStatus{};
	HWND hInfoLabel{};
	HWND hIps{};
	HWND hOptionsLabel{};
	HWND hAutoStart{};
	HWND hAllowLan{};
	HWND hStartOnLaunch{};
	NOTIFYICONDATAW nid{};
	UINT taskbarCreatedMsg{};
	bool trayIconAdded{};
	HFONT hFont{};
	HFONT hTitleFont{};
	HFONT hSmallFont{};
	HFONT hMonoFont{};
	HBRUSH hBgBrush{};
	HBRUSH hSurfaceBrush{};
	HBRUSH hEditBrush{};
	bool closeHintShown{};

	CpuMonitor cpuMon;
	std::mutex cpuMonMutex;
	std::atomic<bool> running{};
	std::uint16_t port{};
	bool allowLan{};
	bool startOnLaunch{ true };
	NetworkServer* server{};
	HANDLE serverThread{};
};

static void applyFonts(AppState& st) {
	setControlFont(st.hTitle, st.hTitleFont);
	setControlFont(st.hSubtitle, st.hSmallFont);
	setControlFont(st.hVersion, st.hSmallFont);
	setControlFont(st.hPortLabel, st.hSmallFont);
	setControlFont(st.hPort, st.hFont);
	setControlFont(st.hToggle, st.hFont);
	setControlFont(st.hStatus, st.hFont);
	setControlFont(st.hInfoLabel, st.hSmallFont);
	setControlFont(st.hIps, st.hMonoFont);
	setControlFont(st.hOptionsLabel, st.hSmallFont);
	setControlFont(st.hAutoStart, st.hFont);
	setControlFont(st.hAllowLan, st.hFont);
	setControlFont(st.hStartOnLaunch, st.hFont);
}

static void deleteUiResources(AppState& st) {
	if (st.hFont) DeleteObject(st.hFont);
	if (st.hTitleFont) DeleteObject(st.hTitleFont);
	if (st.hSmallFont) DeleteObject(st.hSmallFont);
	if (st.hMonoFont) DeleteObject(st.hMonoFont);
	if (st.hBgBrush) DeleteObject(st.hBgBrush);
	if (st.hSurfaceBrush) DeleteObject(st.hSurfaceBrush);
	if (st.hEditBrush) DeleteObject(st.hEditBrush);
	st.hFont = nullptr;
	st.hTitleFont = nullptr;
	st.hSmallFont = nullptr;
	st.hMonoFont = nullptr;
	st.hBgBrush = nullptr;
	st.hSurfaceBrush = nullptr;
	st.hEditBrush = nullptr;
}

static std::wstring listeningStatus(const AppState& st) {
	if (st.allowLan) {
		return L"Listening on LAN port " + std::to_wstring(st.port);
	}
	return L"Listening on 127.0.0.1:" + std::to_wstring(st.port);
}

static void updateUi(AppState& st) {
	SetWindowTextW(st.hIps, formatDeviceInfo(&st.cpuMon, &st.cpuMonMutex, false).c_str());
	RedrawWindow(st.hIps, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);

	if (st.running.load()) {
		SetWindowTextW(st.hStatus, listeningStatus(st).c_str());
		SetWindowTextW(st.hToggle, L"Stop");
	} else {
		SetWindowTextW(st.hStatus, L"Stopped");
		SetWindowTextW(st.hToggle, L"Start");
	}
	if (st.hAutoStart) {
		Button_SetCheck(st.hAutoStart, isAutoStartEnabled() ? BST_CHECKED : BST_UNCHECKED);
	}
	if (st.hAllowLan) {
		Button_SetCheck(st.hAllowLan, st.allowLan ? BST_CHECKED : BST_UNCHECKED);
		EnableWindow(st.hAllowLan, !st.running.load());
	}
	if (st.hStartOnLaunch) {
		Button_SetCheck(st.hStartOnLaunch, st.startOnLaunch ? BST_CHECKED : BST_UNCHECKED);
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

static void redrawStatus(AppState& st) {
	RECT rc;
	GetWindowRect(st.hStatus, &rc);
	MapWindowPoints(nullptr, st.hwnd, reinterpret_cast<POINT*>(&rc), 2);
	InflateRect(&rc, 2, 2);
	RedrawWindow(st.hwnd, &rc, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
}

static void showStartFailure(AppState& st) {
	SetWindowTextW(st.hStatus, (L"Failed to listen on port " + std::to_wstring(st.port)).c_str());
	SetWindowTextW(st.hToggle, L"Start");
	redrawStatus(st);
}

static void stopServer(AppState& st) {
	const bool hadServer = st.running.exchange(false) || st.server || st.serverThread;
	if (!hadServer) return;

	if (st.server) {
		st.server->stop();
	}

	if (st.serverThread) {
		WaitForSingleObject(st.serverThread, INFINITE);
		CloseHandle(st.serverThread);
		st.serverThread = nullptr;
	}
	stopForegroundPresentFpsMonitor();

	if (st.server) {
		delete st.server;
		st.server = nullptr;
	}
}

static bool startServer(AppState& st, std::uint16_t port, bool allowLan) {
	stopServer(st);
	if (port < kMinPort || port > kMaxPort) port = kDefaultPort;
	st.port = port;
	st.allowLan = allowLan;

	HANDLE readyEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	if (!readyEvent) return false;
	auto listening = std::make_shared<std::atomic<bool>>(false);

	st.server = new NetworkServer(port, allowLan, [&st]() {
		// Send JSON as UTF-8 over TCP (one JSON object per line).
		return formatDeviceInfoJson(&st.cpuMon, &st.cpuMonMutex);
	}, [listening, readyEvent]() {
		listening->store(true, std::memory_order_release);
		SetEvent(readyEvent);
	}, []() {
		stopForegroundPresentFpsMonitor();
	});

	st.serverThread = CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
		auto* stp = reinterpret_cast<AppState*>(p);
		NetworkServer* srv = stp->server;
		return srv ? static_cast<DWORD>(srv->run()) : 1;
	}, &st, 0, nullptr);

	if (!st.serverThread) {
		CloseHandle(readyEvent);
		delete st.server;
		st.server = nullptr;
		return false;
	}

	HANDLE handles[] = { readyEvent, st.serverThread };
	DWORD wait = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
	const bool started = (wait == WAIT_OBJECT_0 && listening->load(std::memory_order_acquire));
	CloseHandle(readyEvent);

	if (!started) {
		if (st.server) st.server->stop();
		WaitForSingleObject(st.serverThread, INFINITE);
		CloseHandle(st.serverThread);
		st.serverThread = nullptr;
		delete st.server;
		st.server = nullptr;
		st.running = false;
		return false;
	}

	st.running = true;
	return true;
}

static bool addTrayIcon(AppState& st) {
	st.nid.cbSize = sizeof(st.nid);
	st.nid.hWnd = st.hwnd;
	st.nid.uID = 1;
	st.nid.uFlags = NIF_MESSAGE | NIF_TIP | NIF_ICON;
	st.nid.uCallbackMessage = WM_TRAYICON;
	st.nid.hIcon = LoadIconW(st.hInst, MAKEINTRESOURCEW(IDI_APP_ICON));
	wcscpy_s(st.nid.szTip, _countof(st.nid.szTip), L"SysMonitor");

	if (st.trayIconAdded && Shell_NotifyIconW(NIM_MODIFY, &st.nid)) {
		return true;
	}

	st.trayIconAdded = false;
	if (Shell_NotifyIconW(NIM_ADD, &st.nid)) {
		st.trayIconAdded = true;
		st.nid.uVersion = NOTIFYICON_VERSION_4;
		Shell_NotifyIconW(NIM_SETVERSION, &st.nid);
		KillTimer(st.hwnd, TIMER_ID_TRAY_RETRY);
		return true;
	}

	SetTimer(st.hwnd, TIMER_ID_TRAY_RETRY, kTrayRetryMs, nullptr);
	return false;
}

static void removeTrayIcon(AppState& st) {
	KillTimer(st.hwnd, TIMER_ID_TRAY_RETRY);
	if (st.nid.cbSize && st.trayIconAdded) Shell_NotifyIconW(NIM_DELETE, &st.nid);
	st.trayIconAdded = false;
}

static void showMainWindow(AppState& st) {
	ShowWindow(st.hwnd, SW_RESTORE);
	SetForegroundWindow(st.hwnd);
}

static void showTrayMenu(AppState& st) {
	HMENU menu = CreatePopupMenu();
	AppendMenuW(menu, MF_STRING, 1, L"Show");
	AppendMenuW(menu, MF_STRING, 2, st.running.load() ? L"Stop server" : L"Start server");
	AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
	AppendMenuW(menu, MF_STRING, 3, L"Stop SysMonitor");

	POINT p;
	GetCursorPos(&p);
	SetForegroundWindow(st.hwnd);
	UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, p.x, p.y, 0, st.hwnd, nullptr);
	DestroyMenu(menu);
	PostMessageW(st.hwnd, WM_NULL, 0, 0);

	switch (cmd) {
	case 1:
		showMainWindow(st);
		break;
	case 2: {
		if (st.running.load()) {
			stopServer(st);
			updateUi(st);
		} else {
			std::uint16_t port = st.port;
			if (!parsePortFromEdit(st.hPort, port)) port = st.port;
			bool started = startServer(st, port, st.allowLan);
			updateUi(st);
			if (!started) showStartFailure(st);
		}
		break;
	}
	case 3:
		PostMessageW(st.hwnd, WM_TRAY_EXIT, 0, 0);
		break;
	default:
		break;
	}
}

static void paintPanel(HDC hdc, const RECT& rc) {
	static HBRUSH s_surfaceBrush = CreateSolidBrush(kColorSurface);
	static HPEN s_borderPen = CreatePen(PS_SOLID, 1, kColorBorder);
	HGDIOBJ oldBrush = SelectObject(hdc, s_surfaceBrush);
	HGDIOBJ oldPen = SelectObject(hdc, s_borderPen);
	RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, 8, 8);
	SelectObject(hdc, oldPen);
	SelectObject(hdc, oldBrush);
}

static void paintAppBackground(HDC hdc, const RECT& rc) {
	static HBRUSH s_bgBrush = CreateSolidBrush(kColorBg);
	FillRect(hdc, &rc, s_bgBrush);

	const int margin = 16;
	const int panelRight = (rc.right - margin > margin + 1) ? (rc.right - margin) : (margin + 1);
	RECT serverPanel{ margin, 76, panelRight, 158 };
	RECT infoPanel{ margin, 168, panelRight, 318 };
	RECT optionsPanel{ margin, 328, panelRight, 454 };
	paintPanel(hdc, serverPanel);
	paintPanel(hdc, infoPanel);
	paintPanel(hdc, optionsPanel);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	auto* st = reinterpret_cast<AppState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

	if (st && st->taskbarCreatedMsg != 0 && msg == st->taskbarCreatedMsg) {
		st->trayIconAdded = false;
		addTrayIcon(*st);
		return 0;
	}

	switch (msg) {
	case WM_ERASEBKGND: {
		HDC hdc = reinterpret_cast<HDC>(wParam);
		RECT rc{};
		GetClientRect(hwnd, &rc);
		paintAppBackground(hdc, rc);
		return 1;
	}
	case WM_CTLCOLOREDIT: {
		HDC hdc = reinterpret_cast<HDC>(wParam);
		SetBkMode(hdc, OPAQUE);
		SetBkColor(hdc, kColorSurface);
		SetTextColor(hdc, kColorText);
		return reinterpret_cast<INT_PTR>(st && st->hEditBrush ? st->hEditBrush : GetStockObject(WHITE_BRUSH));
	}
	case WM_CTLCOLORSTATIC: {
		HDC hdc = reinterpret_cast<HDC>(wParam);
		HWND ctl = reinterpret_cast<HWND>(lParam);
		SetBkMode(hdc, TRANSPARENT);
		SetTextColor(hdc, kColorText);
		if (st) {
			if (ctl == st->hSubtitle || ctl == st->hVersion || ctl == st->hPortLabel || ctl == st->hInfoLabel || ctl == st->hOptionsLabel) {
				SetTextColor(hdc, kColorMuted);
			} else if (ctl == st->hStatus) {
				SetTextColor(hdc, st->running.load() ? kColorRunning : kColorStopped);
				return reinterpret_cast<INT_PTR>(st->hBgBrush);
			}
			if (ctl == st->hPortLabel || ctl == st->hInfoLabel || ctl == st->hIps || ctl == st->hOptionsLabel) {
				return reinterpret_cast<INT_PTR>(st->hSurfaceBrush);
			}
			return reinterpret_cast<INT_PTR>(st->hBgBrush);
		}
		return reinterpret_cast<INT_PTR>(GetStockObject(WHITE_BRUSH));
	}
	case WM_CTLCOLORBTN: {
		HDC hdc = reinterpret_cast<HDC>(wParam);
		SetBkMode(hdc, TRANSPARENT);
		SetTextColor(hdc, kColorText);
		return reinterpret_cast<INT_PTR>(st && st->hSurfaceBrush ? st->hSurfaceBrush : GetStockObject(WHITE_BRUSH));
	}
	case WM_CREATE: {
		CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
		st = reinterpret_cast<AppState*>(cs->lpCreateParams);
		st->hwnd = hwnd;
		SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
		st->taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");

		st->hFont = createUiFont(9);
		st->hTitleFont = createUiFont(15, FW_SEMIBOLD);
		st->hSmallFont = createUiFont(8);
		st->hMonoFont = createUiFont(8, FW_NORMAL, L"Consolas");
		st->hBgBrush = CreateSolidBrush(kColorBg);
		st->hSurfaceBrush = CreateSolidBrush(kColorSurface);
		st->hEditBrush = CreateSolidBrush(kColorSurface);

		st->hTitle = CreateWindowW(L"STATIC", L"SysMonitor", WS_CHILD | WS_VISIBLE | SS_LEFT,
			20, 14, 180, 24, hwnd, controlIdMenu(IDC_TITLE), st->hInst, nullptr);
		st->hSubtitle = CreateWindowW(L"STATIC", L"Local telemetry server", WS_CHILD | WS_VISIBLE | SS_LEFT,
			20, 38, 220, 16, hwnd, controlIdMenu(IDC_SUBTITLE), st->hInst, nullptr);
		std::wstring versionText = appVersionText();
		st->hVersion = CreateWindowW(L"STATIC", versionText.c_str(), WS_CHILD | WS_VISIBLE | SS_LEFT,
			20, 56, 300, 16, hwnd, controlIdMenu(IDC_VERSION_LABEL), st->hInst, nullptr);
		st->hStatus = CreateWindowW(L"STATIC", L"Stopped", WS_CHILD | WS_VISIBLE | SS_RIGHT,
			248, 24, 200, 22, hwnd, controlIdMenu(IDC_STATUS), st->hInst, nullptr);

		st->hPortLabel = CreateWindowW(L"STATIC", L"PORT", WS_CHILD | WS_VISIBLE | SS_LEFT,
			32, 92, 100, 16, hwnd, controlIdMenu(IDC_PORT_LABEL), st->hInst, nullptr);
		st->hPort = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", std::to_wstring(st->port).c_str(),
			WS_CHILD | WS_VISIBLE | ES_NUMBER | ES_AUTOHSCROLL, 32, 114, 116, 26,
			hwnd, controlIdMenu(IDC_PORT), st->hInst, nullptr);
		st->hToggle = CreateWindowW(L"BUTTON", L"Start", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
			164, 113, 96, 28, hwnd, controlIdMenu(IDC_BTN_TOGGLE), st->hInst, nullptr);

		st->hInfoLabel = CreateWindowW(L"STATIC", L"SYSTEM SNAPSHOT", WS_CHILD | WS_VISIBLE | SS_LEFT,
			32, 184, 160, 16, hwnd, controlIdMenu(IDC_INFO_LABEL), st->hInst, nullptr);
		st->hIps = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
			32, 206, 400, 104, hwnd, controlIdMenu(IDC_IPS), st->hInst, nullptr);

		st->hOptionsLabel = CreateWindowW(L"STATIC", L"OPTIONS", WS_CHILD | WS_VISIBLE | SS_LEFT,
			32, 342, 120, 16, hwnd, controlIdMenu(IDC_OPTIONS_LABEL), st->hInst, nullptr);
		st->hStartOnLaunch = CreateWindowW(L"BUTTON", L"Start server on launch", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
			32, 362, 220, 22, hwnd, controlIdMenu(IDC_START_ON_LAUNCH), st->hInst, nullptr);
		st->hAutoStart = CreateWindowW(L"BUTTON", L"Run at startup", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
			32, 388, 160, 22, hwnd, controlIdMenu(IDC_AUTOSTART), st->hInst, nullptr);
		st->hAllowLan = CreateWindowW(L"BUTTON", L"Allow LAN connections", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
			32, 414, 220, 22, hwnd, controlIdMenu(IDC_ALLOW_LAN), st->hInst, nullptr);
		applyFonts(*st);

		addTrayIcon(*st);
		syncDefaultAutoStart();
		st->startOnLaunch = loadStartOnLaunchPreference();
		if (st->startOnLaunch) {
			bool started = startServer(*st, st->port, st->allowLan);
			updateUi(*st);
			if (!started) showStartFailure(*st);
		} else {
			updateUi(*st);
		}
		SetTimer(hwnd, TIMER_ID_SEND, kUiRefreshMs, nullptr);
		return 0;
	}
	case WM_TIMER:
		if (wParam == TIMER_ID_SEND && st) {
			if (IsWindowVisible(st->hwnd) && !IsIconic(st->hwnd)) {
				SetWindowTextW(st->hIps, formatDeviceInfo(&st->cpuMon, &st->cpuMonMutex).c_str());
				RedrawWindow(st->hIps, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
			}
			return 0;
		}
		if (wParam == TIMER_ID_TRAY_RETRY && st) {
			addTrayIcon(*st);
			return 0;
		}
		return 0;
	case WM_SIZE:
		if (wParam == SIZE_MINIMIZED && st) {
			ShowWindow(hwnd, SW_HIDE);
			return 0;
		}
		return 0;
	case WM_COMMAND:
		if (!st) return 0;
		if (LOWORD(wParam) == IDC_START_ON_LAUNCH && HIWORD(wParam) == BN_CLICKED) {
			bool enable = Button_GetCheck(st->hStartOnLaunch) == BST_CHECKED;
			if (!writeStartOnLaunchPreference(enable)) {
				Button_SetCheck(st->hStartOnLaunch, st->startOnLaunch ? BST_CHECKED : BST_UNCHECKED);
				SetWindowTextW(st->hStatus, L"Failed to update launch setting");
				redrawStatus(*st);
			} else {
				st->startOnLaunch = enable;
				updateUi(*st);
			}
			return 0;
		}
		if (LOWORD(wParam) == IDC_AUTOSTART && HIWORD(wParam) == BN_CLICKED) {
			bool enable = Button_GetCheck(st->hAutoStart) == BST_CHECKED;
			if (!setAutoStartEnabled(enable) || !writeAutoStartPreference(enable)) {
				Button_SetCheck(st->hAutoStart, enable ? BST_UNCHECKED : BST_CHECKED);
				SetWindowTextW(st->hStatus, L"Failed to update startup setting");
				redrawStatus(*st);
			} else {
				updateUi(*st);
			}
			return 0;
		}
		if (LOWORD(wParam) == IDC_ALLOW_LAN && HIWORD(wParam) == BN_CLICKED) {
			if (!st->running.load()) {
				st->allowLan = Button_GetCheck(st->hAllowLan) == BST_CHECKED;
			}
			updateUi(*st);
			return 0;
		}
		if (LOWORD(wParam) == IDC_BTN_TOGGLE) {
			if (st->running.load()) {
				stopServer(*st);
				updateUi(*st);
			} else {
				std::uint16_t port = st->port;
				if (!parsePortFromEdit(st->hPort, port)) port = st->port;
				bool allowLan = Button_GetCheck(st->hAllowLan) == BST_CHECKED;
				bool started = startServer(*st, port, allowLan);
				updateUi(*st);
				if (!started) showStartFailure(*st);
			}
			return 0;
		}
		return 0;
	case WM_TRAYICON:
		if (!st) return 0;
		switch (LOWORD(lParam)) {
		case WM_RBUTTONUP:
		case WM_CONTEXTMENU:
			showTrayMenu(*st);
			return 0;
		case WM_LBUTTONDBLCLK:
		case NIN_SELECT:
		case NIN_KEYSELECT:
			showMainWindow(*st);
			return 0;
		default:
			return 0;
		}
	case WM_TRAY_EXIT:
		DestroyWindow(hwnd);
		return 0;
	case WM_SHOW_MAIN_WINDOW:
		if (st) showMainWindow(*st);
		return 0;
	case WM_CLOSE:
		if (st && !st->closeHintShown) {
			st->closeHintShown = true;
			MessageBoxW(hwnd,
				L"SysMonitor will keep running in the notification area.\n\n"
				L"To exit completely, right-click the tray icon and choose Stop SysMonitor.",
				L"SysMonitor",
				MB_OK | MB_ICONINFORMATION);
		}
		ShowWindow(hwnd, SW_HIDE);
		return 0;
	case WM_DESTROY:
		if (st) {
			stopServer(*st);
			removeTrayIcon(*st);
			deleteUiResources(*st);
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
	wc.lpszClassName = UiWindowClassName();
	wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));
	wc.hCursor = LoadCursor(nullptr, IDC_ARROW);

	if (!RegisterClassW(&wc)) return 1;

	DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
	DWORD exStyle = 0;
	RECT wndRect{ 0, 0, kWndWidth, kWndHeight };
	AdjustWindowRectEx(&wndRect, style, FALSE, exStyle);
	int windowWidth = wndRect.right - wndRect.left;
	int windowHeight = wndRect.bottom - wndRect.top;

	HWND hwnd = CreateWindowExW(exStyle, UiWindowClassName(), L"SysMonitor", style,
		CW_USEDEFAULT, CW_USEDEFAULT, windowWidth, windowHeight, nullptr, nullptr, hInstance, &st);
	if (!hwnd) return 1;

	if (cfg.startMinimized) {
		ShowWindow(hwnd, SW_HIDE);
	} else {
		ShowWindow(hwnd, SW_SHOWNORMAL);
		UpdateWindow(hwnd);
	}

	MSG msg;
	while (GetMessageW(&msg, nullptr, 0, 0)) {
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}
	return static_cast<int>(msg.wParam);
}

} // namespace sysmon
