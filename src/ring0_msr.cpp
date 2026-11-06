#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifdef WIN32_LEAN_AND_MEAN
#undef WIN32_LEAN_AND_MEAN
#endif
#include <winsvc.h>
#include <winioctl.h>
#define WIN32_LEAN_AND_MEAN
#endif

#include "platform.h"
#include "ring0_msr.h"
#include "logger.h"

#ifdef PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <stdio.h>
#include <string>
#include <algorithm>

// WinRing0-compatible IOCTL codes (fallback path).
static constexpr uint32_t kOlsType = 40000;
static constexpr uint32_t kIoctlReadMsr =
    CTL_CODE(kOlsType, 0x821, METHOD_BUFFERED, FILE_ANY_ACCESS);

namespace Ring0Msr
{
    // Backend selection: true = WinRing0x64.dll (RealTemp-style), false = direct driver.
    enum class Backend { None, Dll, Driver };

    // DLL function pointers.
    typedef bool(__stdcall* FnInitializeOls)();
    typedef void(__stdcall* FnDeinitOls)();
    typedef bool(__stdcall* FnRdmsr)(uint32_t, uint32_t*, uint32_t*);
    typedef bool(__stdcall* FnIsMsr)();
    typedef int(__stdcall* FnGetDllStatus)();
    typedef int(__stdcall* FnGetDriverType)();

    static HMODULE            s_hDll      = nullptr;
    static FnInitializeOls    s_fnInit    = nullptr;
    static FnDeinitOls        s_fnDeinit  = nullptr;
    static FnRdmsr            s_fnRdmsr   = nullptr;
    static FnIsMsr            s_fnIsMsr   = nullptr;
    static FnGetDllStatus     s_fnDllStatus = nullptr;
    static FnGetDriverType    s_fnDrvType   = nullptr;

    // Driver-state fields (used when backend == Driver).
    static HANDLE   s_hDevice  = INVALID_HANDLE_VALUE;
    static SC_HANDLE s_hScm    = nullptr;
    static SC_HANDLE s_hSvc    = nullptr;
    static bool     s_drvOpened = false;

    static Backend s_backend = Backend::None;

    // --- DLL backend (WinRing0x64.dll from RealTemp) --------------------------

    static std::wstring FindDll(const wchar_t* exeDir)
    {
        static const wchar_t* kCandidates[] = {
            L"WinRing0x64.dll",
            L"WinRing0.dll",
        };
        wchar_t dir[MAX_PATH] = {};
        if (exeDir) wcscpy_s(dir, exeDir);
        else
        {
            DWORD sz = GetModuleFileNameW(nullptr, dir, MAX_PATH);
            if (sz > 0)
            {
                wchar_t* back = wcsrchr(dir, L'\\');
                if (back) *(back + 1) = L'\0';
            }
        }
        for (const auto* name : kCandidates)
        {
            std::wstring path(dir);
            if (!path.empty() && path.back() != L'\\') path += L'\\';
            path += name;
            if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
                return path;
        }
        return L"";
    }

    static bool OpenDll(const wchar_t* exeDir)
    {
        std::wstring dllPath = FindDll(exeDir);
        if (dllPath.empty())
        {
            LOGI("DIAG MSR: WinRing0x64.dll not found.");
            return false;
        }

        s_hDll = LoadLibraryW(dllPath.c_str());
        if (!s_hDll)
        {
            LOGI("DIAG MSR: LoadLibraryW failed err=%lu", GetLastError());
            return false;
        }

        s_fnInit  = reinterpret_cast<FnInitializeOls>(
                        GetProcAddress(s_hDll, "InitializeOls"));
        s_fnDeinit = reinterpret_cast<FnDeinitOls>(
                        GetProcAddress(s_hDll, "DeinitializeOls"));
        s_fnRdmsr = reinterpret_cast<FnRdmsr>(
                        GetProcAddress(s_hDll, "Rdmsr"));
        s_fnIsMsr   = reinterpret_cast<FnIsMsr>(
                        GetProcAddress(s_hDll, "IsMsr"));
        s_fnDllStatus = reinterpret_cast<FnGetDllStatus>(
                        GetProcAddress(s_hDll, "GetDllStatus"));
        s_fnDrvType   = reinterpret_cast<FnGetDriverType>(
                        GetProcAddress(s_hDll, "GetDriverType"));

        if (!s_fnInit || !s_fnDeinit || !s_fnRdmsr)
        {
            LOGI("DIAG MSR: WinRing0x64.dll missing required exports.");
            FreeLibrary(s_hDll);
            s_hDll = nullptr;
            return false;
        }

        if (!s_fnInit())
        {
            LOGI("DIAG MSR: InitializeOls failed.");
            s_fnDeinit();
            FreeLibrary(s_hDll);
            s_hDll = nullptr;
            return false;
        }

        if (s_fnIsMsr && !s_fnIsMsr())
        {
            int dsts = s_fnDllStatus ? s_fnDllStatus() : -1;
            int drvt = s_fnDrvType ? s_fnDrvType() : -1;
            LOGI("DIAG MSR: IsMsr=false DllStatus=%d DriverType=%d", dsts, drvt);
            s_fnDeinit();
            FreeLibrary(s_hDll);
            s_hDll = nullptr;
            return false;
        }

        LOGI("DIAG MSR: loaded WinRing0x64.dll from %s",
             wxString(dllPath.c_str()).mb_str().data());
        s_backend = Backend::Dll;
        return true;
    }

