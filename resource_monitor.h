#pragma once

// Do NOT redefine UNICODE/_UNICODE here — the project already sets them.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <windowsx.h>   // GET_X_LPARAM, GET_Y_LPARAM
#include <pdh.h>
#include <pdhmsg.h>
#include <psapi.h>
#include <shellapi.h>
#include <commctrl.h>
#include <shlobj.h>
#include <iphlpapi.h>
#include <string>
#include <vector>
#include <deque>
#include <mutex>
#include <algorithm>

#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "iphlpapi.lib")

// ─── Window & Timer IDs ──────────────────────────────────────────────────────
constexpr UINT   WM_TRAYICON      = WM_USER + 1;
constexpr UINT   IDT_POLL_TIMER   = 1001;
constexpr UINT   IDI_TRAYICON     = 2001;
constexpr UINT   POLL_INTERVAL_MS = 1000;
constexpr size_t HISTORY_DEPTH    = 60;

// Window dimensions — shared between app.cpp and main.cpp
constexpr int WIN_W = 520;
constexpr int WIN_H = 620;

// ─── Threshold Config ────────────────────────────────────────────────────────
struct ThresholdConfig {
    double cpu         = 85.0;
    double ram         = 90.0;
    double disk        = 95.0;
    double net         = 80.0;
    int    cooldownSec = 30;
};

// ─── Resource Snapshot ───────────────────────────────────────────────────────
struct ResourceSnapshot {
    double    cpuPercent   = 0.0;
    double    ramPercent   = 0.0;
    double    ramUsedMB    = 0.0;
    double    ramTotalMB   = 0.0;
    double    diskReadMBs  = 0.0;
    double    diskWriteMBs = 0.0;
    double    netSendMbps  = 0.0;
    double    netRecvMbps  = 0.0;
    ULONGLONG timestamp    = 0;
};

// ─── History (60-second sparklines) ─────────────────────────────────────────
struct ResourceHistory {
    std::deque<double> cpu, ram, diskRead, diskWrite, netSend, netRecv;

    void push(const ResourceSnapshot& s) {
        auto add = [](std::deque<double>& d, double v) {
            d.push_back(v);
            if (d.size() > HISTORY_DEPTH) d.pop_front();
        };
        add(cpu,       s.cpuPercent);
        add(ram,       s.ramPercent);
        add(diskRead,  s.diskReadMBs);
        add(diskWrite, s.diskWriteMBs);
        add(netSend,   s.netSendMbps);
        add(netRecv,   s.netRecvMbps);
    }
};

// ─── Notification Cooldown State ─────────────────────────────────────────────
struct NotifyState {
    ULONGLONG lastCpuNotify  = 0;
    ULONGLONG lastRamNotify  = 0;
    ULONGLONG lastDiskNotify = 0;
    ULONGLONG lastNetNotify  = 0;
};

// ─── PDH Collector ───────────────────────────────────────────────────────────
class PdhCollector {
public:
    PdhCollector();
    ~PdhCollector();

    bool         initialize();
    bool         collect(ResourceSnapshot& out);
    std::wstring lastError() const { return m_lastError; }

private:
    PDH_HQUERY   m_query      = nullptr;
    PDH_HCOUNTER m_ctrCpu     = nullptr;
    PDH_HCOUNTER m_ctrDiskR   = nullptr;
    PDH_HCOUNTER m_ctrDiskW   = nullptr;
    PDH_HCOUNTER m_ctrNetSend = nullptr;
    PDH_HCOUNTER m_ctrNetRecv = nullptr;
    std::wstring m_lastError;
    double       m_netPeakMbps = 100.0;

    bool addCounter(const wchar_t* path, PDH_HCOUNTER& handle);
};

// ─── Main Application ────────────────────────────────────────────────────────
class SysMonitorApp {
public:
    SysMonitorApp();
    ~SysMonitorApp();

    bool    init(HINSTANCE hInst);
    int     run();

    LRESULT handleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

private:
    bool createMainWindow(HINSTANCE hInst);
    void setupTrayIcon();
    void removeTrayIcon();
    void onTimer();
    void checkThresholds(const ResourceSnapshot& snap);
    void sendNotification(const std::wstring& title, const std::wstring& body);
    void onPaint(HDC hdc, const RECT& clientRect);
    void drawHeader(HDC hdc, RECT& rc);
    void drawMetricRow(HDC hdc, RECT& rc, const wchar_t* label,
                       double value, double maxVal, COLORREF barColor,
                       const std::deque<double>& history, const wchar_t* unit);
    void drawSparkline(HDC hdc, const RECT& rc, const std::deque<double>& data,
                       double maxVal, COLORREF color);
    void drawThresholdPanel(HDC hdc, RECT& rc);
    void openThresholdDialog();
    void invalidateWindow();

    HWND             m_hwnd            = nullptr;
    HINSTANCE        m_hInst           = nullptr;
    NOTIFYICONDATA   m_nid             = {};
    PdhCollector     m_pdh;
    ResourceSnapshot m_current;
    ResourceHistory  m_history;
    ThresholdConfig  m_thresholds;
    NotifyState      m_notifyState;
    std::mutex       m_dataMutex;
    bool             m_minimizedToTray = false;
    HFONT            m_fontTitle       = nullptr;
    HFONT            m_fontLabel       = nullptr;
    HFONT            m_fontSmall       = nullptr;
    HBRUSH           m_brushBg         = nullptr;
    HBRUSH           m_brushCard       = nullptr;
};

// ─── Threshold Dialog ────────────────────────────────────────────────────────
INT_PTR CALLBACK ThresholdDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam);

struct ThresholdDlgParam {
    ThresholdConfig* cfg = nullptr;
};
