#include "sys_gpu.h"

#include <windows.h>
#include <tlhelp32.h>
#include <pdh.h>
#include <pdhmsg.h>

#include <evntrace.h>
#include <tdh.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>
#include <algorithm>

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "tdh.lib")
#pragma comment(lib, "rpcrt4.lib") // For UuidCreate

namespace sysmon {

struct EtwEventKey {
	GUID provider;
	USHORT id{};
	UCHAR version{};
	UCHAR opcode{};

	bool operator==(const EtwEventKey& o) const noexcept {
		return id == o.id && version == o.version && opcode == o.opcode &&
			0 == memcmp(&provider, &o.provider, sizeof(GUID));
	}
};

struct EtwEventKeyHash {
	size_t operator()(const EtwEventKey& k) const noexcept {
		const std::uint64_t* p = reinterpret_cast<const std::uint64_t*>(&k.provider);
		size_t h = static_cast<size_t>(p[0] ^ p[1]);
		h ^= static_cast<size_t>(k.id) << 1;
		h ^= static_cast<size_t>(k.version) << 9;
		h ^= static_cast<size_t>(k.opcode) << 17;
		return h;
	}
};

class PresentEtwMonitor {
public:
	static PresentEtwMonitor& instance() {
		static PresentEtwMonitor m;
		return m;
	}

	// Chromium 的 GPU process 是子 process，需合併前景 + 子 process 的 Present 事件
	static void collectPidsForFps(DWORD rootPid, std::vector<DWORD>& out) {
		out.clear();
		out.push_back(rootPid);
		HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
		if (snap == INVALID_HANDLE_VALUE) return;
		PROCESSENTRY32W pe{};
		pe.dwSize = sizeof(pe);
		for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
			if (pe.th32ParentProcessID == rootPid)
				out.push_back(pe.th32ProcessID);
		}
		CloseHandle(snap);
	}

	PresentFpsInfo getForegroundFps() {
		ensureStarted();

		PresentFpsInfo out{};
		HWND fg = GetForegroundWindow();
		HWND top = fg ? GetAncestor(fg, GA_ROOT) : nullptr;
		if (!top) top = fg;
		if (top) {
			static constexpr int kMaxWindowTitleLen = 4096;
			int len = GetWindowTextLengthW(top);
			if (len > 0) {
				if (len > kMaxWindowTitleLen) len = kMaxWindowTitleLen;
				std::wstring title(static_cast<size_t>(len), L'\0');
				int got = GetWindowTextW(top, &title[0], len + 1);
				if (got > 0) {
					title.resize(static_cast<size_t>(got));
					out.windowTitle = std::move(title);
				}
			}
		}
		DWORD pid = 0;
		if (fg) GetWindowThreadProcessId(fg, &pid);
		out.pid = static_cast<std::uint32_t>(pid);
		
		if (pid == 0) return out;
		if (!_running.load(std::memory_order_acquire)) return out;

		std::vector<DWORD> pidsToCheck;
		collectPidsForFps(pid, pidsToCheck);

		std::lock_guard<std::mutex> g(_mtx);
		std::vector<double> merged;
		for (DWORD p : pidsToCheck) {
			auto it = _perPid.find(p);
			if (it != _perPid.end()) {
				for (double t : it->second) merged.push_back(t);
			}
		}
		std::sort(merged.begin(), merged.end());
		merged.erase(std::unique(merged.begin(), merged.end()), merged.end());
		if (merged.size() >= 2) {
			double dt = merged.back() - merged.front();
			if (dt > 0.0) {
				out.ok = true;
				out.fps = static_cast<double>(merged.size() - 1) / dt;
			}
		}

		return out;
	}

	bool isRunning() const {
		return _running.load(std::memory_order_acquire);
	}

private:
	PresentEtwMonitor() {
		QueryPerformanceFrequency(&_qpf);
	}

	~PresentEtwMonitor() {
		stop();
	}

	PresentEtwMonitor(const PresentEtwMonitor&) = delete;
	PresentEtwMonitor& operator=(const PresentEtwMonitor&) = delete;

	void ensureStarted() {
		bool expected = false;
		if (!_startOnce.compare_exchange_strong(expected, true)) return;
		start();
	}

	static void WINAPI onEventRecord(EVENT_RECORD* rec) {
		auto* self = reinterpret_cast<PresentEtwMonitor*>(rec->UserContext);
		if (!self) return;
		self->handleEvent(*rec);
	}

