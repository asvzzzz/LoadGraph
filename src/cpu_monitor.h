#ifndef CPU_MONITOR_H
#define CPU_MONITOR_H

#include "metrics_collector.h"
#include "platform.h"

#ifdef PLATFORM_WINDOWS
// Keep windows.h from pulling winsock.h: later headers (wx via logger.h)
// include winsock2.h, and the two cannot coexist in one translation unit.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <pdh.h>
#endif

#include <cstdint>
#include <string>
#include <vector>

// Structure holding system-wide CPU metrics.
struct CpuMetrics
{
    double    loadPercent = 0.0;   // Overall CPU usage percentage [0.0, 100.0]
    double    temperature = -1.0;  // Temperature in Celsius, -1.0 means N/A
    int       fanRpm      = -1;    // Fan speed in RPM, -1 means N/A
    uint64_t  uptimeMs    = 0;     // System uptime in milliseconds
    std::wstring cpuName;          // CPU model name (empty if unavailable)
};

// Collects overall CPU load, temperature and fan speed.
// Load uses the PDH counter "\Processor(_Total)\% Processor Time" on Windows -
// the same source Task Manager displays, so values match. On Linux uses /proc/stat.
// Temperature/fan use WMI (Windows) or /sys/class/thermal (Linux) and fall back
// to "N/A" if unavailable.
class CpuMonitor : public MetricsCollector
{
public:
    CpuMonitor();
    ~CpuMonitor() override;

    // Get current CPU metrics (thread-safe copy).
    CpuMetrics GetMetrics();

    // Sample the CPU load right now (call from the GUI thread each tick).
    // On Windows uses PDH for zero-lag reading; on Linux reads /proc/stat.
    // Returns the load percentage, or -1.0 on failure.
    double SampleLoad();

    // Read the CPU model name (called once at startup before polling starts).
    void QueryCpuName();

protected:
    void Collect() override;

private:
#ifdef PLATFORM_WINDOWS
    bool QueryOverallLoad(double& loadPercent);
    unsigned int ProbeWmiSensor(double& tempC, int& fanRpm);
    PDH_HQUERY   m_pdhQuery   = nullptr;
    PDH_HCOUNTER m_pdhCounter = nullptr;
    bool         m_pdhTried   = false;
    bool         m_pdhOk      = false;
    ULONGLONG m_prevIdle   = 0;
    ULONGLONG m_prevKernel = 0;
    ULONGLONG m_prevUser   = 0;
    bool      m_prevTimesValid = false;
    int  m_wmiCooldown = 0;
#endif
    CpuMetrics m_metrics;
    // Shared probe control (used by both Windows WMI and Linux thermal probe).
    bool m_wmiTried   = false;
    bool m_wmiOk      = false;
};

#endif // CPU_MONITOR_H
