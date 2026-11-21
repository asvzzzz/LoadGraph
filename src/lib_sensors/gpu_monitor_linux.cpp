#include "gpu_monitor.h"
#include "amd_monitor.h"
#include "power_table.h"
#include "platform.h"
#include "logger.h"
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <dirent.h>
#include <map>
#include <string>
#include <vector>

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

// NVML nvmlFanSpeedInfo_v1 (nvml.h): fan index in, RPM out. The version field
// is NVML_STRUCT_VERSION(FanSpeedInfo, 1) = size | (version << 24).
struct NvmlFanSpeedInfo
{
    unsigned int version;
    unsigned int fan;
    unsigned int speed;
};

static const unsigned int NVML_FANSPEED_INFO_V1 =
    static_cast<unsigned int>(sizeof(NvmlFanSpeedInfo)) | (1u << 24);

// ----------------------------------------------------------------------------
// An NVIDIA card without the proprietary driver: libnvidia-ml is missing, but
// the device itself is visible on the bus (/sys/bus/pci/devices exists in any
// case). The list of PCI devices with vendor 0x10de of the display class 0x03
// is needed to show the model from pci.ids with N/A metrics, as is done for
// AMD cards without a driver. sysfs reads only, no external processes.
// ----------------------------------------------------------------------------

/// First line of a sysfs file without surrounding whitespace. False on error.
static bool ReadPciSysfsStr(const std::string& path, std::string& out)
{
    out.clear();
    FILE* f = fopen(path.c_str(), "r");
    if (!f)
        return false;
    char buf[128];
    const bool got = (fgets(buf, sizeof(buf), f) != nullptr);
    fclose(f);
    if (!got)
        return false;

    size_t n = strlen(buf);
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' ||
                     buf[n - 1] == ' '  || buf[n - 1] == '\t'))
        --n;
    size_t begin = 0;
    while (begin < n && (buf[begin] == ' ' || buf[begin] == '\t'))
        ++begin;
    out.assign(buf + begin, n - begin);
    return !out.empty();
}

/// Find all NVIDIA display controllers (vendor 0x10de, class 0x03xx) on the bus.
/// devIds - device ID in the "0x0a65" form, bdfs - address like 0000:04:00.0.
static void ScanNvidiaPci(std::vector<std::string>& devIds,
                          std::vector<std::string>& bdfs)
{
    devIds.clear();
    bdfs.clear();

    static const char PCI_ROOT[] = "/sys/bus/pci/devices";
    DIR* dir = opendir(PCI_ROOT);
    if (!dir)
        return;

    dirent* ent;
    while ((ent = readdir(dir)) != nullptr)
    {
        if (ent->d_name[0] == '.')
            continue;
        const std::string base = std::string(PCI_ROOT) + "/" + ent->d_name;

        std::string vendor, cls, dev;
        if (!ReadPciSysfsStr(base + "/vendor", vendor) || vendor != "0x10de")
            continue;
        if (!ReadPciSysfsStr(base + "/class", cls))
            continue;
        // class "0x030000": 0x03 - VGA/3D controllers. The HD-audio function
        // of the same card (class 0x0403) is of no interest to us.
        if (cls.size() < 6 || cls.compare(0, 4, "0x03") != 0)
            continue;
        if (!ReadPciSysfsStr(base + "/device", dev))
            continue;

        devIds.push_back(dev);
        bdfs.push_back(ent->d_name);
    }
    closedir(dir);
}

/// Add stub metrics for NVIDIA cards without a driver: name from pci.ids,
/// all values N/A (-1), the GUI hides the graphs itself on negative values.
/// pci.ids is parsed once per device ID, the log is written once per run.
static void AppendUndrivenNvidia(std::vector<GpuMetrics>& out)
{
    static std::map<std::string, std::string> s_nameCache;
    static bool s_logged = false;

    std::vector<std::string> devIds;
    std::vector<std::string> bdfs;
    ScanNvidiaPci(devIds, bdfs);

    for (size_t i = 0; i < devIds.size(); ++i)
    {
        GpuMetrics gm{};
        // Explicit N/A for load: without NVML there is no load reading, and the
        // GUI hides the graph instead of a plausible-looking but false "0%".
        gm.loadPercent = -1.0;

        std::map<std::string, std::string>::iterator it = s_nameCache.find(devIds[i]);
        if (it == s_nameCache.end())
        {
            std::string nm = AmdMonitor::ResolvePciName("0x10de", devIds[i]);
            if (nm.empty())
                nm = "NVIDIA GPU";
            it = s_nameCache.insert(std::make_pair(devIds[i], nm)).first;
        }
        gm.name = it->second;
        out.push_back(gm);
    }

    if (!s_logged && !out.empty())
    {
        s_logged = true;
        LOGI("NVIDIA: %d display device(s) on PCI but NVML is absent - all "
             "metrics N/A. Install the proprietary NVIDIA driver (nvidia-smi "
             "must work), then restart LoadGraph.", (int)out.size());
    }
}

