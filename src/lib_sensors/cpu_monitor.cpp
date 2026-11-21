#include "cpu_monitor.h"
#include "msr_sensor.h"
#include "ring0_msr.h"
#include "power_table.h"
#include "logger.h"
#include <cstdio>    // snprintf for the diagnostic buffers

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <new>       // std::nothrow for heap allocation without exceptions
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

    // Heap-allocated context so the compiler cannot clobber these values across
    // the SEH boundary (MSVC docs flag register-clobbering as UB for __try/__except).
    struct Context {
        IWbemLocator*            pLoc  = nullptr;
        IWbemServices*           pSvc  = nullptr;
        IEnumWbemClassObject*    pEnum = nullptr;
        IWbemClassObject*        pObj  = nullptr;
        bool                     ok    = false;
        bool                     comInit = false;
    };

    Context* ctx = new (std::nothrow) Context();
    if (!ctx) return false;

    __try
    {
        HRESULT hres = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        // RPC_E_CHANGED_MODE means COM is already initialized differently on
        // this thread; that is fine, we just must not uninitialize it.
        ctx->comInit = SUCCEEDED(hres);

        // Best-effort security setup - ignored if already configured.
        CoInitializeSecurity(nullptr, -1, nullptr, nullptr,
                             RPC_C_AUTHN_LEVEL_DEFAULT,
                             RPC_C_IMP_LEVEL_IMPERSONATE,
                             nullptr, EOAC_NONE, nullptr);

        hres = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IWbemLocator,
                                reinterpret_cast<LPVOID*>(&ctx->pLoc));
        if (SUCCEEDED(hres) && ctx->pLoc)
        {
            BSTR bNs = SysAllocString(ns);
            hres = ctx->pLoc->ConnectServer(bNs, nullptr, nullptr, nullptr, 0,
                                       nullptr, nullptr, &ctx->pSvc);
            SysFreeString(bNs);

            if (SUCCEEDED(hres) && ctx->pSvc)
            {
                CoSetProxyBlanket(ctx->pSvc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE,
                                  nullptr, RPC_C_AUTHN_LEVEL_CALL,
                                  RPC_C_IMP_LEVEL_IMPERSONATE, nullptr,
                                  EOAC_NONE);

                BSTR bQ = SysAllocString(query);
                hres = ctx->pSvc->ExecQuery(BSTR(L"WQL"), bQ,
                                            WBEM_FLAG_FORWARD_ONLY |
                                            WBEM_FLAG_RETURN_IMMEDIATELY,
                                            nullptr, &ctx->pEnum);
                SysFreeString(bQ);

                if (SUCCEEDED(hres) && ctx->pEnum)
                {
                    ULONG returned = 0;
                    if (ctx->pEnum->Next(WBEM_INFINITE, 1, &ctx->pObj, &returned) == S_OK &&
                        returned > 0 && ctx->pObj)
                    {
                        VARIANT vt;
                        VariantInit(&vt);
                        BSTR bProp = SysAllocString(prop);
                        hres = ctx->pObj->Get(bProp, 0, &vt, nullptr, nullptr);
                        SysFreeString(bProp);

                        if (SUCCEEDED(hres))
                        {
                            if (vt.vt == VT_I4)
                            {
                                outValue = static_cast<double>(vt.lVal);
                                ctx->ok = true;
                            }
                            else if (vt.vt == VT_R8)
                            {
                                outValue = vt.dblVal;
                                ctx->ok = true;
                            }
                            else if (vt.vt == VT_UI4)
                            {
                                outValue = static_cast<double>(vt.ulVal);
                                ctx->ok = true;
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
        ctx->ok = false;
        outValue = -1.0;
    }

    // Best-effort cleanup (skipped if we took the exception path).
    if (ctx->pObj)  ctx->pObj->Release();
    if (ctx->pEnum) ctx->pEnum->Release();
    if (ctx->pSvc)  ctx->pSvc->Release();
    if (ctx->pLoc)  ctx->pLoc->Release();
    if (ctx->comInit) CoUninitialize();

    // Read ok BEFORE delete - reading ctx->ok after delete is use-after-free.
    const bool ok = ctx->ok;
    delete ctx;
    return ok;
}

// WQL for the CPU package power sensor exposed by LibreHardwareMonitor /
// OpenHardwareMonitor. NB: WQL has no LIMIT clause - the first row of the
// result set is taken instead.
static const wchar_t kPowerSensorQuery[] =
    L"SELECT Value FROM Sensor WHERE SensorType='Power' AND Name='CPU Package'";

// Detect the number of physical CPU cores via GetLogicalProcessorInformation.
// Used by the idle-floor power estimate (P_idle scales with core count).
// The XP-era API is used on purpose: it needs no _WIN32_WINNT >= 0x0601 guard,
// so the code compiles with both MSVC and MinGW out of the box.
// Returns 0 on failure (caller falls back to 25% of TDP).
static uint32_t DetectPhysicalCoreCount()
{
    DWORD len = 0;
    // First call only queries the required buffer size and fails with
    // ERROR_INSUFFICIENT_BUFFER (a TRUE return would mean "no processors").
    if (GetLogicalProcessorInformation(nullptr, &len) ||
        GetLastError() != ERROR_INSUFFICIENT_BUFFER || len == 0)
        return 0;

    // +1 element: the required size may grow between the two calls.
    const size_t capacity = len / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION) + 1;
    std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> info(capacity);
    if (!GetLogicalProcessorInformation(info.data(), &len))
        return 0;

    // One RelationProcessorCore entry per physical core (hyperthread
    // siblings share the same entry).
    uint32_t cores = 0;
    const size_t usable = len / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION);
    for (size_t i = 0; i < usable && i < capacity; ++i)
    {
        if (info[i].Relationship == RelationProcessorCore)
            ++cores;
    }
    // Systems with > 64 logical processors are not covered by this API;
    // returning 0 makes the estimate use the 25% TDP fallback.
    return cores;
}

CpuMonitor::CpuMonitor()
{
}

CpuMonitor::~CpuMonitor()
{
    MsrSensor::Shutdown();
    if (m_pdhQuery)
    {
        PdhCloseQuery(m_pdhQuery);
        m_pdhQuery = nullptr;
        m_pdhCounter = nullptr;
    }
}

void CpuMonitor::GetMetrics(CpuMetrics& out)
{
    // Copy fields individually under the lock. We must NOT use a temporary
    // and assign it to `out` because NRVO may alias `tmp` with `out`, causing
    // the destructor of `tmp` (which runs when GetMetrics returns) to free
    // memory that `out` still needs. Copying field-by-field avoids this trap.
    std::lock_guard<std::mutex> lock(m_mutex);
    out.loadPercent   = m_metrics.loadPercent;
    out.temperature   = m_metrics.temperature;
    out.fanRpm        = m_metrics.fanRpm;
    out.uptimeMs      = m_metrics.uptimeMs;
#ifdef PLATFORM_WINDOWS
    wcsncpy(out.cpuName, m_metrics.cpuName, sizeof(out.cpuName) / sizeof(wchar_t) - 1);
#else
    strncpy(out.cpuName, m_metrics.cpuName, sizeof(out.cpuName) - 1);
#endif
    out.powerWatts    = m_metrics.powerWatts;
    out.tdpWatts      = m_metrics.tdpWatts;
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
        // CPU power: a real sensor (LHM WMI) wins; otherwise use the idle-floor
        // estimate P_idle + load% * (TDP - P_idle). Plain load x TDP reported
        // ~1 W at idle because a loaded-out CPU still draws static power.
        if (!m_powerReal.load(std::memory_order_relaxed))
        {
            const double tdp = (m_metrics.tdpWatts > 0.0) ? m_metrics.tdpWatts : 125.0;
            m_metrics.powerWatts = PowerTable::EstimateCpuPowerWatts(
                load,
                static_cast<uint32_t>(tdp + 0.5),
                m_coreCount.load(std::memory_order_relaxed));
        }
    }

    return load;
}

// Forward declaration: probes every non-admin temp source on startup.
static void ProbeAllTempSources();
// Forward declaration: identifies the Super I/O chip and returns the I/O base
// of its hardware monitor window, or 0 when no supported chip answers.
static uint16_t ProbeSuperIoHwm();
// Forward declaration: decodes the fan tachometers of that window.
static int ReadSuperIoFanRpm(uint16_t base, bool verbose);
// Slow retry period of the full WMI probe in polling ticks (1 tick = 1000 ms).
// A single WMI round trip per 30 s is unmeasurable for the CPU budget and lets
// a LibreHardwareMonitor instance started after LoadGraph fill the fan row.
static constexpr int kWmiRetryTicks = 30;
// Re-read period of the Super I/O fan counters in polling ticks.
static constexpr int kSuperIoRefreshTicks = 5;

void CpuMonitor::Collect()
{
    // CPU load - PDH delta sample once per tick, same cadence as the polling loop.
    // SampleLoad writes loadPercent and powerWatts into m_metrics under its own lock;
    // calling it here (instead of from a separate timer thread) keeps the delta
    // calculation single-threaded and avoids race conditions.
    (void)SampleLoad();

    // Temperature / fan: try MSR (kernel driver or direct rdmsr) first, then WMI.
    double temp = -1.0;
    int    fan  = -1;
    unsigned int filled = 0;

    // --- MSR path ----------------------------------------------------------
    if (!m_msrTried)
    {
        m_msrTried = true;
        const double msrTemp = MsrSensor::ProbeTemperature(
            m_msrFamily, m_msrModel, m_msrStepping, m_msrTjmax);
        if (msrTemp >= 0.0)
        {
            temp       = msrTemp;
            m_msrOk    = true;
            filled     = 1u; // bit0 = temperature present.
            LOGI("MSR sensor probe: temp=%.1f C", msrTemp);
        }
        else
        {
            LOGI("MSR sensor probe: unavailable (driver absent or MSR blocked)");
        }
    }
    else if (m_msrOk)
    {
        if (m_msrCooldown <= 0)
        {
            const double msrTemp = MsrSensor::ProbeTemperature(
                m_msrFamily, m_msrModel, m_msrStepping, m_msrTjmax);
            if (msrTemp >= 0.0)
            {
                temp = msrTemp;
                m_lastMsrTemp = msrTemp; // save for cooldown restore
            }
            else
            {
                m_msrOk = false;
                LOGI("MSR sensor lost, falling back to WMI.");
            }
            m_msrCooldown = 5;
        }
        else
        {
            --m_msrCooldown;
            temp = m_lastMsrTemp; // restore last known value, avoid spurious WMI fallback
        }
        if (temp >= 0.0) filled = 1u;
    }

    // --- WMI sensors -------------------------------------------------------
    // Independent of the MSR path: a machine can have a working MSR
    // temperature and still need WMI for the fan, because LibreHardwareMonitor
    // and OpenHardwareMonitor are the only source that exposes fan RPM to a
    // non-admin process. The previous gate looked at the temperature bit only,
    // so whenever MSR worked the fan query never ran at all.
    if (!m_wmiTried && (filled & 3u) != 3u)
    {
        m_wmiTried = true;
        m_wmiRetry = kWmiRetryTicks;
        filled = RefreshWmiSensors(temp, fan, filled);
        m_wmiOk = (m_wmiMask != 0);
        LOGI("WMI sensor probe: temp=%hs fan=%hs",
             (m_wmiMask & 1u) ? "OK" : "N/A",
             (m_wmiMask & 2u) ? "OK" : "N/A");
        // Probe every available non-admin source so we know what works on this machine.
        ProbeAllTempSources();
    }
    else if (m_wmiTried)
    {
        // WMI owns a sensor when its last probe supplied one. The fan RPM is
        // dynamic and the WMI temperature is only useful while MSR is down, so
        // either case keeps the 5 s refresh. While some sensor is still
        // missing the full probe is retried on the slow 30 s cycle instead.
        const bool ownsFan  = (m_wmiMask & 2u) != 0;
        const bool ownsTemp = (m_wmiMask & 1u) != 0 && !m_msrOk;
        const bool missing  = (filled & 3u) != 3u;
        const bool owns     = ownsFan || ownsTemp;
        if ((owns || missing) && m_wmiCooldown <= 0)
        {
            const unsigned int before = filled;
            filled = RefreshWmiSensors(temp, fan, filled);
            m_wmiOk       = (m_wmiMask != 0);
            m_wmiCooldown = owns ? 5 : kWmiRetryTicks;
            // Only report a change: the retry cycle must not add log lines
            // while the machine simply has no fan sensor at all.
            if ((filled & 2u) && !(before & 2u))
                LOGI("WMI fan sensor appeared: %d RPM", fan);
            if ((filled & 1u) && !(before & 1u))
                LOGI("WMI temperature sensor appeared: %.1f C", temp);
        }
        else if (owns || missing)
        {
            --m_wmiCooldown;
        }
    }

    // --- Super I/O fan probe (WinRing0 I/O port access) --------------------
    // The chip is identified once per session; its tachometers are then
    // re-read on a 5 s cadence because RPM is dynamic. Disabled by default -
    // direct port I/O can affect unknown LPC devices. Enable it in
    // loadgraph.cfg ("superio_fan": true) or from the View menu, which pushes
    // the request through MetricsSetSuperIoFanProbe().
    //
    // m_superIoEnableReq is the only probe field another thread writes. When it
    // differs from what this thread already applied, all probe state owned by
    // this thread is dropped here and the identification runs again.
    const bool wantProbe = m_superIoEnableReq.load(std::memory_order_relaxed);
    if (wantProbe != m_superIoApplied)
    {
        m_superIoApplied = wantProbe;
        m_fanIoTried     = false;
        m_fanIoOk        = false;
        m_fanIoBase      = 0;
        m_fanIoCool      = 0;
        m_fanIoLogged    = false;
        LOGI("Super I/O fan probe %s.", wantProbe ? "enabled" : "disabled");
        if (!wantProbe && m_fanIoLastRpm >= 0)
        {
            // The RPM on screen came from the probe that was just switched
            // off, and WMI does not own the fan: report N/A instead of keeping
            // a frozen value in the chart forever.
            m_fanIoLastRpm = -1;
            if (!(m_wmiMask & 2u))
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_metrics.fanRpm = -1;
            }
        }
        else
        {
            m_fanIoLastRpm = -1;
        }
    }

    if (wantProbe && fan < 0 && Ring0Msr::IsOpen() && !m_fanIoTried)
    {
        m_fanIoTried = true;
        m_fanIoBase = ProbeSuperIoHwm();
        m_fanIoOk   = (m_fanIoBase != 0);
    }
    if (m_fanIoOk && fan < 0)
    {
        if (m_fanIoCool <= 0)
        {
            m_fanIoCool = kSuperIoRefreshTicks;
            // The channel table is logged once, later reads stay quiet unless
            // the sensor appears or disappears.
            const int ioFan = ReadSuperIoFanRpm(m_fanIoBase, !m_fanIoLogged);
            m_fanIoLogged = true;
            if (ioFan > 0)
            {
                fan = ioFan;
                filled |= 2u;
                if (m_fanIoLastRpm < 0)
                    LOGI("Super I/O fan sensor: %d RPM", fan);
            }
            else if (m_fanIoLastRpm >= 0)
            {
                LOGI("Super I/O fan sensor lost - no tachometer data.");
            }
            m_fanIoLastRpm = ioFan;
        }
        else
        {
            --m_fanIoCool;
        }
    }

    // Optional real CPU package power from LibreHardwareMonitor /
    // OpenHardwareMonitor. The namespace is probed once (guarded, SEH-safe
    // QueryWmiValueRaw); while the sensor lives its value is read every tick
    // and suppresses the estimate (m_powerReal). If LHM is closed mid-run the
    // code falls back to the idle-floor estimate.
    double power = -1.0;
    if (!m_powerLhmTried)
    {
        m_powerLhmTried = true;
        static const wchar_t* kPowerNs[] = { L"root\\LHM", L"root\\OpenHardwareMonitor" };
        const size_t nsCount = sizeof(kPowerNs) / sizeof(kPowerNs[0]);
        for (size_t i = 0; i < nsCount && !m_powerLhmOk; ++i)
        {
            double v = -1.0;
            if (QueryWmiValueRaw(kPowerNs[i], kPowerSensorQuery, L"Value", v) && v > 0.0)
            {
                m_powerLhmNs  = kPowerNs[i];
                m_powerLhmOk  = true;
                power         = v;
                LOGI("CPU power sensor found (%s): %.1f W", kPowerNs[i], v);
            }
        }
        if (!m_powerLhmOk)
            LOGI("CPU power sensor unavailable - using idle-floor estimate.");
    }
    else if (m_powerLhmOk)
    {
        double v = -1.0;
        if (QueryWmiValueRaw(m_powerLhmNs.c_str(), kPowerSensorQuery, L"Value", v) && v > 0.0)
        {
            power = v;
        }
        else
        {
            m_powerLhmOk = false;
            m_powerReal.store(false, std::memory_order_relaxed);
            LOGI("CPU power sensor lost - using idle-floor estimate.");
        }
    }

    // All writes to m_metrics must hold the mutex: GetMetrics() reads it from
    // the GUI thread (data race otherwise).
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_metrics.uptimeMs = GetTickCount64();
        if (filled & 1u) m_metrics.temperature = temp;
        if (filled & 2u) m_metrics.fanRpm      = fan;
        if (power > 0.0)
        {
            m_metrics.powerWatts = power;
            m_powerReal.store(true, std::memory_order_relaxed);
        }
    }
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
    // Temperature (skip if already obtained). Prefer the CPU package sensor so
    // a GPU temperature row cannot be picked up as CPU temperature. WQL has no
    // LIMIT clause - the first row of the result set is taken instead.
    if (!(filled & 1u))
    {
        double v = -1.0;
        if (QueryWmiValueRaw(L"root\\OpenHardwareMonitor",
                             L"SELECT Value FROM Sensor WHERE SensorType='Temperature' AND Name='CPU Package'",
                             L"Value", v) && v > 0.0 && v < 150.0)
        {
            tempC = v;
            filled |= 1u;
        }
        else if (QueryWmiValueRaw(L"root\\OpenHardwareMonitor",
                                  L"SELECT Value FROM Sensor WHERE SensorType='Temperature'",
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
                             L"SELECT Value FROM Sensor WHERE SensorType='Fan'",
                             L"Value", v) && v > 0.0)
        {
            fanRpm = static_cast<int>(v + 0.5);
            filled |= 2u;
        }
    }

    return filled;
}

