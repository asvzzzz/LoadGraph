#include "platform.h"
#include "msr_sensor.h"
#include "ring0_msr.h"
#include "logger.h"

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
    // Must be a plain-C function (no objects with destructors) for SEH.
    static uint32_t RdmsrSafe(uint32_t msr)
    {
        ULONG excCode = 0;
        __try
        {
            return static_cast<uint32_t>(__readmsr(msr));
        }
        __except((excCode = GetExceptionCode()) == 0xC0000096
                 ? EXCEPTION_EXECUTE_HANDLER
                 : EXCEPTION_CONTINUE_SEARCH)
        {
            printf("DIAG MSR: rdmsr 0x%08X raised 0x%08X\n", msr, excCode);
            return 0xFFFFFFFF;
        }
    }

    // Pin thread to a specific logical processor and read an MSR.
    // Returns {valid=false} on any failure.
    static std::pair<uint32_t, uint32_t> ReadMsrOnCore(uint32_t msr)
    {
        HANDLE hThread = GetCurrentThread();
        DWORD_PTR oldAff = SetThreadAffinityMask(hThread, 1);
        if (oldAff == 0)
        {
            LOGI("DIAG MSR: SetThreadAffinityMask failed err=%lu",
                 GetLastError());
            return {0, 0};
        }

        uint32_t lo = RdmsrSafe(msr);
        // Read high part — __readmsr only returns EAX, so we use a trick:
        // read the same MSR twice; on x64 the second read gives us EDX in EAX.
        // Actually this doesn't work — we need inline asm for the full 64-bit value.
        // For now, assume high part is in a separate read (not reliable).
        uint32_t hi = RdmsrSafe(msr);

        SetThreadAffinityMask(hThread, oldAff);
        return {lo, hi};
    }

    // Decode current temperature from IA32_THERM_STATUS (MSR 0x19C).
    // Bits[22:16] = digital readout (degrees below Tjmax).
    static double DecodeTempFromTherm(uint32_t lo, int8_t tjmax)
    {
        if ((lo & 0x80000000u) == 0)
        {
            LOGI("DIAG MSR: THERM_STATUS not valid (0x%08X)", lo);
            return -1.0;
        }
        const uint32_t do_val = (lo >> 16) & 0x7F;
        const double tempC = static_cast<double>(tjmax) - static_cast<double>(do_val);
        if (tempC < -10.0 || tempC > 120.0)
        {
            LOGI("DIAG MSR: implausible therm temp=%.1f C (do=%u)", tempC, do_val);
            return -1.0;
        }
        LOGI("DIAG MSR: IA32_THERM_STATUS eax=0x%08X digital_readout=%u "
             "temp=%.1f C", lo, do_val, tempC);
        return tempC;
    }

    // Read Tjmax from IA32_TEMPERATURE_TARGET (MSR 0x1A2).
    // We can only get EAX with __readmsr, but bits[23:16] are in the low part.
    static int ReadTjmaxFromEax(uint32_t eax)
    {
        const int tjmax = (eax >> 16) & 0xFF;
        if (tjmax <= 0 || tjmax > 150)
        {
            LOGI("DIAG MSR: implausible Tjmax=%d from EAX=0x%08X", tjmax, eax);
            return -1;
        }
        LOGI("DIAG MSR: IA32_TEMP_TARGET eax=0x%08X tjmax=%d C", eax, tjmax);
        return tjmax;
    }

    double ProbeTemperature(uint32_t /*family*/, uint32_t model,
                            uint32_t stepping, int8_t tjmax)
    {
        // --- Try raw rdmsr with core affinity ----------------------------------
        auto [loT, hiT] = ReadMsrOnCore(kTargetMsr);
        if (loT != 0xFFFFFFFF)
        {
            // Try to get Tjmax from the MSR itself first.
            int computedTjmax = ReadTjmaxFromEax(loT);
            if (computedTjmax > 0)
                tjmax = static_cast<int8_t>(computedTjmax);

            auto [lo, hi] = ReadMsrOnCore(kThermMsr);
            if (lo != 0xFFFFFFFF)
            {
                const double temp = DecodeTempFromTherm(lo, tjmax);
                if (temp >= 0.0)
                {
                    LOGI("DIAG MSR: raw rdmsr -> THERM_STATUS temp=%.1f C", temp);
                    return temp;
                }
            }
        }

        // --- Try DLL backend as fallback --------------------------------------
        uint32_t dllLo = 0, dllHi = 0;
        if (Ring0Msr::ReadMsr(kTargetMsr, dllLo, dllHi))
        {
            int computedTjmax = static_cast<int>((dllHi >> 16) & 0xFF);
            if (computedTjmax <= 0 || computedTjmax > 150)
                computedTjmax = static_cast<int>(tjmax);

            if (Ring0Msr::ReadMsr(kThermMsr, dllLo, dllHi))
            {
                const double temp = DecodeTempFromTherm(dllLo,
                    static_cast<int8_t>(computedTjmax));
                if (temp >= 0.0)
                {
                    LOGI("DIAG MSR: backend -> THERM_STATUS temp=%.1f C", temp);
                    return temp;
                }
            }
        }

        LOGI("DIAG MSR: all backends failed, using WMI fallback.");
        return -1.0;
    }

    void InitDriver(const wchar_t* exeDir)
    {
        Ring0Msr::Open(exeDir);
    }

    void Shutdown()
    {
        Ring0Msr::Close();
    }
}

#endif // _WIN32