// ----------------------------------------------------------------------------
// GpuMonitor implementation for Linux (NVML via dlopen only, no NVAPI).
// ----------------------------------------------------------------------------

GpuMonitor::GpuMonitor()
    : m_amdMonitor(nullptr)
{
    // NVML is cross-platform and IS available under WSL2/Linux: NVIDIA ships
    // /usr/lib/wsl/lib/libnvidia-ml.so.1 and nvidia-smi works there. NVAPI is
    // Windows-only (no libnvapi.so exists for Linux), so only NVML is tried.
    m_nvvmAvailable = LoadNvml();
    if (m_nvvmAvailable)
        LOGI("NVML loaded OK - GPU metrics will be collected (Linux/WSL).");
    else
        LOGI("NVML not available on this system - GPU graphs hidden.");

    // NVIDIA on the bus but without NVML (driver not installed): the GPU block
    // must stay visible - the card shows as the pci.ids model with N/A metrics.
    if (!m_nvvmAvailable)
    {
        std::vector<std::string> nvidiaIds;
        std::vector<std::string> nvidiaBdfs;
        ScanNvidiaPci(nvidiaIds, nvidiaBdfs);
        m_available = !nvidiaIds.empty();
    }

    // Initialize AMD monitor once - lifetime managed with GpuMonitor.
    m_amdMonitor = new AmdMonitor();
    if (m_amdMonitor && m_amdMonitor->IsAvailable())
        LOGI("AMD GPU monitor initialized (Linux).");
}

GpuMonitor::~GpuMonitor()
{
    delete m_amdMonitor;
    m_amdMonitor = nullptr;
    FreeNvml();
}

std::vector<GpuMetrics> GpuMonitor::GetMetrics()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_metrics;
}

bool GpuMonitor::IsAvailable() const
{
    // Availability = NVIDIA (NVML) OR AMD. Otherwise, on a system without the
    // NVIDIA driver the whole GPU block is hidden even when an amdgpu card is
    // found and read. On Linux m_available means "NVIDIA visible on PCI, but
    // NVML is unavailable" (the card is shown with N/A metrics).
    return m_nvvmAvailable || m_available ||
           (m_amdMonitor && m_amdMonitor->IsAvailable());
}

