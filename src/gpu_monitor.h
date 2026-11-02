#ifndef GPU_MONITOR_H
#define GPU_MONITOR_H

#include "metrics_collector.h"
#include <windows.h>
#include <string>
#include <vector>

// Structure holding metrics for a single GPU.
struct GpuMetrics
{
    std::string name;            // GPU model name
    double      loadPercent     = 0.0;   // GPU usage percentage [0.0, 100.0]
    double      gpuFreqMhz      = 0.0;   // Current GPU core frequency in MHz
    ULONGLONG   vramTotalBytes  = 0;     // Total VRAM
    ULONGLONG   vramUsedBytes   = 0;     // Used VRAM
    double      vramLoadPercent = 0.0;   // VRAM usage percentage
    double      vramFreqMhz     = 0.0;   // Current VRAM frequency in MHz
    double      temperature     = -1.0;  // GPU temp in Celsius, -1.0 means N/A
    int         fanRpm          = -1;    // Fan speed in RPM, -1 means N/A
    double      fanPercent      = -1.0;  // Fan speed in percent [0,100], -1.0 = N/A
};

// Collects GPU metrics for all Nvidia GPUs.
// Primary source: NVML (nvml.dll, direct exports).
// Fallback: NVAPI (nvapi64.dll, resolved via nvapi_QueryInterface).
// Dynamically loads both libraries — falls back to empty list if unavailable.
class GpuMonitor : public MetricsCollector
{
public:
    GpuMonitor();
    ~GpuMonitor() override;

    // Get current GPU metrics (thread-safe copy).
    std::vector<GpuMetrics> GetMetrics();

    // Returns true if NVAPI was successfully loaded and at least one GPU found.
    bool IsAvailable() const;

protected:
    void Collect() override;

private:
    // NVML function pointer types (C ABI, exported directly from nvml.dll).
    using FnNvmlInit                    = int (__cdecl*)();
    using FnNvmlShutdown                = int (__cdecl*)();
    using FnNvmlDeviceGetCount          = int (__cdecl*)(unsigned int*);
    using FnNvmlDeviceGetHandleByIndex  = int (__cdecl*)(unsigned int, void**);
    using FnNvmlDeviceGetName           = int (__cdecl*)(void*, char*, unsigned int);
    using FnNvmlDeviceGetUtilizationRates = int (__cdecl*)(void*, void*);
    using FnNvmlDeviceGetMemoryInfo     = int (__cdecl*)(void*, void*);
    using FnNvmlDeviceGetTemperature    = int (__cdecl*)(void*, unsigned int, unsigned int*);
    using FnNvmlDeviceGetClockInfo      = int (__cdecl*)(void*, unsigned int, unsigned int*);
    using FnNvmlDeviceGetFanSpeed       = int (__cdecl*)(void*, unsigned int*);

    // NVAPI function pointer types.
    using FnNvAPI_Initialize           = int (__cdecl*)();
    using FnNvAPI_EnumPhysicalGpus     = int (__cdecl*)(void**, unsigned int*);
    using FnNvAPI_GPU_GetFullName      = int (__cdecl*)(void*, char*, unsigned int);
    using FnNvAPI_GPU_GetDynamicPstatesInfoEx = int (__cdecl*)(void*, void*);
    using FnNvAPI_GPU_GetMemoryInfo    = int (__cdecl*)(void*, void*);
    using FnNvAPI_GPU_GetThermalSettings = int (__cdecl*)(void*, unsigned int, void*);
    using FnNvAPI_GPU_GetAllClockFrequencies = int (__cdecl*)(void*, void*);
    using FnNvAPI_Unload               = int (__cdecl*)();

    HMODULE                            m_hNvApi  = nullptr;
    HMODULE                            m_hNvml   = nullptr;
    FnNvmlInit                         m_fnNvmlInit          = nullptr;
    FnNvmlShutdown                     m_fnNvmlShutdown      = nullptr;
    FnNvmlDeviceGetCount               m_fnNvmlGetCount      = nullptr;
    FnNvmlDeviceGetHandleByIndex       m_fnNvmlGetHandle     = nullptr;
    FnNvmlDeviceGetName                m_fnNvmlGetName       = nullptr;
    FnNvmlDeviceGetUtilizationRates    m_fnNvmlGetUsage      = nullptr;
    FnNvmlDeviceGetMemoryInfo          m_fnNvmlGetMemory     = nullptr;
    FnNvmlDeviceGetTemperature         m_fnNvmlGetTemp       = nullptr;
    FnNvmlDeviceGetClockInfo           m_fnNvmlGetClock      = nullptr;
    FnNvmlDeviceGetFanSpeed            m_fnNvmlGetFan        = nullptr;
    FnNvAPI_Initialize                 m_fnInitialize       = nullptr;
    FnNvAPI_EnumPhysicalGpus           m_fnEnumPhysicalGpus = nullptr;
    FnNvAPI_GPU_GetFullName            m_fnGetFullName      = nullptr;
    FnNvAPI_GPU_GetDynamicPstatesInfoEx m_fnGetDynamicPstatesInfoEx = nullptr;
    FnNvAPI_GPU_GetMemoryInfo          m_fnGetMemoryInfo    = nullptr;
    FnNvAPI_GPU_GetThermalSettings     m_fnGetThermalSettings = nullptr;
    FnNvAPI_GPU_GetAllClockFrequencies m_fnGetAllClockFrequencies = nullptr;
    FnNvAPI_Unload                     m_fnUnload           = nullptr;

    std::vector<GpuMetrics> m_metrics;
    bool                    m_available    = false;  // NVAPI available
    bool                    m_nvvmAvailable = false; // NVML available (primary source)

    bool LoadNvml();
    void FreeNvml();
    void CollectNvml();

    bool LoadNvApi();
    void FreeNvApi();
    void CollectNvApi();
};

#endif // GPU_MONITOR_H
