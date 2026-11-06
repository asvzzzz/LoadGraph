#ifndef MSR_SENSOR_H
#define MSR_SENSOR_H

// MSR temperature reader for Windows.
// Two back-ends are tried in order:
//   1. Kernel driver (WinRing0-compatible, e.g. hwinterfacex64.sys) placed next
//      to the executable — used when present.
//   2. Direct __readmsr() intrinsic as a last resort (only works on CPUs where
//      rdmsr is not hardware-blocked).
// If neither works temperature is reported as N/A (-1.0).
//
// The caller must supply family/model/stepping/Tjmax pre-computed on the main
// thread (see CpuMonitor::ProbeMsrInit) so this module stays free of WMI and
// registry calls.

#ifdef _WIN32
#include <cstdint>

namespace MsrSensor
{
    // Initialize the kernel driver if its .sys file is present next to exe.
    // Safe to call from any thread; idempotent.
    void InitDriver(const wchar_t* exeDir);

    // Probe IA32_TEMPERATURE_REPORTING_POINT_STATUS (0xC001_0294) and decode
    // Celsius from Tjmax - (bits[15:8] * resolution). Returns -1.0 on any
    // failure (driver absent, MSR blocked, implausible value).
    double ProbeTemperature(uint32_t family, uint32_t model,
                            uint32_t stepping, int8_t tjmax);

    // Shut down the driver handle and stop the service.
    void Shutdown();
}

#endif // _WIN32
#endif // MSR_SENSOR_H
