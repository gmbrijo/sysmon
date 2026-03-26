/*  pdh_collector.cpp
 *
 *  Uses Windows Performance Data Helper (PDH) — the same underlying API
 *  that Task Manager uses. Poll cadence: 1 second.
 */

#include "resource_monitor.h"

// Avoid Windows min/max macro conflicts
#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif

// ─────────────────────────────────────────────────────────────────────────────
PdhCollector::PdhCollector() = default;

PdhCollector::~PdhCollector() {
    if (m_query) {
        PdhCloseQuery(m_query);
        m_query = nullptr;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
bool PdhCollector::addCounter(const wchar_t* path, PDH_HCOUNTER& handle) {
    PDH_STATUS st = PdhAddCounterW(m_query, path, 0, &handle);
    if (st != ERROR_SUCCESS) {
        wchar_t buf[256];
        swprintf_s(buf, L"PdhAddCounter failed for [%s]: 0x%08X", path, st);
        m_lastError = buf;
        handle = nullptr;
        return false;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
bool PdhCollector::initialize() {
    PDH_STATUS st = PdhOpenQueryW(nullptr, 0, &m_query);
    if (st != ERROR_SUCCESS) {
        m_lastError = L"PdhOpenQuery failed";
        return false;
    }

    addCounter(L"\\Processor(_Total)\\% Processor Time",           m_ctrCpu);
    addCounter(L"\\PhysicalDisk(_Total)\\Disk Read Bytes/sec",     m_ctrDiskR);
    addCounter(L"\\PhysicalDisk(_Total)\\Disk Write Bytes/sec",    m_ctrDiskW);
    addCounter(L"\\Network Interface(*)\\Bytes Sent/sec",          m_ctrNetSend);
    addCounter(L"\\Network Interface(*)\\Bytes Received/sec",      m_ctrNetRecv);

    // Seed — first real collect will be valid
    PdhCollectQueryData(m_query);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
static double clampD(double v, double lo, double hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}
static double maxD(double a, double b) { return a > b ? a : b; }

// ─────────────────────────────────────────────────────────────────────────────
bool PdhCollector::collect(ResourceSnapshot& out) {
    out.timestamp = GetTickCount64();

    PDH_STATUS st = PdhCollectQueryData(m_query);
    if (st != ERROR_SUCCESS && st != PDH_NO_DATA) {
        m_lastError = L"PdhCollectQueryData failed";
        return false;
    }

    // ── CPU ──────────────────────────────────────────────────────────────────
    if (m_ctrCpu) {
        PDH_FMT_COUNTERVALUE val{};
        if (PdhGetFormattedCounterValue(m_ctrCpu, PDH_FMT_DOUBLE, nullptr, &val)
                == ERROR_SUCCESS) {
            out.cpuPercent = clampD(val.doubleValue, 0.0, 100.0);
        }
    }

    // ── RAM ──────────────────────────────────────────────────────────────────
    {
        MEMORYSTATUSEX ms{};
        ms.dwLength = sizeof(ms);
        if (GlobalMemoryStatusEx(&ms)) {
            out.ramPercent = static_cast<double>(ms.dwMemoryLoad);
            out.ramTotalMB = static_cast<double>(ms.ullTotalPhys) / (1024.0 * 1024.0);
            out.ramUsedMB  = out.ramTotalMB
                           - static_cast<double>(ms.ullAvailPhys) / (1024.0 * 1024.0);
        }
    }

    // ── Disk ─────────────────────────────────────────────────────────────────
    if (m_ctrDiskR) {
        PDH_FMT_COUNTERVALUE val{};
        if (PdhGetFormattedCounterValue(m_ctrDiskR, PDH_FMT_DOUBLE, nullptr, &val)
                == ERROR_SUCCESS) {
            out.diskReadMBs = maxD(0.0, val.doubleValue / (1024.0 * 1024.0));
        }
    }
    if (m_ctrDiskW) {
        PDH_FMT_COUNTERVALUE val{};
        if (PdhGetFormattedCounterValue(m_ctrDiskW, PDH_FMT_DOUBLE, nullptr, &val)
                == ERROR_SUCCESS) {
            out.diskWriteMBs = maxD(0.0, val.doubleValue / (1024.0 * 1024.0));
        }
    }

    // ── Network — wildcard counter, sum all adapters ──────────────────────
    auto sumWildcard = [](PDH_HCOUNTER ctr) -> double {
        if (!ctr) return 0.0;

        DWORD bufSize = 0, itemCount = 0;
        PDH_STATUS s = PdhGetFormattedCounterArrayW(
            ctr, PDH_FMT_DOUBLE, &bufSize, &itemCount, nullptr);

        if (s != PDH_MORE_DATA && s != ERROR_SUCCESS) return 0.0;
        if (bufSize == 0) return 0.0;

        std::vector<BYTE> buf(static_cast<size_t>(bufSize));
        auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buf.data());

        s = PdhGetFormattedCounterArrayW(
            ctr, PDH_FMT_DOUBLE, &bufSize, &itemCount, items);
        if (s != ERROR_SUCCESS) return 0.0;

        double total = 0.0;
        for (DWORD i = 0; i < itemCount; ++i)
            total += items[i].FmtValue.doubleValue;
        return total > 0.0 ? total : 0.0;
    };

    double sendBps = sumWildcard(m_ctrNetSend);
    double recvBps = sumWildcard(m_ctrNetRecv);

    out.netSendMbps = sendBps * 8.0 / (1024.0 * 1024.0);
    out.netRecvMbps = recvBps * 8.0 / (1024.0 * 1024.0);

    double totalMbps = out.netSendMbps + out.netRecvMbps;
    if (totalMbps > m_netPeakMbps) m_netPeakMbps = totalMbps;

    return true;
}
