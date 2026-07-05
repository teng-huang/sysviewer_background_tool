#include "sys_gpu.h"

#include <windows.h>

#include <evntrace.h>
#include <pdh.h>
#include <tdh.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <cstring>
#include <deque>
#include <functional>
#include <limits>
#include <mutex>
#include <numeric>
#include <thread>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "tdh.lib")

namespace sysmon {

static bool findProviderGuidByName(const wchar_t* providerName, GUID& outGuid) {
	ULONG bytes = 0;
	ULONG status = TdhEnumerateProviders(nullptr, &bytes);
	if (status != ERROR_INSUFFICIENT_BUFFER || bytes == 0) return false;

	std::vector<std::uint8_t> buf(bytes);
	auto* info = reinterpret_cast<PPROVIDER_ENUMERATION_INFO>(buf.data());
	status = TdhEnumerateProviders(info, &bytes);
	if (status != ERROR_SUCCESS) return false;

	for (ULONG i = 0; i < info->NumberOfProviders; ++i) {
		const TRACE_PROVIDER_INFO& p = info->TraceProviderInfoArray[i];
		const wchar_t* name = reinterpret_cast<const wchar_t*>(reinterpret_cast<const std::uint8_t*>(info) + p.ProviderNameOffset);
		if (!_wcsicmp(name, providerName)) {
			outGuid = p.ProviderGuid;
			return true;
		}
	}
	return false;
}

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

static const GUID kDxgKrnlProviderGuid = { 0x802ec45a, 0x1e99, 0x4b83, { 0x99, 0x20, 0x87, 0xc9, 0x82, 0x77, 0xba, 0x9d } };
static const GUID kDxgiProviderGuid = { 0xca11c036, 0x0102, 0x4a2d, { 0xa6, 0xad, 0xf0, 0x3c, 0xfe, 0xd5, 0xd3, 0xc9 } };
static const GUID kSessionGuid = { 0xa77f596a, 0xb61d, 0x46fb, { 0x9f, 0x8a, 0x5e, 0xf8, 0x64, 0x39, 0x1b, 0x38 } };

static bool containsInsensitive(std::wstring text, const wchar_t* needle) {
	if (!needle || !*needle) return true;
	std::transform(text.begin(), text.end(), text.begin(), [](wchar_t ch) {
		return static_cast<wchar_t>(std::towlower(ch));
	});
	std::wstring n(needle);
	std::transform(n.begin(), n.end(), n.begin(), [](wchar_t ch) {
		return static_cast<wchar_t>(std::towlower(ch));
	});
	return text.find(n) != std::wstring::npos;
}

static double clampPercent(double value) {
	if (!std::isfinite(value)) return 0.0;
	return std::max(0.0, std::min(100.0, value));
}

static void collectProcessTreePids(DWORD rootPid, std::vector<DWORD>& out) {
	out.clear();
	if (rootPid == 0) return;
	out.push_back(rootPid);

	struct ProcLink {
		DWORD pid{};
		DWORD parent{};
	};

	std::vector<ProcLink> processes;
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snap == INVALID_HANDLE_VALUE) return;

	PROCESSENTRY32W pe{};
	pe.dwSize = sizeof(pe);
	for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
		processes.push_back({ pe.th32ProcessID, pe.th32ParentProcessID });
	}
	CloseHandle(snap);

	for (size_t i = 0; i < out.size() && out.size() < 64; ++i) {
		DWORD parent = out[i];
		for (const auto& p : processes) {
			if (p.parent == parent && std::find(out.begin(), out.end(), p.pid) == out.end()) {
				out.push_back(p.pid);
				if (out.size() >= 64) break;
			}
		}
	}
}

class PresentEtwMonitor {
public:
	static PresentEtwMonitor& instance() {
		static PresentEtwMonitor m;
		return m;
	}

