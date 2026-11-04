#ifdef PLATFORM_WINDOWS
#include <winsock2.h>
#endif
#include "gpu_monitor.h"
#include "power_table.h"
#include "logger.h"
#include <cstdio>

// NVAPI function IDs (publicly documented).
static const unsigned int NVAPI_INITIALIZE_ID                = 0x0150E828;
static const unsigned int NVAPI_ENUM_PHYSICAL_GPUS_ID        = 0xE5AC921F;
static const unsigned int NVAPI_GPU_GET_FULL_NAME_ID         = 0xCEEE8E9F;
static const unsigned int NVAPI_GPU_GET_DYNAMIC_PSTATES_EX_ID = 0x409D954A;
static const unsigned int NVAPI_GPU_GET_USAGES_ID            = 0x189A1FDF;
static const unsigned int NVAPI_GPU_GET_MEMORY_INFO_ID       = 0x04611521;
static const unsigned int NVAPI_GPU_GET_THERMAL_SETTINGS_ID  = 0xE9C4B2D4;
static const unsigned int NVAPI_GPU_GET_ALL_CLOCK_FREQS_ID   = 0x1BD69F49;
static const unsigned int NVAPI_UNLOAD_ID                    = 0xD22BDD7E;

// NvGPUUsageInfo structure for NvAPI_GPU_GetUsages.
struct NvGPUUsageInfo
{
    unsigned int version;
    unsigned int reserved[3];  // padding
    unsigned int usage;        // overall usage percentage
    unsigned int reserved2[15];
};

// NVAPI struct definitions (minimal, for dynamic loading).
#pragma pack(push, 1)

// NvMemoryInfo v2
struct NvMemoryInfo
{
    unsigned int   version;
    unsigned int   reserved[31];
    unsigned int   availableDedicatedVideoMemory;
    unsigned int   dedicatedSystemMemory;
    unsigned int   sharedSystemMemory;
    unsigned long long dedicatedVideoMemory;
};

// NvDynamicPstatesInfoEx v1
struct NvDynamicPstatesInfoEx
{
    unsigned int version;
    unsigned int flags;
    struct {
        unsigned int bIsPresent : 1;
        unsigned int reserved : 31;
    } utilization;
    struct {
        unsigned int percentage;
        unsigned int reserved1;
    } utilizationPercent[8];
    unsigned int reserved2[7];
};

// NvThermalSettings v2
struct NvThermalSettings
{
    unsigned int version;
    unsigned int count;
    struct {
        unsigned int controller;
        int          defaultTarget;
        float        currentTemperature;
        unsigned int reserved[5];
    } sensor[3];
};

// NvClockFrequencies v2
struct NvClockFrequencies
{
    unsigned int version;
    unsigned int flags;
    struct {
        unsigned int present : 1;
        unsigned int reserved : 31;
        unsigned int frequency;
    } domain[8];
};

#pragma pack(pop)

static const unsigned int NVAPI_MAX_GPUS = 64;
static const unsigned int NVAPI_CLOCK_DOMAIN_GPU = 0;
static const unsigned int NVAPI_CLOCK_DOMAIN_MEM = 4;

// NVAPI status codes
static const int NVAPI_OK = 0;

// NVML status code.
static const int NVML_SUCCESS = 0;

// NVML constants.
static const unsigned int NVML_TEMPERATURE_GPU = 0;
static const unsigned int NVML_CLOCK_GRAPHICS  = 0;
static const unsigned int NVML_CLOCK_SM        = 1;
static const unsigned int NVML_CLOCK_MEM       = 2;

// NVML utilization structure (gpu + memory busy percentage).
struct NvmlUtilization
{
    unsigned int gpu;
    unsigned int memory;
};

// NVML memory info structure (bytes).
struct NvmlMemory
{
    unsigned long long total;
    unsigned long long free;
    unsigned long long used;
};

// Type of nvapi_QueryInterface.
using FnNvAPI_QueryInterface = void* (__cdecl*)(unsigned int interfaceId);

GpuMonitor::GpuMonitor()
{
    // Try NVML first — it is the primary metrics source.
    m_nvvmAvailable = LoadNvml();
    if (m_nvvmAvailable)
    {
        LOGI("NVML loaded OK, will be used as primary GPU source.");
    }
    else
    {
        LOGI("NVML not available, falling back to NVAPI.");
    }

    // Always try NVAPI too — used for GPU names as fallback and for GPUs
    // that NVML might miss (e.g. MUX-less laptops are not a concern here).
    m_available = LoadNvApi();
    if (m_available)
        LOGI("NVAPI loaded OK, GPUs will be enumerated.");
    else
        LOGI("NVAPI load FAILED.");

    if (!m_nvvmAvailable && !m_available)
        LOGI("No GPU metrics source available.");
}

