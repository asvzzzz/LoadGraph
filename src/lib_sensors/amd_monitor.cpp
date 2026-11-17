#include "amd_monitor.h"
#include "logger.h"
#include <cstdio>
#include <cstring>

#ifdef PLATFORM_WINDOWS
#include <windows.h>
#endif

// ADL error codes (from adl_defines.h).
#define ADL_OK 0

// ADL struct definitions (minimal for our needs).
struct ADLActivity {
    int iEngineBusy;      // GPU engine busy percentage
    int iMemoryBusy;      // Memory engine busy percentage
    int iVidEngineBusy;   // Video engine busy percentage
    int reserved[3];
};

struct ADLFanSpeedInfo {
    int iSize;
    int iFlags;
    int iMinPercent;
    int iMaxPercent;
    int iMinRPM;
    int iMaxRPM;
};

struct ADLFanSpeedValue {
    int iSize;
    int iType;  // 0 = percent, 1 = RPM
    int iTargetSpeed;
};

// AMD Monitor Implementation
AmdMonitor::AmdMonitor() : m_available(false)
{
#ifdef PLATFORM_WINDOWS
    LoadAdl();
#else
    // Linux: could probe for amd-smi or libamd_smi
    m_available = false;
#endif
}

AmdMonitor::~AmdMonitor()
{
#ifdef PLATFORM_WINDOWS
    FreeAdl();
#endif
}

bool AmdMonitor::IsAvailable() const
{
    return m_available;
}

std::vector<GpuMetrics> AmdMonitor::GetMetrics()
{
    std::vector<GpuMetrics> result;
#ifdef PLATFORM_WINDOWS
    if (m_available) {
        CollectAdl(result);
    }
#else
    // Linux AMD SMI support would go here
#endif
    return result;
}

#ifdef PLATFORM_WINDOWS
bool AmdMonitor::LoadAdl()
{
    // Try to load ADL DLL (AMD Display Library).
    // Common names: atiadlxx.dll (modern), atiadlfb.dll (older)
    const char* dllNames[] = { "atiadlxx.dll", "atiadlfb.dll", "amdtallib.dll" };
    
    for (const char* name : dllNames) {
        m_hAdl = LoadLibraryA(name);
        if (m_hAdl) break;
    }
    
    if (!m_hAdl) {
        LOGI("AMD: ADL library not found, skipping AMD GPU support.");
        return false;
    }
    
    // Resolve function pointers.
    m_fnAdlCreate = reinterpret_cast<FnADL_Main_Control_Create>(
        GetProcAddress(m_hAdl, "ADL_Main_Control_Create"));
    m_fnAdlDestroy = reinterpret_cast<FnADL_Main_Control_Destroy>(
        GetProcAddress(m_hAdl, "ADL_Main_Control_Destroy"));
    m_fnGetActivity = reinterpret_cast<FnADL_Overdrive6_GetCurrentActivity>(
        GetProcAddress(m_hAdl, "ADL_Overdrive6_GetCurrentActivity"));
    m_fnGetTemp = reinterpret_cast<FnADL_Overdrive6_GetTemperature>(
        GetProcAddress(m_hAdl, "ADL_Overdrive6_GetTemperature"));
    m_fnGetFanSpeedInfo = reinterpret_cast<FnADL_Overdrive6_GetFanSpeedInfo>(
        GetProcAddress(m_hAdl, "ADL_Overdrive6_GetFanSpeedInfo"));
    m_fnGetFanSpeed = reinterpret_cast<FnADL_Overdrive6_GetFanSpeed>(
        GetProcAddress(m_hAdl, "ADL_Overdrive6_GetFanSpeed"));
    m_fnGetAdapterCount = reinterpret_cast<FnADL_Adapter_NumberOfAdapters>(
        GetProcAddress(m_hAdl, "ADL_Adapter_NumberOfAdapters"));
    m_fnGetAdapterInfo = reinterpret_cast<FnADL_Adapter_GetAdapterInfo>(
        GetProcAddress(m_hAdl, "ADL_Adapter_GetAdapterInfo"));
    
    // Check if we got all required functions.
    if (!m_fnAdlCreate || !m_fnAdlDestroy || !m_fnGetActivity) {
        LOGI("AMD: ADL library loaded but missing required functions.");
        FreeAdl();
        return false;
    }
    
    // Initialize ADL.
    void* pContext = nullptr;
    int result = m_fnAdlCreate(&pContext, 1);
    if (result != ADL_OK || !pContext) {
        LOGI("AMD: ADL initialization failed (code %d).", result);
        FreeAdl();
        return false;
    }
    
    // Test with getting adapter count.
    int adapterCount = 0;
    result = m_fnGetAdapterCount(&adapterCount);
    if (result != ADL_OK || adapterCount == 0) {
        LOGI("AMD: No AMD adapters found.");
        m_fnAdlDestroy(pContext);
        FreeAdl();
        return false;
    }
    
    LOGI("AMD: ADL initialized, found %d adapter(s).", adapterCount);
    m_available = true;
    return true;
}

