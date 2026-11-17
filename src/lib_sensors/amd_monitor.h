#ifndef AMD_MONITOR_H
#define AMD_MONITOR_H

#include "gpu_monitor.h"
#include "platform.h"

#ifdef PLATFORM_WINDOWS
#include <windows.h>
#endif

#include <string>
#include <vector>

// Forward declaration - will be defined in amd_monitor.cpp
struct AdlContext;

// AMD GPU monitor — supports Radeon cards via ADL (Windows) and AMD SMI (Linux).
// Works alongside NVML for mixed NVIDIA/AMD systems.
class AmdMonitor
{
public:
    AmdMonitor();
    ~AmdMonitor();

    // Get current GPU metrics for all AMD GPUs.
    // Returns empty vector if no AMD GPUs found or library unavailable.
    std::vector<GpuMetrics> GetMetrics();

    // Returns true if ADL/AMD SMI was successfully loaded and at least one GPU found.
    bool IsAvailable() const;

private:
#ifdef PLATFORM_WINDOWS
    bool LoadAdl();
    void FreeAdl();
    void CollectAdl(std::vector<GpuMetrics>& out);

    // ADL function pointers.
    using FnADL_Main_Control_Create = int (*)(void** ppAdlContext, int iEnumDevicesVersion);
    using FnADL_Main_Control_Destroy = int (*)(void* pAdlContext);
    using FnADL_Overdrive6_GetCurrentActivity = int (*)(void* pAdlContext, int iAdapterIndex, void* lpActivity);
    using FnADL_Overdrive6_GetTemperature = int (*)(void* pAdlContext, int iAdapterIndex, int iSensorType, float* pfTemp);
    using FnADL_Overdrive6_GetFanSpeedInfo = int (*)(void* pAdlContext, int iAdapterIndex, void* lpFanSpeedInfo);
    using FnADL_Overdrive6_GetFanSpeed = int (*)(void* pAdlContext, int iAdapterIndex, void* lpFanSpeed);
    using FnADL_Overdrive6_SetFanSpeed = int (*)(void* pAdlContext, int iAdapterIndex, void* lpFanSpeed);
    using FnADL_Adapter_NumberOfAdapters = int (*)(int* adapterCount);
    using FnADL_Adapter_GetAdapterInfo = int (*)(void* pAdlContext, int iAdapterIndex, void* lpInfo);
    using FnADL_Overdrive6_GetPowerControlInformation = int (*)(void* pAdlContext, int iAdapterIndex, int iPowerControlType, void* lpPowerControlInformation);

    HMODULE m_hAdl = nullptr;
    FnADL_Main_Control_Create m_fnAdlCreate = nullptr;
    FnADL_Main_Control_Destroy m_fnAdlDestroy = nullptr;
    FnADL_Overdrive6_GetCurrentActivity m_fnGetActivity = nullptr;
    FnADL_Overdrive6_GetTemperature m_fnGetTemp = nullptr;
    FnADL_Overdrive6_GetFanSpeedInfo m_fnGetFanSpeedInfo = nullptr;
    FnADL_Overdrive6_GetFanSpeed m_fnGetFanSpeed = nullptr;
    FnADL_Adapter_NumberOfAdapters m_fnGetAdapterCount = nullptr;
    FnADL_Adapter_GetAdapterInfo m_fnGetAdapterInfo = nullptr;
#else
    // Linux: AMD SMI support (via amd-smi command or libamd_smi).
    bool LoadAmdSmi();
    void FreeAmdSmi();
    void CollectAmdSmi(std::vector<GpuMetrics>& out);

    // For future libamd_smi integration.
    bool m_amdSmiAvailable = false;
#endif

    std::vector<GpuMetrics> m_metrics;
    bool m_available = false;
};

#endif // AMD_MONITOR_H
