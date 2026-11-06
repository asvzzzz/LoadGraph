#ifndef RING0_MSR_H
#define RING0_MSR_H

#ifdef PLATFORM_WINDOWS

#include <cstdint>

namespace Ring0Msr
{
    // Initialize the MSR backend: try WinRing0x64.dll first (RealTemp-style),
    // then fall back to direct kernel driver via CreateService.
    // exeDir points to the directory containing the executable (or nullptr for
    // current working directory). Returns true if any backend became available.
    bool Open(const wchar_t* exeDir);

    // Read one MSR. outLo = EAX (low 32), outHi = EDX (high 32).
    // Returns true on success.
    bool ReadMsr(uint32_t msr, uint32_t& outLo, uint32_t& outHi);

    // Shut down the backend.
    void Close();

    // Check whether the backend is currently loaded and ready.
    bool IsOpen();
}

#endif // PLATFORM_WINDOWS
#endif // RING0_MSR_H