GpuMonitor::~GpuMonitor()
{
    FreeNvml();
    FreeNvApi();
}

std::vector<GpuMetrics> GpuMonitor::GetMetrics()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_metrics;
}

bool GpuMonitor::IsAvailable() const
{
    return m_nvvmAvailable || m_available;
}

void GpuMonitor::Collect()
{
    // Primary source: NVML.
    if (m_nvvmAvailable)
    {
        CollectNvml();
        return;
    }

    // Fallback: NVAPI.
    if (!m_available)
        return;

    CollectNvApi();
}

void GpuMonitor::CollectNvApi()
{
    // Enumerate GPUs.
    void* hGpus[NVAPI_MAX_GPUS] = {};
    unsigned int count = NVAPI_MAX_GPUS;
    int ret = m_fnEnumPhysicalGpus(hGpus, &count);
    if (ret != NVAPI_OK || count == 0)
        return;

    std::vector<GpuMetrics> localMetrics;
    localMetrics.reserve(count);

    for (unsigned int i = 0; i < count; ++i)
    {
        GpuMetrics meta{};

        // GPU name.
        char szName[256] = {};
        if (m_fnGetFullName(hGpus[i], szName, sizeof(szName)) == NVAPI_OK)
            meta.name = szName;
        else
            meta.name = "GPU " + std::to_string(i);

        // GPU load via dynamic pstates.
        if (m_fnGetDynamicPstatesInfoEx)
        {
            NvDynamicPstatesInfoEx pstates{};
            pstates.version = sizeof(NvDynamicPstatesInfoEx);
            if (m_fnGetDynamicPstatesInfoEx(hGpus[i], &pstates) == NVAPI_OK)
                meta.loadPercent = static_cast<double>(
                    pstates.utilizationPercent[0].percentage);
        }

        // Memory info.
        if (m_fnGetMemoryInfo)
        {
            NvMemoryInfo memInfo{};
            memInfo.version = sizeof(NvMemoryInfo);
            if (m_fnGetMemoryInfo(hGpus[i], &memInfo) == NVAPI_OK)
            {
                meta.vramTotalBytes = memInfo.dedicatedVideoMemory;
                meta.vramUsedBytes  = 0;
                meta.vramLoadPercent = 0.0;
            }
        }

        // Thermal settings.
        if (m_fnGetThermalSettings)
        {
            NvThermalSettings thermal{};
            thermal.version = sizeof(NvThermalSettings);
            if (m_fnGetThermalSettings(hGpus[i], 0 /* GPU */, &thermal) == NVAPI_OK)
            {
                for (unsigned int s = 0; s < thermal.count && s < 3; ++s)
                {
                    meta.temperature = static_cast<double>(
                        thermal.sensor[s].currentTemperature);
                    break;
                }
            }
        }

        // Clock frequencies.
        if (m_fnGetAllClockFrequencies)
        {
            NvClockFrequencies clocks{};
            clocks.version = sizeof(NvClockFrequencies);
            if (m_fnGetAllClockFrequencies(hGpus[i], &clocks) == NVAPI_OK)
            {
                if (clocks.domain[NVAPI_CLOCK_DOMAIN_GPU].present)
                    meta.gpuFreqMhz = clocks.domain[NVAPI_CLOCK_DOMAIN_GPU].frequency / 1000.0;
                if (clocks.domain[NVAPI_CLOCK_DOMAIN_MEM].present)
                    meta.vramFreqMhz = clocks.domain[NVAPI_CLOCK_DOMAIN_MEM].frequency / 1000.0;
            }
        }

        localMetrics.push_back(meta);
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_metrics = std::move(localMetrics);
    }
}

