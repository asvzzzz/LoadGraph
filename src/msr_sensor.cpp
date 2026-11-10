#include "platform.h"
#include "msr_sensor.h"
#include "ring0_msr.h"
#include "logger.h"
#include <wx/wx.h>

#ifdef _WIN32
// platform.h already defines WIN32_LEAN_AND_MEAN and includes windows.h.
#include <intrin.h>

namespace MsrSensor
{
    // MSR addresses (Intel SDM Vol 3B).
    // IA32_TEMPERATURE_TARGET = 0x0000_01A2 — Tjmax in bits[23:16].
    // IA32_THERM_STATUS       = 0x0000_019C — digital readout in bits[22:16].
    static constexpr uint32_t kTargetMsr = 0x000001A2;
    static constexpr uint32_t kThermMsr  = 0x0000019C;

    // Read a 32-bit MSR value using the rdmsr intrinsic.
    // Returns 0xFFFFFFFF on any exception (privilege violation, etc.).
    // Must be a plain-C function (no objects with destructors) for SEH:
    // MSVC error C2712 forbids __try in functions requiring unwinding.
    // The exception code is reported through outExcCode (POD only).
    static uint32_t RdmsrRaw(uint32_t msr, ULONG& outExcCode)
    {
        outExcCode = 0;
        uint32_t value = 0xFFFFFFFFu;
        __try
        {
            value = static_cast<uint32_t>(__readmsr(msr));
        }
        __except((outExcCode = GetExceptionCode()) == 0xC0000096
                 ? EXCEPTION_EXECUTE_HANDLER
                 : EXCEPTION_CONTINUE_SEARCH)
        {
            value = 0xFFFFFFFFu;
        }
        return value;
    }

    // SEH-safe wrapper: performs the read and logs a failure once.
    // Logging lives here (not in RdmsrRaw) because wxString temporaries make
    // a function ineligible for __try.
    static uint32_t RdmsrSafe(uint32_t msr)
    {
        ULONG excCode = 0;
        const uint32_t value = RdmsrRaw(msr, excCode);
        if (excCode != 0)
            LOGI("MSR 0x%08X read raised 0x%08X, treating as unavailable",
                 msr, static_cast<unsigned>(excCode));
        return value;
    }

    // Pin thread to a specific logical processor and read an MSR.
    // Returns {EAX, EDX} from the single rdmsr instruction. RdmsrRaw returns
    // only EAX (low 32 bits); EDX is not separately addressable here, so hi is
    // set to 0 — callers that need the high half should use Ring0Msr::ReadMsr().
    static std::pair<uint32_t, uint32_t> ReadMsrOnCore(uint32_t msr)
    {
        HANDLE hThread = GetCurrentThread();
        DWORD_PTR oldAff = SetThreadAffinityMask(hThread, 1);
        if (oldAff == 0)
            return {0, 0};

        ULONG excCode = 0;
        const uint32_t lo = RdmsrRaw(msr, excCode);
        // hi is intentionally left as 0: rdmsr returns EDX:EAX in one instruction
        // and RdmsrRaw only captures EAX. A second call would read the SAME MSR
        // (not EDX), so we avoid the misleading double-read.

        SetThreadAffinityMask(hThread, oldAff);
        return {lo, 0u};
    }

    // Decode current temperature from IA32_THERM_STATUS (MSR 0x19C).
    // Bits[22:16] = digital readout (degrees below Tjmax).
    static double DecodeTempFromTherm(uint32_t lo, int8_t tjmax)
    {
        if ((lo & 0x80000000u) == 0)
            return -1.0;
        const uint32_t do_val = (lo >> 16) & 0x7F;
        const double tempC = static_cast<double>(tjmax) - static_cast<double>(do_val);
        if (tempC < -10.0 || tempC > 120.0)
            return -1.0;
        return tempC;
    }