	std::wstring getEventNameCached(const EVENT_RECORD& rec) {
		EtwEventKey key;
		key.provider = rec.EventHeader.ProviderId;
		key.id = rec.EventHeader.EventDescriptor.Id;
		key.version = rec.EventHeader.EventDescriptor.Version;
		key.opcode = rec.EventHeader.EventDescriptor.Opcode;

		{
			std::lock_guard<std::mutex> g(_metaMtx);
			auto it = _eventNameCache.find(key);
			if (it != _eventNameCache.end()) return it->second;
		}

		ULONG sz = 0;
		ULONG st = TdhGetEventInformation(const_cast<EVENT_RECORD*>(&rec), 0, nullptr, nullptr, &sz);
		if (st != ERROR_INSUFFICIENT_BUFFER || sz == 0) {
			return {};
		}
		std::vector<std::uint8_t> buf(sz);
		auto* info = reinterpret_cast<PTRACE_EVENT_INFO>(buf.data());
		st = TdhGetEventInformation(const_cast<EVENT_RECORD*>(&rec), 0, nullptr, info, &sz);
		if (st != ERROR_SUCCESS) return {};

		std::wstring name;
		if (info->EventNameOffset != 0) {
			const wchar_t* p = reinterpret_cast<const wchar_t*>(reinterpret_cast<const std::uint8_t*>(info) + info->EventNameOffset);
			name = p ? p : L"";
		}

		{
			std::lock_guard<std::mutex> g(_metaMtx);
			_eventNameCache.emplace(key, name);
		}
		return name;
	}

	bool isPresentEvent(const EVENT_RECORD& rec) {
		// --- BYPASS NAME CHECK FOR KNOWN IDs ---
		// If name resolution fails (which it is), fall back to hardcoded IDs for DxgKrnl.
		// ID 42: Present_Start
		// ID 48: PresentMultiplaneOverlay_Start
		// ID 184: Flip (IndependentFlip)
		USHORT id = rec.EventHeader.EventDescriptor.Id;
		if (id == 42 || id == 48 || id == 184) return true;

		std::wstring name = getEventNameCached(rec);

		if (name.empty()) return false;

		std::wstring lower = name;
		std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);

		if (!_wcsicmp(name.c_str(), L"Present")) return true;
		if (!_wcsicmp(name.c_str(), L"Present_Stop")) return true;
		if (!_wcsicmp(name.c_str(), L"PresentStop")) return true;
		if (!_wcsicmp(name.c_str(), L"Flip")) return true;
		if (!_wcsicmp(name.c_str(), L"IndependentFlip")) return true;
		if (!_wcsicmp(name.c_str(), L"FlipInterval")) return true;
		if (!_wcsicmp(name.c_str(), L"TokenCompositionSurfaceObject")) return true;
		
		// Broad catch-all for anything "present" or "flip"
		if (lower.find(L"present") != std::wstring::npos) return true;
		if (lower.find(L"flip") != std::wstring::npos) return true;
		
