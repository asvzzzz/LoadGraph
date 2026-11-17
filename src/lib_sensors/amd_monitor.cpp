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

// ADLAdapterInfo - contains adapter name and hardware info.
// Layout from https://gpuopen-librariesandsdks.github.io/adl/structAdapterInfo.html
struct ADLAdapterInfo {
    int iSize;                   // Size of structure
    int iAdapterIndex;           // ADL index handle
    char strUDID[256];          // Unique device ID
    int iBusNumber;             // BUS number
    int iDeviceNumber;          // Driver number
    int iFunctionNumber;        // Function number
    int iVendorID;              // Vendor ID (0x1002 for AMD)
    char strAdapterName[256];   // Adapter name (e.g., "Radeon RX 6800")
    char strDisplayName[256];   // Display name (e.g., "\\.\Display1")
    int iPresent;               // 1 if present
    int iExist;                 // 1 if exists
    char strDriverPath[256];   // Driver registry path
    char strDriverPathExt[256];// Extended driver path
    char strPNPString[256];    // PNP string from Windows
    int iOSDisplayIndex;       // iOS display index
};

// ADLMemoryInfo - VRAM information (simplified).
struct ADLMemoryInfo {
    int iSize;
    int iVendorId;
    int iDeviceId;
    // VRAM in KB
    unsigned int uVramSize;
    unsigned int uVramFree;
    // VRAM used = vramSize - vramFree
};

// ADLClocksInfo - current clock frequencies.
struct ADLClocksInfo {
    int iSize;
    // Clock speeds in MHz
    unsigned int uEngineClock;
    unsigned int uMemoryClock;
};

// ADLPowerInformation - power metrics (simplified).
// Units: Power in 0.001W (milliwatts), Energy in 0.01mWh
struct ADLPowerInformation {
    int iSize;
    int iPowerControlType;      // Type of power control
    unsigned int uCurrentPower;     // Current power in milliwatts
    unsigned int uAveragePower;     // Average power
    unsigned int uMaxPower;         // Max/power limit in milliwatts
    unsigned int uMinPower;         // Min power
    unsigned int uSampleTime;       // Sample time in seconds
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
    m_fnGetMemoryInfo = reinterpret_cast<FnADL_Overdrive6_GetMemoryInfo>(
        GetProcAddress(m_hAdl, "ADL_Overdrive6_GetMemoryInfo"));
    m_fnGetCurrentClocks = reinterpret_cast<FnADL_Overdrive6_GetCurrentClocks>(
        GetProcAddress(m_hAdl, "ADL_Overdrive6_GetCurrentClocks"));
    m_fnGetPowerInfo = reinterpret_cast<FnADL_Overdrive6_GetPowerControlInformation>(
        GetProcAddress(m_hAdl, "ADL_Overdrive6_GetPowerControlInformation"));
    
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
    m_fnGetMemoryInfo = nullptr;
    m_fnGetCurrentClocks = nullptr;
    m_fnGetPowerInfo = nullptr;
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
        ADLAdapterInfo adapterInfo{};
        adapterInfo.iSize = sizeof(ADLAdapterInfo);
        if (m_fnGetAdapterInfo(m_hAdl, i, &adapterInfo) == ADL_OK) {
            // Use strAdapterName if available, otherwise fallback.
            if (adapterInfo.strAdapterName[0] != '\0') {
                gm.name = adapterInfo.strAdapterName;
            } else {
                snprintf(adapterInfo.strAdapterName, sizeof(adapterInfo.strAdapterName),
                        "AMD Radeon GPU %d", i);
                gm.name = adapterInfo.strAdapterName;
            }
            LOGI("AMD: Adapter %d: %s (VendorID=0x%X)", 
                 i, gm.name.c_str(), adapterInfo.iVendorID);
        } else {
            gm.name = "AMD Radeon GPU " + std::to_string(i);
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
        
        // Get VRAM info.
        ADLMemoryInfo memInfo{};
        memInfo.iSize = sizeof(ADLMemoryInfo);
        if (m_fnGetMemoryInfo && m_fnGetMemoryInfo(m_hAdl, i, &memInfo) == ADL_OK) {
            // Convert KB to bytes.
            gm.vramTotalBytes = static_cast<uint64_t>(memInfo.uVramSize) * 1024ULL;
            gm.vramUsedBytes = static_cast<uint64_t>(memInfo.uVramSize - memInfo.uVramFree) * 1024ULL;
            if (gm.vramTotalBytes > 0) {
                gm.vramLoadPercent = static_cast<double>(gm.vramUsedBytes) / 
                                   static_cast<double>(gm.vramTotalBytes) * 100.0;
            }
            LOGI("AMD: GPU %d VRAM: total=%u KB, free=%u KB", 
                 i, memInfo.uVramSize, memInfo.uVramFree);
        }
        
        // Get current clocks.
        ADLClocksInfo clocks{};
        clocks.iSize = sizeof(ADLClocksInfo);
        if (m_fnGetCurrentClocks && m_fnGetCurrentClocks(m_hAdl, i, &clocks) == ADL_OK) {
            gm.gpuFreqMhz = static_cast<double>(clocks.uEngineClock);
            gm.vramFreqMhz = static_cast<double>(clocks.uMemoryClock);
        }
        
        // Get power information.
        if (m_fnGetPowerInfo) {
            ADLPowerInformation powerInfo{};
            powerInfo.iSize = sizeof(ADLPowerInformation);
            powerInfo.iPowerControlType = 0;  // Request current power
            if (m_fnGetPowerInfo(m_hAdl, i, 0, &powerInfo) == ADL_OK) {
                // Convert milliwatts to watts.
                gm.powerWatts = static_cast<double>(powerInfo.uCurrentPower) / 1000.0;
                if (powerInfo.uMaxPower > 0) {
                    gm.powerLimitWatts = static_cast<double>(powerInfo.uMaxPower) / 1000.0;
                }
                LOGI("AMD: GPU %d Power: current=%.1fW, limit=%.1fW",
                     i, gm.powerWatts, gm.powerLimitWatts);
            }
        }
        
        out.push_back(gm);
    }
    