// Collect metrics from NVML for all devices.
// Runs only on the collector thread; NVML was initialized in constructor
// on the main thread — nvmlInit/nvmlShutdown are never called concurrently.
void GpuMonitor::CollectNvml()
{
    if (!m_fnNvmlGetCount || !m_fnNvmlGetHandle)
        return;

    unsigned int count = 0;
    int ret = m_fnNvmlGetCount(&count);
    if (ret != NVML_SUCCESS || count == 0)
        return;

    std::vector<GpuMetrics> localMetrics;
    localMetrics.reserve(count);

    for (unsigned int i = 0; i < count; ++i)
    {
        void* hDevice = nullptr;
        if (m_fnNvmlGetHandle(i, &hDevice) != NVML_SUCCESS || !hDevice)
            continue;

        GpuMetrics meta{};

        // GPU name.
        char szName[256] = {};
        if (m_fnNvmlGetName && m_fnNvmlGetName(hDevice, szName, sizeof(szName)) == NVML_SUCCESS)
            meta.name = szName;
        else
            meta.name = "GPU " + std::to_string(i);

        // GPU + memory controller utilization (percent busy).
        if (m_fnNvmlGetUsage)
        {
            NvmlUtilization util{};
            if (m_fnNvmlGetUsage(hDevice, &util) == NVML_SUCCESS)
                meta.loadPercent = static_cast<double>(util.gpu);
        }

        // VRAM: total / used / load percent.
        if (m_fnNvmlGetMemory)
        {
            NvmlMemory mem{};
            if (m_fnNvmlGetMemory(hDevice, &mem) == NVML_SUCCESS)
            {
                meta.vramTotalBytes = mem.used + mem.free;
                meta.vramUsedBytes  = mem.used;
                if (meta.vramTotalBytes > 0)
                {
                    meta.vramLoadPercent = static_cast<double>(mem.used) /
                                            static_cast<double>(meta.vramTotalBytes) * 100.0;
                }
            }
        }

        // GPU temperature.
        if (m_fnNvmlGetTemp)
        {
            unsigned int tempC = 0;
            if (m_fnNvmlGetTemp(hDevice, NVML_TEMPERATURE_GPU, &tempC) == NVML_SUCCESS)
                meta.temperature = static_cast<double>(tempC);
        }

        // Core clock. nvmlDeviceGetClockInfo returns MHz (not kHz).
        if (m_fnNvmlGetClock)
        {
            unsigned int mhz = 0;
            if (m_fnNvmlGetClock(hDevice, NVML_CLOCK_GRAPHICS, &mhz) == NVML_SUCCESS)
                meta.gpuFreqMhz = static_cast<double>(mhz);

            mhz = 0;
            if (m_fnNvmlGetClock(hDevice, NVML_CLOCK_MEM, &mhz) == NVML_SUCCESS)
                meta.vramFreqMhz = static_cast<double>(mhz);
        }

        // Fan speed in percent (stable NVML API does not expose RPM).
        if (m_fnNvmlGetFan)
        {
            unsigned int fanPct = 0;
            if (m_fnNvmlGetFan(hDevice, &fanPct) == NVML_SUCCESS)
                meta.fanPercent = static_cast<double>(fanPct);
        }

        // Power draw in milliwatts -> convert to watts.
        if (m_fnNvmlGetPower)
        {
            unsigned int powerMw = 0;
            if (m_fnNvmlGetPower(hDevice, &powerMw) == NVML_SUCCESS)
                meta.powerWatts = static_cast<double>(powerMw) / 1000.0;
        }

        // Power management limit in milliwatts -> convert to watts.
        if (m_fnNvmlGetPowerLimit)
        {
            unsigned int limitMw = 0;
            if (m_fnNvmlGetPowerLimit(hDevice, &limitMw) == NVML_SUCCESS)
                meta.powerLimitWatts = static_cast<double>(limitMw) / 1000.0;
        }

        // Fallback: static TDP table when NVML does not expose the limit.
        if (meta.powerLimitWatts <= 0.0)
        {
            const uint32_t tdp = PowerTable::GetGpuTdp(meta.name);
            if (tdp > 0)
                meta.powerLimitWatts = static_cast<double>(tdp);
        }

        localMetrics.push_back(meta);
    }

    if (localMetrics.empty())
        return;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_metrics = std::move(localMetrics);
    }
}