		return false;
	}

	static double qpcToSeconds(const LARGE_INTEGER& qpc, const LARGE_INTEGER& freq) {
		return static_cast<double>(qpc.QuadPart) / static_cast<double>(freq.QuadPart);
	}

	void handleEvent(const EVENT_RECORD& rec) {
		if (!_running.load(std::memory_order_acquire)) return;

		if (!isPresentEvent(rec)) return;

		const DWORD pid = rec.EventHeader.ProcessId;
		if (pid == 0) return;

		double t = 0.0;
		LARGE_INTEGER ts = rec.EventHeader.TimeStamp;
		t = qpcToSeconds(ts, _qpf);

		std::lock_guard<std::mutex> g(_mtx);
		auto& dq = _perPid[pid];
		dq.push_back(t);
		while (!dq.empty() && (t - dq.front()) > 2.0) dq.pop_front();
		if (dq.size() > 600) dq.erase(dq.begin(), dq.end() - 600);
	}

	void start() {
		// Hardcoded GUIDs to avoid lookup failure
		// Microsoft-Windows-DxgKrnl
		GUID dxgGuid = { 0x802ec45a, 0x1e99, 0x4b83, { 0x99, 0x20, 0x87, 0xc9, 0x82, 0x77, 0xba, 0x9d } };
		// Microsoft-Windows-DXGI
		GUID dxgiGuid = { 0xca11c036, 0x0102, 0x4a2d, { 0xa6, 0xad, 0xf0, 0x3c, 0xfe, 0xd5, 0xd3, 0xc9 } };
		
		const wchar_t* kSessionName = L"SysMonitorPresentSession";
		const ULONG propsSize = sizeof(EVENT_TRACE_PROPERTIES) + 2 * 1024;
		std::vector<std::uint8_t> propsBuf(propsSize);
		auto* props = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(propsBuf.data());
		ZeroMemory(props, propsSize);
		props->Wnode.BufferSize = propsSize;
		props->Wnode.Flags = WNODE_FLAG_TRACED_GUID; 
		props->Wnode.ClientContext = 1; // QPC
		props->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
		props->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
		
		// Generate a Session GUID
		UuidCreate(&props->Wnode.Guid);

		TRACEHANDLE session = 0;
		ULONG st = StartTraceW(&session, kSessionName, props);
		if (st == ERROR_ALREADY_EXISTS) {
			ControlTraceW(0, kSessionName, props, EVENT_TRACE_CONTROL_STOP);
			st = StartTraceW(&session, kSessionName, props);
		}
		if (st != ERROR_SUCCESS) {
			return;
		}

		// Enable DxgKrnl
		st = EnableTraceEx2(session, &dxgGuid, EVENT_CONTROL_CODE_ENABLE_PROVIDER, TRACE_LEVEL_VERBOSE, 0xFFFFFFFFFFFFFFFF, 0, 0, nullptr);

		// Enable DXGI
		st = EnableTraceEx2(session, &dxgiGuid, EVENT_CONTROL_CODE_ENABLE_PROVIDER, TRACE_LEVEL_VERBOSE, 0xFFFFFFFFFFFFFFFF, 0, 0, nullptr);

		EVENT_TRACE_LOGFILEW lf{};
		lf.LoggerName = const_cast<LPWSTR>(kSessionName);
		lf.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
		lf.EventRecordCallback = &PresentEtwMonitor::onEventRecord;
		lf.Context = this;

		TRACEHANDLE trace = OpenTraceW(&lf);
		if (trace == INVALID_PROCESSTRACE_HANDLE) {
			ControlTraceW(session, kSessionName, props, EVENT_TRACE_CONTROL_STOP);
			return;
		}

		_session = session;
		_trace = trace;
		_running.store(true, std::memory_order_release);
		_thread = std::thread([this]() {
			ProcessTrace(&_trace, 1, nullptr, nullptr);
			_running.store(false, std::memory_order_release);
		});
	}

	void stop() {
		if (!_startOnce.load()) return;
		_running.store(false, std::memory_order_release);

		TRACEHANDLE trace = _trace;
		if (trace && trace != INVALID_PROCESSTRACE_HANDLE) {
			CloseTrace(trace);
			_trace = 0;
		}

		if (_thread.joinable()) _thread.join();

		if (_session) {
			const wchar_t* kSessionName = L"SysMonitorPresentSession";
			const ULONG propsSize = sizeof(EVENT_TRACE_PROPERTIES) + 2 * 1024;
			std::vector<std::uint8_t> propsBuf(propsSize);
			auto* props = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(propsBuf.data());
			ZeroMemory(props, propsSize);
			props->Wnode.BufferSize = propsSize;
			props->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
			ControlTraceW(_session, kSessionName, props, EVENT_TRACE_CONTROL_STOP);
			_session = 0;
		}
	}

	std::atomic<bool> _startOnce{ false };
	std::atomic<bool> _running{ false };
	LARGE_INTEGER _qpf{};
	TRACEHANDLE _session{};
	TRACEHANDLE _trace{};
	std::thread _thread;

	std::mutex _mtx;
	std::unordered_map<DWORD, std::deque<double>> _perPid;

	std::mutex _metaMtx;
	std::unordered_map<EtwEventKey, std::wstring, EtwEventKeyHash> _eventNameCache;
};