    LOGI("AMD: Collected metrics for %d adapter(s).", static_cast<int>(out.size()));
}
#endif

// ============================================================================
// Linux implementation using amd-smi command-line tool.
// ============================================================================
#ifdef PLATFORM_LINUX

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <sstream>
#include <algorithm>
#include <fstream>

bool AmdMonitor::LoadAmdSmi()
{
    // Check if amd-smi is available.
    const char* cmd = "which amd-smi";
    int result = system(cmd);
    if (result != 0) {
        LOGI("AMD: amd-smi not found, checking for rocm-smi...");
        // Fallback to rocm-smi if available.
        const char* cmd2 = "which rocm-smi";
        result = system(cmd2);
        if (result != 0) {
            LOGI("AMD: Neither amd-smi nor rocm-smi found.");
            return false;
        }
    }
    
    // Test if we can get GPU count.
    FILE* pipe = popen("amd-smi query --gpu-count 2>/dev/null || rocm-smi --query-gpu=count --format=json 2>/dev/null", "r");
    if (!pipe) {
        LOGI("AMD: Failed to open pipe to amd-smi.");
        return false;
    }
    
    char buffer[256] = {};
    std::string result_str;
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        result_str += buffer;
    }
    pclose(pipe);
    
    // Parse GPU count.
    int gpuCount = 0;
    try {
        gpuCount = std::stoi(result_str);
    } catch (...) {
        LOGI("AMD: Failed to parse GPU count from amd-smi.");
        return false;
    }
    
    if (gpuCount <= 0) {
        LOGI("AMD: No AMD GPUs found.");
        return false;
    }
    
    LOGI("AMD: Found %d AMD GPU(s) via amd-smi.", gpuCount);
    m_available = true;
    return true;
}

void AmdMonitor::FreeAmdSmi()
{
    m_available = false;
}

static std::string ExecAmdSmi(const std::string& query)
{
    std::string cmd = "amd-smi " + query + " 2>/dev/null || rocm-smi " + query + " 2>/dev/null";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "";
    
    char buffer[1024] = {};
    std::string result;
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        result += buffer;
    }
    pclose(pipe);
    
    // Trim whitespace.
    result.erase(0, result.find_first_not_of(" \t\n\r"));
    result.erase(result.find_last_not_of(" \t\n\r") + 1);
    return result;
}