// Dynamically load nvml.dll and resolve required entry points.
bool GpuMonitor::LoadNvml()
{
    m_hNvml = LoadLibraryW(L"nvml.dll");
    if (!m_hNvml)
    {
        LOGI("nvml.dll not found ( GetLastError=%lu ).", GetLastError());
        return false;
    }
    LOGI("nvml.dll loaded: 0x%p", static_cast<void*>(m_hNvml));

    m_fnNvmlInit     = reinterpret_cast<FnNvmlInit>(GetProcAddress(m_hNvml, "nvmlInit_v2"));
    m_fnNvmlShutdown = reinterpret_cast<FnNvmlShutdown>(GetProcAddress(m_hNvml, "nvmlShutdown"));
    m_fnNvmlGetCount = reinterpret_cast<FnNvmlDeviceGetCount>(GetProcAddress(m_hNvml, "nvmlDeviceGetCount_v2"));
    m_fnNvmlGetHandle = reinterpret_cast<FnNvmlDeviceGetHandleByIndex>(GetProcAddress(m_hNvml, "nvmlDeviceGetHandleByIndex_v2"));
    m_fnNvmlGetName  = reinterpret_cast<FnNvmlDeviceGetName>(GetProcAddress(m_hNvml, "nvmlDeviceGetName"));
    m_fnNvmlGetUsage = reinterpret_cast<FnNvmlDeviceGetUtilizationRates>(GetProcAddress(m_hNvml, "nvmlDeviceGetUtilizationRates"));
    m_fnNvmlGetMemory = reinterpret_cast<FnNvmlDeviceGetMemoryInfo>(GetProcAddress(m_hNvml, "nvmlDeviceGetMemoryInfo"));
    m_fnNvmlGetTemp  = reinterpret_cast<FnNvmlDeviceGetTemperature>(GetProcAddress(m_hNvml, "nvmlDeviceGetTemperature"));
    m_fnNvmlGetClock = reinterpret_cast<FnNvmlDeviceGetClockInfo>(GetProcAddress(m_hNvml, "nvmlDeviceGetClockInfo"));
    m_fnNvmlGetFan   = reinterpret_cast<FnNvmlDeviceGetFanSpeed>(GetProcAddress(m_hNvml, "nvmlDeviceGetFanSpeed"));
    m_fnNvmlGetPower = reinterpret_cast<FnNvmlDeviceGetPowerUsage>(GetProcAddress(m_hNvml, "nvmlDeviceGetPowerUsage"));
    m_fnNvmlGetPowerLimit = reinterpret_cast<FnNvmlDeviceGetPowerLimit>(GetProcAddress(m_hNvml, "nvmlDeviceGetPowerManagementLimit"));

    LOGI("nvmlInit_v2=%p nvmlDeviceGetCount_v2=%p nvmlDeviceGetHandleByIndex_v2=%p",
         reinterpret_cast<void*>(m_fnNvmlInit),
         reinterpret_cast<void*>(m_fnNvmlGetCount),
         reinterpret_cast<void*>(m_fnNvmlGetHandle));
    LOGI("nvmlDeviceGetName=%p nvmlDeviceGetUtilizationRates=%p nvmlDeviceGetMemoryInfo=%p",
         reinterpret_cast<void*>(m_fnNvmlGetName),
         reinterpret_cast<void*>(m_fnNvmlGetUsage),
         reinterpret_cast<void*>(m_fnNvmlGetMemory));
    LOGI("nvmlDeviceGetTemperature=%p nvmlDeviceGetClockInfo=%p nvmlDeviceGetFanSpeed=%p",
         reinterpret_cast<void*>(m_fnNvmlGetTemp),
         reinterpret_cast<void*>(m_fnNvmlGetClock),
         reinterpret_cast<void*>(m_fnNvmlGetFan));
    LOGI("nvmlDeviceGetPowerUsage=%p nvmlDeviceGetPowerManagementLimit=%p",
         reinterpret_cast<void*>(m_fnNvmlGetPower),
         reinterpret_cast<void*>(m_fnNvmlGetPowerLimit));

    if (!m_fnNvmlInit || !m_fnNvmlGetCount || !m_fnNvmlGetHandle)
    {
        LOGI("Critical NVML functions not found.");
        FreeNvml();
        return false;
    }

    int ret = m_fnNvmlInit();
    LOGI("nvmlInit_v2 returned %d", ret);
    if (ret != NVML_SUCCESS)
    {
        FreeNvml();
        return false;
    }

    unsigned int count = 0;
    if (m_fnNvmlGetCount(&count) == NVML_SUCCESS)
        LOGI("NVML reports %u GPU(s).", count);

    return true;
}

// Shut down and unload nvml.dll. Safe to call multiple times.
void GpuMonitor::FreeNvml()
{
    if (m_fnNvmlShutdown)
    {
        m_fnNvmlShutdown();
        m_fnNvmlShutdown = nullptr;
    }

    if (m_hNvml)
    {
        FreeLibrary(m_hNvml);
        m_hNvml = nullptr;
    }

    m_nvvmAvailable = false;
    m_fnNvmlInit      = nullptr;
    m_fnNvmlGetCount  = nullptr;
    m_fnNvmlGetHandle = nullptr;
    m_fnNvmlGetName   = nullptr;
    m_fnNvmlGetUsage  = nullptr;
    m_fnNvmlGetMemory = nullptr;
    m_fnNvmlGetTemp   = nullptr;
    m_fnNvmlGetClock  = nullptr;
    m_fnNvmlGetFan    = nullptr;
    m_fnNvmlGetPower  = nullptr;
    m_fnNvmlGetPowerLimit = nullptr;
}

