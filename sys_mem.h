#pragma once
#include <cstdint>

namespace sysmon {

struct MemInfo {
	std::uint64_t totalPhysBytes{};
	std::uint64_t availPhysBytes{};
	bool ok{};
};

MemInfo getMemInfo();

} // namespace sysmon