void AmdMonitor::FreeAdl()
{
    if (m_hAdl) {
        FreeLibrary(m_hAdl);
        m_hAdl = nullptr;
    }
    m_fnAdlCreate = nullptr;
    m_fnAdlDestroy = nullptr;
    m_fnGetActivity = nullptr;
    m_fnGetTemp = nullptr;
    m_fnGetFanSpeedInfo = nullptr;
    m_fnGetFanSpeed = nullptr;
    m_fnGetAdapterCount = nullptr;
    m_fnGetAdapterInfo = nullptr;
    m_available = false;
}

void AmdMonitor::CollectAdl(std::vector<GpuMetrics>& out)
{
    if (!m_available || !m_fnAdlCreate) return;
    
    // Get adapter count.
    int adapterCount = 0;
    if (m_fnGetAdapterCount(&adapterCount) != ADL_OK) return;
    
    out.clear();
    out.reserve(adapterCount);
    
    for (int i = 0; i < adapterCount; ++i) {
        GpuMetrics gm{};
        
        // Get adapter info (includes name).
        void* pInfo = nullptr;
        if (m_fnGetAdapterInfo(m_hAdl, i, &pInfo) == ADL_OK && pInfo) {
            // Extract name from ADLAdapterInfo structure.
            // Layout: iAdapterIndex, strAdapterName, strHardwareInfo, etc.
            // We'll use a simple approach here.
            char name[256] = {};
            snprintf(name, sizeof(name), "AMD Radeon GPU %d", i);
            gm.name = name;
        }
        
        // Get GPU utilization.
        ADLActivity activity{};
        if (m_fnGetActivity(m_hAdl, i, &activity) == ADL_OK) {
            gm.loadPercent = static_cast<double>(activity.iEngineBusy);
        }
        
        // Get temperature.
        float temp = 0.0f;
        if (m_fnGetTemp(m_hAdl, i, 0, &temp) == ADL_OK) {
            gm.temperature = static_cast<double>(temp);
        } else {
            gm.temperature = -1.0;
        }
        
        // Get fan speed info and current speed.
        ADLFanSpeedInfo fanInfo{};
        fanInfo.iSize = sizeof(ADLFanSpeedInfo);
        if (m_fnGetFanSpeedInfo(m_hAdl, i, &fanInfo) == ADL_OK) {
            ADLFanSpeedValue fanSpeed{};
            fanSpeed.iSize = sizeof(ADLFanSpeedValue);
            if (m_fnGetFanSpeed(m_hAdl, i, &fanSpeed) == ADL_OK) {
                if (fanSpeed.iType == 0) {
                    // Percentage
                    gm.fanPercent = static_cast<double>(fanSpeed.iTargetSpeed);
                } else {
                    // RPM
                    gm.fanRpm = fanSpeed.iTargetSpeed;
                }
            }
        }
        
        // Note: VRAM, power, and clock info require additional ADL calls.
        // These are left as -1/0 for now and can be added later.
        
        out.push_back(gm);
    }
    
    LOGI("AMD: Collected metrics for %d adapter(s).", static_cast<int>(out.size()));
}
#endif

// Linux implementation placeholder.
#ifdef PLATFORM_LINUX
bool AmdMonitor::LoadAmdSmi()
{
    // Check if amd-smi is available.
    const char* cmd = "which amd-smi";
    int result = system(cmd);
    if (result != 0) {
        LOGI("AMD: amd-smi not found, skipping AMD GPU support.");
        return false;
    }
    
    // Parse amd-smi output to get metrics.
    // This would involve spawning a process and parsing text output.
    // Implementation left for future enhancement.
    m_available = false;
    return false;
}

void AmdMonitor::FreeAmdSmi()
{
    m_available = false;
}

void AmdMonitor::CollectAmdSmi(std::vector<GpuMetrics>& out)
{
    // Placeholder for Linux AMD SMI implementation.
    out.clear();
}
#endif