	PresentFpsInfo getForegroundFps() {
		ensureStarted();

		PresentFpsInfo out{};
		HWND fg = GetForegroundWindow();
		HWND top = fg ? GetAncestor(fg, GA_ROOT) : nullptr;
		if (!top) top = fg;
		if (top) {
			int len = GetWindowTextLengthW(top);
			if (len > 0) {
				std::wstring title(static_cast<size_t>(len) + 1, L'\0');
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

		std::vector<DWORD> targetPids;
		collectProcessTreePids(pid, targetPids);
		if (targetPids.empty()) targetPids.push_back(pid);
		updateTargetPids(targetPids);

		const double nowSeconds = qpcNowSeconds();

		std::lock_guard<std::mutex> g(_mtx);
		buildStatsLocked(targetPids, nowSeconds, out);
		return out;
	}

	void shutdown() {
		stop();
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
		if (_running.load(std::memory_order_acquire)) return;

		ULONGLONG now = GetTickCount64();
		std::lock_guard<std::mutex> g(_startMtx);
		if (_running.load(std::memory_order_acquire)) return;
		if (now < _nextStartAttemptTick) return;
		if (_thread.joinable()) _thread.join();

		_nextStartAttemptTick = now + 5000;
		if (start()) {
			_nextStartAttemptTick = 0;
		}
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

	struct PresentEventClass {
		bool track{};
		int priority{};
	};

	PresentEventClass classifyPresentEvent(const EVENT_RECORD& rec) {
		// DxgKrnl present/flip events often arrive without a useful TDH name on
		// some Windows/GPU driver combinations, so keep the known event IDs as a
		// cheap first pass and fall back to cached event-name matching.
		PresentEventClass out{};
		const UCHAR opcode = rec.EventHeader.EventDescriptor.Opcode;
		if (opcode == EVENT_TRACE_TYPE_START) return out;

		const USHORT id = rec.EventHeader.EventDescriptor.Id;
		if (id == 42 || id == 48 || id == 184) {
			out.track = true;
			out.priority = (opcode == EVENT_TRACE_TYPE_STOP) ? 3 : 2;
		}

		std::wstring name = getEventNameCached(rec);
		if (!name.empty()) {
			if (containsInsensitive(name, L"start")) return {};
			if (!_wcsicmp(name.c_str(), L"Present_Stop") || !_wcsicmp(name.c_str(), L"PresentStop") ||
				!_wcsicmp(name.c_str(), L"Flip") || !_wcsicmp(name.c_str(), L"IndependentFlip")) {
				out.track = true;
				out.priority = 3;
			} else if (!_wcsicmp(name.c_str(), L"Present")) {
				out.track = true;
				out.priority = std::max(out.priority, 2);
			} else if (containsInsensitive(name, L"present") || containsInsensitive(name, L"flip")) {
				out.track = true;
				out.priority = std::max(out.priority, 2);
			}
		}
		if (out.track && opcode == EVENT_TRACE_TYPE_STOP) out.priority = std::max(out.priority, 3);
		return out;
	}

	static double qpcToSeconds(const LARGE_INTEGER& qpc, const LARGE_INTEGER& freq) {
		return static_cast<double>(qpc.QuadPart) / static_cast<double>(freq.QuadPart);
	}

	double qpcNowSeconds() const {
		LARGE_INTEGER now{};
		QueryPerformanceCounter(&now);
		return qpcToSeconds(now, _qpf);
	}

	struct FrameSample {
		double seconds{};
		int priority{};
	};

	struct FrameHistory {
		std::deque<FrameSample> frames;
	};

	static constexpr double kFrameHistorySeconds = 30.0;
	static constexpr double kCurrentFpsWindowSeconds = 1.0;
	static constexpr double kStaleFrameSeconds = 2.0;
	static constexpr double kDefaultDuplicateFrameSeconds = 0.0010;
	static constexpr double kMinDuplicateFrameSeconds = 0.00025;
	static constexpr size_t kMaxFramesPerPid = 12000;

	static void pruneHistoryLocked(FrameHistory& history, double nowSeconds) {
		while (!history.frames.empty() && (nowSeconds - history.frames.front().seconds) > kFrameHistorySeconds) {
			history.frames.pop_front();
		}
		if (history.frames.size() > kMaxFramesPerPid) {
			history.frames.erase(history.frames.begin(), history.frames.end() - kMaxFramesPerPid);
		}
	}

	static double recentMedianFrameInterval(const FrameHistory& history) {
		if (history.frames.size() < 3) return 0.0;

		std::vector<double> intervals;
		const size_t count = history.frames.size();
		const size_t start = (count > 9) ? (count - 8) : 1;
		intervals.reserve(count - start);
		for (size_t i = start; i < count; ++i) {
			double dt = history.frames[i].seconds - history.frames[i - 1].seconds;
			if (dt > 0.0) intervals.push_back(dt);
		}
		if (intervals.empty()) return 0.0;
		std::sort(intervals.begin(), intervals.end());
		return intervals[intervals.size() / 2];
	}

	static double duplicateFrameWindowSeconds(const FrameHistory& history) {
		double window = kDefaultDuplicateFrameSeconds;
		double median = recentMedianFrameInterval(history);
		if (median > 0.0) {
			window = std::min(window, median * 0.35);
		}
		return std::max(kMinDuplicateFrameSeconds, window);
	}

	static void appendFrameLocked(FrameHistory& history, double seconds, int priority) {
		if (!history.frames.empty()) {
			FrameSample& last = history.frames.back();
			double delta = seconds - last.seconds;
			if (delta <= 0.0) return;
			if (delta <= duplicateFrameWindowSeconds(history)) {
				if (priority >= last.priority) {
					last.seconds = seconds;
					last.priority = priority;
				}
				return;
			}
		}

		history.frames.push_back({ seconds, priority });
		pruneHistoryLocked(history, seconds);
	}

	static std::vector<double> buildFrameIntervals(const FrameHistory& history, double minEndSeconds) {
		std::vector<double> intervals;
		if (history.frames.size() < 2) return intervals;
		intervals.reserve(history.frames.size() - 1);

		for (size_t i = 1; i < history.frames.size(); ++i) {
			const double endSeconds = history.frames[i].seconds;
			if (endSeconds < minEndSeconds) continue;
			const double dt = endSeconds - history.frames[i - 1].seconds;
			if (dt > 0.0 && std::isfinite(dt)) intervals.push_back(dt);
		}
		return intervals;
	}

	static double fpsFromIntervals(const std::vector<double>& intervals) {
		if (intervals.empty()) return 0.0;
		double totalSeconds = std::accumulate(intervals.begin(), intervals.end(), 0.0);
		if (totalSeconds <= 0.0 || !std::isfinite(totalSeconds)) return 0.0;
		return static_cast<double>(intervals.size()) / totalSeconds;
	}

	static double lowFpsFromIntervals(std::vector<double> intervals, double fraction) {
		if (intervals.empty()) return 0.0;
		std::sort(intervals.begin(), intervals.end(), std::greater<double>());
		size_t count = static_cast<size_t>(std::ceil(static_cast<double>(intervals.size()) * fraction));
		count = std::max<size_t>(1, std::min(count, intervals.size()));
		double slowSeconds = std::accumulate(intervals.begin(), intervals.begin() + count, 0.0);
		if (slowSeconds <= 0.0 || !std::isfinite(slowSeconds)) return 0.0;
		return static_cast<double>(count) / slowSeconds;
	}

	void buildStatsLocked(const std::vector<DWORD>& pids, double nowSeconds, PresentFpsInfo& out) {
		FrameHistory* bestHistory = nullptr;
		size_t bestRecentCount = 0;

		for (DWORD pid : pids) {
			auto it = _perPid.find(pid);
			if (it == _perPid.end()) continue;

			FrameHistory& history = it->second;
			pruneHistoryLocked(history, nowSeconds);
			if (history.frames.size() < 2) continue;
			if ((nowSeconds - history.frames.back().seconds) > kStaleFrameSeconds) continue;

			size_t recentCount = 0;
			for (const auto& frame : history.frames) {
				if (frame.seconds >= nowSeconds - kCurrentFpsWindowSeconds) ++recentCount;
			}

			if (recentCount > bestRecentCount) {
				bestRecentCount = recentCount;
				bestHistory = &history;
			}
		}

		if (!bestHistory) return;

		FrameHistory& history = *bestHistory;
		pruneHistoryLocked(history, nowSeconds);
		if (history.frames.size() < 2) return;
		if ((nowSeconds - history.frames.back().seconds) > kStaleFrameSeconds) return;

		auto allIntervals = buildFrameIntervals(history, -std::numeric_limits<double>::infinity());
		if (allIntervals.empty()) return;

		auto currentIntervals = buildFrameIntervals(history, nowSeconds - kCurrentFpsWindowSeconds);
		if (currentIntervals.empty()) currentIntervals.push_back(allIntervals.back());

		out.ok = true;
		out.fps = fpsFromIntervals(currentIntervals);
		out.avgFps = fpsFromIntervals(allIntervals);
		out.low1PercentFps = lowFpsFromIntervals(allIntervals, 0.01);
		out.low01PercentFps = lowFpsFromIntervals(allIntervals, 0.001);
		out.frameTimeMs = allIntervals.back() * 1000.0;
	}

	void updateTargetPids(const std::vector<DWORD>& pids) {
		std::lock_guard<std::mutex> g(_targetMtx);
		_targetPids = pids;
	}

	bool isTargetPid(DWORD pid) {
		std::lock_guard<std::mutex> g(_targetMtx);
		return std::find(_targetPids.begin(), _targetPids.end(), pid) != _targetPids.end();
	}

	void pruneAllHistoriesLocked(double nowSeconds) {
		for (auto it = _perPid.begin(); it != _perPid.end();) {
			pruneHistoryLocked(it->second, nowSeconds);
			if (it->second.frames.empty()) {
				it = _perPid.erase(it);
			} else {
				++it;
			}
		}
	}

	void handleEvent(const EVENT_RECORD& rec) {
		if (!_running.load(std::memory_order_acquire)) return;
		PresentEventClass eventClass = classifyPresentEvent(rec);
		if (!eventClass.track) return;

		const DWORD pid = rec.EventHeader.ProcessId;
		if (pid == 0) return;
		if (!isTargetPid(pid)) return;

		double t = 0.0;
		// With ClientContext=QPC, TimeStamp is QPC.
		LARGE_INTEGER ts = rec.EventHeader.TimeStamp;
		t = qpcToSeconds(ts, _qpf);

		std::lock_guard<std::mutex> g(_mtx);
		auto& dq = _perPid[pid];
		appendFrameLocked(dq, t, eventClass.priority);

		if (t - _lastPruneSeconds > 5.0) {
			pruneAllHistoriesLocked(t);
			_lastPruneSeconds = t;
		}
	}

	bool start() {
		GUID dxgGuid = kDxgKrnlProviderGuid;
		GUID dxgiGuid = kDxgiProviderGuid;
		GUID foundGuid{};
		if (findProviderGuidByName(L"Microsoft-Windows-DxgKrnl", foundGuid)) dxgGuid = foundGuid;
		if (findProviderGuidByName(L"Microsoft-Windows-DXGI", foundGuid)) dxgiGuid = foundGuid;

		const wchar_t* kSessionName = L"SysMonitorPresentSession";
		const ULONG propsSize = sizeof(EVENT_TRACE_PROPERTIES) + 2 * 1024;
		std::vector<std::uint8_t> propsBuf(propsSize);
		auto* props = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(propsBuf.data());
		ZeroMemory(props, propsSize);
		props->Wnode.BufferSize = propsSize;
		props->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
		props->Wnode.Guid = kSessionGuid;
		props->Wnode.ClientContext = 1; // QPC
		props->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
		props->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);

		TRACEHANDLE session = 0;
		ULONG st = StartTraceW(&session, kSessionName, props);
		if (st == ERROR_ALREADY_EXISTS) {
			// Try to stop a stale session with the same name.
			ControlTraceW(0, kSessionName, props, EVENT_TRACE_CONTROL_STOP);
			st = StartTraceW(&session, kSessionName, props);
		}
		if (st != ERROR_SUCCESS) return false;

		bool enabledProvider = false;
		ULONG enableDxg = EnableTraceEx2(session, &dxgGuid, EVENT_CONTROL_CODE_ENABLE_PROVIDER,
			TRACE_LEVEL_VERBOSE, 0xFFFFFFFFFFFFFFFFull, 0, 0, nullptr);
		if (enableDxg == ERROR_SUCCESS) enabledProvider = true;

		ULONG enableDxgi = EnableTraceEx2(session, &dxgiGuid, EVENT_CONTROL_CODE_ENABLE_PROVIDER,
			TRACE_LEVEL_VERBOSE, 0xFFFFFFFFFFFFFFFFull, 0, 0, nullptr);
		if (enableDxgi == ERROR_SUCCESS) enabledProvider = true;

		if (!enabledProvider) {
			ControlTraceW(session, kSessionName, props, EVENT_TRACE_CONTROL_STOP);
			return false;
		}

		EVENT_TRACE_LOGFILEW lf{};
		lf.LoggerName = const_cast<LPWSTR>(kSessionName);
		lf.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
		lf.EventRecordCallback = &PresentEtwMonitor::onEventRecord;
		lf.Context = this;

		TRACEHANDLE trace = OpenTraceW(&lf);
		if (trace == INVALID_PROCESSTRACE_HANDLE) {
			ControlTraceW(session, kSessionName, props, EVENT_TRACE_CONTROL_STOP);
			return false;
		}

		_session = session;
		_trace = trace;
		_running.store(true, std::memory_order_release);
		try {
			TRACEHANDLE traceForThread = trace;
			_thread = std::thread([this, traceForThread]() mutable {
				ProcessTrace(&traceForThread, 1, nullptr, nullptr);
				_running.store(false, std::memory_order_release);
			});
		} catch (...) {
			_running.store(false, std::memory_order_release);
			CloseTrace(trace);
			ControlTraceW(session, kSessionName, props, EVENT_TRACE_CONTROL_STOP);
			_trace = 0;
			_session = 0;
			return false;
		}
		return true;
	}

	void stop() {
		std::lock_guard<std::mutex> startGuard(_startMtx);
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
		_nextStartAttemptTick = 0;
	}

	std::mutex _startMtx;
	ULONGLONG _nextStartAttemptTick{};
	std::atomic<bool> _running{ false };
	LARGE_INTEGER _qpf{};
	TRACEHANDLE _session{};
	TRACEHANDLE _trace{};
	std::thread _thread;
	std::mutex _targetMtx;
	std::vector<DWORD> _targetPids;

	std::mutex _mtx;
	std::unordered_map<DWORD, FrameHistory> _perPid;
	double _lastPruneSeconds{};

	std::mutex _metaMtx;
	std::unordered_map<EtwEventKey, std::wstring, EtwEventKeyHash> _eventNameCache;
};

static void fillGpuDescCaps(GpuMemInfo& out, IDXGIAdapter1* adapter) {
	DXGI_ADAPTER_DESC1 desc{};
	if (SUCCEEDED(adapter->GetDesc1(&desc))) {
		out.adapterName = desc.Description;
		if (desc.DedicatedVideoMemory != 0) {
			out.dedicatedCapacityBytes = { true, static_cast<std::uint64_t>(desc.DedicatedVideoMemory) };
		}
		if (desc.SharedSystemMemory != 0) {
			out.sharedCapacityBytes = { true, static_cast<std::uint64_t>(desc.SharedSystemMemory) };
		}
	}
}

class GpuVideoMemorySampler {
public:
	~GpuVideoMemorySampler() {
		if (_adapter3) _adapter3->Release();
	}

	GpuMemInfo sample() {
		std::lock_guard<std::mutex> g(_mtx);
		if (!_initialized) initialize();

		GpuMemInfo out = _baseInfo;
		if (!_adapter3) return out;

		DXGI_QUERY_VIDEO_MEMORY_INFO info{};
		bool anyUsage = false;
		if (SUCCEEDED(_adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
			out.dedicatedBytes = { true, static_cast<std::uint64_t>(info.CurrentUsage) };
			anyUsage = true;
		}
		if (SUCCEEDED(_adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &info))) {
			out.sharedBytes = { true, static_cast<std::uint64_t>(info.CurrentUsage) };
			anyUsage = true;
		}
		out.isUsage = anyUsage;
		return out;
	}

private:
	void initialize() {
		_initialized = true;

		IDXGIFactory1* factoryBase = nullptr;
		if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factoryBase))) || !factoryBase) {
			return;
		}

		IDXGIFactory6* factory6 = nullptr;
		if (SUCCEEDED(factoryBase->QueryInterface(__uuidof(IDXGIFactory6), reinterpret_cast<void**>(&factory6))) && factory6) {
			IDXGIAdapter1* adapter = nullptr;
			if (SUCCEEDED(factory6->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, __uuidof(IDXGIAdapter1), reinterpret_cast<void**>(&adapter))) && adapter) {
				useAdapter(adapter);
				adapter->Release();
			}
			factory6->Release();
			factoryBase->Release();
			return;
		}

		IDXGIAdapter1* adapter = nullptr;
		for (UINT i = 0; factoryBase->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
			DXGI_ADAPTER_DESC1 desc{};
			if (SUCCEEDED(adapter->GetDesc1(&desc)) && (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
				adapter->Release();
				adapter = nullptr;
				continue;
			}

			useAdapter(adapter);
			adapter->Release();
			break;
		}

		factoryBase->Release();
	}

	void useAdapter(IDXGIAdapter1* adapter) {
		fillGpuDescCaps(_baseInfo, adapter);
		IDXGIAdapter3* adapter3 = nullptr;
		if (SUCCEEDED(adapter->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&adapter3))) && adapter3) {
			_adapter3 = adapter3;
		}
	}

	std::mutex _mtx;
	bool _initialized{};
	GpuMemInfo _baseInfo;
	IDXGIAdapter3* _adapter3{};
};