// 使用 PDH 取得系統級 GPU 記憶體使用量（DXGI QueryVideoMemoryInfo 僅回傳「本 process」使用量，會不準）
// 使用 PdhAddEnglishCounterW 以支援非英文 Windows；使用 (*) wildcard 取得所有 adapter
static bool getGpuMemoryViaPdh(std::uint64_t& outDedicatedBytes, std::uint64_t& outSharedBytes) {
	outDedicatedBytes = 0;
	outSharedBytes = 0;
	PDH_HQUERY hQuery = nullptr;
	if (PdhOpenQueryW(nullptr, 0, &hQuery) != ERROR_SUCCESS) return false;

	auto closeQuery = [&]() { if (hQuery) { PdhCloseQuery(hQuery); hQuery = nullptr; } };

	// 使用 (*) 與 PdhAddEnglishCounterW：instance 名稱為 luid_0x..._phys_0_part_0 等，非 "0"
	using PdhAddEnglishCounterFn = PDH_STATUS(WINAPI*)(PDH_HQUERY, LPCWSTR, DWORD_PTR, PDH_HCOUNTER*);
	static PdhAddEnglishCounterFn pAddEng = []() -> PdhAddEnglishCounterFn {
		HMODULE h = GetModuleHandleW(L"pdh.dll");
		return h ? reinterpret_cast<PdhAddEnglishCounterFn>(GetProcAddress(h, "PdhAddEnglishCounterW")) : nullptr;
	}();

	auto addCounter = [&](const wchar_t* path) -> PDH_HCOUNTER {
		PDH_HCOUNTER hCnt = nullptr;
		PDH_STATUS st = pAddEng ? pAddEng(hQuery, path, 0, &hCnt) : PdhAddCounterW(hQuery, path, 0, &hCnt);
		return (st == ERROR_SUCCESS) ? hCnt : nullptr;
	};

	auto getCounterSum = [&](PDH_HCOUNTER hCnt) -> std::uint64_t {
		if (!hCnt) return 0;
		PdhCollectQueryData(hQuery);
		PdhCollectQueryData(hQuery);  // 部分計數器需要兩次取樣
		DWORD bufSize = 0, itemCount = 0;
		PDH_STATUS st = PdhGetFormattedCounterArrayW(hCnt, PDH_FMT_LARGE, &bufSize, &itemCount, nullptr);
		if (st != PDH_MORE_DATA || bufSize == 0) {
			PdhRemoveCounter(hCnt);
			return 0;
		}
		std::vector<std::uint8_t> buf(bufSize);
		PDH_FMT_COUNTERVALUE_ITEM_W* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buf.data());
		if (PdhGetFormattedCounterArrayW(hCnt, PDH_FMT_LARGE, &bufSize, &itemCount, items) != ERROR_SUCCESS) {
			PdhRemoveCounter(hCnt);
			return 0;
		}
		std::uint64_t sum = 0;
		for (DWORD i = 0; i < itemCount; ++i) {
			if (items[i].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA) {
				sum += static_cast<std::uint64_t>(items[i].FmtValue.largeValue);
			}
		}
		PdhRemoveCounter(hCnt);
		return sum;
	};

	PDH_HCOUNTER hLocal = addCounter(L"\\GPU Local Adapter Memory(*)\\Local Usage");
	if (hLocal) outDedicatedBytes = getCounterSum(hLocal);

	PDH_HCOUNTER hNonLocal = addCounter(L"\\GPU Non Local Adapter Memory(*)\\Non Local Usage");
	if (hNonLocal) outSharedBytes = getCounterSum(hNonLocal);

	closeQuery();
	return (outDedicatedBytes > 0 || outSharedBytes > 0);
}

