#include "sys_monitor.h"

#include <windows.h>

namespace sysmon {

RefreshTriple getMonitorRefreshHz123() {
	RefreshTriple out;
	DISPLAY_DEVICEW dd{};
	dd.cb = sizeof(dd);

	int monitorIdx = 0;
	for (DWORD devNum = 0; EnumDisplayDevicesW(nullptr, devNum, &dd, 0); ++devNum) {
		if ((dd.StateFlags & DISPLAY_DEVICE_ACTIVE) == 0) {
			dd = DISPLAY_DEVICEW{};
			dd.cb = sizeof(dd);
			continue;
		}
		if ((dd.StateFlags & DISPLAY_DEVICE_MIRRORING_DRIVER) != 0) {
			dd = DISPLAY_DEVICEW{};
			dd.cb = sizeof(dd);
			continue;
		}

		DEVMODEW dm{};
		dm.dmSize = sizeof(dm);
		if (EnumDisplaySettingsExW(dd.DeviceName, ENUM_CURRENT_SETTINGS, &dm, 0)) {
			if (dm.dmDisplayFrequency > 1) {
				monitorIdx++;
				OptDbl hz{ true, static_cast<double>(dm.dmDisplayFrequency) };
				switch (monitorIdx) {
				case 1: out.m1 = hz; break;
				case 2: out.m2 = hz; break;
				case 3: out.m3 = hz; break;
				default: break;
				}
				if (monitorIdx >= 3) break;
			}
		}

		dd = DISPLAY_DEVICEW{};
		dd.cb = sizeof(dd);
	}
	return out;
}

} // namespace sysmon
