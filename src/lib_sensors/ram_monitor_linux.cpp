#include "ram_monitor.h"
#include "platform.h"
#include "logger.h"

#ifdef PLATFORM_LINUX

#include <fstream>
#include <sstream>

// ----------------------------------------------------------------------------
// Linux-specific RAM monitor implementation.
// Uses /proc/meminfo for memory statistics.
// ----------------------------------------------------------------------------

namespace
{
    // Parse /proc/meminfo and fill metrics.
    bool ReadMemInfo(RamMetrics& metrics)
    {
        std::string content;
        if (!ReadFileToString("/proc/meminfo", content))
            return false;

        std::istringstream iss(content);
        std::string line;
        while (std::getline(iss, line))
        {
            size_t colon = line.find(':');
            if (colon == std::string::npos) continue;

            std::string key = line.substr(0, colon);
            size_t valStart = line.find_first_of("0123456789-", colon + 1);
            if (valStart == std::string::npos) continue;

            long long valueKb = 0;
            try { valueKb = std::stoll(line.substr(valStart)); }
            catch (...) { continue; }

            if (key == "MemTotal")
                metrics.totalBytes = static_cast<uint64_t>(valueKb) * 1024ULL;
            else if (key == "MemAvailable" || key == "MemFree")
                metrics.freeBytes = static_cast<uint64_t>(valueKb) * 1024ULL;
        }

        // Calculate load percent.
        if (metrics.totalBytes > 0)
        {
            metrics.loadPercent = static_cast<double>(
                metrics.totalBytes - metrics.freeBytes) /
                static_cast<double>(metrics.totalBytes) * 100.0;
        }

        return metrics.totalBytes > 0;
    }
} // anonymous namespace

RamMonitor::RamMonitor()
{
    LOGI("RAM monitor initializing (Linux /proc/meminfo)...");
}



RamMetrics RamMonitor::GetMetrics()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_metrics;
}

void RamMonitor::Collect()
{
    RamMetrics fresh{};
    if (ReadMemInfo(fresh))
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_metrics = fresh;
        LOGI("RAM: total=%.0f GB free=%.0f GB used=%.0f%%",
             static_cast<double>(m_metrics.totalBytes) / 1073741824.0,
             static_cast<double>(m_metrics.freeBytes) / 1073741824.0,
             m_metrics.loadPercent);
    }
    else
    {
        LOGE("Failed to read /proc/meminfo");
    }
}

bool RamMonitor::QueryMemoryStatus(RamMetrics& metrics)
{
    return ReadMemInfo(metrics);
}

bool RamMonitor::QueryRamSpeed(int& speedMhz)
{
    // Not available via simple file read on Linux.
    // Could parse dmidecode output but requires root.
    speedMhz = -1;
    return false;
}

#endif // PLATFORM_LINUX
