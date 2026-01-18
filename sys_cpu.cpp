#include "sys_cpu.h"

#include <windows.h>

namespace sysmon {

static std::uint64_t fileTimeToUint64(const FILETIME& ft) {
	ULARGE_INTEGER u{};
	u.LowPart = ft.dwLowDateTime;
	u.HighPart = ft.dwHighDateTime;
	return static_cast<std::uint64_t>(u.QuadPart);
}

static bool sampleCpuTimes(CpuTimesSample& out) {
	FILETIME idleFT{}, kernelFT{}, userFT{};
	if (!GetSystemTimes(&idleFT, &kernelFT, &userFT)) return false;
	out.idle = fileTimeToUint64(idleFT);
	out.kernel = fileTimeToUint64(kernelFT);
	out.user = fileTimeToUint64(userFT);
	return true;
}

static bool calcCpuUsagePercent(const CpuTimesSample& prev, const CpuTimesSample& cur, double& outPercent) {
	const std::uint64_t idleDelta = cur.idle - prev.idle;
	const std::uint64_t kernelDelta = cur.kernel - prev.kernel;
	const std::uint64_t userDelta = cur.user - prev.user;
	const std::uint64_t totalDelta = kernelDelta + userDelta;
	if (totalDelta == 0) return false;
	const std::uint64_t busyDelta = totalDelta > idleDelta ? (totalDelta - idleDelta) : 0;
	outPercent = (static_cast<double>(busyDelta) * 100.0) / static_cast<double>(totalDelta);
	return true;
}

bool CpuMonitor::init() {
	CpuTimesSample s;
	if (!sampleCpuTimes(s)) return false;
	_prev = s;
	_hasPrev = true;
	return true;
}

bool CpuMonitor::getCpuPercent(double& outPercent) {
	CpuTimesSample cur;
	if (!sampleCpuTimes(cur)) return false;
	bool ok = false;
	if (_hasPrev) ok = calcCpuUsagePercent(_prev, cur, outPercent);
	_prev = cur;
	_hasPrev = true;
	return ok;
}

} // namespace sysmon
