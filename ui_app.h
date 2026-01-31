#pragma once

#include <windows.h>
#include <cstdint>
#include <string>

namespace sysmon {

struct UiAppConfig {
	std::uint16_t defaultPort{ 6666 };
};

int RunTrayApp(HINSTANCE hInstance, const UiAppConfig& cfg);

} // namespace sysmon
