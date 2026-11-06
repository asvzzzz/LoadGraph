#include "platform.h"
#include "msr_sensor.h"
#include "ring0_msr.h"
#include "logger.h"

#ifdef _WIN32
// platform.h already defines WIN32_LEAN_AND_MEAN and includes windows.h.
#include <intrin.h>

namespace MsrSensor
{
    // Pure-C helper: reads EAX part of an MSR via raw rdmsr.
    // Safe to call inside __try/__except.
    static uint32_t RdmsrEax(uint32_t msr)
    {
        return static_cast<uint32_t>(__readmsr(msr));
    }

    // Wrapper that catches STATUS_PRIVILEGE_INSTRUCTION from raw rdmsr.
    static uint32_t RdmsrOrFallback(uint32_t msr)
    {
        ULONG excCode = 0;
        __try
        {
            return RdmsrEax(msr);
        }
        __except((excCode = GetExceptionCode()) == 0xC0000096
                 ? EXCEPTION_EXECUTE_HANDLER
                 : EXCEPTION_CONTINUE_SEARCH)
        {
            printf("DIAG MSR: __readmsr raised 0x%08X, skipping\n", excCode);
            return 0xFFFFFFFF;
        }
    }

    // Try the kernel backend (DLL or driver) first.
    static uint32_t ReadMsrLo(uint32_t msr)
    {
        uint32_t lo = 0, hi = 0;
        if (Ring0Msr::ReadMsr(msr, lo, hi))
            return lo;
        return 0xFFFFFFFF;
    }

    static bool ReadMsrFull(uint32_t msr, uint32_t& lo, uint32_t& hi)
    {
        return Ring0Msr::ReadMsr(msr, lo, hi);
    }

    // Decode from IA32_TEMPERATURE_REPORTING_POINT_STATUS (0xC001_0294).
    static double DecodeTempFromStatus(uint32_t lo, int8_t tjmax)
    {
        if ((lo & 1u) == 0)
        {
            LOGI("DIAG MSR: TEMP_REPORTING_POINT_STATUS not present (0x%08X)", lo);
            return -1.0;
        }
        const double resolution = static_cast<double>((lo >> 1) & 0x7F);
        if (resolution <= 0.0)
        {
            LOGI("DIAG MSR: zero resolution (0x%08X)", lo);
            return -1.0;
        }
        const double delta = static_cast<double>((lo >> 8) & 0xFF) * resolution;
        const double tempC = static_cast<double>(tjmax) - delta;
        if (tempC < -10.0 || tempC > 120.0)
        {
            LOGI("DIAG MSR: implausible temp=%.1f C (delta=%.2f)", tempC, delta);
            return -1.0;
        }
        return tempC;
    }

    // Decode from IA32_TEMPERATURE_TARGET (0x0000_019C).
    // Bits[23:16] of the 64-bit value = Toffset (degrees below Tjmax).
    // This is the MSR that RealTemp / RTCore.dll reads.
    static double DecodeTempFromTarget(uint32_t lo, uint32_t hi, int8_t tjmax)
    {
        const uint32_t toffset = (hi >> 16) & 0x7F;
        const double tempC = static_cast<double>(tjmax) - static_cast<double>(toffset);
        if (tempC < -10.0 || tempC > 120.0)
        {
            LOGI("DIAG MSR: Target implausible toffset=%u temp=%.1f C",
                 toffset, tempC);
            return -1.0;
        }
        LOGI("DIAG MSR: IA32_TEMP_TARGET eax=0x%08X edx=0x%08X "
             "toffset=%u temp=%.1f C",
             lo, hi, toffset, tempC);
        return tempC;
    }

    double ProbeTemperature(uint32_t /*family*/, uint32_t model,
                            uint32_t stepping, int8_t tjmax)
    {
        static constexpr uint32_t kStatusMsr = 0xC0010294;
        static constexpr uint32_t kTargetMsr = 0x0000019C;
        // IA32_PACKAGE_THERM_STATUS — contains current thermal status and delta.
        static constexpr uint32_t kPkgThermMsr = 0xC00102B1;

        // --- Debug: dump all candidate MSRs via backend ------------------------
        struct MsrEntry
        {
            uint32_t addr;
            const char* name;
        };
        static constexpr MsrEntry kCandidates[] = {
            {kStatusMsr,   "TEMP_REPORTING_POINT_STATUS"},
            {kPkgThermMsr, "PACKAGE_THERM_STATUS"},
            {kTargetMsr,   "IA32_TEMPERATURE_TARGET"},
        };
        for (const auto& m : kCandidates)
        {
            uint32_t lo = 0, hi = 0;
            if (Ring0Msr::ReadMsr(m.addr, lo, hi))
                LOGI("DIAG MSR: %s(0x%08X) = EAX=0x%08X EDX=0x%08X",
                     m.name, m.addr, lo, hi);
            else
                LOGI("DIAG MSR: %s(0x%08X) FAILED", m.name, m.addr);
        }

        // --- Backend path (DLL or driver) --------------------------------------
        uint32_t lo = ReadMsrLo(kStatusMsr);
        if (lo != 0xFFFFFFFF)
        {
            const double temp = DecodeTempFromStatus(lo, tjmax);
            if (temp >= 0.0)
            {
                LOGI("DIAG MSR: backend -> TEMP_REPORTING_POINT_STATUS "
                     "temp=%.1f C", temp);
                return temp;
            }
        }

        // Try IA32_TEMPERATURE_TARGET via backend (needs both halves).
        uint32_t loT = 0, hiT = 0;
        if (ReadMsrFull(kTargetMsr, loT, hiT))
        {
            const double temp = DecodeTempFromTarget(loT, hiT, tjmax);
            if (temp >= 0.0)
            {
                LOGI("DIAG MSR: backend -> IA32_TEMP_TARGET temp=%.1f C", temp);
                return temp;
            }
        }

        // Try PACKAGE_THERM_STATUS via backend.
        uint32_t loP = 0, hiP = 0;
        if (ReadMsrFull(kPkgThermMsr, loP, hiP))
        {
            // Bits[16:8] = temperature delta from Tjmax (in 1-degree steps on most CPUs).
            const uint32_t delta = (hiP >> 8) & 0x7F;
            const double tempC = static_cast<double>(tjmax) - static_cast<double>(delta);
            if (tempC >= -10.0 && tempC <= 120.0)
            {
                LOGI("DIAG MSR: backend -> PACKAGE_THERM_STATUS "
                     "toffset=%u temp=%.1f C", delta, tempC);
                return tempC;
            }
        }

        // --- Raw rdmsr fallback for Status MSR (EAX only, sufficient here) -----
        lo = RdmsrOrFallback(kStatusMsr);
        if (lo != 0xFFFFFFFF)
        {
            const double temp = DecodeTempFromStatus(lo, tjmax);
            if (temp >= 0.0)
            {
                LOGI("DIAG MSR: raw rdmsr -> TEMP_REPORTING_POINT_STATUS "
                     "temp=%.1f C", temp);
                return temp;
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