// Merge helper for the WMI sensors. Runs ProbeWmiSensor() and takes over only
// the values that are still missing, so a temperature already supplied by the
// MSR path is never replaced by a coarser WMI reading.
unsigned int CpuMonitor::RefreshWmiSensors(double& tempC, int& fanRpm,
                                           unsigned int filled)
{
    double wmiTemp = -1.0;
    int    wmiFan  = -1;
    const unsigned int w = ProbeWmiSensor(wmiTemp, wmiFan);
    m_wmiMask = w;
    if ((w & 1u) && !(filled & 1u)) tempC  = wmiTemp;
    if ((w & 2u) && !(filled & 2u)) fanRpm = wmiFan;
    return filled | w;
}

// Runtime switch of the Super I/O tachometer probe. The caller (startup code or
// the View menu of the GUI) only publishes the request; Collect() of the
// polling thread applies it and reinitializes the probe state there, so the
// state needs no lock and can never be observed half updated.
void CpuMonitor::SetSuperIoFanProbe(bool enable)
{
    m_superIoEnableReq.store(enable, std::memory_order_relaxed);
}

bool CpuMonitor::IsSuperIoFanProbeEnabled() const
{
    return m_superIoEnableReq.load(std::memory_order_relaxed);
}

// ----------------------------------------------------------------------------
// Nuvoton NCT679x Super I/O access through WinRing0 port I/O.
//
// The LPC config interface is an index/data port pair (0x2E and 0x2F, or 0x4E
// and 0x4F). Writing the key byte 0x87 twice enters config mode, after which a
// config register is read by writing its number to the index port and reading
// the data port. Registers 0x20/0x21 hold the 16 bit device ID that names the
// chip. Logical device 0x0B is the hardware monitor: register 0x30 tells
// whether it is active and 0x60/0x61 hold the base of the I/O window it
// decodes.
//
// Inside that window the index register is NOT at base + 0: the NCT679x maps
// it to base + 5 with the data register at base + 6, exactly as the Linux
// nct6775 driver and LibreHardwareMonitor (Nct677X: ADDRESS_REGISTER_OFFSET
// 0x05, DATA_REGISTER_OFFSET 0x06) address it. Reading base + 0 instead hits
// an unclaimed port, which mirrors the last write back and makes a perfectly
// live window look dead. Register numbers are 16 bit wide, so the high byte
// selects a bank through index register 0x4E before the low byte is written.
//
// On the NCT679x family config register 0x28 bit 4, the hardware monitor I/O
// space lock, also keeps the window dark and LibreHardwareMonitor clears it
// while still in config mode. Only that lock and the identification registers
// are written: no fan control, limit or divisor register is touched, so the
// chip keeps running exactly the way the firmware configured it.
// ----------------------------------------------------------------------------

