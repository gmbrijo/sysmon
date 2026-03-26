/*  app.cpp
 *
 *  SysMonitorApp implementation.
 *  Responsibilities:
 *    • Win32 window + message loop
 *    • WM_TIMER-driven polling (1 s)
 *    • Double-buffered GDI drawing (no flicker)
 *    • Sparkline history graphs
 *    • System tray icon with context menu
 *    • Windows balloon / Shell_NotifyIcon alerts
 */

#include "resource_monitor.h"
#include <cmath>

// Prevent Windows min/max macros conflicting with std::
#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif

// ─── Colours ─────────────────────────────────────────────────────────────────
constexpr COLORREF CLR_BG         = RGB(15,  17,  22);   // near-black
constexpr COLORREF CLR_CARD       = RGB(24,  28,  36);
constexpr COLORREF CLR_BORDER     = RGB(45,  52,  65);
constexpr COLORREF CLR_TEXT_MAIN  = RGB(220, 225, 235);
constexpr COLORREF CLR_TEXT_DIM   = RGB(120, 130, 150);
constexpr COLORREF CLR_CPU        = RGB( 77, 166, 255);
constexpr COLORREF CLR_RAM        = RGB(120, 220, 140);
constexpr COLORREF CLR_DISK       = RGB(255, 185,  80);
constexpr COLORREF CLR_NET        = RGB(200, 120, 255);
constexpr COLORREF CLR_ALERT      = RGB(255,  80,  80);
constexpr COLORREF CLR_BAR_BG     = RGB(35,  40,  52);


// ─────────────────────────────────────────────────────────────────────────────
SysMonitorApp::SysMonitorApp() = default;

SysMonitorApp::~SysMonitorApp() {
    removeTrayIcon();
    if (m_fontTitle)  DeleteObject(m_fontTitle);
    if (m_fontLabel)  DeleteObject(m_fontLabel);
    if (m_fontSmall)  DeleteObject(m_fontSmall);
    if (m_brushBg)    DeleteObject(m_brushBg);
    if (m_brushCard)  DeleteObject(m_brushCard);
}

