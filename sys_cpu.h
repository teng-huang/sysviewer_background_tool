#pragma once
#include <cstdint>

namespace sysmon {

struct OptDbl {
	bool has{};
	double value{};
};

struct CpuTimesSample {
	std::uint64_t idle{};
	std::uint64_t kernel{};
	std::uint64_t user{};
};

class CpuMonitor {
public:
	bool init();
	bool getCpuPercent(double& outPercent);

private:
	CpuTimesSample _prev{};
	bool _hasPrev{};
};

} // namespace sysmon
