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

struct GpuMemInfo {
	OptU64 dedicatedBytes;
	OptU64 sharedBytes;
	OptU64 dedicatedCapacityBytes;
	OptU64 sharedCapacityBytes;
	std::wstring adapterName;
	bool isUsage{};
	// 專用記憶體使用率 0~100，有值時表示 dedicatedBytes/dedicatedCapacityBytes 皆有效
	double dedicatedUsagePercent{-1.0};
	// GPU 運算使用率 0~100（類似 CPU %），取自 PDH GPU Engine
	double utilizationPercent{-1.0};
};

struct PresentFpsInfo {
	bool ok{};
	double fps{};
	std::uint32_t pid{};
	std::wstring windowTitle;
};

GpuMemInfo getGpuVideoMemoryInfo();

// Uses ETW "Present" events to estimate FPS for the current foreground process.
// Returns ok=false if ETW isn't available or there isn't enough data yet.
PresentFpsInfo getForegroundPresentFps();

// 回傳 ETW 是否已成功啟動。若為 false，FPS 會一直是 n/a，需以管理員身分執行。
bool isFpsEtwRunning();

} // namespace sysmon
