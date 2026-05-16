#pragma once

#include <windows.h>
#include <cstdint>
#include <string>

namespace sysmon {

enum : UINT {
	WM_SHOW_MAIN_WINDOW = WM_APP + 3
};

const wchar_t* UiWindowClassName() noexcept;

struct UiAppConfig {
	std::uint16_t defaultPort{ 6666 };
	bool startMinimized{ false };
};

int RunTrayApp(HINSTANCE hInstance, const UiAppConfig& cfg);

} // namespace sysmon
