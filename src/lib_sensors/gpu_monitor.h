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
    int         fanRpm          = -1;    // Fan speed in RPM, -1 means N/A (0 = stopped)
    double      fanPercent      = -1.0;  // Fan speed in percent [0,100], -1.0 = N/A
    // Upper tachometer bound of this card reported by the driver. Only AMD/ADL
    // exposes it (ADLFanSpeedInfo.iMaxRPM); NVIDIA has no RPM maximum in NVML
    // or NVAPI, so this stays -1 there and the charts use the shared 3000 RPM
    // scale. hwmon fanN_max is deliberately not used: it is an alarm threshold,
    // not a rated fan speed.
    int         fanMaxRpm       = -1;    // Maximum fan speed in RPM, -1 means N/A
    double      powerWatts      = -1.0;  // Current GPU power draw in watts, -1.0 = N/A
    double      powerLimitWatts = -1.0;  // Maximum GPU power limit in watts, -1.0 = N/A
};

class AmdMonitor; // Forward declaration, defined in amd_monitor.h

// Collects GPU metrics for all GPUs (NVIDIA via NVML/NVAPI, AMD via ADL).
// Primary source: NVML (nvml.dll on Windows, libnvidia-ml.so on Linux).
// Fallback: NVAPI (nvapi64.dll on Windows) - Windows only.
// AMD support: ADL (atiadlxx.dll on Windows).
// Dynamically loads libraries - falls back to empty list if unavailable.
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
    // nvmlDeviceGetFanSpeedRPM, added in NVML 12.4 (driver branch R550 and
    // newer): the argument is nvmlFanSpeedInfo_v1 (fan index in, RPM out).
    // Missing on older drivers, so the pointer is optional.
    using FnNvmlDeviceGetFanSpeedRPM    = int (__cdecl*)(void*, void*);
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
    // NvAPI_GPU_GetTachReading: current fan tachometer in RPM. Only source of
    // NVIDIA RPM on drivers whose NVML is too old for nvmlDeviceGetFanSpeedRPM.
    using FnNvAPI_GPU_GetTachReading   = int (__cdecl*)(void*, unsigned int*);
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
    FnNvmlDeviceGetFanSpeedRPM         m_fnNvmlGetFanRpm     = nullptr;
    FnNvmlDeviceGetPowerUsage          m_fnNvmlGetPower      = nullptr;
    FnNvmlDeviceGetPowerLimit          m_fnNvmlGetPowerLimit = nullptr;
    FnNvAPI_Initialize                 m_fnInitialize       = nullptr;
    FnNvAPI_EnumPhysicalGpus           m_fnEnumPhysicalGpus = nullptr;
    FnNvAPI_GPU_GetFullName            m_fnGetFullName      = nullptr;
    FnNvAPI_GPU_GetDynamicPstatesInfoEx m_fnGetDynamicPstatesInfoEx = nullptr;
    FnNvAPI_GPU_GetMemoryInfo          m_fnGetMemoryInfo    = nullptr;
    FnNvAPI_GPU_GetThermalSettings     m_fnGetThermalSettings = nullptr;
    FnNvAPI_GPU_GetAllClockFrequencies m_fnGetAllClockFrequencies = nullptr;
    FnNvAPI_GPU_GetTachReading         m_fnGetTachReading    = nullptr;
    FnNvAPI_Unload                     m_fnUnload           = nullptr;
    bool                               m_initialized       = false;  // true only after successful NVAPI_Initialize.
    // One-shot NVAPI diagnostics: return codes of individual calls are printed
    // only in the first collection cycle. The flag is written and read only by
    // the collector thread (Collect calls CollectNvApi in sequence), no mutex needed.
    bool                               m_nvapiDiagLogged   = false;

    bool LoadNvml();
    void FreeNvml();
    /// NVML metrics for all GPUs: the result is appended to out. The list is
    /// published (replaced as a whole) only in Collect().
    void CollectNvml(std::vector<GpuMetrics>& out);

    bool LoadNvApi();
    void FreeNvApi();
    /// NVAPI metrics for all GPUs: the result is appended to out.
    void CollectNvApi(std::vector<GpuMetrics>& out);
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
    // nvmlDeviceGetFanSpeedRPM (NVML 12.4+, optional symbol), see the Windows
    // declaration: the argument is nvmlFanSpeedInfo_v1.
    using FnGetFanRpm       = int (*)(void*, void*);
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
    FnGetFanRpm  m_fnGetFanRpm     = nullptr;
    FnGetPower   m_fnGetPower      = nullptr;
    FnGetPowerLimit m_fnGetPowerLimit = nullptr;

    bool LoadNvml();
    void FreeNvml();
    /// NVML metrics for all GPUs: the result is appended to out. The list is
    /// published (replaced as a whole) only in Collect().
    void CollectNvml(std::vector<GpuMetrics>& out);
#endif

    std::vector<GpuMetrics> m_metrics;
    bool                    m_available     = false; // NVAPI available (Win) or last probe result
    bool                    m_nvvmAvailable = false; // NVML available

    // AMD GPU monitor - lifetime managed with GpuMonitor to avoid repeated
    // ADL/driver handle init on every poll.
    AmdMonitor* m_amdMonitor = nullptr;
};

#endif // GPU_MONITOR_H
