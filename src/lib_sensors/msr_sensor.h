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

    // True when the MSR backend (DLL or kernel driver) is open and usable.
    bool IsAvailable();

    // Make sure the MSR backend is available. If it failed to open, driver
    // assets exist (.sys next to exe or a registered WinRing0 service) and
    // the process is not elevated, asks the user once (UAC consent) to run
    // the elevated helper that installs/starts the driver service, then
    // retries opening the backend. Declining the prompt or a failed UAC
    // elevation is not an error: the caller simply falls back to WMI.
    // Must be called from the GUI thread (shows a message box).
    void EnsureDriver(const wchar_t* exeDir);

    // Elevated helper mode (--install-driver): registers/starts the driver
    // service without creating any GUI. Returns 0 on success.
    int RunInstallHelper(const wchar_t* exeDir);

    // Probe IA32_THERM_STATUS (0x19C) + IA32_TEMPERATURE_TARGET (0x1A2),
    // decode Celsius as Tjmax - digital_readout. Returns -1.0 on any failure
    // (driver absent, MSR blocked, implausible value).
    double ProbeTemperature(uint32_t family, uint32_t model,
                            uint32_t stepping, int8_t tjmax);

    // Shut down the driver handle and stop the service.
    void Shutdown();
}

#endif // _WIN32
#endif // MSR_SENSOR_H
