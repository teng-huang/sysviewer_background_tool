#pragma once

#if __has_include(<dxgi1_6.h>)
  #include <dxgi1_6.h>
#else
  #include <dxgi1_4.h>
#endif

#include <cstdint>
#include <string>

namespace sysmon {

struct OptU64 {
	bool has{};
	std::uint64_t value{};
};

struct GpuOptDbl {
	bool has{};
	double value{};
};

struct GpuMemInfo {
	OptU64 dedicatedBytes;
	OptU64 sharedBytes;
	OptU64 dedicatedCapacityBytes;
	OptU64 sharedCapacityBytes;
	GpuOptDbl utilizationPercent;
	GpuOptDbl memoryUsagePercent;
	std::wstring adapterName;
	bool isUsage{};
};

struct PresentFpsInfo {
	bool ok{};
	double fps{};
	double avgFps{};
	double low1PercentFps{};
	double low01PercentFps{};
	double frameTimeMs{};
	std::uint32_t pid{};
	std::wstring windowTitle;
};

GpuMemInfo getGpuVideoMemoryInfo();

// Uses ETW "Present" events to estimate FPS for the current foreground process.
// Returns ok=false if ETW isn't available or there isn't enough data yet.
PresentFpsInfo getForegroundPresentFps();
void stopForegroundPresentFpsMonitor();

} // namespace sysmon
