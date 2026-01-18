#include <windows.h>

#include "ui_app.h"

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
	sysmon::UiAppConfig cfg;
	cfg.defaultPort = 6666;
	return sysmon::RunTrayApp(hInstance, cfg);
}
