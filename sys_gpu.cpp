#include "sys_gpu.h"

#include <windows.h>

#pragma comment(lib, "dxgi.lib")

namespace sysmon {

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

} // namespace sysmon
