#pragma once
#include <cstdint>
#include <vector>

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
	bool getPerCoreCpuPercent(std::vector<double>& outPercents);

private:
	CpuTimesSample _prev{};
	bool _hasPrev{};
	std::vector<CpuTimesSample> _prevPerCore;
	bool _hasPrevPerCore{};
	double _prevOutPercent{0.0};
	bool _hasPrevOut{};
	std::vector<double> _prevPerCoreOut;
};

} // namespace sysmon