// Device IDs of the chips whose tachometers use the 13 bit counters decoded
// below. The ID is read from config registers 0x20/0x21 and masked with 0xFFF8,
// because the low 3 bits carry a stepping revision. LibreHardwareMonitor lists
// these parts as id/revision pairs (LpcIO); collapsing a pair into the masked
// ID is also what makes a board that reports revision 0x20 for an NCT6793D
// match, while the strict pair table of that project does not.
struct NctChip { unsigned int id; const char* name; };
static constexpr NctChip kNct679xChips[] = {
    { 0xC560u, "NCT6779D" },
    { 0xC800u, "NCT6791D" },
    { 0xC910u, "NCT6792D" },
    { 0xD120u, "NCT6793D" },
    { 0xD350u, "NCT6795D" },
    { 0xD420u, "NCT6796D" },
    { 0xD428u, "NCT6796DR/NCT6798D" },
    { 0xD450u, "NCT6797D" },
    { 0xD800u, "NCT6799D" },
};

// Nuvoton parts that are recognized here but deliberately not decoded, so the
// log can name the chip instead of calling it unknown: the NCT668x keep their
// monitor registers behind an EC page interface rather than the banked index
// and data window, the NCT6771F and NCT6776F report RPM in 16 bit registers
// instead of a counter, the NCT610XD maps its tachometers elsewhere, and the
// NCT6701D reads its source registers in another order. These entries are
// matched on the full id/revision pair, before the mask is applied, because
// 0xD8/0x06 (NCT6701D) and 0xD8/0x02 (NCT6799D) collapse to the same masked
// value. Pairs are taken from the identification table of that project.
static constexpr NctChip kNctOtherChips[] = {
    { 0xB470u, "NCT6771F" },
    { 0xC330u, "NCT6776F" },
    { 0xC450u, "NCT610XD" },
    { 0xC732u, "NCT6683D" },
    { 0xD440u, "NCT6686D" },
    { 0xD441u, "NCT6686D" },
    { 0xD592u, "NCT6687D" },
    { 0xD806u, "NCT6701D" },
};

