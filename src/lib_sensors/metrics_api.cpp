#include "metrics_api.h"
#include "cpu_monitor.h"
#include "ram_monitor.h"
#include "gpu_monitor.h"
#include "storage_monitor.h"
#include "app_config.h"
#include <chrono>
#include <thread>
#include <cstring>

#ifdef PLATFORM_WINDOWS
#include <windows.h>
#endif

// Static singleton monitors - shared between GUI and HTTP server.
static CpuMonitor*      s_cpu       = nullptr;
static RamMonitor*      s_ram       = nullptr;
static GpuMonitor*      s_gpu       = nullptr;
static StorageMonitor*  s_storage   = nullptr;

bool MetricsInit()
{
    s_cpu = new CpuMonitor();
    s_ram = new RamMonitor();
    s_gpu = new GpuMonitor();

#ifdef PLATFORM_WINDOWS
    // The Super I/O tachometer probe writes to the LPC config port of an
    // unknown chip, so it stays off unless loadgraph.cfg asks for it. The View
    // menu can raise the request later through MetricsSetSuperIoFanProbe();
    // applying it before CollectNow() makes the first sample use it as well.
    s_cpu->SetSuperIoFanProbe(LoadAppConfig().superio_fan);
#endif

    s_cpu->QueryCpuName();
    s_cpu->CollectNow();
    s_ram->CollectNow();
    if (s_gpu->IsAvailable()) s_gpu->CollectNow();

    s_cpu->Start();
    s_ram->Start();
    if (s_gpu->IsAvailable()) s_gpu->Start();

    s_storage = CreateStorageMonitor();
    if (s_storage) s_storage->Start();

    return true;
}

void MetricsShutdown()
{
    if (s_cpu)   { s_cpu->Stop();   delete s_cpu;   s_cpu = nullptr; }
    if (s_ram)   { s_ram->Stop();   delete s_ram;   s_ram = nullptr; }
    if (s_gpu)   { s_gpu->Stop();   delete s_gpu;   s_gpu = nullptr; }
    if (s_storage) { s_storage->Stop(); delete s_storage; s_storage = nullptr; }
}

void MetricsGetCpu(CpuData& out)
{
    // Zero-initialize to prevent reading garbage from uninitialized padding.
    std::memset(&out, 0, sizeof(out));
    if (!s_cpu) return;
    CpuMetrics m{};
    s_cpu->GetMetrics(m);
    out.loadPercent   = m.loadPercent;
    out.temperature   = m.temperature;
    out.fanRpm        = m.fanRpm;
    out.uptimeMs      = m.uptimeMs;
    out.cpuName = m.cpuName;
    out.powerWatts    = m.powerWatts;
    out.tdpWatts      = m.tdpWatts;
}

void MetricsGetRam(RamData& out)
{
    std::memset(&out, 0, sizeof(out));
    if (!s_ram) return;
    const auto& m = s_ram->GetMetrics();
    out.totalBytes   = m.totalBytes;
    out.freeBytes    = m.freeBytes;
    out.loadPercent  = m.loadPercent;
    out.powerWatts   = m.powerWatts;
}

std::vector<GpuData> MetricsGetGpus()
{
    std::vector<GpuData> out;
    if (!s_gpu || !s_gpu->IsAvailable()) return out;
    auto metrics = s_gpu->GetMetrics();
    out.reserve(metrics.size());
    for (const auto& m : metrics)
    {
        GpuData d{};
        d.name           = m.name;
        d.loadPercent    = m.loadPercent;
        d.temperature    = m.temperature;
        d.vramTotalBytes = m.vramTotalBytes;
        d.vramUsedBytes  = m.vramUsedBytes;
        d.fanRpm         = m.fanRpm;
        d.fanPercent     = m.fanPercent;
        d.fanMaxRpm      = m.fanMaxRpm;
        d.powerWatts     = m.powerWatts;
        // The limit was never copied: the web power chart and the TTY power bar
        // always fell back to their hard coded maximum.
        d.powerLimitWatts = m.powerLimitWatts;
        out.push_back(d);
    }
    return out;
}

bool MetricsGpuAvailable()
{
    return s_gpu && s_gpu->IsAvailable();
}

void MetricsSetSuperIoFanProbe(bool enable)
{
#ifdef PLATFORM_WINDOWS
    if (s_cpu) s_cpu->SetSuperIoFanProbe(enable);
#else
    // Linux reads the CPU tachometers from /sys/class/hwmon, which involves no
    // port access and needs no switch.
    (void)enable;
#endif
}

bool MetricsSuperIoFanProbeEnabled()
{
#ifdef PLATFORM_WINDOWS
    return s_cpu ? s_cpu->IsSuperIoFanProbeEnabled() : false;
#else
    return false;
#endif
}

std::vector<StorageData> MetricsGetStorage()
{
    std::vector<StorageData> out;
    if (!s_storage) return out;
    return s_storage->GetMetrics();
}
