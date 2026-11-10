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

#include <atomic>
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
#ifdef PLATFORM_WINDOWS
    wchar_t   cpuName[256] = {};   // UTF-16 from registry (Windows)
#else
    char      cpuName[256] = {};   // UTF-8 from /proc/cpuinfo (Linux)
#endif
    double    powerWatts  = -1.0;  // Current CPU power draw in watts, -1.0 = N/A
    double    tdpWatts    = -1.0;  // CPU TDP (max power) in watts, -1.0 = N/A
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

    // Get current CPU metrics (thread-safe, field-by-field under lock).
    // Returns each field individually while holding the lock to prevent
    // any race condition with the polling thread's m_metrics writes.
    void GetMetrics(CpuMetrics& out);

    // Sample CPU load (PDH delta on Windows, /proc/stat on Linux).
    // Thread-safe: writes into m_metrics under its own lock.
    // Called from the polling thread inside Collect() — do NOT call from GUI thread.
    double SampleLoad();

    // Read the CPU model name (called once at startup before polling starts).
    void QueryCpuName();
    // Probe MSR thermal sensor params on the main thread.
    void ProbeMsrInit();
#ifdef PLATFORM_LINUX
    // Read RAPL package energy counter and return power in watts.
    // Returns -1.0 when RAPL is unavailable (WSL2, AMD, root-only perms).
    double ReadRaplPowerWatts();
#endif

protected:
    void Collect() override;

#ifdef PLATFORM_LINUX
    // RAPL package energy counter state — monotonic microjoule counter with
    // wrap-around handling. Accessed only from the GUI thread (1 Hz tick), so
    // no extra locking is needed beyond m_mutex for m_metrics writes.
    struct RaplState
    {
        bool        opened      = false;  // probe already attempted
        bool        valid       = false;  // counter path found and readable
        std::string path;                 // energy_uj file path
        long long   prevEnergyUj = 0;
        double      prevTimeSec  = 0.0;
        long long   maxEnergyUj  = 0;     // wrap-around range (0 = unknown)
    };
#endif

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
    bool m_fanIoTried  = false;  // true after first Super I/O fan probe attempt
    // Optional real CPU package power sensor (LibreHardwareMonitor /
    // OpenHardwareMonitor WMI namespace). Probed once, then read each tick.
    bool        m_powerLhmTried = false;
    bool        m_powerLhmOk    = false;
    std::wstring m_powerLhmNs;          // namespace that exposed the sensor (UTF-16)
    // MSR-based temperature (kernel driver or direct rdmsr). Probed once per session.
    bool        m_msrTried   = false;
    bool        m_msrOk      = false;
    int         m_msrCooldown = 0;
    double      m_lastMsrTemp = -1.0; // last successful MSR temperature (restored during cooldown)
    uint32_t    m_msrFamily  = 0;
    uint32_t    m_msrModel   = 0;
    uint32_t    m_msrStepping = 0;
    int8_t      m_msrTjmax   = 0;
#endif
    CpuMetrics m_metrics;
    // Shared probe control (used by both Windows WMI and Linux thermal probe).
    bool m_wmiTried   = false;
    bool m_wmiOk      = false;
    // Physical core count for the idle-floor power estimate (0 = unknown).
    // Written once at startup (QueryCpuName / constructor) before polling
    // starts, read from SampleLoad() - atomic for cross-thread visibility.
    std::atomic<uint32_t> m_coreCount{0};
    // True while a real power sensor (LHM WMI or Linux RAPL) supplies
    // powerWatts; SampleLoad() then skips the load x TDP estimate.
    std::atomic<bool> m_powerReal{false};
#ifdef PLATFORM_LINUX
    // Previous /proc/stat values for delta-based CPU load calculation.
    double   m_prevUser = 0, m_prevNice = 0, m_prevSystem = 0, m_prevIdle = 0;
    bool     m_loadFirst = true;          // first sample is a warm-up discard
    RaplState m_rapl;                    // RAPL package energy counter state
#endif
};

#endif // CPU_MONITOR_H