class GpuUtilizationSampler {
public:
	~GpuUtilizationSampler() {
		if (_query) {
			PdhCloseQuery(_query);
		}
	}

	GpuOptDbl sample() {
		std::lock_guard<std::mutex> g(_mtx);
		if (!_initialized) initialize();
		if (!_query || !_counter) return {};

		PDH_STATUS status = PdhCollectQueryData(_query);
		if (status != ERROR_SUCCESS) return {};

		DWORD bufferSize = 0;
		DWORD itemCount = 0;
		status = PdhGetFormattedCounterArrayW(_counter, PDH_FMT_DOUBLE, &bufferSize, &itemCount, nullptr);
		if (bufferSize == 0 || itemCount == 0) return {};

		std::vector<std::uint8_t> buffer(bufferSize);
		auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
		status = PdhGetFormattedCounterArrayW(_counter, PDH_FMT_DOUBLE, &bufferSize, &itemCount, items);
		if (status != ERROR_SUCCESS) return {};

		double total = 0.0;
		for (DWORD i = 0; i < itemCount; ++i) {
			if (items[i].FmtValue.CStatus != ERROR_SUCCESS) continue;
			const wchar_t* instanceName = items[i].szName ? items[i].szName : L"";
			if (!isGraphicsEngine(instanceName)) continue;
			const double value = items[i].FmtValue.doubleValue;
			if (std::isfinite(value) && value > 0.0) total += value;
		}

		if (!std::isfinite(total)) return {};
		return { true, clampPercent(total) };
	}

private:
	void initialize() {
		_initialized = true;
		if (PdhOpenQueryW(nullptr, 0, &_query) != ERROR_SUCCESS) {
			_query = nullptr;
			return;
		}

		PDH_STATUS status = PdhAddEnglishCounterW(_query, L"\\GPU Engine(*)\\Utilization Percentage", 0, &_counter);
		if (status != ERROR_SUCCESS) {
			PdhCloseQuery(_query);
			_query = nullptr;
			_counter = nullptr;
			return;
		}

		PdhCollectQueryData(_query);
	}