// Outcome of naming a config space device ID.
enum class NctChipKind { Unknown, Decoded, OtherLayout };

// Names the Nuvoton chip behind a device ID read from config registers
// 0x20/0x21. 'name' receives the chip name for Decoded and OtherLayout and a
// null pointer for Unknown. Note that this only labels the chip: what proves
// that a monitor window is really alive is the vendor ID read inside it
// (NctWindowAlive). 'id' is the raw pair; the exclusion list is matched first
// and only then the family table, which is matched on the masked value.
static NctChipKind MatchNctChip(unsigned int id, const char*& name)
{
    for (const NctChip& c : kNctOtherChips)
    {
        if (c.id == id) { name = c.name; return NctChipKind::OtherLayout; }
    }
    const unsigned int masked = id & 0xFFF8u;
    for (const NctChip& c : kNct679xChips)
    {
        if (c.id == masked) { name = c.name; return NctChipKind::Decoded; }
    }
    name = nullptr;
    return NctChipKind::Unknown;
}
// Config register that locks the monitor I/O space on the NCT679x family.
static constexpr uint8_t kNct6791RegIoLock = 0x28;
// Index and data ports inside a monitor window, plus the bank select register
// used to reach the registers above 0xFF.
static constexpr uint16_t kNctIndexOffset = 0x05;
static constexpr uint16_t kNctDataOffset  = 0x06;
static constexpr uint8_t  kNctBankReg     = 0x4E;
// Vendor ID pair: bank 0x80 register 0x4F holds the high byte, bank 0x00
// register 0x4F the low byte. A window that decodes answers 0x5CA3 there.
static constexpr uint16_t kNctVendorIdHigh = 0x804F;
static constexpr uint16_t kNctVendorIdLow  = 0x004F;
static constexpr int      kNuvotonVendorId = 0x5CA3;
// 13 bit fan tachometer counters of the NCT679x family: the high byte sits at
// the register itself, the low 5 bits at register + 1 (bank 4). Fan 1 is the
// CPU fan header on the reference design of this chip family.
static constexpr uint16_t kNctFanCountRegs[] = { 0x4B0, 0x4B2, 0x4B4, 0x4B6,
                                                 0x4B8, 0x4BA, 0x4CC };
// Counter bounds: a count at or above the maximum means the tachometer sees a
// stopped fan, a count below the minimum cannot be turned into RPM. The RPM
// bound rejects a stuck or unimplemented register, whose value would otherwise
// stretch the chart scale.
static constexpr unsigned int kNctMaxFanCount = 0x1FFFu;
static constexpr unsigned int kNctMinFanCount = 0x0015u;
static constexpr unsigned int kNctMaxFanRpm   = 15000u;

// Reads one config register of the LPC interface. Returns the byte value, or
// -1 when either the index write or the data read was rejected by the driver.
static int ReadSuperIoConfig(uint16_t cfgPort, uint8_t reg)
{
    if (!Ring0Msr::WriteIoPortByte(cfgPort, reg))
        return -1;
    uint8_t v = 0;
    if (!Ring0Msr::ReadIoPortByte(cfgPort + 1, v))
        return -1;
    return static_cast<int>(v);
}

// Writes one config register: index byte, then the value on the data port.
static bool WriteSuperIoConfig(uint16_t cfgPort, uint8_t reg, uint8_t value)
{
    return Ring0Msr::WriteIoPortByte(cfgPort, reg) &&
           Ring0Msr::WriteIoPortByte(cfgPort + 1, value);
}

// Reads one byte of a monitor window (defined below). Forward declaration so
// the identification can test a window before it is selected.
static int NctReadByte(uint16_t base, uint16_t reg);

// Reports whether a monitor window really decodes and returns what the probe
// reads. The reliable test is the vendor ID pair: only a live NCT679x answers
// 0x5CA3 there. The failure modes stay visible in the log - a window no device
// claims leaves the data bus undriven and an unclaimed I/O read mirrors the
// payload of the last write back (probing bank 0 and bank 4 therefore reports
// the echo pair), while a range another device owns reads all ones. Chip ID
// and revision of bank 0 are reported alongside for diagnostics.
static bool NctWindowAlive(uint16_t base, int& vendorId, int& chipId, int& chipRev)
{
    const int vhi = NctReadByte(base, kNctVendorIdHigh);
    const int vlo = NctReadByte(base, kNctVendorIdLow);
    vendorId = (vhi >= 0 && vlo >= 0) ? ((vhi << 8) | vlo) : -1;

    chipId  = NctReadByte(base, 0x000);
    chipRev = NctReadByte(base, 0x001);

    return vendorId == kNuvotonVendorId;
}

