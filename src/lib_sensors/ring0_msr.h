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

    // Read a 32-bit I/O port. Returns the value read, or 0 on failure.
    uint32_t ReadIoPortDword(uint16_t port);

    // Write a byte to an I/O port. Returns true on success.
    bool WriteIoPortByte(uint16_t port, uint8_t value);

    // Shut down the backend.
    void Close();

    // Check whether the backend is currently loaded and ready.
    bool IsOpen();

    // True when the process token has administrative privileges (UAC
    // elevation). Used to decide whether a driver install can be attempted
    // directly or needs the elevated helper.
    bool IsElevated();

    // True when installing/starting the driver could plausibly succeed:
    // a .sys file exists next to the exe (or an explicit registry path) or a
    // well-known service (WinRing0_1_2_0 etc.) is already registered.
    bool HasDriverAssets(const wchar_t* exeDir);

    // Relaunch this executable with the --install-driver argument using the
    // UAC "runas" verb, wait for the helper to finish and return true when
    // the helper exited with code 0. Shows the UAC consent prompt once.
    bool InstallElevated();

    // Body of the elevated helper mode (--install-driver): registers/starts
    // the driver service and leaves it running. Returns 0 on success.
    // Must be called before any GUI is created.
    int RunInstallHelper(const wchar_t* exeDir);
}

#endif // PLATFORM_WINDOWS
#endif // RING0_MSR_H
