#ifndef METRICS_API_H
#define METRICS_API_H

// Public API for reading current system metrics.
// Used by the HTTP server to build JSON responses without depending on wxWidgets.

#include "platform.h"   // defines PLATFORM_WINDOWS / PLATFORM_LINUX and includes windows.h

#include <cstdint>
#include <string>
#include <vector>

// Storage device metrics (power estimated from interface type).
struct StorageData {
    std::string name;
    std::string iface;        // Interface type: "SATA", "NVMe", "USB"
    uint64_t    totalBytes   = 0;
    double      powerWatts   = -1.0;
};

struct CpuData {
    double  loadPercent  = 0.0;
    double  temperature  = -1.0;
    int     fanRpm       = -1;
    uint64_t uptimeMs    = 0;
#ifdef PLATFORM_WINDOWS
    std::wstring cpuName;   // UTF-16 from registry (ProcessorNameString)
#else
    std::string  cpuName;   // UTF-8 from /proc/cpuinfo
#endif
    double  powerWatts   = -1.0;
    double  tdpWatts     = -1.0;
};

struct RamData {
    uint64_t totalBytes  = 0;
    uint64_t freeBytes   = 0;
    double   loadPercent = 0.0;
    double   powerWatts  = -1.0;  // estimated RAM power consumption
};

struct GpuData {
    std::string name;
    double      loadPercent     = 0.0;
    double      temperature     = -1.0;
    uint64_t    vramTotalBytes  = 0;
    uint64_t    vramUsedBytes   = 0;
    int         fanRpm          = -1;
    double      fanPercent      = -1.0;
    double      powerWatts      = -1.0;
    double      powerLimitWatts = -1.0;
};

// Initialize the metrics system (called once at app startup).
// Returns true if at least one data source is available.
bool MetricsInit();

// Shutdown and free resources.
void MetricsShutdown();

// Fetch current CPU metrics. Thread-safe.
CpuData  MetricsGetCpu();

// Fetch current RAM metrics. Thread-safe.
RamData  MetricsGetRam();

// Fetch current GPU metrics. Thread-safe. Returns empty vector if no GPU.
std::vector<GpuData> MetricsGetGpus();

// Returns true if NVML/NVAPI is available.
bool MetricsGpuAvailable();

// Fetch current storage metrics. Thread-safe. Returns empty vector if none.
std::vector<StorageData> MetricsGetStorage();

#endif // METRICS_API_H
