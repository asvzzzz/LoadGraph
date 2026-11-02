#include "cpu_monitor.h"
#include "logger.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <objbase.h>   // CoInitializeEx, CoCreateInstance, CoSetProxyBlanket
#include <oleauto.h>   // SysAllocString, VariantInit, VariantClear
#include <wbemidl.h>   // IWbemLocator / IWbemServices / CLSID_WbemLocator
#include <pdh.h>
#include <pdhmsg.h>    // PDH_CSTATUS_VALID_DATA / PDH_CSTATUS_NEW_DATA

#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "wbemuuid.lib")

// ----------------------------------------------------------------------------
// Raw WMI helper.
//
// Runs a single WQL query against a namespace and reads the first numeric
// property of the first returned object. Everything is done with raw pointers
// and BSTRs (no C++ objects with destructors) so the whole body can be wrapped
// in __try/__except. That guard is essential: on some systems querying WMI
// thermal sensors raises an access violation inside fastprox.dll, which would
// otherwise crash the whole application.
//
// Returns true and fills outValue on success.
// ----------------------------------------------------------------------------
static bool QueryWmiValueRaw(const wchar_t* ns, const wchar_t* query,
                             const wchar_t* prop, double& outValue)
{
    outValue = -1.0;

    IWbemLocator*            pLoc  = nullptr;
    IWbemServices*           pSvc  = nullptr;
    IEnumWbemClassObject*    pEnum = nullptr;
    IWbemClassObject*        pObj  = nullptr;
    bool                     ok    = false;
    bool                     comInit = false;

    __try
    {
        HRESULT hres = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        // RPC_E_CHANGED_MODE means COM is already initialized differently on
        // this thread; that is fine, we just must not uninitialize it.
        comInit = SUCCEEDED(hres);

        // Best-effort security setup - ignored if already configured.
        CoInitializeSecurity(nullptr, -1, nullptr, nullptr,
                             RPC_C_AUTHN_LEVEL_DEFAULT,
                             RPC_C_IMP_LEVEL_IMPERSONATE,
                             nullptr, EOAC_NONE, nullptr);

        hres = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IWbemLocator,
                                reinterpret_cast<LPVOID*>(&pLoc));
        if (SUCCEEDED(hres) && pLoc)
        {
            BSTR bNs = SysAllocString(ns);
            hres = pLoc->ConnectServer(bNs, nullptr, nullptr, nullptr, 0,
                                       nullptr, nullptr, &pSvc);
            SysFreeString(bNs);

            if (SUCCEEDED(hres) && pSvc)
            {
                CoSetProxyBlanket(pSvc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE,
                                  nullptr, RPC_C_AUTHN_LEVEL_CALL,
                                  RPC_C_IMP_LEVEL_IMPERSONATE, nullptr,
                                  EOAC_NONE);

                BSTR bQ = SysAllocString(query);
                hres = pSvc->ExecQuery(BSTR(L"WQL"), bQ,
                                       WBEM_FLAG_FORWARD_ONLY |
                                       WBEM_FLAG_RETURN_IMMEDIATELY,
                                       nullptr, &pEnum);
                SysFreeString(bQ);

                if (SUCCEEDED(hres) && pEnum)
                {
                    ULONG returned = 0;
                    if (pEnum->Next(WBEM_INFINITE, 1, &pObj, &returned) == S_OK &&
                        returned > 0 && pObj)
                    {
                        VARIANT vt;
                        VariantInit(&vt);
                        BSTR bProp = SysAllocString(prop);
                        hres = pObj->Get(bProp, 0, &vt, nullptr, nullptr);
                        SysFreeString(bProp);

                        if (SUCCEEDED(hres))
                        {
                            if (vt.vt == VT_I4)
                            {
                                outValue = static_cast<double>(vt.lVal);
                                ok = true;
                            }
                            else if (vt.vt == VT_R8)
                            {
                                outValue = vt.dblVal;
                                ok = true;
                            }
                            else if (vt.vt == VT_UI4)
                            {
                                outValue = static_cast<double>(vt.ulVal);
                                ok = true;
                            }
                        }
                        VariantClear(&vt);
                    }
                }
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        // fastprox.dll (or another WMI component) crashed. Disable WMI.
        ok = false;
        outValue = -1.0;
    }

    // Best-effort cleanup (skipped if we took the exception path).
    if (pObj)  pObj->Release();
    if (pEnum) pEnum->Release();
    if (pSvc)  pSvc->Release();
    if (pLoc)  pLoc->Release();
    if (comInit) CoUninitialize();

    return ok;
}

CpuMonitor::CpuMonitor()
{
}

CpuMonitor::~CpuMonitor()
{
    if (m_pdhQuery)
    {
        PdhCloseQuery(m_pdhQuery);
        m_pdhQuery = nullptr;
        m_pdhCounter = nullptr;
    }
}

CpuMetrics CpuMonitor::GetMetrics()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_metrics;
}

