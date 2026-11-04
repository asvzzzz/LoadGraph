#ifndef GPU_MONITOR_H
#define GPU_MONITOR_H

#include "metrics_collector.h"
#include "platform.h"

#ifdef PLATFORM_WINDOWS
#include <windows.h>
#endif

#include <cstdint>
#include <string>
#include <vector>

// Structure holding metrics for a single GPU.
struct GpuMetrics
{
    std::string name;            // GPU model name
    double      loadPercent     = 0.0;   // GPU usage percentage [0.0, 100.0]
    double      gpuFreqMhz      = 0.0;   // Current GPU core frequency in MHz
    uint64_t    vramTotalBytes  = 0;     // Total VRAM
    uint64_t    vramUsedBytes   = 0;     // Used VRAM
    double      vramLoadPercent = 0.0;   // VRAM usage percentage
    double      vramFreqMhz     = 0.0;   // Current VRAM frequency in MHz
    double      temperature     = -1.0;  // GPU temp in Celsius, -1.0 means N/A
    int         fanRpm          = -1;    // Fan speed in RPM, -1 means N/A
    double      fanPercent      = -1.0;  // Fan speed in percent [0,100], -1.0 = N/A
    double      powerWatts      = -1.0;  // Current GPU power draw in watts, -1.0 = N/A
    double      powerLimitWatts = -1.0;  // Maximum GPU power limit in watts, -1.0 = N/A
};

// Collects GPU metrics for all Nvidia GPUs.
// Primary source: NVML (nvml.dll on Windows, libnvidia-ml.so on Linux).
// Fallback: NVAPI (nvapi64.dll on Windows) — Windows only.
// Dynamically loads the library — falls back to empty list if unavailable.
class GpuMonitor : public MetricsCollector
{
public:
    GpuMonitor();
    ~GpuMonitor() override;

    // Get current GPU metrics (thread-safe copy).
    std::vector<GpuMetrics> GetMetrics();

    // Returns true if NVML/NVAPI was successfully loaded and at least one GPU found.
    bool IsAvailable() const;

protected:
    void Collect() override;

#ifdef PLATFORM_WINDOWS
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
    using FnNvmlDeviceGetPowerUsage     = int (__cdecl*)(void*, unsigned int*);
    using FnNvmlDeviceGetPowerLimit     = int (__cdecl*)(void*, unsigned int*);

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
    FnNvmlDeviceGetPowerUsage          m_fnNvmlGetPower      = nullptr;
    FnNvmlDeviceGetPowerLimit          m_fnNvmlGetPowerLimit = nullptr;
    FnNvAPI_Initialize                 m_fnInitialize       = nullptr;
    FnNvAPI_EnumPhysicalGpus           m_fnEnumPhysicalGpus = nullptr;
    FnNvAPI_GPU_GetFullName            m_fnGetFullName      = nullptr;
    FnNvAPI_GPU_GetDynamicPstatesInfoEx m_fnGetDynamicPstatesInfoEx = nullptr;
    FnNvAPI_GPU_GetMemoryInfo          m_fnGetMemoryInfo    = nullptr;
    FnNvAPI_GPU_GetThermalSettings     m_fnGetThermalSettings = nullptr;
    FnNvAPI_GPU_GetAllClockFrequencies m_fnGetAllClockFrequencies = nullptr;
    FnNvAPI_Unload                     m_fnUnload           = nullptr;

    bool LoadNvml();
    void FreeNvml();
    void CollectNvml();

    bool LoadNvApi();
    void FreeNvApi();
    void CollectNvApi();
#else
private:
    // Linux: only NVML via dlopen (no NVAPI).
    using FnInit            = int (*)();
    using FnShutdown        = int (*)();
    using FnGetCount        = int (*)(unsigned int*);
    using FnGetHandle       = int (*)(unsigned int, void**);
    using FnGetName         = int (*)(void*, char*, unsigned int);
    using FnGetUsage        = int (*)(void*, void*);
    using FnGetMemory       = int (*)(void*, void*);
    using FnGetTemp         = int (*)(void*, unsigned int, unsigned int*);
    using FnGetClock        = int (*)(void*, unsigned int, unsigned int*);
    using FnGetFan          = int (*)(void*, unsigned int*);
    using FnGetPower        = int (*)(void*, unsigned int*);
    using FnGetPowerLimit   = int (*)(void*, unsigned int*);

    void*  m_hNvml   = nullptr;
    FnInit       m_fnInit          = nullptr;
    FnShutdown   m_fnShutdown      = nullptr;
    FnGetCount   m_fnGetCount      = nullptr;
    FnGetHandle  m_fnGetHandle     = nullptr;
    FnGetName    m_fnGetName       = nullptr;
    FnGetUsage   m_fnGetUsage      = nullptr;
    FnGetMemory  m_fnGetMemory     = nullptr;
    FnGetTemp    m_fnGetTemp       = nullptr;
    FnGetClock   m_fnGetClock      = nullptr;
    FnGetFan     m_fnGetFan        = nullptr;
    FnGetPower   m_fnGetPower      = nullptr;
    FnGetPowerLimit m_fnGetPowerLimit = nullptr;

    bool LoadNvml();
    void FreeNvml();
    void CollectNvml();
#endif

    std::vector<GpuMetrics> m_metrics;
    bool                    m_available     = false; // NVAPI available (Win) or last probe result
    bool                    m_nvvmAvailable = false; // NVML available
};

#endif // GPU_MONITOR_H