    double ProbeTemperature(uint32_t /*family*/, uint32_t model,
                            uint32_t stepping, int8_t tjmax)
    {
        // --- Backend path (DLL or driver) with core affinity -------------------
        uint32_t dllLo = 0, dllHi = 0;
        if (Ring0Msr::ReadMsr(kTargetMsr, dllLo, dllHi))
        {
            // Tjmax is in bits[23:16] of EAX (low part), not EDX.
            int computedTjmax = static_cast<int>((dllLo >> 16) & 0xFF);
            if (computedTjmax <= 0 || computedTjmax > 150)
                computedTjmax = static_cast<int>(tjmax);

            if (Ring0Msr::ReadMsr(kThermMsr, dllLo, dllHi))
            {
                const double temp = DecodeTempFromTherm(dllLo,
                    static_cast<int8_t>(computedTjmax));
                if (temp >= 0.0)
                    return temp;
            }
        }

        // --- Raw rdmsr fallback (EAX only) -------------------------------------
        auto [loT, hiT] = ReadMsrOnCore(kTargetMsr);
        if (loT != 0xFFFFFFFF)
        {
            // Tjmax is in bits[23:16] of EAX (low part).
            int computedTjmax = (loT >> 16) & 0xFF;
            if (computedTjmax <= 0 || computedTjmax > 150)
                computedTjmax = static_cast<int>(tjmax);

            auto [lo, hi] = ReadMsrOnCore(kThermMsr);
            if (lo != 0xFFFFFFFF)
            {
                const double temp = DecodeTempFromTherm(lo,
                    static_cast<int8_t>(computedTjmax));
                if (temp >= 0.0)
                    return temp;
            }
        }

        return -1.0;
    }

    void InitDriver(const wchar_t* exeDir)
    {
        Ring0Msr::Open(exeDir);
    }

    bool IsAvailable()
    {
        return Ring0Msr::IsOpen();
    }

    void EnsureDriver(const wchar_t* exeDir)
    {
        // Backend already works (e.g. RealTemp loaded the driver): nothing to do.
        if (Ring0Msr::IsOpen())
            return;

        // Nothing we could install: no .sys next to the exe, no registry path
        // and no registered WinRing0 service. Elevation would not help.
        if (!Ring0Msr::HasDriverAssets(exeDir))
        {
            LOGI("MSR: no driver assets found, using WMI fallback.");
            return;
        }

        // Assets exist but we are already elevated: the install was attempted
        // and failed for another reason (bad .sys, blocked service, ...).
        // Asking for UAC again would not change anything.
        if (Ring0Msr::IsElevated())
        {
            LOGI("MSR: driver assets present but backend failed even when "
                 "elevated, using WMI fallback.");
            return;
        }

        const int answer = wxMessageBox(
            "LoadGraph needs a small kernel driver (WinRing0) to read the "
            "CPU temperature.\n\n"
            "Install it now? Windows will ask for administrator permission.\n\n"
            "If you decline, temperature will be shown as N/A.",
            "LoadGraph - CPU temperature",
            wxYES_NO | wxICON_QUESTION);

        if (answer != wxYES)
        {
            LOGI("MSR: driver installation declined by user, using WMI fallback.");
            return;
        }

        if (!Ring0Msr::InstallElevated())
        {
            LOGI("MSR: elevated installation failed or was cancelled, "
                 "using WMI fallback.");
            return;
        }

        // The helper left the service running: reopen the backend without
        // any privileges (device ACL allows plain users to open it).
        if (Ring0Msr::Open(exeDir))
            LOGI("MSR: backend opened after elevated driver install.");
        else
            LOGI("MSR: backend still unavailable after elevated install, "
                 "using WMI fallback.");
    }

    int RunInstallHelper(const wchar_t* exeDir)
    {
        return Ring0Msr::RunInstallHelper(exeDir);
    }

    void Shutdown()
    {
        Ring0Msr::Close();
    }
}

#endif // _WIN32
