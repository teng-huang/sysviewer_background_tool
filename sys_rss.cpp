#include "sys_rss.h"

#include <windows.h>
#include <psapi.h>

#pragma comment(lib, "psapi.lib")

namespace sysmon {

std::uint64_t getProcessRssBytes() {
	PROCESS_MEMORY_COUNTERS_EX pmc{};
	pmc.cb = sizeof(pmc);
	if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
		return static_cast<std::uint64_t>(pmc.WorkingSetSize);
	}
	return 0;
}

} // namespace sysmon
