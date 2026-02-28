#include "sys_cpu.h"

#include <windows.h>
#include <winternl.h>

#include <algorithm>
#include <vector>

namespace sysmon {

#ifndef STATUS_INFO_LENGTH_MISMATCH
#define STATUS_INFO_LENGTH_MISMATCH static_cast<NTSTATUS>(0xC0000004L)
#endif

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

static std::uint64_t liToUint64(const LARGE_INTEGER& li) {
	return static_cast<std::uint64_t>(li.QuadPart);
}

static bool samplePerCoreCpuTimes(std::vector<CpuTimesSample>& out) {
	using NtQuerySystemInformationFn = NTSTATUS(WINAPI*)(SYSTEM_INFORMATION_CLASS, PVOID, ULONG, PULONG);
	static NtQuerySystemInformationFn ntqsi = []() -> NtQuerySystemInformationFn {
		HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
		if (!ntdll) return nullptr;
		auto p = reinterpret_cast<NtQuerySystemInformationFn>(GetProcAddress(ntdll, "NtQuerySystemInformation"));
		return p;
	}();

	if (!ntqsi) return false;

	ULONG bufSize = 0;
	ULONG retLen = 0;

	SYSTEM_INFO si{};
	GetSystemInfo(&si);
	ULONG guessCount = si.dwNumberOfProcessors ? static_cast<ULONG>(si.dwNumberOfProcessors) : 1;
	bufSize = static_cast<ULONG>(guessCount * sizeof(SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION));

	std::vector<std::uint8_t> buf;
	buf.resize(std::max<ULONG>(bufSize, static_cast<ULONG>(sizeof(SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION))));

	for (int attempt = 0; attempt < 4; ++attempt) {
		retLen = 0;
		NTSTATUS st = ntqsi(SystemProcessorPerformanceInformation, buf.data(), static_cast<ULONG>(buf.size()), &retLen);
		if (st == STATUS_INFO_LENGTH_MISMATCH) {
			ULONG want = retLen ? retLen : static_cast<ULONG>(buf.size() * 2);
			buf.resize(want);
			continue;
		}
		if (!NT_SUCCESS(st)) return false;

		ULONG bytes = retLen ? retLen : static_cast<ULONG>(buf.size());
		ULONG count = bytes / static_cast<ULONG>(sizeof(SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION));
		if (count == 0) return false;

		auto* infos = reinterpret_cast<const SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION*>(buf.data());
		out.clear();
		out.reserve(count);
		for (ULONG i = 0; i < count; ++i) {
			CpuTimesSample s;
			s.idle = liToUint64(infos[i].IdleTime);
			s.kernel = liToUint64(infos[i].KernelTime);
			s.user = liToUint64(infos[i].UserTime);
			out.push_back(s);
		}
		return true;
	}

	return false;
}

bool CpuMonitor::init() {
	CpuTimesSample s;
	if (!sampleCpuTimes(s)) return false;
	_prev = s;
	_hasPrev = true;

	std::vector<CpuTimesSample> per;
	if (samplePerCoreCpuTimes(per)) {
		_prevPerCore = std::move(per);
		_hasPrevPerCore = true;
	} else {
		_prevPerCore.clear();
		_hasPrevPerCore = false;
	}
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

bool CpuMonitor::getPerCoreCpuPercent(std::vector<double>& outPercents) {
	std::vector<CpuTimesSample> cur;
	if (!samplePerCoreCpuTimes(cur)) return false;

	bool ok = false;
	if (_hasPrevPerCore && _prevPerCore.size() == cur.size() && !cur.empty()) {
		outPercents.clear();
		outPercents.resize(cur.size(), 0.0);
		ok = true;
		for (size_t i = 0; i < cur.size(); ++i) {
			double pct = 0.0;
			if (!calcCpuUsagePercent(_prevPerCore[i], cur[i], pct)) {
				ok = false;
				break;
			}
			outPercents[i] = pct;
		}
	} else {
		outPercents.clear();
	}

	_prevPerCore = std::move(cur);
	_hasPrevPerCore = true;
	return ok;
}

} // namespace sysmon