// Identifies the Super I/O chip on both port pairs and returns the I/O base of
// its hardware monitor window, or 0 when no supported chip answers.
static uint16_t ProbeSuperIoHwm()
{
    // Need the port I/O backend.
    if (!Ring0Msr::IsOpen()) return 0;

    // Canary: port 0x61 (8042 keyboard status) exists on every x86 PC and is
    // never claimed by a PnP device, so a successful byte read proves that the
    // loaded driver implements its port IOCTLs at all.
    uint8_t canary = 0;
    const bool canaryOk = Ring0Msr::ReadIoPortByte(0x61, canary);
    LOGI("Super I/O: canary port 0x61 ok=%d value=0x%02X",
         canaryOk ? 1 : 0, canary);

    static const uint16_t kCfgPorts[] = { 0x2E, 0x4E };

    // Base addresses reported by the monitor logical devices of both port
    // pairs. A candidate is only usable once it proves that it decodes.
    struct Candidate { uint16_t cfgPort; uint16_t base; };
    Candidate candidates[16];
    int nCand = 0;

    for (const uint16_t cfg : kCfgPorts)
    {
        // Leave config mode first so the following reads are meaningful.
        Ring0Msr::WriteIoPortByte(cfg, 0xAA);
        Sleep(1);

        // Unlock key used by Nuvoton, ITE and SMSC/Fintek compatible chips.
        if (!Ring0Msr::WriteIoPortByte(cfg, 0x87) ||
            !Ring0Msr::WriteIoPortByte(cfg, 0x87))
        {
            LOGI("Super I/O 0x%02X: config mode write rejected by the driver", cfg);
            continue;
        }
        Sleep(1);

        const int idHi = ReadSuperIoConfig(cfg, 0x20);
        const int idLo = ReadSuperIoConfig(cfg, 0x21);
        if (idHi < 0 || idLo < 0)
        {
            LOGI("Super I/O 0x%02X: device ID read failed", cfg);
            Ring0Msr::WriteIoPortByte(cfg, 0xAA);
            continue;
        }

        // Raw id/revision pair: the exclusion list is matched on it exactly,
        // the family table only on the masked value (the low 3 bits are the
        // stepping, and some boards report 0x20 where the vendor table says
        // 0x21).
        const unsigned int chipId =
            (static_cast<unsigned int>(idHi) << 8) |
            static_cast<unsigned int>(idLo);

        const char* chipName = nullptr;
        const NctChipKind kind = MatchNctChip(chipId, chipName);
        if (kind == NctChipKind::OtherLayout)
        {
            LOGI("Super I/O 0x%02X: Nuvoton %s, device ID 0x%04X - its "
                 "tachometers use another register layout and are not decoded",
                 cfg, chipName, chipId);
            Ring0Msr::WriteIoPortByte(cfg, 0xAA);
            continue;
        }
        if (kind == NctChipKind::Unknown)
        {
            LOGI("Super I/O 0x%02X: device ID 0x%04X is not a Nuvoton chip of "
                 "the NCT679x family", cfg, chipId);
            Ring0Msr::WriteIoPortByte(cfg, 0xAA);
            continue;
        }
        LOGI("Super I/O 0x%02X: Nuvoton %s, device ID 0x%04X (masked 0x%04X)",
             cfg, chipName, chipId, chipId & 0xFFF8u);

        // The NCT679x family splits its hardware monitor function over several
        // logical devices, and the firmware picks the I/O address of the
        // decoded window. Walk the monitor logical devices, clear the I/O space
        // lock the NCT679x uses to keep a window dark, and collect every base
        // address the chip reports.
        static const uint8_t kHwmLd[] = { 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12 };

        for (const uint8_t ld : kHwmLd)
        {
            if (!WriteSuperIoConfig(cfg, 0x07, ld))
                break;
            Sleep(1);
            if (ReadSuperIoConfig(cfg, 0x07) != static_cast<int>(ld))
                continue;

            const int ena  = ReadSuperIoConfig(cfg, 0x30);
            const int hi   = ReadSuperIoConfig(cfg, 0x60);
            const int lo   = ReadSuperIoConfig(cfg, 0x61);
            const int lock = ReadSuperIoConfig(cfg, kNct6791RegIoLock);
            // The second I/O range of a logical device: the NCT679x keeps part
            // of its monitor registers behind it, so log it for LD 0x0B.
            const int hi2  = ReadSuperIoConfig(cfg, 0x62);
            const int lo2  = ReadSuperIoConfig(cfg, 0x63);

            if (lock >= 0 && (lock & 0x10))
            {
                // While bit 4 is set the window is not decoded; both the kernel
                // driver and LibreHardwareMonitor clear it before reading.
                WriteSuperIoConfig(cfg, kNct6791RegIoLock,
                                   static_cast<uint8_t>(lock & ~0x10));
            }

            // Registers of an unimplemented logical device read back as all
            // ones, which would produce the bogus base 0xFFF0.
            if (hi < 0 || lo < 0 || hi == 0xFF)
                continue;

            // A base address is only trusted when a second read agrees, the
            // check LibreHardwareMonitor performs before using it.
            Sleep(1);
            const int hiV = ReadSuperIoConfig(cfg, 0x60);
            const int loV = ReadSuperIoConfig(cfg, 0x61);
            if (hiV != hi || loV != lo)
            {
                LOGI("Super I/O 0x%02X: LD 0x%02X base address read is unstable "
                     "(0x%02X%02X then 0x%02X%02X)", cfg,
                     static_cast<unsigned int>(ld),
                     hi & 0xFF, lo & 0xFF, hiV & 0xFF, loV & 0xFF);
                continue;
            }

            // A base address is a 16 bit value aligned to 16 bytes.
            const unsigned int base =
                ((static_cast<unsigned int>(hi) << 8) |
                 static_cast<unsigned int>(lo)) & 0xFFF0u;
            if (base == 0)
                continue;

            bool dup = false;
            for (int i = 0; i < nCand; ++i)
            {
                if (candidates[i].base == base) { dup = true; break; }
            }
            if (!dup && nCand < static_cast<int>(sizeof(candidates) / sizeof(candidates[0])))
            {
                candidates[nCand].cfgPort = cfg;
                candidates[nCand].base    = static_cast<uint16_t>(base);
                ++nCand;
            }

            // The secondary range of the monitor logical device holds part of
            // the tachometer registers on the NCT679x, so it is a candidate too.
            const unsigned int base2 =
                (hi2 == 0xFF || hi2 < 0 || lo2 < 0)
                    ? 0u
                    : (((static_cast<unsigned int>(hi2) << 8) |
                        static_cast<unsigned int>(lo2)) & 0xFFF0u);
            if (base2 != 0 && base2 != base)
            {
                dup = false;
                for (int i = 0; i < nCand; ++i)
                {
                    if (candidates[i].base == base2) { dup = true; break; }
                }
                if (!dup && nCand < static_cast<int>(sizeof(candidates) / sizeof(candidates[0])))
                {
                    candidates[nCand].cfgPort = cfg;
                    candidates[nCand].base    = static_cast<uint16_t>(base2);
                    ++nCand;
                }
            }

            LOGI("Super I/O 0x%02X: LD 0x%02X enable=0x%02X lock=0x%02X io=0x%03X secondary=0x%03X",
                 cfg, static_cast<unsigned int>(ld),
                 static_cast<unsigned int>(ena & 0xFF),
                 static_cast<unsigned int>(lock & 0xFF), base, base2);
        }

        Ring0Msr::WriteIoPortByte(cfg, 0xAA);
        Sleep(1);
    }

    // Some firmware writes an address into the monitor logical device without
    // enabling the decode of that range, while the chip itself still answers on
    // one of the addresses the NCT677x and NCT679x families ship with. Test
    // those as well, and leave config mode before doing it.
    static const uint16_t kDefaultWindows[] = { 0x290, 0x490, 0x680, 0x690 };
    for (const uint16_t b : kDefaultWindows)
    {
        bool dup = false;
        for (int i = 0; i < nCand; ++i)
        {
            if (candidates[i].base == b) { dup = true; break; }
        }
        if (dup || nCand >= static_cast<int>(sizeof(candidates) / sizeof(candidates[0])))
            continue;
        // Only a monitor logical device that is active decodes its window, so
        // the search is wrapped up before the fixed addresses are probed.
        candidates[nCand].cfgPort = 0x00;
        candidates[nCand].base    = b;
        ++nCand;
    }

    // The chip may report more than one window and the firmware may leave the
    // one it names undecoded, so use the first candidate that answers with the
    // Nuvoton vendor ID.
    for (int i = 0; i < nCand; ++i)
    {
        int vendor = 0, chipId = 0, chipRev = 0;
        if (!NctWindowAlive(candidates[i].base, vendor, chipId, chipRev))
        {
            if (candidates[i].cfgPort)
                LOGI("Super I/O 0x%02X: window 0x%03X does not report the Nuvoton "
                     "vendor ID, probe reads vendor 0x%04X, bank 0 regs 0x00=0x%02X "
                     "0x01=0x%02X",
                     candidates[i].cfgPort, candidates[i].base,
                     vendor & 0xFFFF, chipId & 0xFF, chipRev & 0xFF);
            else
                LOGI("Super I/O: default window 0x%03X does not report the Nuvoton "
                     "vendor ID, probe reads vendor 0x%04X, bank 0 regs 0x00=0x%02X "
                     "0x01=0x%02X",
                     candidates[i].base, vendor & 0xFFFF, chipId & 0xFF, chipRev & 0xFF);
            continue;
        }
        LOGI("Super I/O: monitor window at 0x%03X, index 0x%03X data 0x%03X, "
             "vendor ID 0x%04X, bank 0 regs 0x00=0x%02X 0x01=0x%02X",
             candidates[i].base,
             static_cast<unsigned int>(candidates[i].base) + kNctIndexOffset,
             static_cast<unsigned int>(candidates[i].base) + kNctDataOffset,
             vendor & 0xFFFF, chipId & 0xFF, chipRev & 0xFF);
        return candidates[i].base;
    }

    LOGI("Super I/O: no monitor window decodes - the firmware keeps the "
         "hardware monitor block disabled, its registers cannot be read");
    return 0;
}

