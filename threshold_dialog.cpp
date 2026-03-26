/*  threshold_dialog.cpp
 *
 *  A modal dialog that lets the user set alert thresholds and the
 *  per-resource cooldown period.  Uses standard Win32 dialog box API.
 *
 *  The dialog is defined programmatically (no .rc resource file needed),
 *  making the project a single self-contained set of .cpp/.h files.
 */

#include "resource_monitor.h"
#include <windowsx.h>
#include <cstdlib>

// Control IDs
enum DlgCtrl : int {
    IDC_CPU_EDIT      = 100,
    IDC_RAM_EDIT      = 101,
    IDC_DISK_EDIT     = 102,
    IDC_NET_EDIT      = 103,
    IDC_COOLDOWN_EDIT = 104,
    IDC_OK_BTN        = IDOK,
    IDC_CANCEL_BTN    = IDCANCEL,
    IDC_RESET_BTN     = 110,
};

// ─── Helper: create a label + edit pair ─────────────────────────────────────
static void CreateLabelEdit(HWND hDlg, const wchar_t* label,
                             int ctrlId, int y, double defaultVal,
                             const wchar_t* suffix)
{
    // Label
    CreateWindowExW(0, L"STATIC", label,
        WS_CHILD | WS_VISIBLE | SS_RIGHT,
        20, y + 3, 180, 20,
        hDlg, nullptr, nullptr, nullptr);

    // Edit box
    wchar_t buf[32];
    swprintf_s(buf, L"%.1f", defaultVal);
    CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", buf,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER | ES_RIGHT,
        210, y, 70, 24,
        hDlg, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ctrlId)),
        nullptr, nullptr);

    // Suffix label (e.g. "%" or "sec")
    CreateWindowExW(0, L"STATIC", suffix,
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        288, y + 3, 50, 20,
        hDlg, nullptr, nullptr, nullptr);
}

// ─────────────────────────────────────────────────────────────────────────────
// Reads a double from an edit control; returns fallback if invalid.
static double ReadDouble(HWND hDlg, int ctrlId, double fallback,
                         double lo, double hi)
{
    wchar_t buf[64] = {};
    GetDlgItemTextW(hDlg, ctrlId, buf, 63);
    wchar_t* end = nullptr;
    double v = std::wcstod(buf, &end);
    if (end == buf || v < lo || v > hi) return fallback;
    return v;
}

// ─────────────────────────────────────────────────────────────────────────────
INT_PTR CALLBACK ThresholdDlgProc(HWND hDlg, UINT msg,
                                   WPARAM wParam, LPARAM lParam)
{
    static ThresholdDlgParam* pParam = nullptr;

    switch (msg) {
    // ── WM_INITDIALOG ───────────────────────────────────────────────────────
    case WM_INITDIALOG: {
        pParam = reinterpret_cast<ThresholdDlgParam*>(lParam);
        ThresholdConfig& cfg = *pParam->cfg;

        SetWindowTextW(hDlg, L"Alert Thresholds");

        // Title label
        CreateWindowExW(0, L"STATIC",
            L"Set the usage level (%) that triggers a Windows notification.",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            20, 14, 350, 20, hDlg, nullptr, nullptr, nullptr);

        // Separator
        CreateWindowExW(0, L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ,
            10, 40, 360, 4, hDlg, nullptr, nullptr, nullptr);

        // Rows
        CreateLabelEdit(hDlg, L"CPU threshold:",      IDC_CPU_EDIT,      55,  cfg.cpu,        L"%");
        CreateLabelEdit(hDlg, L"RAM threshold:",      IDC_RAM_EDIT,      90,  cfg.ram,        L"%");
        CreateLabelEdit(hDlg, L"Disk I/O threshold:", IDC_DISK_EDIT,     125, cfg.disk,       L"%");
        CreateLabelEdit(hDlg, L"Network threshold:",  IDC_NET_EDIT,      160, cfg.net,        L"%");
        CreateLabelEdit(hDlg, L"Cooldown period:",    IDC_COOLDOWN_EDIT, 205, static_cast<double>(cfg.cooldownSec), L"sec");

        // Hint below cooldown
        CreateWindowExW(0, L"STATIC",
            L"(Minimum seconds between repeat alerts for the same resource)",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            20, 232, 350, 18, hDlg, nullptr, nullptr, nullptr);

        // Separator
        CreateWindowExW(0, L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ,
            10, 258, 360, 4, hDlg, nullptr, nullptr, nullptr);

        // Buttons
        CreateWindowExW(0, L"BUTTON", L"Reset Defaults",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            20, 272, 120, 28, hDlg,
            reinterpret_cast<HMENU>(IDC_RESET_BTN), nullptr, nullptr);

        CreateWindowExW(0, L"BUTTON", L"Cancel",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            230, 272, 70, 28, hDlg,
            reinterpret_cast<HMENU>(IDCANCEL), nullptr, nullptr);

        CreateWindowExW(0, L"BUTTON", L"Apply",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            310, 272, 70, 28, hDlg,
            reinterpret_cast<HMENU>(IDOK), nullptr, nullptr);

        // Center on parent
        HWND hParent = GetParent(hDlg);
        if (hParent) {
            RECT rp{}, rd{};
            GetWindowRect(hParent, &rp);
            GetWindowRect(hDlg,    &rd);
            int w = rd.right  - rd.left;
            int h = rd.bottom - rd.top;
            int x = rp.left + (rp.right  - rp.left - w) / 2;
            int y = rp.top  + (rp.bottom - rp.top  - h) / 2;
            SetWindowPos(hDlg, nullptr, x, y, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER);
        }

        return TRUE;
    }

    // ── WM_COMMAND ──────────────────────────────────────────────────────────
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDOK: {
            // Validate & write back
            if (!pParam) { EndDialog(hDlg, IDCANCEL); return TRUE; }
            ThresholdConfig& cfg = *pParam->cfg;

            double cpu  = ReadDouble(hDlg, IDC_CPU_EDIT,      cfg.cpu,  1.0, 100.0);
            double ram  = ReadDouble(hDlg, IDC_RAM_EDIT,      cfg.ram,  1.0, 100.0);
            double disk = ReadDouble(hDlg, IDC_DISK_EDIT,     cfg.disk, 1.0, 100.0);
            double net  = ReadDouble(hDlg, IDC_NET_EDIT,      cfg.net,  1.0, 100.0);
            int    cool = static_cast<int>(
                            ReadDouble(hDlg, IDC_COOLDOWN_EDIT,
                                       static_cast<double>(cfg.cooldownSec), 5.0, 3600.0));

            cfg.cpu         = cpu;
            cfg.ram         = ram;
            cfg.disk        = disk;
            cfg.net         = net;
            cfg.cooldownSec = cool;

            EndDialog(hDlg, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(hDlg, IDCANCEL);
            return TRUE;

        case IDC_RESET_BTN: {
            // Restore factory defaults
            ThresholdConfig def{};
            SetDlgItemTextW(hDlg, IDC_CPU_EDIT,      L"85.0");
            SetDlgItemTextW(hDlg, IDC_RAM_EDIT,      L"90.0");
            SetDlgItemTextW(hDlg, IDC_DISK_EDIT,     L"95.0");
            SetDlgItemTextW(hDlg, IDC_NET_EDIT,      L"80.0");
            SetDlgItemTextW(hDlg, IDC_COOLDOWN_EDIT, L"30");
            (void)def;
            return TRUE;
        }
        }
        break;

    case WM_CLOSE:
        EndDialog(hDlg, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}
