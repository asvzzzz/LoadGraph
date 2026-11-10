#include "metrics_api.h"
#include "cpu_monitor.h"
#include "ram_monitor.h"
#include "gpu_monitor.h"
#include "storage_monitor.h"
#include <chrono>
#include <thread>

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

CpuData MetricsGetCpu()
{
    CpuData d{};
    if (!s_cpu) return d;
    const auto& m = s_cpu->GetMetrics();
    d.loadPercent   = m.loadPercent;
    d.temperature   = m.temperature;
    d.fanRpm        = m.fanRpm;
    d.uptimeMs      = m.uptimeMs;
    d.cpuName       = m.cpuName;
    d.powerWatts    = m.powerWatts;
    d.tdpWatts      = m.tdpWatts;
    return d;
}

RamData MetricsGetRam()
{
    RamData d{};
    if (!s_ram) return d;
    const auto& m = s_ram->GetMetrics();
    d.totalBytes   = m.totalBytes;
    d.freeBytes    = m.freeBytes;
    d.loadPercent  = m.loadPercent;
    d.powerWatts   = m.powerWatts;
    return d;
}

std::vector<GpuData> MetricsGetGpus()
{
    std::vector<GpuData> out;
    if (!s_gpu || !s_gpu->IsAvailable()) return out;
    const auto& metrics = s_gpu->GetMetrics();
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
        d.powerWatts     = m.powerWatts;
        out.push_back(d);
    }
    return out;
}

bool MetricsGpuAvailable()
{
    return s_gpu && s_gpu->IsAvailable();
}

std::vector<StorageData> MetricsGetStorage()
{
    std::vector<StorageData> out;
    if (!s_storage) return out;
    return s_storage->GetMetrics();
}