	static bool isGraphicsEngine(const wchar_t* instanceName) {
		if (!instanceName || !*instanceName) return false;
		return containsInsensitive(instanceName, L"engtype_3D") ||
			containsInsensitive(instanceName, L"engtype_VideoDecode") ||
			containsInsensitive(instanceName, L"engtype_VideoEncode") ||
			containsInsensitive(instanceName, L"engtype_Compute");
	}

	std::mutex _mtx;
	bool _initialized{};
	PDH_HQUERY _query{};
	PDH_HCOUNTER _counter{};
};

GpuMemInfo getGpuVideoMemoryInfo() {
	static GpuVideoMemorySampler sampler;
	static GpuUtilizationSampler utilizationSampler;
	GpuMemInfo info = sampler.sample();
	info.utilizationPercent = utilizationSampler.sample();

	const auto used = info.dedicatedBytes.has ? info.dedicatedBytes : info.sharedBytes;
	const auto capacity = info.dedicatedCapacityBytes.has ? info.dedicatedCapacityBytes : info.sharedCapacityBytes;
	if (used.has && capacity.has && capacity.value > 0) {
		const double percent = (static_cast<double>(used.value) * 100.0) / static_cast<double>(capacity.value);
		info.memoryUsagePercent = { true, clampPercent(percent) };
	}

	return info;
}

PresentFpsInfo getForegroundPresentFps() {
	return PresentEtwMonitor::instance().getForegroundFps();
}

void stopForegroundPresentFpsMonitor() {
	PresentEtwMonitor::instance().shutdown();
}

} // namespace sysmon
