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

    // Estimate CPU power draw (watts) from load% when no real counter is
    // available (Windows has no user-mode RAPL; Linux RAPL is root-only or
    // missing in WSL). Models the idle floor: even at 0% load a CPU still
    // draws static leakage and uncore power, so pure load x TDP reports ~1 W
    // at idle while the real package draw is tens of watts.
    //   P = P_idle + load% * (TDP - P_idle)
    //   P_idle = min(0.35 * TDP, 2.5 W * physicalCores)
    // physicalCores = 0 -> fallback P_idle = 0.25 * TDP.
    // The result is clamped to [0, TDP]; loadPercent is clamped to [0, 100].
    double EstimateCpuPowerWatts(double loadPercent, uint32_t tdpWatts,
                                 uint32_t physicalCores);
}

#endif // POWER_TABLE_H