// 使用 PDH GPU Engine 取得 GPU 運算使用率（0~100，類似 CPU %）
static bool getGpuUtilizationViaPdh(double& outPercent) {
	outPercent = -1.0;
	PDH_HQUERY hQuery = nullptr;
	if (PdhOpenQueryW(nullptr, 0, &hQuery) != ERROR_SUCCESS) return false;

	using PdhAddEnglishCounterFn = PDH_STATUS(WINAPI*)(PDH_HQUERY, LPCWSTR, DWORD_PTR, PDH_HCOUNTER*);
	static PdhAddEnglishCounterFn pAddEng = []() -> PdhAddEnglishCounterFn {
		HMODULE h = GetModuleHandleW(L"pdh.dll");
		return h ? reinterpret_cast<PdhAddEnglishCounterFn>(GetProcAddress(h, "PdhAddEnglishCounterW")) : nullptr;
	}();

	PDH_HCOUNTER hCnt = nullptr;
	PDH_STATUS st = pAddEng ? pAddEng(hQuery, L"\\GPU Engine(*)\\Utilization Percentage", 0, &hCnt) : PdhAddCounterW(hQuery, L"\\GPU Engine(*)\\Utilization Percentage", 0, &hCnt);
	if (st != ERROR_SUCCESS || !hCnt) {
		PdhCloseQuery(hQuery);
		return false;
	}

	PdhCollectQueryData(hQuery);
	Sleep(50);
	PdhCollectQueryData(hQuery);

	DWORD bufSize = 0, itemCount = 0;
	st = PdhGetFormattedCounterArrayW(hCnt, PDH_FMT_DOUBLE, &bufSize, &itemCount, nullptr);
	if (st != PDH_MORE_DATA || bufSize == 0) {
		PdhRemoveCounter(hCnt);
		PdhCloseQuery(hQuery);
		return false;
	}
	std::vector<std::uint8_t> buf(bufSize);
	PDH_FMT_COUNTERVALUE_ITEM_W* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buf.data());
	if (PdhGetFormattedCounterArrayW(hCnt, PDH_FMT_DOUBLE, &bufSize, &itemCount, items) != ERROR_SUCCESS) {
		PdhRemoveCounter(hCnt);
		PdhCloseQuery(hQuery);
		return false;
	}
	double maxVal = 0.0;
	for (DWORD i = 0; i < itemCount; ++i) {
		if (items[i].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA) {
			double v = items[i].FmtValue.doubleValue;
			if (v > maxVal) maxVal = v;
		}
	}
	PdhRemoveCounter(hCnt);
	PdhCloseQuery(hQuery);
	outPercent = (maxVal > 100.0) ? 100.0 : maxVal;
	return (outPercent >= 0.0);
}

	GpuMemInfo getGpuVideoMemoryInfo() {
	GpuMemInfo out;

	auto fillDescCaps = [&out](IDXGIAdapter1* a) {
		DXGI_ADAPTER_DESC1 desc{};
		if (SUCCEEDED(a->GetDesc1(&desc))) {
			out.adapterName = desc.Description;
			if (desc.DedicatedVideoMemory != 0) {
				out.dedicatedCapacityBytes = { true, static_cast<std::uint64_t>(desc.DedicatedVideoMemory) };
			}
			if (desc.SharedSystemMemory != 0) {
				out.sharedCapacityBytes = { true, static_cast<std::uint64_t>(desc.SharedSystemMemory) };
			}
		}
	};

	// 遍歷所有 adapter 並累加 capacity，以對應 PDH 的「多 GPU 加總」usage
	auto sumAllAdapterCapacity = [&]() {
		IDXGIFactory1* fact = nullptr;
		if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&fact))) || !fact) return;
		IDXGIAdapter1* adapter = nullptr;
		std::uint64_t totalDedicated = 0, totalShared = 0;
		for (UINT i = 0; fact->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
			DXGI_ADAPTER_DESC1 desc{};
			if (SUCCEEDED(adapter->GetDesc1(&desc)) && (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
				totalDedicated += static_cast<std::uint64_t>(desc.DedicatedVideoMemory);
				totalShared += static_cast<std::uint64_t>(desc.SharedSystemMemory);
				if (out.adapterName.empty()) out.adapterName = desc.Description;
			}
			adapter->Release();
		}
		fact->Release();
		if (totalDedicated > 0) out.dedicatedCapacityBytes = { true, totalDedicated };
		if (totalShared > 0) out.sharedCapacityBytes = { true, totalShared };
	};
	// 當 usage > capacity 時，以 usage 為下限（多 GPU 或 DXGI 枚舉不全時）
	auto ensureCapacityGeUsage = [&]() {
		if (out.dedicatedBytes.has && out.dedicatedCapacityBytes.has &&
			out.dedicatedBytes.value > out.dedicatedCapacityBytes.value) {
			out.dedicatedCapacityBytes = { true, out.dedicatedBytes.value };
		}
	};

	IDXGIFactory1* factoryBase = nullptr;
	if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factoryBase))) || !factoryBase) {
		return out;
	}

	IDXGIFactory6* factory6 = nullptr;
	if (SUCCEEDED(factoryBase->QueryInterface(__uuidof(IDXGIFactory6), reinterpret_cast<void**>(&factory6))) && factory6) {
		IDXGIAdapter1* adapter = nullptr;
		if (SUCCEEDED(factory6->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, __uuidof(IDXGIAdapter1), reinterpret_cast<void**>(&adapter))) && adapter) {
			fillDescCaps(adapter);

			// 使用 PDH 取得「系統級」GPU 記憶體使用量（DXGI CurrentUsage 只回傳本 process 使用量，不準）
			std::uint64_t pdhDedicated = 0, pdhShared = 0;
			bool pdhOk = getGpuMemoryViaPdh(pdhDedicated, pdhShared);
			if (pdhOk) {
				out.dedicatedBytes = { true, pdhDedicated };
				out.sharedBytes = { true, pdhShared };
				out.isUsage = true;
				sumAllAdapterCapacity();
				ensureCapacityGeUsage();
			} else {
				// Fallback: DXGI（僅本 process，數值通常很小）
				IDXGIAdapter3* adapter3 = nullptr;
				if (SUCCEEDED(adapter->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&adapter3))) && adapter3) {
					DXGI_QUERY_VIDEO_MEMORY_INFO info{};
					if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
						out.dedicatedBytes = { true, static_cast<std::uint64_t>(info.CurrentUsage) };
						out.isUsage = true;
					}
					if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &info))) {
						out.sharedBytes = { true, static_cast<std::uint64_t>(info.CurrentUsage) };
						out.isUsage = true;
					}
					adapter3->Release();
				}
			}

			// 計算專用記憶體使用率
			if (out.dedicatedBytes.has && out.dedicatedCapacityBytes.has && out.dedicatedCapacityBytes.value > 0) {
				out.dedicatedUsagePercent = (static_cast<double>(out.dedicatedBytes.value) * 100.0) / static_cast<double>(out.dedicatedCapacityBytes.value);
				if (out.dedicatedUsagePercent > 100.0) out.dedicatedUsagePercent = 100.0;
			}
			// GPU 運算使用率
			double utilPct = -1.0;
			if (getGpuUtilizationViaPdh(utilPct)) out.utilizationPercent = utilPct;

			adapter->Release();
		}
		factory6->Release();
		factoryBase->Release();
		return out;
	}

	// 無 Factory6，用 EnumAdapters1
	IDXGIAdapter1* adapter = nullptr;
	for (UINT i = 0; factoryBase->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
		DXGI_ADAPTER_DESC1 desc{};
		if (SUCCEEDED(adapter->GetDesc1(&desc))) {
			if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
				adapter->Release();
				adapter = nullptr;
				continue;
			}
			out.adapterName = desc.Description;
			if (desc.DedicatedVideoMemory != 0) out.dedicatedCapacityBytes = { true, static_cast<std::uint64_t>(desc.DedicatedVideoMemory) };
			if (desc.SharedSystemMemory != 0) out.sharedCapacityBytes = { true, static_cast<std::uint64_t>(desc.SharedSystemMemory) };
		}

		std::uint64_t pdhDedicated = 0, pdhShared = 0;
		if (getGpuMemoryViaPdh(pdhDedicated, pdhShared)) {
			out.dedicatedBytes = { true, pdhDedicated };
			out.sharedBytes = { true, pdhShared };
			out.isUsage = true;
			sumAllAdapterCapacity();
			ensureCapacityGeUsage();
		} else {
			IDXGIAdapter3* adapter3 = nullptr;
			if (SUCCEEDED(adapter->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&adapter3))) && adapter3) {
				DXGI_QUERY_VIDEO_MEMORY_INFO info{};
				if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
					out.dedicatedBytes = { true, static_cast<std::uint64_t>(info.CurrentUsage) };
					out.isUsage = true;
				}
				if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &info))) {
					out.sharedBytes = { true, static_cast<std::uint64_t>(info.CurrentUsage) };
					out.isUsage = true;
				}
				adapter3->Release();
			}
		}

		if (out.dedicatedBytes.has && out.dedicatedCapacityBytes.has && out.dedicatedCapacityBytes.value > 0) {
			out.dedicatedUsagePercent = (static_cast<double>(out.dedicatedBytes.value) * 100.0) / static_cast<double>(out.dedicatedCapacityBytes.value);
			if (out.dedicatedUsagePercent > 100.0) out.dedicatedUsagePercent = 100.0;
		}
		double utilPct2 = -1.0;
		if (getGpuUtilizationViaPdh(utilPct2)) out.utilizationPercent = utilPct2;

		adapter->Release();
		break;
	}

	factoryBase->Release();
	return out;
}

PresentFpsInfo getForegroundPresentFps() {
	return PresentEtwMonitor::instance().getForegroundFps();
}

bool isFpsEtwRunning() {
	return PresentEtwMonitor::instance().isRunning();
}

} // namespace sysmon
