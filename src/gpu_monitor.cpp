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
// IDs from nvapi_interface.h (NVIDIA/nvapi repo, Sep 2 2026).
static const unsigned int NVAPI_GPU_GET_DYNAMIC_PSTATES_EX_ID = 0x60DED2ED;
// Note: NvAPI_GPU_GetUsages (0x189A1FDF) is not supported on modern drivers —
// GPU load is read via NvAPI_GPU_GetDynamicPstatesInfoEx instead.
static const unsigned int NVAPI_GPU_GET_MEMORY_INFO_ID       = 0x07F9B368;
static const unsigned int NVAPI_GPU_GET_THERMAL_SETTINGS_ID  = 0xE3640A56;
static const unsigned int NVAPI_GPU_GET_ALL_CLOCK_FREQS_ID   = 0xDCB616C3;
static const unsigned int NVAPI_UNLOAD_ID                    = 0xD22BDD7E;

// NVAPI struct definitions (minimal, for dynamic loading).
// Sizes and layouts taken from nvapi.h (R550+). Passing wrong sizes causes
// INVALID_ARGUMENT or stack corruption.
// NVAPI standard: version in high 16 bits, size in low 16 bits.
// (nvapi.h convention differs from what I wrote earlier — sorry.)
#define MAKE_NVAPI_VERSION(sz, ver) \
    ((static_cast<unsigned int>(sz)) | (static_cast<unsigned int>(ver) << 16))

// Match nvapi.h: #pragma pack(push,8). All fields here are NvU32 / int, so
// default alignment already matches pack(1) for these structs — but we must
// not override the driver's expectation.
#pragma pack(push, 8)

static const unsigned int NVAPI_MAX_GPU_PUBLIC_CLOCKS = 32;

// NV_DISPLAY_DRIVER_MEMORY_INFO_V1 — sizeof = 20 bytes, VER = 1.
// Official layout from nvapi_lite_common.h (NVIDIA/nvapi, Sep 2026).
// No "reserved" field exists in any version. All size fields are in KB.
struct NvMemoryInfo
{
    unsigned int version;                          // offset 0x00
    unsigned int dedicatedVideoMemory;             // offset 0x04, VRAM size in KB
    unsigned int availableDedicatedVideoMemory;    // offset 0x08, free VRAM in KB
    unsigned int systemVideoMemory;                // offset 0x0C
    unsigned int sharedSystemMemory;               // offset 0x10
};

// NV_GPU_DYNAMIC_PSTATES_INFO_EX_V1 — sizeof = 40 bytes, VER = 1.
struct NvDynamicPstatesInfoEx
{
    unsigned int version;
    unsigned int flags;
    struct {
        unsigned char  bIsPresent;
        unsigned char  percentage;
        unsigned short reserved;
    } utilization[8];
};

// NV_GPU_THERMAL_SETTINGS_V2 — sizeof = 68 bytes, VER = 2.
// Field names differ from V1: defaultMinTemp/defaultMaxTemp/currentTemp (not
// defaultTarget/currentTemperature).
struct NvThermalSettings
{
    unsigned int version;
    unsigned int count;
    struct {
        unsigned int controller;
        int          defaultMinTemp;
        int          defaultMaxTemp;
        int          currentTemp;
        unsigned int target;
    } sensor[3];
};

// NV_GPU_CLOCK_FREQUENCIES_V1 — sizeof = 264 bytes, VER = 1.
// domain[NVAPI_MAX_GPU_PUBLIC_CLOCKS] where the constant equals 32 in nvapi.h.
struct NvClockFrequencies
{
    unsigned int version;
    unsigned int reserved;
    struct {
        unsigned int bIsPresent : 1;
        unsigned int reserved   : 31;
        unsigned int frequency;              // kHz
    } domain[NVAPI_MAX_GPU_PUBLIC_CLOCKS];
};