// Sample the CPU load right now via PDH. This is called from the GUI thread
// once per tick so the displayed value has no phase lag relative to Task
// Manager (which reads the same PDH counter).
double CpuMonitor::SampleLoad()
{
    double load = -1.0;

    if (!m_pdhTried)
    {
        m_pdhTried = true;
        // PdhAddEnglishCounterW is locale independent (always "\Processor(_Total)\%...").
        if (PdhOpenQueryW(nullptr, 0, &m_pdhQuery) == ERROR_SUCCESS &&
            PdhAddEnglishCounterW(m_pdhQuery,
                                  L"\\Processor(_Total)\\% Processor Time",
                                  0, &m_pdhCounter) == ERROR_SUCCESS)
        {
            m_pdhOk = true;
            // Prime the counter: the first collect only establishes a baseline.
            PdhCollectQueryData(m_pdhQuery);
            LOGI("PDH CPU counter initialized.");
            // Reading right now would measure a ~0 ms window and return a
            // bogus spike (typically 100%). Skip the value on this first tick.
            return -1.0;
        }
        else
        {
            m_pdhOk = false;
            LOGI("PDH CPU counter FAILED (err=%lu), using GetSystemTimes fallback.",
                 GetLastError());
            if (m_pdhQuery) { PdhCloseQuery(m_pdhQuery); m_pdhQuery = nullptr; }
            m_pdhCounter = nullptr;
        }
    }

    if (m_pdhOk && m_pdhQuery && m_pdhCounter)
    {
        if (PdhCollectQueryData(m_pdhQuery) == ERROR_SUCCESS)
        {
            PDH_FMT_COUNTERVALUE fmt{};
            if (PdhGetFormattedCounterValue(m_pdhCounter, PDH_FMT_DOUBLE,
                                            nullptr, &fmt) == ERROR_SUCCESS &&
                (fmt.CStatus == PDH_CSTATUS_VALID_DATA ||
                 fmt.CStatus == PDH_CSTATUS_NEW_DATA))
            {
                load = fmt.doubleValue;
                if (load < 0.0)   load = 0.0;
                if (load > 100.0) load = 100.0;
            }
        }
    }
    else
    {
        // Fallback path (no PDH).
        QueryOverallLoad(load);
    }

    if (load >= 0.0)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_metrics.loadPercent = load;
    }

    return load;
}

void CpuMonitor::Collect()
{
    // Temperature / fan: probe WMI once (guarded). If it fails or crashes we
    // permanently disable it and keep showing "N/A".
    double temp = -1.0;
    int    fan  = -1;
    unsigned int filled = 0;

    if (!m_wmiTried)
    {
        m_wmiTried = true;
        filled = ProbeWmiSensor(temp, fan);
        m_wmiOk = (filled != 0);
        LOGI("WMI sensor probe: temp=%s fan=%s",
             (filled & 1u) ? "OK" : "N/A",
             (filled & 2u) ? "OK" : "N/A");
    }

    // All writes to m_metrics must hold the mutex: GetMetrics() reads it from
    // the GUI thread (data race otherwise).
    std::lock_guard<std::mutex> lock(m_mutex);
    m_metrics.uptimeMs = GetTickCount64();
    if (filled & 1u) m_metrics.temperature = temp;
    if (filled & 2u) m_metrics.fanRpm      = fan;
}