// Reads one byte of the monitor window. The index port is base + 5 and the data
// port base + 6; registers above 0xFF live in a bank selected through index
// register 0x4E (the sequence used by the nct6775 driver and by
// LibreHardwareMonitor). Returns the value, or -1 when a port operation failed.
static int NctReadByte(uint16_t base, uint16_t reg)
{
    const uint8_t bank = static_cast<uint8_t>(reg >> 8);
    if (!Ring0Msr::WriteIoPortByte(static_cast<uint16_t>(base + kNctIndexOffset),
                                   kNctBankReg))                                return -1;
    if (!Ring0Msr::WriteIoPortByte(static_cast<uint16_t>(base + kNctDataOffset),
                                   bank))                                       return -1;
    if (!Ring0Msr::WriteIoPortByte(static_cast<uint16_t>(base + kNctIndexOffset),
                                   static_cast<uint8_t>(reg & 0xFF)))           return -1;
    uint8_t v = 0;
    if (!Ring0Msr::ReadIoPortByte(static_cast<uint16_t>(base + kNctDataOffset), v))
        return -1;
    return static_cast<int>(v);
}

// Decodes the fan tachometers of the monitor window the way LibreHardwareMonitor
// and the Linux nct6775 driver do. The 13 bit counter of a channel is built from
// the byte at the channel register (its high part) and the low 5 bits of the
// byte at the next address. A counter at or above the maximum means the
// tachometer sees a stopped fan, a counter below the minimum is too slow to be
// converted; otherwise RPM is 1350000 / count. Channel 1 is the CPU fan header
// on the reference design of this chip family, and it is the first entry of the
// register table, so the first channel that reports a fan is used. Returns the
// RPM, or -1 when no channel reports a tachometer. verbose logs every channel
// so the sensor mapping can be checked in the log.
static int ReadSuperIoFanRpm(uint16_t base, bool verbose)
{
    static constexpr size_t kFanChannels =
        sizeof(kNctFanCountRegs) / sizeof(kNctFanCountRegs[0]);

    if (verbose)
    {
        // Raw dump of the tachometer block, logged once per run. Different
        // bytes in consecutive registers prove that the window index port
        // really decodes, which the decoded RPM alone cannot show.
        char dump[80];
        int off = 0;
        dump[0] = '\0';
        for (unsigned int r = 0xB0; r <= 0xBF; ++r)
        {
            if (off >= static_cast<int>(sizeof(dump)) - 4)
                break;
            const int v = NctReadByte(base, static_cast<uint16_t>(0x400 + r));
            const int n = snprintf(dump + off, sizeof(dump) - off, "%02X ",
                                   v < 0 ? 0 : (v & 0xFF));
            if (n <= 0) break;
            off += n;
        }
        LOGI("Super I/O bank 4, registers 0xB0..0xBF: %s", dump);
    }

    int rpm = -1;
    for (size_t i = 0; i < kFanChannels; ++i)
    {
        const uint16_t reg = kNctFanCountRegs[i];
        const int hi = NctReadByte(base, reg);
        const int lo = NctReadByte(base, static_cast<uint16_t>(reg + 1));
        if (hi < 0 || lo < 0)
        {
            LOGI("Super I/O: tachometer register 0x%03X read failed",
                 static_cast<unsigned int>(reg));
            return -1;
        }

        const unsigned int count =
            (static_cast<unsigned int>(hi) << 5) |
            (static_cast<unsigned int>(lo) & 0x1Fu);

        unsigned int val = 0;
        if (count >= kNctMinFanCount && count < kNctMaxFanCount)
            val = 1350000u / count;
        if (val > kNctMaxFanRpm)
            val = 0;                         // stuck register, not a real fan

        if (verbose)
            LOGI("Super I/O fan%u: count=0x%04X rpm=%u",
                 static_cast<unsigned int>(i) + 1u, count, val);

        if (val > 0 && rpm < 0)
            rpm = static_cast<int>(val);
    }
    return rpm;
}




// Enumerate all properties of a WMI object and log them for diagnostics.
static void DumpWmiObject(IWbemClassObject* pObj)
{
    if (!pObj) return;
    SAFEARRAY* psaNames = nullptr;
    HRESULT hr = pObj->GetNames(nullptr, WBEM_FLAG_ALWAYS, nullptr, &psaNames);
    if (SUCCEEDED(hr) && psaNames)
    {
        long cnt = 0;
        if (SafeArrayGetDim(psaNames) != 1) return; // expect a single-dimension BSTR array
        SafeArrayGetUBound(psaNames, 1, &cnt);
        for (long i = 0; i < cnt; ++i)
        {
            BSTR bName = nullptr;
            if (SafeArrayGetElement(psaNames, &i, &bName) == S_OK && bName)
            {
                VARIANT vt; VariantInit(&vt);
                HRESULT hr2 = pObj->Get(bName, 0, &vt, nullptr, nullptr);
                wchar_t desc[128] = {};
                if (SUCCEEDED(hr2))
                {
                    switch (vt.vt)
                    {
                    case VT_I4:      swprintf(desc, 128, L"I4=%ld", vt.lVal); break;
                    case VT_UI4:     swprintf(desc, 128, L"UI4=%lu", vt.ulVal); break;
                    case VT_R8:      swprintf(desc, 128, L"R8=%.2f", vt.dblVal); break;
                    case VT_BSTR:    swprintf(desc, 128, L"BSTR=\"%s\"", vt.bstrVal ? vt.bstrVal : L"(null)"); break;
                    case VT_EMPTY:   wcscpy(desc, L"EMPTY"); break;
                    default:         swprintf(desc, 128, L"vt=%d", vt.vt); break;
                    }
                }
                else
                {
                    swprintf(desc, 128, L"Get err=0x%08X", (unsigned)hr2);
                }
                LOGI("DIAG WMI prop %ls: %ls", bName, desc);
                VariantClear(&vt);
                SysFreeString(bName);
            }
        }
        SafeArrayDestroy(psaNames);
    }
}