// ─────────────────────────────────────────────────────────────────────────────
bool SysMonitorApp::init(HINSTANCE hInst) {
    m_hInst = hInst;

    // Fonts
    m_fontTitle = CreateFontW(22, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    m_fontLabel = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    m_fontSmall = CreateFontW(12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    // Brushes
    m_brushBg   = CreateSolidBrush(CLR_BG);
    m_brushCard = CreateSolidBrush(CLR_CARD);

    // Init PDH
    if (!m_pdh.initialize()) {
        MessageBoxW(nullptr, m_pdh.lastError().c_str(),
                    L"PDH Init Failed", MB_ICONERROR);
        return false;
    }

    if (!createMainWindow(hInst)) return false;

    setupTrayIcon();

    // Seed first reading
    {
        std::lock_guard<std::mutex> lk(m_dataMutex);
        m_pdh.collect(m_current);
        m_history.push(m_current);
    }

    // 1-second polling timer
    SetTimer(m_hwnd, IDT_POLL_TIMER, POLL_INTERVAL_MS, nullptr);

    ShowWindow(m_hwnd, SW_SHOW);
    UpdateWindow(m_hwnd);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
int SysMonitorApp::run() {
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

// ─────────────────────────────────────────────────────────────────────────────
bool SysMonitorApp::createMainWindow(HINSTANCE hInst) {
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = SysMonitorApp::WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = m_brushBg;
    wc.lpszClassName = L"SysMonitorWnd";
    wc.hIcon         = LoadIconW(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);

    // Centre on screen
    int sx = GetSystemMetrics(SM_CXSCREEN);
    int sy = GetSystemMetrics(SM_CYSCREEN);
    int x  = (sx - WIN_W) / 2;
    int y  = (sy - WIN_H) / 2;

    m_hwnd = CreateWindowExW(
        WS_EX_APPWINDOW,
        L"SysMonitorWnd",
        L"System Resource Monitor",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        x, y, WIN_W, WIN_H,
        nullptr, nullptr, hInst, this);   // pass 'this' via lpCreateParams

    return m_hwnd != nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
void SysMonitorApp::setupTrayIcon() {
    ZeroMemory(&m_nid, sizeof(m_nid));
    m_nid.cbSize           = sizeof(m_nid);
    m_nid.hWnd             = m_hwnd;
    m_nid.uID              = IDI_TRAYICON;
    m_nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    m_nid.uCallbackMessage = WM_TRAYICON;
    m_nid.hIcon            = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(m_nid.szTip, L"System Resource Monitor");
    Shell_NotifyIconW(NIM_ADD, &m_nid);
}

void SysMonitorApp::removeTrayIcon() {
    Shell_NotifyIconW(NIM_DELETE, &m_nid);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Static WndProc — retrieves 'this' from GWLP_USERDATA
// ─────────────────────────────────────────────────────────────────────────────
LRESULT CALLBACK SysMonitorApp::WndProc(HWND hwnd, UINT msg,
                                         WPARAM wp, LPARAM lp)
{
    SysMonitorApp* pApp = nullptr;

    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        pApp = reinterpret_cast<SysMonitorApp*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(pApp));
        pApp->m_hwnd = hwnd;
    } else {
        pApp = reinterpret_cast<SysMonitorApp*>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (pApp) return pApp->handleMessage(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ─────────────────────────────────────────────────────────────────────────────
LRESULT SysMonitorApp::handleMessage(HWND hwnd, UINT msg,
                                      WPARAM wp, LPARAM lp)
{
    switch (msg) {

    // ── Timer → poll ────────────────────────────────────────────────────────
    case WM_TIMER:
        if (wp == IDT_POLL_TIMER) {
            onTimer();
            return 0;
        }
        break;

    // ── Paint (double-buffered) ─────────────────────────────────────────────
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hwnd, &ps);

        RECT rc{};
        GetClientRect(hwnd, &rc);
        int w = rc.right, h = rc.bottom;

        // Create off-screen buffer
        HDC memDC = CreateCompatibleDC(hdc);
        HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
        HBITMAP old = static_cast<HBITMAP>(SelectObject(memDC, bmp));

        // Fill background
        FillRect(memDC, &rc, m_brushBg);

        // Draw everything into memDC
        {
            std::lock_guard<std::mutex> lk(m_dataMutex);
            onPaint(memDC, rc);
        }

        // Blit to screen
        BitBlt(hdc, 0, 0, w, h, memDC, 0, 0, SRCCOPY);

        SelectObject(memDC, old);
        DeleteObject(bmp);
        DeleteDC(memDC);

        EndPaint(hwnd, &ps);
        return 0;
    }

    // ── Erase background — suppress to avoid flicker ────────────────────────
    case WM_ERASEBKGND:
        return 1;

    // ── Tray icon ───────────────────────────────────────────────────────────
    case WM_TRAYICON:
        if (lp == WM_LBUTTONDBLCLK) {
            ShowWindow(hwnd, SW_RESTORE);
            SetForegroundWindow(hwnd);
            m_minimizedToTray = false;
        } else if (lp == WM_RBUTTONUP) {
            POINT pt{};
            GetCursorPos(&pt);
            HMENU hMenu = CreatePopupMenu();
            AppendMenuW(hMenu, MF_STRING, 1, L"Open Monitor");
            AppendMenuW(hMenu, MF_STRING, 2, L"Set Thresholds...");
            AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(hMenu, MF_STRING, 3, L"Exit");
            SetForegroundWindow(hwnd);
            int cmd = TrackPopupMenu(hMenu,
                TPM_RETURNCMD | TPM_RIGHTBUTTON,
                pt.x, pt.y, 0, hwnd, nullptr);
            DestroyMenu(hMenu);
            if (cmd == 1) {
                ShowWindow(hwnd, SW_RESTORE);
                SetForegroundWindow(hwnd);
                m_minimizedToTray = false;
            } else if (cmd == 2) {
                openThresholdDialog();
            } else if (cmd == 3) {
                DestroyWindow(hwnd);
            }
        }
        return 0;

    // ── Minimize to tray ────────────────────────────────────────────────────
    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_MINIMIZE) {
            ShowWindow(hwnd, SW_HIDE);
            m_minimizedToTray = true;
            return 0;
        }
        break;

    // ── Set Thresholds button (WM_COMMAND id=1) ─────────────────────────────
    case WM_COMMAND:
        if (LOWORD(wp) == 1) { openThresholdDialog(); return 0; }
        break;

    case WM_DESTROY:
        KillTimer(hwnd, IDT_POLL_TIMER);
        removeTrayIcon();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ─────────────────────────────────────────────────────────────────────────────
void SysMonitorApp::onTimer() {
    ResourceSnapshot snap{};
    if (m_pdh.collect(snap)) {
        std::lock_guard<std::mutex> lk(m_dataMutex);
        m_current = snap;
        m_history.push(snap);
    }
    checkThresholds(snap);
    invalidateWindow();
}

// ─────────────────────────────────────────────────────────────────────────────
void SysMonitorApp::checkThresholds(const ResourceSnapshot& s) {
    ULONGLONG now = GetTickCount64();
    ULONGLONG coolMs = static_cast<ULONGLONG>(m_thresholds.cooldownSec) * 1000ULL;

    // CPU
    if (s.cpuPercent >= m_thresholds.cpu &&
        (now - m_notifyState.lastCpuNotify) > coolMs) {
        wchar_t body[128];
        swprintf_s(body, L"CPU usage is %.1f%% (threshold: %.1f%%)",
                   s.cpuPercent, m_thresholds.cpu);
        sendNotification(L"⚠ High CPU Usage", body);
        m_notifyState.lastCpuNotify = now;
    }
    // RAM
    if (s.ramPercent >= m_thresholds.ram &&
        (now - m_notifyState.lastRamNotify) > coolMs) {
        wchar_t body[128];
        swprintf_s(body, L"RAM usage is %.1f%% (%.0f / %.0f MB)",
                   s.ramPercent, s.ramUsedMB, s.ramTotalMB);
        sendNotification(L"⚠ High Memory Usage", body);
        m_notifyState.lastRamNotify = now;
    }
    // Disk — trigger when combined I/O > threshold% of 500 MB/s nominal
    double diskPct = (s.diskReadMBs + s.diskWriteMBs) / 5.0; // /500 * 100
    if (diskPct >= m_thresholds.disk &&
        (now - m_notifyState.lastDiskNotify) > coolMs) {
        wchar_t body[128];
        swprintf_s(body, L"Disk I/O: R %.1f MB/s  W %.1f MB/s",
                   s.diskReadMBs, s.diskWriteMBs);
        sendNotification(L"⚠ High Disk I/O", body);
        m_notifyState.lastDiskNotify = now;
    }
    // Network
    double netPct = (s.netSendMbps + s.netRecvMbps) / 100.0 * 100.0; // % of 100 Mbps
    if (netPct >= m_thresholds.net &&
        (now - m_notifyState.lastNetNotify) > coolMs) {
        wchar_t body[128];
        swprintf_s(body, L"Network: ↑ %.2f Mbps  ↓ %.2f Mbps",
                   s.netSendMbps, s.netRecvMbps);
        sendNotification(L"⚠ High Network Usage", body);
        m_notifyState.lastNetNotify = now;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
void SysMonitorApp::sendNotification(const std::wstring& title,
                                      const std::wstring& body)
{
    // Uses Shell_NotifyIcon balloon — works on all Windows versions
    // without requiring WinRT / UWP APIs.
    NOTIFYICONDATAW nid{};
    nid.cbSize      = sizeof(nid);
    nid.hWnd        = m_hwnd;
    nid.uID         = IDI_TRAYICON;
    nid.uFlags      = NIF_INFO;
    nid.dwInfoFlags = NIIF_WARNING;
    nid.uTimeout    = 5000;

    wcsncpy_s(nid.szInfoTitle, title.c_str(), ARRAYSIZE(nid.szInfoTitle) - 1);
    wcsncpy_s(nid.szInfo,      body.c_str(),  ARRAYSIZE(nid.szInfo)      - 1);

    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

// ─────────────────────────────────────────────────────────────────────────────
void SysMonitorApp::openThresholdDialog() {
    // Build dialog template in memory (no .rc required)
    struct {
        DLGTEMPLATE tmpl;
        WORD        menu, cls, title;
    } dlgTmpl{};
    dlgTmpl.tmpl.style          = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_SETFONT | DS_MODALFRAME | DS_CENTER;
    dlgTmpl.tmpl.cx             = 220;
    dlgTmpl.tmpl.cy             = 185;

    ThresholdConfig cfg = m_thresholds;
    ThresholdDlgParam param{ &cfg };

    INT_PTR result = DialogBoxIndirectParamW(
        m_hInst,
        &dlgTmpl.tmpl,
        m_hwnd,
        ThresholdDlgProc,
        reinterpret_cast<LPARAM>(&param));

    if (result == IDOK) {
        std::lock_guard<std::mutex> lk(m_dataMutex);
        m_thresholds = cfg;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
void SysMonitorApp::invalidateWindow() {
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

// ═══════════════════════════════════════════════════════════════════════════ //
//  DRAWING
// ═══════════════════════════════════════════════════════════════════════════ //

static void SetTextCol(HDC hdc, COLORREF c) { SetTextColor(hdc, c); }
static void FillR(HDC hdc, RECT r, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    FillRect(hdc, &r, b);
    DeleteObject(b);
}
static void DrawRoundRect(HDC hdc, RECT r, int rad, COLORREF fill, COLORREF border) {
    HBRUSH fb = CreateSolidBrush(fill);
    HPEN   bp = CreatePen(PS_SOLID, 1, border);
    HBRUSH ob = static_cast<HBRUSH>(SelectObject(hdc, fb));
    HPEN   op = static_cast<HPEN>(SelectObject(hdc, bp));
    RoundRect(hdc, r.left, r.top, r.right, r.bottom, rad, rad);
    SelectObject(hdc, ob); SelectObject(hdc, op);
    DeleteObject(fb); DeleteObject(bp);
}

// ─────────────────────────────────────────────────────────────────────────────
void SysMonitorApp::onPaint(HDC hdc, const RECT& clientRect) {
    SetBkMode(hdc, TRANSPARENT);
    RECT rc = clientRect;

    drawHeader(hdc, rc);

    // ── Metric rows ──
    drawMetricRow(hdc, rc, L"CPU",
        m_current.cpuPercent, 100.0, CLR_CPU,
        m_history.cpu, L"%");

    wchar_t ramLabel[64];
    swprintf_s(ramLabel, L"RAM  %.0f/%.0f MB", m_current.ramUsedMB, m_current.ramTotalMB);
    drawMetricRow(hdc, rc, ramLabel,
        m_current.ramPercent, 100.0, CLR_RAM,
        m_history.ram, L"%");

    wchar_t diskLabel[64];
    swprintf_s(diskLabel, L"Disk R:%.1f W:%.1f MB/s",
               m_current.diskReadMBs, m_current.diskWriteMBs);
    double diskPct = std::clamp((m_current.diskReadMBs + m_current.diskWriteMBs) / 5.0, 0.0, 100.0);
    drawMetricRow(hdc, rc, diskLabel,
        diskPct, 100.0, CLR_DISK,
        m_history.diskRead, L"%");

    wchar_t netLabel[64];
    swprintf_s(netLabel, L"Net  ↑%.2f ↓%.2f Mbps",
               m_current.netSendMbps, m_current.netRecvMbps);
    double netPct = std::clamp((m_current.netSendMbps + m_current.netRecvMbps), 0.0, 100.0);
    drawMetricRow(hdc, rc, netLabel,
        netPct, 100.0, CLR_NET,
        m_history.netSend, L"%");

    drawThresholdPanel(hdc, rc);
}

// ─────────────────────────────────────────────────────────────────────────────
void SysMonitorApp::drawHeader(HDC hdc, RECT& rc) {
    RECT header = { rc.left, rc.top, rc.right, rc.top + 60 };
    FillR(hdc, header, CLR_CARD);

    // Border bottom
    HPEN pen = CreatePen(PS_SOLID, 1, CLR_BORDER);
    HPEN old = static_cast<HPEN>(SelectObject(hdc, pen));
    MoveToEx(hdc, 0, 59, nullptr);
    LineTo(hdc, rc.right, 59);
    SelectObject(hdc, old); DeleteObject(pen);

    // Title
    SelectObject(hdc, m_fontTitle);
    SetTextCol(hdc, CLR_TEXT_MAIN);
    RECT tr = { 20, 12, 350, 40 };
    DrawTextW(hdc, L"System Resource Monitor", -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // "Set Thresholds" button — drawn as a clickable rect area
    // We use WM_COMMAND id=1 via a child button-less approach:
    // Just render a styled rect; real clicks route through WM_LBUTTONUP below.
    RECT btnR = { WIN_W - 150, 17, WIN_W - 16, 43 };
    DrawRoundRect(hdc, btnR, 6, CLR_BORDER, CLR_CPU);
    SelectObject(hdc, m_fontSmall);
    SetTextCol(hdc, CLR_CPU);
    DrawTextW(hdc, L"⚙ Set Thresholds", -1, &btnR,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    rc.top = 68;
}

// ─────────────────────────────────────────────────────────────────────────────
void SysMonitorApp::drawMetricRow(HDC hdc, RECT& rc,
                                   const wchar_t* label,
                                   double value, double maxVal,
                                   COLORREF barColor,
                                   const std::deque<double>& history,
                                   const wchar_t* /*unit*/)
{
    const int CARD_H     = 110;
    const int PADDING    = 10;
    const int BAR_H      = 14;
    const int SPARK_H    = 50;
    const int SPARK_W    = 180;

    RECT card = { PADDING, rc.top + PADDING,
                  WIN_W - PADDING, rc.top + PADDING + CARD_H };
    DrawRoundRect(hdc, card, 8, CLR_CARD, CLR_BORDER);

    // ── Label ──
    SelectObject(hdc, m_fontLabel);
    SetTextCol(hdc, CLR_TEXT_MAIN);
    RECT lr = { card.left + 14, card.top + 12, card.right - SPARK_W - 10, card.top + 32 };
    DrawTextW(hdc, label, -1, &lr, DT_LEFT | DT_SINGLELINE);

    // ── Value % badge ──
    wchar_t pct[16];
    swprintf_s(pct, L"%.1f%%", value);
    SelectObject(hdc, m_fontTitle);
    bool alert = (value >= maxVal * 0.85);
    SetTextCol(hdc, alert ? CLR_ALERT : barColor);
    RECT vr = { card.left + 14, card.top + 34, card.left + 110, card.top + 62 };
    DrawTextW(hdc, pct, -1, &vr, DT_LEFT | DT_SINGLELINE);

    // ── Progress bar ──
    RECT barBg = { card.left + 14, card.top + 68,
                   card.right - SPARK_W - 20, card.top + 68 + BAR_H };
    DrawRoundRect(hdc, barBg, 4, CLR_BAR_BG, CLR_BAR_BG);

    double fraction = std::clamp(value / maxVal, 0.0, 1.0);
    int    fillW    = static_cast<int>((barBg.right - barBg.left) * fraction);
    if (fillW > 2) {
        RECT barFill = { barBg.left, barBg.top,
                         barBg.left + fillW, barBg.bottom };
        DrawRoundRect(hdc, barFill, 4, alert ? CLR_ALERT : barColor, alert ? CLR_ALERT : barColor);
    }

    // ── Sparkline panel ──
    RECT sparkRect = { card.right - SPARK_W - 8, card.top + 10,
                       card.right - 8,           card.top + 10 + SPARK_H };
    FillR(hdc, sparkRect, CLR_BAR_BG);
    drawSparkline(hdc, sparkRect, history, maxVal, barColor);

    // ── Sparkline caption ──
    SelectObject(hdc, m_fontSmall);
    SetTextCol(hdc, CLR_TEXT_DIM);
    RECT scap = { sparkRect.left, sparkRect.bottom + 2,
                  sparkRect.right, sparkRect.bottom + 18 };
    DrawTextW(hdc, L"60 s history", -1, &scap, DT_CENTER | DT_SINGLELINE);

    rc.top = card.bottom + 2;
}

// ─────────────────────────────────────────────────────────────────────────────
void SysMonitorApp::drawSparkline(HDC hdc, const RECT& rc,
                                   const std::deque<double>& data,
                                   double maxVal, COLORREF color)
{
    if (data.size() < 2) return;

    int w = rc.right  - rc.left;
    int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;

    double peak = maxVal;
    for (auto v : data) if (v > peak) peak = v;
    if (peak <= 0) peak = 1.0;

    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HPEN old = static_cast<HPEN>(SelectObject(hdc, pen));

    int n = static_cast<int>(data.size());

    int denom = (n - 1 > 0) ? (n - 1) : 1;
    auto px = [&](int i) {
        return rc.left + static_cast<int>(i * (w - 1) / denom);
    };
    auto py = [&](double v) {
        return rc.bottom - 1 - static_cast<int>((v / peak) * (h - 2));
    };

    MoveToEx(hdc, px(0), py(data[0]), nullptr);
    for (int i = 1; i < n; ++i)
        LineTo(hdc, px(i), py(data[i]));

    SelectObject(hdc, old);
    DeleteObject(pen);
}

// ─────────────────────────────────────────────────────────────────────────────
void SysMonitorApp::drawThresholdPanel(HDC hdc, RECT& rc) {
    int top = rc.top + 8;
    RECT panel = { 10, top, WIN_W - 10, top + 76 };
    DrawRoundRect(hdc, panel, 8, CLR_CARD, CLR_BORDER);

    SelectObject(hdc, m_fontLabel);
    SetTextCol(hdc, CLR_TEXT_DIM);
    RECT tr = { panel.left + 14, panel.top + 8, panel.right - 10, panel.top + 26 };
    DrawTextW(hdc, L"Alert Thresholds (click ⚙ to edit)", -1, &tr,
              DT_LEFT | DT_SINGLELINE);

    // Four inline values
    wchar_t line[256];
    swprintf_s(line,
        L"CPU: %.0f%%     RAM: %.0f%%     Disk I/O: %.0f%%     Network: %.0f%%  |  Cooldown: %ds",
        m_thresholds.cpu, m_thresholds.ram,
        m_thresholds.disk, m_thresholds.net,
        m_thresholds.cooldownSec);

    SelectObject(hdc, m_fontSmall);
    SetTextCol(hdc, CLR_TEXT_MAIN);
    RECT vr = { panel.left + 14, panel.top + 30, panel.right - 10, panel.top + 50 };
    DrawTextW(hdc, line, -1, &vr, DT_LEFT | DT_SINGLELINE);

    // Hint
    SelectObject(hdc, m_fontSmall);
    SetTextCol(hdc, CLR_TEXT_DIM);
    RECT hr = { panel.left + 14, panel.top + 50, panel.right - 10, panel.top + 70 };
    DrawTextW(hdc, L"Notifications appear as Windows balloon alerts from the system tray.",
              -1, &hr, DT_LEFT | DT_SINGLELINE);

    // "Set Thresholds" hit-test — handled via WM_LBUTTONUP routed as WM_COMMAND
    rc.top = panel.bottom + 6;
}
