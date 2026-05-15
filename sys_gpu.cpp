#include "sys_gpu.h"

#include <windows.h>

#include <evntrace.h>
#include <tdh.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "dxgi.lib")
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

		std::lock_guard<std::mutex> g(_mtx);
		auto it = _perPid.find(pid);
		if (it == _perPid.end()) return out;
		const auto& times = it->second;
		if (times.size() < 2) return out;
		double dt = times.back() - times.front();
		if (dt <= 0.0) return out;
		out.ok = true;
		out.fps = static_cast<double>(times.size() - 1) / dt;
		return out;
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
		// Rely on event name (cached) rather than hard-coding event IDs.
		std::wstring name = getEventNameCached(rec);
		if (name.empty()) return false;
		// Common present names from WDDM/DXGI providers.
		if (!_wcsicmp(name.c_str(), L"Present")) return true;
		if (!_wcsicmp(name.c_str(), L"Present_Stop")) return true;
		if (!_wcsicmp(name.c_str(), L"PresentStop")) return true;
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
		// With ClientContext=QPC, TimeStamp is QPC.
		LARGE_INTEGER ts = rec.EventHeader.TimeStamp;
		t = qpcToSeconds(ts, _qpf);

		std::lock_guard<std::mutex> g(_mtx);
		auto& dq = _perPid[pid];
		dq.push_back(t);
		// Keep ~2 seconds of history for stability.
		while (!dq.empty() && (t - dq.front()) > 2.0) dq.pop_front();
		// Avoid unbounded growth even if timestamps go weird.
		if (dq.size() > 600) dq.erase(dq.begin(), dq.end() - 600);
	}

	void start() {
		GUID dxgGuid{};
		GUID dxgiGuid{};
		bool hasDxg = findProviderGuidByName(L"Microsoft-Windows-DxgKrnl", dxgGuid);
		bool hasDxgi = findProviderGuidByName(L"Microsoft-Windows-DXGI", dxgiGuid);
		if (!hasDxg && !hasDxgi) return;

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

		TRACEHANDLE session = 0;
		ULONG st = StartTraceW(&session, kSessionName, props);
		if (st == ERROR_ALREADY_EXISTS) {
			// Try to stop a stale session with the same name.
			ControlTraceW(0, kSessionName, props, EVENT_TRACE_CONTROL_STOP);
			st = StartTraceW(&session, kSessionName, props);
		}
		if (st != ERROR_SUCCESS) return;

		// Enable providers (no special keywords/levels here; we filter by event name).
		if (hasDxg) {
			EnableTraceEx2(session, &dxgGuid, EVENT_CONTROL_CODE_ENABLE_PROVIDER, TRACE_LEVEL_VERBOSE, 0, 0, 0, nullptr);
		}
		if (hasDxgi) {
			EnableTraceEx2(session, &dxgiGuid, EVENT_CONTROL_CODE_ENABLE_PROVIDER, TRACE_LEVEL_VERBOSE, 0, 0, 0, nullptr);
		}

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

	IDXGIFactory1* factoryBase = nullptr;
	if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factoryBase))) || !factoryBase) {
		return out;
	}

	IDXGIFactory6* factory6 = nullptr;
	if (SUCCEEDED(factoryBase->QueryInterface(__uuidof(IDXGIFactory6), reinterpret_cast<void**>(&factory6))) && factory6) {
		IDXGIAdapter1* adapter = nullptr;
		if (SUCCEEDED(factory6->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, __uuidof(IDXGIAdapter1), reinterpret_cast<void**>(&adapter))) && adapter) {
			fillDescCaps(adapter);
			IDXGIAdapter3* adapter3 = nullptr;
			if (SUCCEEDED(adapter->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&adapter3))) && adapter3) {
				DXGI_QUERY_VIDEO_MEMORY_INFO info{};
				bool anyUsage = false;
				if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
					out.dedicatedBytes = { true, static_cast<std::uint64_t>(info.CurrentUsage) };
					anyUsage = true;
				}
				if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &info))) {
					out.sharedBytes = { true, static_cast<std::uint64_t>(info.CurrentUsage) };
					anyUsage = true;
				}
				out.isUsage = anyUsage;
				adapter3->Release();
			}
			adapter->Release();
		}
		factory6->Release();
		factoryBase->Release();
		return out;
	}

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

		IDXGIAdapter3* adapter3 = nullptr;
		if (SUCCEEDED(adapter->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&adapter3))) && adapter3) {
			DXGI_QUERY_VIDEO_MEMORY_INFO info{};
			bool anyUsage = false;
			if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
				out.dedicatedBytes = { true, static_cast<std::uint64_t>(info.CurrentUsage) };
				anyUsage = true;
			}
			if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &info))) {
				out.sharedBytes = { true, static_cast<std::uint64_t>(info.CurrentUsage) };
				anyUsage = true;
			}
			out.isUsage = anyUsage;
			adapter3->Release();
		}

		adapter->Release();
		break;
	}

	factoryBase->Release();
	return out;
}

PresentFpsInfo getForegroundPresentFps() {
	return PresentEtwMonitor::instance().getForegroundFps();
}

} // namespace sysmon