    // --- Driver backend (direct IOCTL) ----------------------------------------

    static const wchar_t* kKnownSvcNames[] = {
        L"WinRing0_1_2_0",
        L"hwinterfacex64",
        L"inpoutx64",
    };

    static const wchar_t* kDeviceNames[] = {
        L"\\\\.\\WinRing0_1_2_0",
        L"\\\\.\\hwinterfacex64",
        L"\\\\.\\inpoutx64",
    };

    static std::wstring FindDriver(const wchar_t* exeDir)
    {
        HKEY hKey = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                          L"SOFTWARE\\LoadGraph", 0, KEY_READ, &hKey) == ERROR_SUCCESS)
        {
            wchar_t path[MAX_PATH] = {};
            DWORD size = sizeof(path);
            if (RegQueryValueExW(hKey, L"MsrDriver", nullptr, nullptr,
                                 reinterpret_cast<LPBYTE>(path), &size) == ERROR_SUCCESS
                && path[0])
            {
                LOGI("DIAG MSR driver from registry: %S", wxString(path).mb_str().data());
                RegCloseKey(hKey);
                return std::wstring(path);
            }
            RegCloseKey(hKey);
        }

        wchar_t exeDirBuf[MAX_PATH] = {};
        if (exeDir) wcscpy_s(exeDirBuf, exeDir);
        else
        {
            DWORD sz = GetModuleFileNameW(nullptr, exeDirBuf, MAX_PATH);
            if (sz > 0)
            {
                wchar_t* back = wcsrchr(exeDirBuf, L'\\');
                if (back) *(back + 1) = L'\0';
            }
        }

        static const wchar_t* kCandidates[] = {
            L"hwinterfacex64.sys",
            L"inpoutx64.sys",
            L"WinRing0x64.sys",
        };
        for (const auto* name : kCandidates)
        {
            std::wstring path(exeDirBuf);
            if (!path.empty() && path.back() != L'\\') path += L'\\';
            path += name;
            if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
            {
                LOGI("DIAG MSR driver found: %s", wxString(path.c_str()).mb_str().data());
                return path;
            }
        }
        return L"";
    }

    static bool OpenDriver(const wchar_t* exeDir)
    {
        std::wstring drvPath = FindDriver(exeDir);
        if (drvPath.empty())
        {
            LOGI("DIAG MSR: no .sys driver found, falling back to WMI.");
            return false;
        }

        s_hScm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
        if (!s_hScm)
        {
            LOGI("DIAG MSR: OpenSCManager failed err=%lu", GetLastError());
            return false;
        }

        // Try well-known service names first (may be installed by RealTemp etc.).
        for (const auto* knownName : kKnownSvcNames)
        {
            s_hSvc = OpenServiceW(s_hScm, knownName, SERVICE_ALL_ACCESS);
            if (s_hSvc)
            {
                if (!StartServiceW(s_hSvc, 0, nullptr))
                {
                    DWORD err = GetLastError();
                    if (err != ERROR_SERVICE_ALREADY_RUNNING)
                    {
                        CloseServiceHandle(s_hSvc);
                        s_hSvc = nullptr;
                        continue;
                    }
                }
                LOGI("DIAG MSR: using existing service %S", knownName);
                break;
            }
        }

        if (!s_hSvc)
        {
            // Create from .sys file.
            std::wstring fileName = drvPath.substr(drvPath.find_last_of(L'\\') + 1);
            auto dot = fileName.find(L'.');
            std::wstring svcName = (dot != std::wstring::npos)
                ? fileName.substr(0, dot) : fileName;
            for (auto& c : svcName)
                if (!isalnum(static_cast<unsigned char>(c)) && c != L'_')
                    c = L'_';
            if (svcName.empty()) svcName = L"MsrDriver";

            s_hSvc = CreateServiceW(s_hScm, svcName.c_str(), svcName.c_str(),
                                    SERVICE_ALL_ACCESS, SERVICE_KERNEL_DRIVER,
                                    SERVICE_DEMAND_START, SERVICE_ERROR_NORMAL,
                                    drvPath.c_str(), nullptr, nullptr, nullptr,
                                    nullptr, nullptr);
            DWORD err = GetLastError();
            if (s_hSvc)
            {
                if (!StartServiceW(s_hSvc, 0, nullptr))
                {
                    err = GetLastError();
                    if (err != ERROR_SERVICE_ALREADY_RUNNING)
                    {
                        LOGI("DIAG MSR: StartService failed err=%lu", err);
                        ::DeleteService(s_hSvc);
                        CloseServiceHandle(s_hSvc);
                        s_hSvc = nullptr;
                        CloseServiceHandle(s_hScm);
                        s_hScm = nullptr;
                        return false;
                    }
                }
            }
            else
            {
                if (err == ERROR_SERVICE_EXISTS || err == ERROR_SERVICE_MARKED_FOR_DELETE)
                {
                    s_hSvc = OpenServiceW(s_hScm, svcName.c_str(), SERVICE_ALL_ACCESS);
                    if (!s_hSvc)
                    {
                        LOGI("DIAG MSR: OpenService failed err=%lu", GetLastError());
                        CloseServiceHandle(s_hScm);
                        s_hScm = nullptr;
                        return false;
                    }
                    if (!StartServiceW(s_hSvc, 0, nullptr))
                    {
                        err = GetLastError();
                        if (err != ERROR_SERVICE_ALREADY_RUNNING)
                        {
                            LOGI("DIAG MSR: StartService(existing) failed err=%lu", err);
                            CloseServiceHandle(s_hSvc);
                            CloseServiceHandle(s_hScm);
                            s_hSvc = nullptr;
                            s_hScm = nullptr;
                            return false;
                        }
                    }
                }
                else
                {
                    LOGI("DIAG MSR: CreateService failed err=%lu", err);
                    CloseServiceHandle(s_hScm);
                    s_hScm = nullptr;
                    return false;
                }
            }
        }

        for (const auto* devName : kDeviceNames)
        {
            s_hDevice = CreateFileW(devName,
                                    GENERIC_READ | GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, OPEN_EXISTING, 0, nullptr);
            if (s_hDevice != INVALID_HANDLE_VALUE)
            {
                LOGI("DIAG MSR: driver open OK via %S", wxString(devName).mb_str().data());
                s_drvOpened = true;
                return true;
            }
        }

        LOGI("DIAG MSR: CreateFile failed all device names err=%lu", GetLastError());
        // Fall through — caller will try DLL backend.
        if (s_hDevice != INVALID_HANDLE_VALUE) { CloseHandle(s_hDevice); s_hDevice = INVALID_HANDLE_VALUE; }
        if (s_hSvc)   { ControlService(s_hSvc, SERVICE_CONTROL_STOP, nullptr); ::DeleteService(s_hSvc); CloseServiceHandle(s_hSvc); s_hSvc = nullptr; }
        if (s_hScm)   { CloseServiceHandle(s_hScm); s_hScm = nullptr; }
        s_drvOpened = false;
        return false;
    }

    // --- Public API -----------------------------------------------------------

    bool Open(const wchar_t* exeDir)
    {
        if (s_backend != Backend::None) return true;

        // Try DLL backend first (RealTemp's WinRing0x64.dll).
        if (OpenDll(exeDir))
            return true;

        // Fallback to direct driver.
        if (OpenDriver(exeDir))
        {
            s_backend = Backend::Driver;
            return true;
        }

        LOGI("DIAG MSR: all backends failed, using WMI fallback.");
        return false;
    }

    bool ReadMsr(uint32_t msr, uint32_t& outLo, uint32_t& outHi)
    {
        if (s_backend == Backend::Dll && s_fnRdmsr)
        {
            uint32_t eax = 0, edx = 0;
            if (s_fnRdmsr(msr, &eax, &edx))
            {
                outLo = eax;
                outHi = edx;
                return true;
            }
            return false;
        }

        if (s_backend != Backend::Driver || !s_drvOpened ||
            s_hDevice == INVALID_HANDLE_VALUE)
            return false;

        uint32_t  inp   = msr;
        uint64_t  out64 = 0;
        DWORD     bytesReturned = 0;

        if (!DeviceIoControl(s_hDevice, kIoctlReadMsr,
                             &inp, sizeof(inp),
                             &out64, sizeof(out64),
                             &bytesReturned, nullptr))
        {
            LOGI("DIAG MSR: DeviceIoControl failed err=%lu", GetLastError());
            return false;
        }

        outLo = static_cast<uint32_t>(out64 & 0xFFFFFFFF);
        outHi = static_cast<uint32_t>((out64 >> 32) & 0xFFFFFFFF);
        return true;
    }

    void Close()
    {
        if (s_backend == Backend::Dll && s_fnDeinit)
            s_fnDeinit();

        if (s_hDevice != INVALID_HANDLE_VALUE)
        {
            CloseHandle(s_hDevice);
            s_hDevice = INVALID_HANDLE_VALUE;
        }
        if (s_hSvc)
        {
            ControlService(s_hSvc, SERVICE_CONTROL_STOP, nullptr);
            ::DeleteService(s_hSvc);
            CloseServiceHandle(s_hSvc);
            s_hSvc = nullptr;
        }
        if (s_hScm)
        {
            CloseServiceHandle(s_hScm);
            s_hScm = nullptr;
        }
        if (s_hDll)
        {
            FreeLibrary(s_hDll);
            s_hDll = nullptr;
        }
        s_fnInit  = nullptr;
        s_fnDeinit = nullptr;
        s_fnRdmsr = nullptr;
        s_fnIsMsr = nullptr;
        s_drvOpened = false;
        s_backend = Backend::None;
    }

    bool IsOpen() { return s_backend != Backend::None; }
}

#endif // PLATFORM_WINDOWS