static std::vector<std::string> SplitLines(const std::string& text)
{
    std::vector<std::string> lines;
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty()) lines.push_back(line);
    }
    return lines;
}

void AmdMonitor::CollectAmdSmi(std::vector<GpuMetrics>& out)
{
    if (!m_available) return;
    
    // Query GPU count.
    std::string countStr = ExecAmdSmi("query --gpu-count");
    int gpuCount = 0;
    try {
        gpuCount = std::stoi(countStr);
    } catch (...) {
        LOGI("AMD: Failed to parse GPU count.");
        return;
    }
    
    out.clear();
    out.reserve(gpuCount);
    
    for (int i = 0; i < gpuCount; ++i) {
        GpuMetrics gm{};
        
        // Get GPU name.
        std::string nameCmd = "query --gpu=" + std::to_string(i) + " --property=name";
        gm.name = ExecAmdSmi(nameCmd);
        if (gm.name.empty()) {
            gm.name = "AMD Radeon GPU " + std::to_string(i);
        }
        
        // Get GPU load.
        std::string loadStr = ExecAmdSmi("query --gpu=" + std::to_string(i) + " --property=percentage.gpu");
        try {
            gm.loadPercent = std::stod(loadStr);
        } catch (...) {
            gm.loadPercent = 0.0;
        }
        
        // Get temperature.
        std::string tempStr = ExecAmdSmi("query --gpu=" + std::to_string(i) + " --property=temperature.gpu");
        try {
            gm.temperature = std::stod(tempStr);
        } catch (...) {
            gm.temperature = -1.0;
        }
        
        // Get fan speed (RPM).
        std::string fanStr = ExecAmdSmi("query --gpu=" + std::to_string(i) + " --property=fan.speed");
        try {
            int fanRpm = std::stoi(fanStr);
            if (fanRpm > 0) {
                gm.fanRpm = fanRpm;
            }
        } catch (...) {
            // Try percentage.
            std::string fanPctStr = ExecAmdSmi("query --gpu=" + std::to_string(i) + " --property=fan.speed.percentage");
            try {
                gm.fanPercent = std::stod(fanPctStr);
            } catch (...) {}
        }
        
        // Get power.
        std::string powerStr = ExecAmdSmi("query --gpu=" + std::to_string(i) + " --property=power.average");
        try {
            gm.powerWatts = std::stod(powerStr);
        } catch (...) {
            gm.powerWatts = -1.0;
        }
        
        // Get VRAM.
        std::string vramTotalStr = ExecAmdSmi("query --gpu=" + std::to_string(i) + " --property=memory.total");
        std::string vramUsedStr = ExecAmdSmi("query --gpu=" + std::to_string(i) + " --property=memory.used");
        try {
            // Values are in KB, convert to bytes.
            gm.vramTotalBytes = static_cast<uint64_t>(std::stoull(vramTotalStr)) * 1024ULL;
            gm.vramUsedBytes = static_cast<uint64_t>(std::stoull(vramUsedStr)) * 1024ULL;
            if (gm.vramTotalBytes > 0) {
                gm.vramLoadPercent = static_cast<double>(gm.vramUsedBytes) / 
                                   static_cast<double>(gm.vramTotalBytes) * 100.0;
            }
        } catch (...) {
            LOGI("AMD: Failed to parse VRAM for GPU %d.", i);
        }
        
        // Get clocks.
        std::string clkStr = ExecAmdSmi("query --gpu=" + std::to_string(i) + " --property=clock.throttle.current");
        // Parse clock throttle data (format: "0 1 2 ...") - get the current active clock.
        auto parts = SplitLines(clkStr);
        if (!parts.empty()) {
            try {
                gm.gpuFreqMhz = std::stod(parts[0]);
            } catch (...) {}
        }
        
        out.push_back(gm);
        LOGI("AMD: GPU %d: %s, load=%.1f%%, temp=%.0fC, power=%.1fW",
             i, gm.name.c_str(), gm.loadPercent, gm.temperature, gm.powerWatts);
    }
    
    LOGI("AMD: Collected metrics for %d adapter(s) via amd-smi.", static_cast<int>(out.size()));
}

#endif // PLATFORM_LINUX
