#include "cpu_monitor.h"
#include "msr_sensor.h"
#include "ring0_msr.h"
#include "power_table.h"
#include "logger.h"

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

    // Read ok BEFORE delete — reading ctx->ok after delete is use-after-free.
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
// Forward declaration: probes Super I/O fan via WinRing0 port I/O.
static int ProbeSuperIoFan();

void CpuMonitor::Collect()
{
    // CPU load — PDH delta sample once per tick, same cadence as the polling loop.
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

    // --- WMI fallback ------------------------------------------------------
    if (!m_wmiTried && !(filled & 1u))
    {
        m_wmiTried = true;
        filled = ProbeWmiSensor(temp, fan);
        m_wmiOk = (filled != 0);
        LOGI("WMI sensor probe: temp=%hs fan=%hs",
             (filled & 1u) ? "OK" : "N/A",
             (filled & 2u) ? "OK" : "N/A");
        // Probe every available non-admin source so we know what works on this machine.
        ProbeAllTempSources();
    }
    else if (m_wmiOk && !(filled & 1u))
    {
        if (m_wmiCooldown <= 0)
        {
            filled = ProbeWmiSensor(temp, fan);
            m_wmiCooldown = 5;
        }
        else
        {
            --m_wmiCooldown;
        }
    }

    // --- Super I/O fan probe (WinRing0 I/O port access) --------------------
    // Runs once per session: if no fan RPM yet but the kernel driver is loaded,
    // try reading the hardware monitoring chip directly. Guarded by m_wmiTried
    // so we only attempt after WMI has been tried at least once.
    // Disabled by default — direct port I/O can affect unknown LPC devices.
    // Enable via environment variable: LOADGRAPH_SUPERIO_FAN=1
    static bool s_superioEnabled = ([]() -> bool {
        const char* v = getenv("LOADGRAPH_SUPERIO_FAN");
        return (v != nullptr && v[0] != '0' && v[0] != 'f' && v[0] != 'F');
    })();
    if (s_superioEnabled && fan < 0 && Ring0Msr::IsOpen() && !m_fanIoTried)
    {
        m_fanIoTried = true;
        LOGI("Super I/O fan probe enabled via LOADGRAPH_SUPERIO_FAN=1");
        int ioFan = ProbeSuperIoFan();
        if (ioFan > 0)
        {
            fan = ioFan;
            filled |= 2u;
            LOGI("Super I/O fan probe: RPM=%d", fan);
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

// ----------------------------------------------------------------------------
// Super I/O hardware monitoring probe via WinRing0 I/O port access.
// Reads CPU fan RPM from common Super I/O chips (Nuvoton, ITE, SMSC).
// Returns the RPM value, or -1 if unavailable.
// ----------------------------------------------------------------------------
static int ProbeSuperIoFan()
{
    // Need WinRing0 backend for port I/O.
    if (!Ring0Msr::IsOpen()) return -1;

    // Common Super I/O config ports and their data ports.
    static const struct { uint16_t cfgPort; uint16_t dataPort; } kPorts[] = {
        { 0x2E, 0x2F },
        { 0x4E, 0x4F },
    };

    for (const auto& p : kPorts)
    {
        // Exit config mode first (some chips stay locked from previous use).
        Ring0Msr::WriteIoPortByte(p.cfgPort, 0xAA);
        Sleep(1);

        // Enter config mode: write 0x55 to config port.
        if (!Ring0Msr::WriteIoPortByte(p.cfgPort, 0x55))
            continue;
        Sleep(1);
        if (!Ring0Msr::WriteIoPortByte(p.cfgPort, 0x55))
            continue;
        Sleep(1);

        // Read vendor ID from logical device config register 0x20.
        if (!Ring0Msr::WriteIoPortByte(p.cfgPort, 0x20))
            continue;
        uint32_t vidReg = Ring0Msr::ReadIoPortDword(p.dataPort);
        // Vendor IDs: Nuvoton=0xBCDE, ITE=0xACAD, SMSC=0x114B (low word), F718xx=0xF0F1.
        uint16_t vid = static_cast<uint16_t>(vidReg & 0xFFFF);
        bool isNuvoton = (vid == 0xBCDE);
        bool isIte     = (vid == 0xACAD);
        bool isSmss    = (vid == 0x114B || vid == 0xF0F1);
        if (!isNuvoton && !isIte && !isSmss) continue;

        LOGI("Super I/O detected at port 0x%04X: vendor=0x%04X", p.cfgPort, vid);

        // Read base I/O address from registers 0x60 (low byte) and 0x61 (high byte).
        if (!Ring0Msr::WriteIoPortByte(p.cfgPort, 0x60)) continue;
        uint32_t baseLow = Ring0Msr::ReadIoPortDword(p.dataPort);
        if (!Ring0Msr::WriteIoPortByte(p.cfgPort, 0x61)) continue;
        uint32_t baseHigh = Ring0Msr::ReadIoPortDword(p.dataPort);
        uint16_t baseAddr = static_cast<uint16_t>(
            (static_cast<uint16_t>(baseHigh & 0xFF) << 8) |
             static_cast<uint16_t>(baseLow  & 0xFF));

        if (baseAddr < 0x100) continue;
        LOGI("Super I/O base I/O = 0x%04X", baseAddr);

        // Select logical device #1 (typically fan controller) — config reg 0x07.
        if (!Ring0Msr::WriteIoPortByte(p.cfgPort, 0x07)) continue;
        Ring0Msr::ReadIoPortDword(p.dataPort); // LD selection ack.
        // If LD#1 is not enabled (bit 0 of status reg at offset 0x30), try another.
        if (!Ring0Msr::WriteIoPortByte(p.cfgPort, 0x30)) continue;
        uint32_t stReg = Ring0Msr::ReadIoPortDword(p.dataPort);
        if (!(stReg & 1u))
        {
            // Try logical device #2 (some boards route fan to LD#2).
            if (!Ring0Msr::WriteIoPortByte(p.cfgPort, 0x07)) continue;
            Ring0Msr::WriteIoPortByte(p.cfgPort, 0x02);
            Sleep(1);
            if (!Ring0Msr::WriteIoPortByte(p.cfgPort, 0x30)) continue;
            stReg = Ring0Msr::ReadIoPortDword(p.dataPort);
            if (!(stReg & 1u)) continue;
        }

        // Read fan divisor register. Layout depends on chip family.
        uint8_t div = 0;
        if (isNuvoton)
        {
            // Nuvoton: fan divisor at config offset 0x43 (low 4 bits of dword read).
            if (!Ring0Msr::WriteIoPortByte(p.cfgPort, 0x43)) continue;
            div = static_cast<uint8_t>(Ring0Msr::ReadIoPortDword(p.dataPort) & 0x0F);
        }
        else if (isIte)
        {
            // ITE: fan divisor at config offset 0x41.
            if (!Ring0Msr::WriteIoPortByte(p.cfgPort, 0x41)) continue;
            div = static_cast<uint8_t>(Ring0Msr::ReadIoPortDword(p.dataPort) & 0x0F);
        }
        else // SMSC
        {
            if (!Ring0Msr::WriteIoPortByte(p.cfgPort, 0x42)) continue;
            div = static_cast<uint8_t>(Ring0Msr::ReadIoPortDword(p.dataPort) & 0x0F);
        }
        // Divisor 0xF means fan input not connected.
        if (div == 0x0F) { LOGI("Super I/O: fan divisor=0xF (no fan connected)"); return -1; }
        if (div == 0)    div = 1; // avoid division by zero.

        // Read fan counter register (16-bit). Nuvoton offset 0x4A, ITE 0x48, SMSC 0x49.
        uint16_t cntRegOff = isNuvoton ? 0x4A : (isIte ? 0x48 : 0x49);
        if (!Ring0Msr::WriteIoPortByte(p.cfgPort, cntRegOff)) continue;
        uint32_t cntLow  = Ring0Msr::ReadIoPortDword(p.dataPort);
        if (!Ring0Msr::WriteIoPortByte(p.cfgPort, cntRegOff + 1)) continue;
        uint32_t cntHigh = Ring0Msr::ReadIoPortDword(p.dataPort);
        uint16_t fanCnt = static_cast<uint16_t>(
            (static_cast<uint16_t>(cntHigh & 0xFF) << 8) |
             static_cast<uint16_t>(cntLow  & 0xFF));

        // Decode RPM from counter. Formula varies by chip:
        //   Nuvoton/ITE: RPM = 1_350_000 / (div * cnt)   (12 pulses per rev, 90ms sample)
        //   SMSC F718xx: RPM = 1_350_000 / (div * cnt)   (same convention).
        if (fanCnt == 0) return -1;
        int rpm = static_cast<int>(1350000.0 / (static_cast<double>(div) * fanCnt) + 0.5);
        if (rpm > 0 && rpm < 50000)
        {
            LOGI("Super I/O fan: base=0x%04X div=%u cnt=0x%04X RPM=%d",
                 baseAddr, div, fanCnt, rpm);
            return rpm;
        }
    }
    return -1;
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
    // CoCreateInstance directly without wrapping it — initialize once here.
    HRESULT comHres = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool comInit = (comHres == S_OK || comHres == S_FALSE);
    (void)comInit;

    // ---- WMI: Win32_TemperatureProbe (root\cimv2) — enumerate ALL rows + props --
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

    // ---- WMI: MSAcpi_ThermalZoneTemperature (root\wmi) — enumerate ALL rows + props --
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
            LOGI("CPU name: %s", m_metrics.cpuName);
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