// Verify struct sizes match nvapi.h R550+ to prevent silent regressions.
static_assert(sizeof(NvMemoryInfo)              == 20, "NvMemoryInfo V1 size");
static_assert(sizeof(NvDynamicPstatesInfoEx)    == 40, "NvDynamicPstatesInfoEx size");
static_assert(sizeof(NvThermalSettings)         == 68, "NvThermalSettings size");
static_assert(sizeof(NvClockFrequencies)        == 264, "NvClockFrequencies V1 size");

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
            pstates.version = MAKE_NVAPI_VERSION(sizeof(pstates), 1);
            if (m_fnGetDynamicPstatesInfoEx(hGpus[i], &pstates) == NVAPI_OK &&
                pstates.utilization[0].bIsPresent)
                meta.loadPercent = static_cast<double>(
                    pstates.utilization[0].percentage);
        }

        // Memory info. NV_DISPLAY_DRIVER_MEMORY_INFO_V1 (20 bytes, VER=1).
        // All size fields are in KB — convert to bytes. For cards > 4 GB the
        // U32 wraps; NVML is the accurate source — this path is a fallback only.
        if (m_fnGetMemoryInfo)
        {
            NvMemoryInfo memInfo{};
            memInfo.version = MAKE_NVAPI_VERSION(sizeof(memInfo), 1);
            int rcMi = m_fnGetMemoryInfo(hGpus[i], &memInfo);
            LOGI("NvAPI_GPU_GetMemoryInfo gpu%u rc=%d vramKB=%u availKB=%u",
                 i, rcMi, memInfo.dedicatedVideoMemory,
                 memInfo.availableDedicatedVideoMemory);
            if (rcMi == NVAPI_OK)
            {
                // dedicatedVideoMemory and availableDedicatedVideoMemory are in KB.
                meta.vramTotalBytes  = static_cast<uint64_t>(
                    memInfo.dedicatedVideoMemory) * 1024ULL;
                const uint64_t availBytes =
                    static_cast<uint64_t>(memInfo.availableDedicatedVideoMemory) * 1024ULL;
                meta.vramUsedBytes   = (meta.vramTotalBytes >= availBytes)
                    ? (meta.vramTotalBytes - availBytes) : 0;
                meta.vramLoadPercent = (meta.vramTotalBytes > 0)
                    ? (static_cast<double>(meta.vramUsedBytes) /
                       static_cast<double>(meta.vramTotalBytes) * 100.0) : 0.0;
            }
        }

        // Thermal settings. currentTemp is int (not float).
        if (m_fnGetThermalSettings)
        {
            NvThermalSettings thermal{};
            thermal.version = MAKE_NVAPI_VERSION(sizeof(thermal), 2);
            if (m_fnGetThermalSettings(hGpus[i], 0 /* GPU */, &thermal) == NVAPI_OK &&
                thermal.count > 0)
                meta.temperature = static_cast<double>(thermal.sensor[0].currentTemp);
        }

        // Clock frequencies. bIsPresent is a bit-field, frequency is 31-bit.
        // domain[NVAPI_MAX_GPU_PUBLIC_CLOCKS] where the constant equals 32.
        if (m_fnGetAllClockFrequencies)
        {
            NvClockFrequencies clocks{};
            clocks.version = MAKE_NVAPI_VERSION(sizeof(clocks), 1);
            int rcClk = m_fnGetAllClockFrequencies(hGpus[i], &clocks);
            LOGI("NvAPI_GPU_GetAllClockFrequencies gpu%u rc=%d", i, rcClk);
            if (rcClk == NVAPI_OK)
            {
                if (clocks.domain[NVAPI_CLOCK_DOMAIN_GPU].bIsPresent)
                    meta.gpuFreqMhz =
                        clocks.domain[NVAPI_CLOCK_DOMAIN_GPU].frequency / 1000.0;
                if (clocks.domain[NVAPI_CLOCK_DOMAIN_MEM].bIsPresent)
                    meta.vramFreqMhz =
                        clocks.domain[NVAPI_CLOCK_DOMAIN_MEM].frequency / 1000.0;
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

    // No fallback for GetDynamicPstatesInfoEx: NVAPI_GPU_GetUsages returns a
    // completely different struct (NvGPUUsageInfo), so casting its function
    // pointer to FnNvAPI_GPU_GetDynamicPstatesInfoEx is an ABI mismatch that
    // would read garbage or crash. If the primary interface is unavailable,
    // we simply skip load reporting for this GPU.

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
    m_initialized = (ret == NVAPI_OK);
    return m_initialized;
}

void GpuMonitor::FreeNvApi()
{
    // Only call NvAPI_Unload if Initialize succeeded — calling Unload on an
    // uninitialised handle is undefined and may crash in newer driver versions.
    if (m_fnUnload && m_initialized)
        m_fnUnload();

    if (m_hNvApi)
    {
        FreeLibrary(m_hNvApi);
        m_hNvApi = nullptr;
    }
    m_initialized = false;
}