// Diagnostic: probe every non-admin source for CPU temperature on this machine.
// Logs one line per method so we can see what actually works without a kernel
// driver or admin privileges. Call once at startup from Collect().
static void ProbeAllTempSources()
{
    // COM must be initialized before any CoCreateInstance / CoInitializeEx call.
    // QueryWmiValueRaw does its own per-call init, but this function calls
    // CoCreateInstance directly without wrapping it - initialize once here.
    HRESULT comHres = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool comInit = (comHres == S_OK || comHres == S_FALSE);
    (void)comInit;

    // ---- WMI: Win32_TemperatureProbe (root\cimv2) - enumerate ALL rows + props --
    {
        IWbemLocator* pLoc = nullptr;
        IWbemServices* pSvc = nullptr;
        IEnumWbemClassObject* pEnum = nullptr;
        HRESULT hr = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_IWbemLocator, reinterpret_cast<LPVOID*>(&pLoc));
        if (SUCCEEDED(hr) && pLoc)
        {
            BSTR bNs = SysAllocString(L"root\\cimv2");
            hr = pLoc->ConnectServer(bNs, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &pSvc);
            SysFreeString(bNs);
            if (SUCCEEDED(hr) && pSvc)
            {
                CoSetProxyBlanket(pSvc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE,
                                  nullptr, RPC_C_AUTHN_LEVEL_CALL,
                                  RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
                BSTR bQ = SysAllocString(
                    L"SELECT * FROM Win32_TemperatureProbe");
                hr = pSvc->ExecQuery(BSTR(L"WQL"), bQ,
                                     WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                                     nullptr, &pEnum);
                SysFreeString(bQ);
                if (SUCCEEDED(hr) && pEnum)
                {
                    ULONG ret = 0; IWbemClassObject* pObj = nullptr;
                    int rowIdx = 0;
                    while (pEnum->Next(WBEM_INFINITE, 1, &pObj, &ret) == S_OK && ret > 0)
                    {
                        LOGI("DIAG Win32_TemperatureProbe row %d:", rowIdx++);
                        DumpWmiObject(pObj);
                        pObj->Release();
                    }
                }
                pSvc->Release();
            }
            pLoc->Release();
        }
    }

    // ---- WMI: MSAcpi_ThermalZoneTemperature (root\wmi) - enumerate ALL rows + props --
    {
        IWbemLocator* pLoc = nullptr;
        IWbemServices* pSvc = nullptr;
        IEnumWbemClassObject* pEnum = nullptr;
        HRESULT hr = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_IWbemLocator, reinterpret_cast<LPVOID*>(&pLoc));
        if (SUCCEEDED(hr) && pLoc)
        {
            BSTR bNs = SysAllocString(L"root\\wmi");
            hr = pLoc->ConnectServer(bNs, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &pSvc);
            SysFreeString(bNs);
            if (SUCCEEDED(hr) && pSvc)
            {
                CoSetProxyBlanket(pSvc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE,
                                  nullptr, RPC_C_AUTHN_LEVEL_CALL,
                                  RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
                BSTR bQ = SysAllocString(
                    L"SELECT * FROM MSAcpi_ThermalZoneTemperature");
                hr = pSvc->ExecQuery(BSTR(L"WQL"), bQ,
                                     WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                                     nullptr, &pEnum);
                SysFreeString(bQ);
                if (SUCCEEDED(hr) && pEnum)
                {
                    ULONG ret = 0; IWbemClassObject* pObj = nullptr;
                    int rowIdx = 0;
                    while (pEnum->Next(WBEM_INFINITE, 1, &pObj, &ret) == S_OK && ret > 0)
                    {
                        LOGI("DIAG MSAcpi_ThermalZoneTemperature row %d:", rowIdx++);
                        DumpWmiObject(pObj);
                        pObj->Release();
                    }
                    if (rowIdx == 0)
                        LOGI("DIAG MSAcpi_ThermalZoneTemperature: no rows returned");
                }
                pSvc->Release();
            }
            pLoc->Release();
        }
    }

    // ---- WMI: root\wmi MSAcpi_ThermalZoneTemperature (already in ProbeWmiSensor, duplicate log) --
    LOGI("DIAG WMI MSAcpi_ThermalZoneTemperature: see above ProbeWmiSensor line");

    // ---- GetSystemFirmwareTable('ACPI', 0x44535054='DSPT') ------------------
    DWORD sz = GetSystemFirmwareTable('ACPI', 0x44535054, nullptr, 0);
    if (sz > 0 && sz < 65536)
    {
        std::vector<BYTE> buf(sz);
        if (GetSystemFirmwareTable('ACPI', 0x44535054, buf.data(), static_cast<DWORD>(buf.size())) == sz)
        {
            LOGI("DIAG ACPI DSPT firmware table: size=%u bytes", sz);
            // Dump first 64 bytes as hex for manual inspection.
            char hex[65 * 4];
            for (unsigned i = 0; i < 64 && i < sz; ++i)
                snprintf(hex + i * 3, sizeof(hex) - i * 3, "%02X ", (unsigned char)buf[i]);
            LOGI("DIAG ACPI DSPT first64: %hs", hex);
        }
        else
        {
            LOGI("DIAG ACPI DSPT firmware table: size=%u but ReadTable failed err=%lu",
                 sz, GetLastError());
        }
    }
    else
    {
        LOGI("DIAG ACPI DSPT firmware table: not present (GetSystemFirmwareTable returned %lu)",
             sz);
    }

    // ---- GetSystemFirmwareTable('ACPI', 0x54524143='TRAC') ------------------
    sz = GetSystemFirmwareTable('ACPI', 0x54524143, nullptr, 0);
    LOGI("DIAG ACPI TRAC firmware table: present=%hs (size=%lu)",
         sz > 0 ? "YES" : "NO", static_cast<unsigned long>(sz));

    // ---- Registry: enumerate HKLM\HARDWARE\DESCRIPTION\System\CentralProcessor\0 ---
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                      0, KEY_READ, &hKey) == ERROR_SUCCESS)
    {
        wchar_t name[256] = {};
        DWORD   type = 0, size = sizeof(name);
        // Scan every value under this key for anything temperature-related.
        LOGI("DIAG Registry CentralProcessor\\0 values:");
        for (DWORD idx = 0; ; ++idx)
        {
            size = sizeof(name);
            DWORD rc = RegEnumValueW(hKey, idx, name, &size, nullptr, &type,
                                     nullptr, nullptr);
            if (rc != ERROR_SUCCESS) break;
            // Print value name and first 32 bytes of content (if binary).
            wchar_t val[128] = {};
            DWORD   vsize = sizeof(val);
            if (RegQueryValueExW(hKey, name, nullptr, &type,
                                 reinterpret_cast<LPBYTE>(val), &vsize) == ERROR_SUCCESS)
            {
                if (type == REG_SZ || type == REG_EXPAND_SZ)
                    LOGI("DIAG Reg %ls = \"%ls\" (type=%lu)", name, val, static_cast<unsigned long>(type));
                else if (type == REG_DWORD)
                    LOGI("DIAG Reg %ls = 0x%08X (type=%lu)", name,
                         *reinterpret_cast<uint32_t*>(val), static_cast<unsigned long>(type));
                else
                    LOGI("DIAG Reg %ls = <binary %lu bytes type=%lu>", name,
                         static_cast<unsigned long>(vsize), static_cast<unsigned long>(type));
            }
        }
        RegCloseKey(hKey);
    }
    else
    {
        LOGI("DIAG Registry CentralProcessor\\0: open failed err=%lu", GetLastError());
    }

    // ---- Performance Counters: thermal zone paths --------------------------
    // Try reading \Temperature\System Temperature\% Value via PDH.
    wchar_t pdhPath[] = L"\\\\_Total\\\\System Temperature\\\\% Value";
    HQUERY  hPdhQuery   = nullptr;
    HCOUNTER hPdhCounter = nullptr;
    PDH_STATUS st = PdhOpenQueryW(nullptr, 0, &hPdhQuery);
    if (st == ERROR_SUCCESS)
    {
        st = PdhAddCounterW(hPdhQuery, pdhPath, 0, &hPdhCounter);
        if (st == ERROR_SUCCESS)
        {
            PDH_FMT_COUNTERVALUE fmtVal;
            st = PdhCollectQueryData(hPdhQuery);
            Sleep(200);
            st = PdhGetFormattedCounterValue(hPdhCounter, PDH_FMT_DOUBLE, nullptr, &fmtVal);
            if (st == ERROR_SUCCESS)
                LOGI("DIAG PDH Temperature counter: %.1f C", fmtVal.doubleValue);
            else
                LOGI("DIAG PDH Temperature counter: read failed st=0x%08X", static_cast<unsigned>(st));
        }
        else
        {
            LOGI("DIAG PDH Temperature counter: add failed st=0x%08X", static_cast<unsigned>(st));
        }
        PdhCloseQuery(hPdhQuery);
    }
    else
    {
        LOGI("DIAG PDH Temperature counter: open failed st=0x%08X", static_cast<unsigned>(st));
    }

    if (comInit) CoUninitialize();
}

