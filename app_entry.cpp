#include <windows.h>

#include <string>

#include "ui_app.h"

static bool hasLaunchArg(const std::wstring& cmdLine, const wchar_t* arg) {
	return cmdLine.find(arg) != std::wstring::npos;
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR cmdLine, int) {
	sysmon::UiAppConfig cfg;
	cfg.defaultPort = 6666;
	std::wstring args = cmdLine ? cmdLine : L"";
	cfg.startMinimized = hasLaunchArg(args, L"--tray") ||
		hasLaunchArg(args, L"--minimized") ||
		hasLaunchArg(args, L"/tray") ||
		hasLaunchArg(args, L"/minimized");
	return sysmon::RunTrayApp(hInstance, cfg);
}
