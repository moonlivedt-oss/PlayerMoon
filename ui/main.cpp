// Точка входа PlayerMoon: COM в однопоточном режиме (диалоги, WIC) и запуск приложения.
// Осведомлённость о DPI (Per-Monitor V2) задаётся манифестом ui/res/app.manifest.

#include <windows.h>

#include <objbase.h>

#include "ui/app.h"

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) return 1;
    int code = 0;
    {
        ui::App app;
        code = app.Run(instance, show);
    }
    CoUninitialize();
    return code;
}
