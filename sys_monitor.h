#pragma once

#include "sys_cpu.h"

namespace sysmon {

struct RefreshTriple {
	OptDbl m1;
	OptDbl m2;
	OptDbl m3;
};

RefreshTriple getMonitorRefreshHz123();

} // namespace sysmon
