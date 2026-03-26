/*  main.cpp
 *
 *  Entry point. Single-instance guard. Subclass to route header-button clicks.
 */

#include "resource_monitor.h"
#include <commctrl.h>

// Header-button hit area (must match drawHeader in app.cpp)
// Using plain ints — RECT cannot be constexpr (non-literal type in MSVC)
static const int BTN_LEFT   = WIN_W - 150;
static const int BTN_TOP    = 17;
static const int BTN_RIGHT  = WIN_W - 16;
static const int BTN_BOTTOM = 43;

static LRESULT CALLBACK SubclassProc(HWND hwnd, UINT msg,
                                      WPARAM wp, LPARAM lp,
                                      UINT_PTR, DWORD_PTR)
{
    if (msg == WM_LBUTTONUP) {
        int mx = GET_X_LPARAM(lp);
        int my = GET_Y_LPARAM(lp);
        if (mx >= BTN_LEFT && mx <= BTN_RIGHT &&
            my >= BTN_TOP  && my <= BTN_BOTTOM) {
            SendMessageW(hwnd, WM_COMMAND, 1, 0);
            return 0;
        }
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

// ─────────────────────────────────────────────────────────────────────────────
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int)
{
    // Enable visual styles
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    // Single-instance guard
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, L"SysMonitor_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr,
            L"System Resource Monitor is already running.\nCheck the system tray.",
            L"Already Running", MB_ICONINFORMATION);
        CloseHandle(hMutex);
        return 0;
    }

    SysMonitorApp app;
    if (!app.init(hInst)) {
        CloseHandle(hMutex);
        return 1;
    }

    // Install subclass after window is created
    HWND hwnd = FindWindowW(L"SysMonitorWnd", nullptr);
    if (hwnd) {
        SetWindowSubclass(hwnd, SubclassProc, 1, 0);
    }

    int result = app.run();
    CloseHandle(hMutex);
    return result;
}
