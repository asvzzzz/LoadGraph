#ifndef METRICS_API_H
#define METRICS_API_H

// Public API for reading current system metrics.
// Used by the HTTP server to build JSON responses without depending on wxWidgets.

#ifdef PLATFORM_WINDOWS
#include <windows.h>
#endif

#include <cstdint>
#include <string>
#include <vector>

struct CpuData {
    double  loadPercent  = 0.0;
    double  temperature  = -1.0;
    int     fanRpm       = -1;
    uint64_t uptimeMs    = 0;
    std::wstring cpuName;
    double  powerWatts   = -1.0;
    double  tdpWatts     = -1.0;
};

struct RamData {
    uint64_t totalBytes  = 0;
    uint64_t freeBytes   = 0;
    double   loadPercent = 0.0;
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

#endif // METRICS_API_H