void GpuMonitor::Collect()
{
    // Metrics are collected into a local vector and published with a single
    // swap under the mutex: a reader (UI at startup, HTTP endpoint) must never
    // see a cleared list. A local vector also rules out accumulating duplicates
    // on the AMD append - a fresh list every cycle.
    std::vector<GpuMetrics> collected;

    // Primary source: NVML.
    if (m_nvvmAvailable)
        CollectNvml(collected);

    const size_t nvmlCount = collected.size();

    // Merge AMD GPU metrics (if any).
    if (m_amdMonitor && m_amdMonitor->IsAvailable())
    {
        std::vector<GpuMetrics> amdMetrics = m_amdMonitor->GetMetrics();
        collected.insert(collected.end(),
                         std::make_move_iterator(amdMetrics.begin()),
                         std::make_move_iterator(amdMetrics.end()));
    }

    // NVML returned no device at all (no driver, or it is not visible): append
    // the NVIDIA cards from the bus as stubs - name from pci.ids, N/A metrics.
    // A hidden card looks like "there is none", which is confusing (a GeForce
    // 210 without a driver on Ubuntu 22.04 looked exactly like that).
    if (nvmlCount == 0)
    {
        std::vector<GpuMetrics> nvidiaStub;
        AppendUndrivenNvidia(nvidiaStub);
        collected.insert(collected.end(),
                         std::make_move_iterator(nvidiaStub.begin()),
                         std::make_move_iterator(nvidiaStub.end()));
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    m_metrics.swap(collected);
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
        LOGI("libnvidia-ml.so not found: %hs", dlerror());
        return false;
    }
    LOGI("NVML library loaded: %p", m_hNvml);

// Map a member to its REAL exported NVML symbol name. The names below match
// the working Windows path (gpu_monitor.cpp); short forms like "nvmlGetCount"
// do not exist in libnvidia-ml and would resolve to NULL.
#define RESOLVE(fn, symbol) \
    m_fn##fn = reinterpret_cast<Fn##fn>(dlsym(m_hNvml, symbol)); \
    LOGI("  " symbol " = %p", (void*)m_fn##fn);

    RESOLVE(Init,      "nvmlInit_v2");
    RESOLVE(Shutdown,  "nvmlShutdown");
    RESOLVE(GetCount,  "nvmlDeviceGetCount_v2");
    RESOLVE(GetHandle, "nvmlDeviceGetHandleByIndex_v2");
    RESOLVE(GetName,   "nvmlDeviceGetName");
    RESOLVE(GetUsage,  "nvmlDeviceGetUtilizationRates");
    RESOLVE(GetMemory, "nvmlDeviceGetMemoryInfo");
    RESOLVE(GetTemp,   "nvmlDeviceGetTemperature");
    RESOLVE(GetClock,  "nvmlDeviceGetClockInfo");
    RESOLVE(GetFan,    "nvmlDeviceGetFanSpeed");
    // Optional symbol: absent before NVML 12.4 (driver branch R550), then the
    // fan chart falls back to the percentage of the call above.
    RESOLVE(GetFanRpm, "nvmlDeviceGetFanSpeedRPM");
    RESOLVE(GetPower,      "nvmlDeviceGetPowerUsage");
    RESOLVE(GetPowerLimit, "nvmlDeviceGetPowerManagementLimit");

#undef RESOLVE

    if (!m_fnInit || !m_fnGetCount || !m_fnGetHandle)
    {
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
    m_fnGetFanRpm = nullptr;
    m_fnGetPower  = nullptr;
    m_fnGetPowerLimit = nullptr;
}

void GpuMonitor::CollectNvml(std::vector<GpuMetrics>& out)
{
    if (!m_fnGetCount || !m_fnGetHandle)
        return;

    unsigned int count = 0;
    int ret = m_fnGetCount(&count);
    if (ret != NVML_SUCCESS || count == 0)
        return;

    out.reserve(out.size() + count);

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

        // Fan speed: RPM first (nvmlDeviceGetFanSpeedRPM, NVML 12.4+), the
        // percentage kept as fallback. NVML exposes no RPM maximum for NVIDIA
        // cards, so fanMaxRpm stays -1 and the chart uses the shared scale.
        if (m_fnGetFanRpm)
        {
            NvmlFanSpeedInfo fanInfo{};
            fanInfo.version = NVML_FANSPEED_INFO_V1;
            fanInfo.fan     = 0;
            if (m_fnGetFanRpm(hDevice, &fanInfo) == NVML_SUCCESS &&
                fanInfo.speed < 30000u)
                meta.fanRpm = static_cast<int>(fanInfo.speed);
        }
        if (m_fnGetFan)
        {
            unsigned int fanPct = 0;
            if (m_fnGetFan(hDevice, &fanPct) == NVML_SUCCESS)
                meta.fanPercent = static_cast<double>(fanPct);
        }

        // Power draw in milliwatts -> convert to watts.
        if (m_fnGetPower)
        {
            unsigned int powerMw = 0;
            if (m_fnGetPower(hDevice, &powerMw) == NVML_SUCCESS)
                meta.powerWatts = static_cast<double>(powerMw) / 1000.0;
        }

        // Power management limit in milliwatts -> convert to watts.
        if (m_fnGetPowerLimit)
        {
            unsigned int limitMw = 0;
            if (m_fnGetPowerLimit(hDevice, &limitMw) == NVML_SUCCESS)
                meta.powerLimitWatts = static_cast<double>(limitMw) / 1000.0;
        }

        // Fallback: static TDP table when NVML does not expose the limit.
        if (meta.powerLimitWatts <= 0.0)
        {
            const uint32_t tdp = PowerTable::GetGpuTdp(meta.name);
            if (tdp > 0)
                meta.powerLimitWatts = static_cast<double>(tdp);
        }

        out.push_back(meta);
    }
}

#endif // PLATFORM_LINUX
