#include "gpu_monitor.h"
#include "platform.h"
#include "logger.h"
#include <cstdio>
#include <dlfcn.h>
#include <cstring>

#ifdef PLATFORM_LINUX

// NVML status code.
static const int NVML_SUCCESS = 0;

// NVML constants.
static const unsigned int NVML_TEMPERATURE_GPU = 0;
static const unsigned int NVML_CLOCK_GRAPHICS  = 0;
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

// ----------------------------------------------------------------------------
// GpuMonitor implementation for Linux (NVML via dlopen only, no NVAPI).
// ----------------------------------------------------------------------------

GpuMonitor::GpuMonitor()
{
    m_nvvmAvailable = LoadNvml();
    if (m_nvvmAvailable)
        LOGI("NVML loaded OK via dlopen, will be used as primary GPU source.");
    else
        LOGI("NVML not available on Linux — GPU monitoring disabled.");
}

GpuMonitor::~GpuMonitor()
{
    FreeNvml();
}

std::vector<GpuMetrics> GpuMonitor::GetMetrics()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_metrics;
}

bool GpuMonitor::IsAvailable() const
{
    return m_nvvmAvailable;
}

void GpuMonitor::Collect()
{
    if (m_nvvmAvailable)
        CollectNvml();
}

// Dynamically load libnvidia-ml.so and resolve entry points.
bool GpuMonitor::LoadNvml()
{
    // Try common library names.
    const char* names[] = {
        "libnvidia-ml.so.1",
        "libnvidia-ml.so",
        nullptr
    };

    for (int i = 0; names[i]; ++i)
    {
        m_hNvml = dlopen(names[i], RTLD_LAZY);
        if (m_hNvml)
            break;
    }

    if (!m_hNvml)
    {
        LOGI("libnvidia-ml.so not found: %s", dlerror());
        return false;
    }
    LOGI("NVML library loaded: %p", m_hNvml);

#define RESOLVE(fn) \
    m_fn##fn = reinterpret_cast<Fn##fn>(dlsym(m_hNvml, "nvml" #fn)); \
    LOGI("  nvml" #fn " = %p", (void*)m_fn##fn);

    RESOLVE(Init);
    RESOLVE(Shutdown);
    RESOLVE(GetCount);
    RESOLVE(GetHandle);
    RESOLVE(GetName);
    RESOLVE(GetUsage);
    RESOLVE(GetMemory);
    RESOLVE(GetTemp);
    RESOLVE(GetClock);
    RESOLVE(GetFan);

#undef RESOLVE

    if (!m_fnInit || !m_fnGetCount || !m_fnGetHandle)
    {
        LOGI("Critical NVML functions not found.");
        FreeNvml();
        return false;
    }

    int ret = m_fnInit();
    LOGI("nvmlInit_v2 returned %d", ret);
    if (ret != NVML_SUCCESS)
    {
        FreeNvml();
        return false;
    }

    unsigned int count = 0;
    if (m_fnGetCount(&count) == NVML_SUCCESS)
        LOGI("NVML reports %u GPU(s).", count);

    return true;
}

void GpuMonitor::FreeNvml()
{
    if (m_fnShutdown)
    {
        m_fnShutdown();
        m_fnShutdown = nullptr;
    }

    if (m_hNvml)
    {
        dlclose(m_hNvml);
        m_hNvml = nullptr;
    }

    m_nvvmAvailable = false;
    m_fnInit      = nullptr;
    m_fnShutdown  = nullptr;
    m_fnGetCount  = nullptr;
    m_fnGetHandle = nullptr;
    m_fnGetName   = nullptr;
    m_fnGetUsage  = nullptr;
    m_fnGetMemory = nullptr;
    m_fnGetTemp   = nullptr;
    m_fnGetClock  = nullptr;
    m_fnGetFan    = nullptr;
}

void GpuMonitor::CollectNvml()
{
    if (!m_fnGetCount || !m_fnGetHandle)
        return;

    unsigned int count = 0;
    int ret = m_fnGetCount(&count);
    if (ret != NVML_SUCCESS || count == 0)
        return;

    std::vector<GpuMetrics> localMetrics;
    localMetrics.reserve(count);

    for (unsigned int i = 0; i < count; ++i)
    {
        void* hDevice = nullptr;
        if (m_fnGetHandle(i, &hDevice) != NVML_SUCCESS || !hDevice)
            continue;

        GpuMetrics meta{};

        char szName[256] = {};
        if (m_fnGetName && m_fnGetName(hDevice, szName, sizeof(szName)) == NVML_SUCCESS)
            meta.name = szName;
        else
            meta.name = "GPU " + std::to_string(i);

        if (m_fnGetUsage)
        {
            NvmlUtilization util{};
            if (m_fnGetUsage(hDevice, &util) == NVML_SUCCESS)
                meta.loadPercent = static_cast<double>(util.gpu);
        }

        if (m_fnGetMemory)
        {
            NvmlMemory mem{};
            if (m_fnGetMemory(hDevice, &mem) == NVML_SUCCESS)
            {
                meta.vramTotalBytes = mem.used + mem.free;
                meta.vramUsedBytes  = mem.used;
                if (meta.vramTotalBytes > 0)
                    meta.vramLoadPercent = static_cast<double>(mem.used) /
                                           static_cast<double>(meta.vramTotalBytes) * 100.0;
            }
        }

        if (m_fnGetTemp)
        {
            unsigned int tempC = 0;
            if (m_fnGetTemp(hDevice, NVML_TEMPERATURE_GPU, &tempC) == NVML_SUCCESS)
                meta.temperature = static_cast<double>(tempC);
        }

        if (m_fnGetClock)
        {
            unsigned int mhz = 0;
            if (m_fnGetClock(hDevice, NVML_CLOCK_GRAPHICS, &mhz) == NVML_SUCCESS)
                meta.gpuFreqMhz = static_cast<double>(mhz);
            mhz = 0;
            if (m_fnGetClock(hDevice, NVML_CLOCK_MEM, &mhz) == NVML_SUCCESS)
                meta.vramFreqMhz = static_cast<double>(mhz);
        }

        if (m_fnGetFan)
        {
            unsigned int fanPct = 0;
            if (m_fnGetFan(hDevice, &fanPct) == NVML_SUCCESS)
                meta.fanPercent = static_cast<double>(fanPct);
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

#endif // PLATFORM_LINUX
