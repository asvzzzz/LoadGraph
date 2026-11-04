#ifndef POWER_TABLE_H
#define POWER_TABLE_H

#include <cstdint>
#include <string>
#include <utility>

// Static TDP (Thermal Design Power) table in watts for CPUs and NVIDIA GPUs.
// Used as maximum power limit when the hardware does not expose it directly
// (e.g. CPU without RAPL, GPU without nvmlDeviceGetPowerManagementLimit).

namespace PowerTable
{
    // Look up CPU TDP by family+model (Linux /proc/cpuinfo) or by name substring.
    // Returns TDP in watts, or a conservative default (125 W) if not found.
    uint32_t GetCpuTdp(const char* familyModel);   // e.g. "6_47" (family 6, model 0x47)
    uint32_t GetCpuTdpByName(const std::string& name); // substring match, Windows

    // Look up NVIDIA GPU TDP by device name substring.
    // Returns TDP in watts, or 0 if not found (caller uses NVML limit or estimate).
    uint32_t GetGpuTdp(const std::string& gpuName);
}

#endif // POWER_TABLE_H