// Read the CPU model name from registry (HKLM\HARDWARE\DESCRIPTION\System\CentralProcessor\0\ProcessorNameString).
// This is fast and doesn't block the UI thread like WMI can.
void CpuMonitor::QueryCpuName()
{
    // CPU model name from registry (fast, no WMI round-trip).
    std::string nameA; // ASCII copy of the name for the TDP table lookup
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
#ifdef PLATFORM_WINDOWS
            wcsncpy(m_metrics.cpuName, name, sizeof(m_metrics.cpuName) / sizeof(wchar_t) - 1);
#else
            strncpy(m_metrics.cpuName, name.c_str(), sizeof(m_metrics.cpuName) - 1);
#endif
        }
        RegCloseKey(hKey);
    }

    // CPU names are ASCII; narrow char-by-char (no locale conversion needed).
    {
        std::lock_guard<std::mutex> lock(m_mutex);
#ifdef PLATFORM_WINDOWS
        for (const wchar_t* p = m_metrics.cpuName; *p; ++p)
            nameA += static_cast<char>(*p);
#else
        for (const char* p = m_metrics.cpuName; *p; ++p)
            nameA += *p;
#endif
    }

    // The ASCII copy is logged: on Windows m_metrics.cpuName is a wchar_t[], so
    // a narrow "%s" would read only one character from it ("Intel..." -> "I").
    LOGI("CPU name: %s", nameA.c_str());

    // Resolve the TDP limit once from the static table (unknown CPU -> 125 W).
    const double tdp = static_cast<double>(PowerTable::GetCpuTdpByName(nameA));
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_metrics.tdpWatts = tdp;
    }
    LOGI("CPU TDP table lookup: %.0f W (name='%hs')", tdp, nameA.c_str());

    // Physical core count feeds the idle-floor power estimate; detected once
    // before the polling threads start.
    const uint32_t cores = DetectPhysicalCoreCount();
    m_coreCount.store(cores, std::memory_order_relaxed);
    LOGI("Physical cores detected: %u", cores);

    // Initialize MSR driver (if present next to exe) and probe thermal params.
    MsrSensor::InitDriver(nullptr); // nullptr = driver path from registry/exe dir
    // If the backend could not be opened but a driver could be installed,
    // ask once for elevation (GUI thread - QueryCpuName runs on it) and retry.
    MsrSensor::EnsureDriver(nullptr);
    ProbeMsrInit();
}

// ----------------------------------------------------------------------------
// Probe MSR thermal sensor: read family/model/stepping from registry, look up
// Tjmax. The actual __readmsr() call happens later in Collect() (background
// thread) via the kernel driver or direct intrinsic.
// ----------------------------------------------------------------------------
void CpuMonitor::ProbeMsrInit()
{
    // Read family/model/stepping from registry (same key opened by QueryCpuName).
    uint32_t family = 0, model = 0, stepping = 0;
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                      0, KEY_READ, &hKey) == ERROR_SUCCESS)
    {
        wchar_t val[256] = {};
        DWORD size = sizeof(val);
        if (RegQueryValueExW(hKey, L"Identifier", nullptr, nullptr,
                             reinterpret_cast<LPBYTE>(val), &size) == ERROR_SUCCESS)
        {
            // Parse "Intel64 Family 6 Model 79 Stepping 1"
            wchar_t* p = wcsstr(val, L"Family ");
            if (p)
            {
                family     = static_cast<uint32_t>(wcstoul(p + 7, nullptr, 10));
                p = wcsstr(p, L"Model ");
                if (p)
                {
                    model = static_cast<uint32_t>(wcstoul(p + 6, nullptr, 10));
                    p = wcsstr(p, L"Stepping ");
                    if (p)
                        stepping = static_cast<uint32_t>(wcstoul(p + 9, nullptr, 10));
                }
            }
        }
        RegCloseKey(hKey);
    }

    // Tjmax lookup table (same set as msr_sensor.cpp).
    static const struct { uint32_t key; int8_t tjmax; } kTjmaxTable[] =
    {
        { (6u << 16) | (0x3F << 8) | 0,  84 }, // Haswell-E/EP
        { (6u << 16) | (0x4F << 8) | 0,   85 },// Broadwell-EP (Xeon E5-2696 v4 = 85)
        { (6u << 16) | (0x55 << 8) | 0,  100 },// Skylake/Cascade Lake-SP
        { (6u << 16) | (0x5E << 8) | 0,  100 },
        { (6u << 16) | (0x7E << 8) | 0,  100 },
        { (6u << 16) | (0x8E << 8) | 0,  100 },
        { (6u << 16) | (0x9E << 8) | 0,  100 },
        { (6u << 16) | (0xA5 << 8) | 0,  100 },
        { (6u << 16) | (0xA7 << 8) | 0,  100 },
        { (6u << 16) | (0x97 << 8) | 0,  100 },
        { (6u << 16) | (0xB7 << 8) | 0,  100 },
        { (6u << 16) | (0x2A << 8) | 0,   100 },
        { (6u << 16) | (0x2D << 8) | 0,   100 },
        { (6u << 16) | (0x3A << 8) | 0,   100 },
        { (6u << 16) | (0x3E << 8) | 0,   100 },
        { (6u << 16) | (0x1A << 8) | 0,   100 },
        { (6u << 16) | (0x1E << 8) | 0,   100 },
        { (6u << 16) | (0x2C << 8) | 0,   100 },
        { (6u << 16) | (0x0F << 8) | 0,   100 },
        { (6u << 16) | (0x17 << 8) | 0,   100 },
    };
    int8_t tjmax = 100;
    const uint32_t keyWithStep = (family << 16) | (model << 8) | stepping;
    for (const auto& e : kTjmaxTable)
        if (e.key == keyWithStep) { tjmax = e.tjmax; break; }
    {
        // Fallback: match family+model ignoring stepping (table keys store 0).
        const uint32_t keyNoStep = (family << 16) | (model << 8);
        for (const auto& e : kTjmaxTable)
            if ((e.key & 0xFFFFFF00u) == keyNoStep)
            { tjmax = e.tjmax; break; }
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_msrFamily   = family;
        m_msrModel    = model;
        m_msrStepping = stepping;
        m_msrTjmax    = tjmax;
    }
    LOGI("MSR init: family=%u model=0x%02X stepping=%u Tjmax=%d",
         family, model, stepping, tjmax);
}
