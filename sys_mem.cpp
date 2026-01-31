#include "sys_mem.h"

#include <windows.h>

namespace sysmon {

MemInfo getMemInfo() {
	MemInfo mi;
	MEMORYSTATUSEX ms{};
	ms.dwLength = sizeof(ms);
	if (!GlobalMemoryStatusEx(&ms)) return mi;
	mi.totalPhysBytes = static_cast<std::uint64_t>(ms.ullTotalPhys);
	mi.availPhysBytes = static_cast<std::uint64_t>(ms.ullAvailPhys);
	mi.ok = true;
	return mi;
}

} // namespace sysmon
