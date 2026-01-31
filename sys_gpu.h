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
};

GpuMemInfo getGpuVideoMemoryInfo();

} // namespace sysmon
