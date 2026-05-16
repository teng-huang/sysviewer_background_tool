#include <windows.h>

#include <string>

#include "ui_app.h"

static constexpr wchar_t kSingleInstanceMutexName[] = L"Local\\SysMonitor.SingleInstance";

static bool hasLaunchArg(const std::wstring& cmdLine, const wchar_t* arg) {
	return cmdLine.find(arg) != std::wstring::npos;
}

static void showExistingInstance() {
	for (int i = 0; i < 20; ++i) {
		HWND hwnd = FindWindowW(sysmon::UiWindowClassName(), nullptr);
		if (hwnd) {
			PostMessageW(hwnd, sysmon::WM_SHOW_MAIN_WINDOW, 0, 0);
			return;
		}
		Sleep(100);
	}

	MessageBoxW(nullptr, L"SysMonitor is already running.", L"SysMonitor", MB_OK | MB_ICONINFORMATION);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR cmdLine, int) {
	sysmon::UiAppConfig cfg;
	cfg.defaultPort = 6666;
	std::wstring args = cmdLine ? cmdLine : L"";
	cfg.startMinimized = hasLaunchArg(args, L"--tray") ||
		hasLaunchArg(args, L"--minimized") ||
		hasLaunchArg(args, L"/tray") ||
		hasLaunchArg(args, L"/minimized");

	HANDLE singleInstanceMutex = CreateMutexW(nullptr, TRUE, kSingleInstanceMutexName);
	if (!singleInstanceMutex) {
		MessageBoxW(nullptr, L"Unable to create the SysMonitor single-instance lock.", L"SysMonitor", MB_OK | MB_ICONERROR);
		return 1;
	}

	if (GetLastError() == ERROR_ALREADY_EXISTS) {
		CloseHandle(singleInstanceMutex);
		if (!cfg.startMinimized) showExistingInstance();
		return 0;
	}

	int exitCode = sysmon::RunTrayApp(hInstance, cfg);
	CloseHandle(singleInstanceMutex);
	return exitCode;
}