// Legacy GetSystemTimes-based load, kept as a fallback when PDH is unavailable.
bool CpuMonitor::QueryOverallLoad(double& loadPercent)
{
    FILETIME idle{}, kernel{}, user{};
    if (!GetSystemTimes(&idle, &kernel, &user))
        return false;

    auto toUll = [](FILETIME ft) -> ULONGLONG {
        return (static_cast<ULONGLONG>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    };

    const ULONGLONG curIdle   = toUll(idle);
    const ULONGLONG curKernel = toUll(kernel);
    const ULONGLONG curUser   = toUll(user);

    if (!m_prevTimesValid)
    {
        // First call: just store the baseline.
        m_prevIdle       = curIdle;
        m_prevKernel     = curKernel;
        m_prevUser       = curUser;
        m_prevTimesValid = true;
        loadPercent = 0.0;
        return true;
    }

    // GetSystemTimes() kernel time INCLUDES idle time.
    //   Total = kernel + user
    //   Busy  = (kernel - idle) + user
    const ULONGLONG idleDelta   = curIdle   - m_prevIdle;
    const ULONGLONG kernelDelta = curKernel - m_prevKernel;
    const ULONGLONG userDelta   = curUser   - m_prevUser;

    const ULONGLONG busy  = kernelDelta - idleDelta + userDelta;
    const ULONGLONG total = kernelDelta + userDelta;

    loadPercent = (total == 0) ? 0.0
        : static_cast<double>(busy) / static_cast<double>(total) * 100.0;

    m_prevIdle   = curIdle;
    m_prevKernel = curKernel;
    m_prevUser   = curUser;
    return true;
}

// Query CPU temperature and fan via WMI. Two sources are tried:
//   1. root\wmi MSAcpi_ThermalZoneTemperature  (temperature only, often admin)
//   2. root\OpenHardwareMonitor Sensor          (temp + fan, needs LHM running)
// Returns bitmask: bit0 = temperature, bit1 = fan.
unsigned int CpuMonitor::ProbeWmiSensor(double& tempC, int& fanRpm)
{
    unsigned int filled = 0;
    tempC = -1.0;
    fanRpm = -1;

    // --- Source 1: ACPI thermal zone (temperature in milli-Kelvin) ---
    double raw = -1.0;
    if (QueryWmiValueRaw(L"root\\wmi",
                         L"SELECT CurrentTemperature FROM MSAcpi_ThermalZoneTemperature",
                         L"CurrentTemperature", raw) && raw > 0.0)
    {
        // Convert milli-Kelvin to Celsius.
        tempC = raw / 1000.0 - 273.15;
        if (tempC > 0.0 && tempC < 150.0)
        {
            filled |= 1u;
        }
        else
        {
            tempC = -1.0;
        }
    }

    // --- Source 2: OpenHardwareMonitor / LibreHardwareMonitor WMI namespace ---
    // Temperature (skip if already obtained).
    if (!(filled & 1u))
    {
        double v = -1.0;
        if (QueryWmiValueRaw(L"root\\OpenHardwareMonitor",
                             L"SELECT Value FROM Sensor WHERE SensorType='Temperature' LIMIT 1",
                             L"Value", v) && v > 0.0 && v < 150.0)
        {
            tempC = v;
            filled |= 1u;
        }
    }

    // Fan speed (RPM).
    {
        double v = -1.0;
        if (QueryWmiValueRaw(L"root\\OpenHardwareMonitor",
                             L"SELECT Value FROM Sensor WHERE SensorType='Fan' LIMIT 1",
                             L"Value", v) && v > 0.0)
        {
            fanRpm = static_cast<int>(v + 0.5);
            filled |= 2u;
        }
    }

    return filled;
}

// Read the CPU model name from registry (HKLM\HARDWARE\DESCRIPTION\System\CentralProcessor\0\ProcessorNameString).
// This is fast and doesn't block the UI thread like WMI can.
void CpuMonitor::QueryCpuName()
{
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                      0, KEY_READ, &hKey) == ERROR_SUCCESS)
    {
        wchar_t name[256] = {};
        DWORD type = 0, size = sizeof(name);
        if (RegQueryValueExW(hKey, L"ProcessorNameString", nullptr, &type,
                             reinterpret_cast<LPBYTE>(name), &size) == ERROR_SUCCESS &&
            type == REG_SZ && name[0])
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_metrics.cpuName = name;
            LOGI("CPU name: %S", name);
        }
        RegCloseKey(hKey);
    }
}