bool GpuMonitor::LoadNvApi()
{
    const wchar_t* dllNames[] = { L"nvapi64.dll", L"nvapi.dll" };

    for (const wchar_t* name : dllNames)
    {
        m_hNvApi = LoadLibraryW(name);
        if (m_hNvApi)
            break;
    }

    if (!m_hNvApi)
    {
        LOGI("nvapi64.dll not found.");
        return false;
    }
    LOGI("nvapi64.dll loaded: 0x%p", static_cast<void*>(m_hNvApi));

    // Get the single exported QueryInterface function.
    auto queryInterface = reinterpret_cast<FnNvAPI_QueryInterface>(
        GetProcAddress(m_hNvApi, "nvapi_QueryInterface"));

    if (!queryInterface)
    {
        LOGI("nvapi_QueryInterface not found.");
        return false;
    }
    LOGI("nvapi_QueryInterface = 0x%p", reinterpret_cast<void*>(queryInterface));

    // Resolve all required functions via QueryInterface.
    m_fnInitialize           = reinterpret_cast<FnNvAPI_Initialize>(
        queryInterface(NVAPI_INITIALIZE_ID));
    m_fnEnumPhysicalGpus     = reinterpret_cast<FnNvAPI_EnumPhysicalGpus>(
        queryInterface(NVAPI_ENUM_PHYSICAL_GPUS_ID));
    m_fnGetFullName          = reinterpret_cast<FnNvAPI_GPU_GetFullName>(
        queryInterface(NVAPI_GPU_GET_FULL_NAME_ID));
    m_fnGetDynamicPstatesInfoEx = reinterpret_cast<FnNvAPI_GPU_GetDynamicPstatesInfoEx>(
        queryInterface(NVAPI_GPU_GET_DYNAMIC_PSTATES_EX_ID));
    m_fnGetMemoryInfo        = reinterpret_cast<FnNvAPI_GPU_GetMemoryInfo>(
        queryInterface(NVAPI_GPU_GET_MEMORY_INFO_ID));
    m_fnGetThermalSettings   = reinterpret_cast<FnNvAPI_GPU_GetThermalSettings>(
        queryInterface(NVAPI_GPU_GET_THERMAL_SETTINGS_ID));
    m_fnGetAllClockFrequencies = reinterpret_cast<FnNvAPI_GPU_GetAllClockFrequencies>(
        queryInterface(NVAPI_GPU_GET_ALL_CLOCK_FREQS_ID));
    m_fnUnload               = reinterpret_cast<FnNvAPI_Unload>(
        queryInterface(NVAPI_UNLOAD_ID));

    // Fallback: try GetUsages if DynamicPstates not available.
    if (!m_fnGetDynamicPstatesInfoEx)
    {
        m_fnGetDynamicPstatesInfoEx = reinterpret_cast<FnNvAPI_GPU_GetDynamicPstatesInfoEx>(
            queryInterface(NVAPI_GPU_GET_USAGES_ID));
        LOGI("Tried GetUsages fallback: 0x%p",
             reinterpret_cast<void*>(m_fnGetDynamicPstatesInfoEx));
    }

    LOGI("NvAPI_Initialize       = 0x%p", reinterpret_cast<void*>(m_fnInitialize));
    LOGI("NvAPI_EnumPhysicalGpus = 0x%p", reinterpret_cast<void*>(m_fnEnumPhysicalGpus));
    LOGI("NvAPI_GPU_GetFullName  = 0x%p", reinterpret_cast<void*>(m_fnGetFullName));
    LOGI("NvAPI_GPU_GetDynamicPstatesInfoEx = 0x%p", reinterpret_cast<void*>(m_fnGetDynamicPstatesInfoEx));

    if (!m_fnInitialize || !m_fnEnumPhysicalGpus)
    {
        LOGI("Critical NVAPI functions not found.");
        return false;
    }

    // Initialize NVAPI.
    int ret = m_fnInitialize();
    LOGI("NvAPI_Initialize returned %d", ret);
    return ret == NVAPI_OK;
}

void GpuMonitor::FreeNvApi()
{
    if (m_fnUnload)
        m_fnUnload();

    if (m_hNvApi)
    {
        FreeLibrary(m_hNvApi);
        m_hNvApi = nullptr;
    }
}
