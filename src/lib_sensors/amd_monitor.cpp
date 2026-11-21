#include "amd_monitor.h"
#include "logger.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <string>
#include <vector>

// ============================================================================
// Common constants
// ============================================================================

// PCI Vendor ID of AMD.
static const int AMD_PCI_VENDOR_ID = 0x1002;

// ADLAdapterInfo.iVendorID is not filled with the PCI hex code but with a number
// built from its decimal digits: AMD gives "1002" = 0x03EA, NVIDIA gives 10.
// Matching on 0x1002 alone rejects every working adapter (verified from a
// dump: live AMD records have vendor equal to 0x03EA).
static const int ADL_VENDOR_ID_AMD_DECIMAL = 1002;

// ============================================================================
// Class skeleton (common to all platforms)
// ============================================================================

AmdMonitor::AmdMonitor()
    : m_available(false)
{
#ifdef PLATFORM_WINDOWS
    m_available = LoadAdl();
#else
    m_available = LoadSysfs();
#endif
}

AmdMonitor::~AmdMonitor()
{
#ifdef PLATFORM_WINDOWS
    FreeAdl();
#endif
}

bool AmdMonitor::IsAvailable() const
{
    return m_available;
}

std::vector<GpuMetrics> AmdMonitor::GetMetrics()
{
    std::vector<GpuMetrics> result;
    if (!m_available)
        return result;

#ifdef PLATFORM_WINDOWS
    CollectAdl(result);
#else
    CollectSysfs(result);
#endif
    return result;
}

// ============================================================================
// Windows: ADL2 + DXGI
// ============================================================================
#ifdef PLATFORM_WINDOWS

#include <windows.h>

// DXGI interface GUIDs must be defined in this TU
// (without the need to link dxguid.lib).
#include <initguid.h>
#include <dxgi.h>
#include <dxgi1_6.h>

// ADL result codes (adl_defines.h).
#define ADL_OK                        0
#define ADL_MAX_PATH                  256
#define ADL_MAX_ADAPTERS              250

// OD6 fan speed types (bit mask in iSpeedType).
#define ADL_OD6_FANSPEED_TYPE_PERCENT 0x00000001
#define ADL_OD6_FANSPEED_TYPE_RPM     0x00000002

// OD5 thermal controller capabilities mask (iFlags field of ADLFanSpeedInfo).
#define ADL_DL_FANCTRL_SUPPORTS_PERCENT_READ  1
#define ADL_DL_FANCTRL_SUPPORTS_RPM_READ      4

// Request unit for ADL2_Overdrive5_FanSpeed_Get (iSpeedType field).
#define ADL_DL_FANCTRL_SPEED_TYPE_PERCENT     1
#define ADL_DL_FANCTRL_SPEED_TYPE_RPM         2

// Maximum number of thermal controllers iterated over in OD5.
static const int ADL_OD5_MAX_THERMAL_CONTROLLERS = 8;

// --- ADL structures. Layout exactly as in adl_structures.h SDK 18.1 ---

namespace {

struct ADLAdapterInfo
{
    int  iSize;
    int  iAdapterIndex;
    char strUDID[ADL_MAX_PATH];
    int  iBusNumber;
    int  iDeviceNumber;
    int  iFunctionNumber;
    int  iVendorID;
    char strAdapterName[ADL_MAX_PATH];
    char strDisplayName[ADL_MAX_PATH];
    int  iPresent;
    int  iExist;
    char strDriverPath[ADL_MAX_PATH];
    char strDriverPathExt[ADL_MAX_PATH];
    char strPNPString[ADL_MAX_PATH];
    int  iOSDisplayIndex;
};

struct ADLPMActivity
{
    int iSize;
    int iEngineClock;
    int iMemoryClock;
    int iVddc;
    int iActivityPercent;
    int iCurrentPerformanceLevel;
    int iCurrentBusSpeed;
    int iCurrentBusLanes;
    int iMaximumBusLanes;
    int iReserved;
};

struct ADLTemperature
{
    int iSize;
    int iTemperature;   // millidegrees Celsius
};

struct ADLFanSpeedInfo
{
    int iSize;
    int iFlags;
    int iMinPercent;
    int iMaxPercent;
    int iMinRPM;
    int iMaxRPM;
};

struct ADLFanSpeedValue
{
    int iSize;
    int iSpeedType;
    int iFanSpeed;
    int iFlags;
};

// ADLOD6CurrentStatus: clocks in units of 10 KHz (documented in the SDK).
struct ADLOD6CurrentStatus
{
    int iEngineClock;
    int iMemoryClock;
    int iActivityPercent;
    int iCurrentPerformanceLevel;
    int iCurrentBusSpeed;
    int iCurrentBusLanes;
    int iMaximumBusLanes;
    int iExtValue;
    int iExtMask;
};

// ADLOD6FanSpeedInfo: iSpeedType - bit mask of field validity.
struct ADLOD6FanSpeedInfo
{
    int iSpeedType;
    int iFanSpeedPercent;
    int iFanSpeedRPM;
    int iExtValue;
    int iExtMask;
};

// ADLMemoryInfo2 (adl_structures.h, SDK 18.1): memory sizes in bytes.
struct ADLMemoryInfo2
{
    long long iMemorySize;         // total video memory size
    char      strMemoryType[ADL_MAX_PATH];
    long long iMemoryBandwidth;    // MByte/s at the maximum P-state
    long long iHyperMemorySize;
    long long iInvisibleMemorySize;
    long long iVisibleMemorySize;
};

/// Character that is part of a word (a Latin letter or a digit).
bool IsWordChar(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9');
}

/// Case-insensitive substring search (ASCII only). Comparison follows the
/// string lengths: there is no reading past the terminating null. wordBounds requires
/// the match to be a standalone word.
bool FindNoCase(const char* text, const char* needle, bool wordBounds)
{
    if (!text || !needle)
        return false;

    const size_t n = strlen(needle);
    if (n == 0)
        return false;

    for (const char* p = text; *p != '\0'; ++p)
    {
        size_t i = 0;
        while (i < n)
        {
            if (p[i] == '\0')
                break;
            if (tolower(static_cast<unsigned char>(p[i])) !=
                tolower(static_cast<unsigned char>(needle[i])))
                break;
            ++i;
        }
        if (i != n)
            continue;
        if (!wordBounds)
            return true;

        // p[0..n-1] are known to exist (otherwise we broke out above), so
        // p[n] is at worst the terminating null, the read is safe.
        const bool leftOk  = (p == text) || !IsWordChar(p[-1]);
        const bool rightOk = !IsWordChar(p[n]);
        if (leftOk && rightOk)
            return true;
    }
    return false;
}

/// Indirect sign of an AMD adapter from its name ("AMD Radeon RX 5700 XT",
/// "ATI Radeon E2460", "AMD FirePro W5100", "Ryzen Graphics"). Needed because
/// some driver versions do not fill in ADLAdapterInfo.iVendorID.
bool LooksLikeAmdName(const char* name)
{
    static const char* const kWords[] = { "radeon", "ati", "amd", "firepro",
                                          "firegl", "ryzen" };
    for (const char* word : kWords)
    {
        if (FindNoCase(name, word, true))
            return true;
    }
    return false;
}

/// True if the ADL iVendorID corresponds to AMD. Drivers report this field in
/// a "decimal" encoding (see ADL_VENDOR_ID_AMD_DECIMAL), so both variants are
/// accepted; comparing only against 0x1002 finds no working adapters.
bool IsAmdAdlVendor(int vendorId)
{
    return vendorId == AMD_PCI_VENDOR_ID || vendorId == ADL_VENDOR_ID_AMD_DECIMAL;
}

/// WCHAR[] -> UTF-8. The source length is passed explicitly, so an unterminated
/// string in DXGI Description will not cause reads past the buffer.
std::string WideToUtf8(const wchar_t* text, size_t chars)
{
    if (!text || chars == 0)
        return std::string();

    size_t len = 0;
    while (len < chars && text[len] != L'\0')
        ++len;
    if (len == 0)
        return std::string();

    const int need = WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(len),
                                         nullptr, 0, nullptr, nullptr);
    if (need <= 0 || need > 4096)
        return std::string();

    std::string out(static_cast<size_t>(need), '\0');
    const int written = WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(len),
                                            &out[0], need, nullptr, nullptr);
    if (written <= 0)
        return std::string();
    out.resize(static_cast<size_t>(written));
    return out;
}

/// One-time dump of all ADL records. Lets the log distinguish "no card" from
/// "ADL sees an adapter, but the wrong vendor/flags".
void LogAdlAdapters(const std::vector<ADLAdapterInfo>& adapters)
{
    for (size_t i = 0; i < adapters.size(); ++i)
    {
        const ADLAdapterInfo& a = adapters[i];
        LOGI("DIAG ADL[%zu]: idx=%d bus=%d dev=%d func=%d vendor=0x%04X (%d) "
             "present=%d exist=%d name='%.48s' display='%.48s'",
             i, a.iAdapterIndex, a.iBusNumber, a.iDeviceNumber,
             a.iFunctionNumber, static_cast<unsigned>(a.iVendorID), a.iVendorID,
             a.iPresent, a.iExist, a.strAdapterName, a.strDisplayName);
    }
}

/// Enumerates Windows display devices (user32 EnumDisplayDevices,
/// documented API, requires no administrator rights) and prints them.
/// Returns true if the OS reports a PCI device of vendor 0x1002 - this is an
/// ADL-independent check that an AMD card exists in the system.
bool LogOsDisplayDevices()
{
    bool seesAmd = false;

    // 16 is a sane limit: EnumDisplayDevices returns FALSE on the very first
    // index past the device list; the loop is guarded in case of driver failure.
    for (DWORD i = 0; i < 16; ++i)
    {
        DISPLAY_DEVICEA dev{};
        dev.cb = sizeof(DISPLAY_DEVICEA);
        if (!EnumDisplayDevicesA(nullptr, i, &dev, 0))
            break;

        const bool amd = FindNoCase(dev.DeviceID, "VEN_1002", false) ||
                         LooksLikeAmdName(dev.DeviceString);
        if (amd)
            seesAmd = true;

        LOGI("DIAG OS display %lu%s: id='%.96s' name='%.48s' flags=0x%08X",
             i, amd ? " (AMD)" : "", dev.DeviceID, dev.DeviceString,
             static_cast<unsigned>(dev.StateFlags));
    }
    return seesAmd;
}

/// String length with an explicit cap: reading stops at limit bytes,
/// so an unterminated string in ADL fields is safe.
size_t StrLenN(const char* text, size_t limit)
{
    if (!text)
        return 0;
    size_t n = 0;
    while (n < limit && text[n] != '\0')
        ++n;
    return n;
}

/// ADL allocation callback (ADL_MAIN_MALLOC_CALLBACK).
/// ADL calls it for its own internal buffers.
void* __stdcall AdlMallocCallback(int iSize)
{
    if (iSize <= 0)
        return nullptr;
    return malloc(static_cast<size_t>(iSize));
}

/// Convert an ADL temperature to degrees: both OD5 (ADLTemperature.iTemperature)
/// and OD6 (ADL2_Overdrive6_Temperature_Get) report millidegrees Celsius.
double NormalizeAdlTemperature(int raw)
{
    if (raw <= 0)
        return -1.0;
    double celsius = static_cast<double>(raw) / 1000.0;
    if (celsius < 1.0 && raw <= 250)
        celsius = static_cast<double>(raw);   // fallback: the driver returned degrees
    if (celsius <= 0.0 || celsius > 250.0)
        return -1.0;
    return celsius;
}

} // namespace

// ---------------------------------------------------------------------------
// ADL/DXGI initialization
// ---------------------------------------------------------------------------

bool AmdMonitor::LoadAdl()
{
    // DXGI comes up first: it is the only source of VRAM size and usage, and
    // when ADL is unavailable (basic display driver, card without ADL2) it is
    // the only way to see the card at all.
    OpenDxgi();

    const bool adlOk = InitAdl();

    // ADL-independent check: does Windows report a display device with PCI
    // vendor 0x1002. Cheap call, done once at startup.
    const bool osSeesAmd = LogOsDisplayDevices();

    // Probe for the presence of at least one AMD card.
    std::vector<GpuMetrics> probe;
    CollectAdl(probe);
    if (probe.empty())
    {
        if (osSeesAmd)
        {
            LOGI("AMD: the OS reports an AMD display device, but neither ADL nor "
                 "DXGI returned data - check the display driver in Device Manager.");
        }
        else
        {
            LOGI("AMD: the OS reports no AMD display device (PCI vendor 0x1002).");
        }
        LOGI("AMD: no AMD GPU available (ADL %s, DXGI %s).",
             adlOk ? "initialized" : "unavailable",
             m_dxgiFactory ? "available" : "unavailable");
        FreeAdl();
        return false;
    }

    // Diagnostics for optional entry points: a missing function means that
    // the corresponding metric will be N/A on this driver.
    if (adlOk)
    {
        if (!m_fnOD6Status)   LOGI("AMD: ADL2_Overdrive6_CurrentStatus_Get missing - load/clocks unavailable.");
        if (!m_fnOD5Activity) LOGI("AMD: ADL2_Overdrive5_CurrentActivity_Get missing - no legacy load source.");
        if (!m_fnOD6Temp)     LOGI("AMD: ADL2_Overdrive6_Temperature_Get missing.");
        if (!m_fnOD5Temp)     LOGI("AMD: ADL2_Overdrive5_Temperature_Get missing.");
        if (!m_fnOD6Fan)      LOGI("AMD: ADL2_Overdrive6_FanSpeed_Get missing.");
        if (!m_fnOD5FanInfo || !m_fnOD5Fan) LOGI("AMD: ADL2_Overdrive5_FanSpeed* missing - fan unavailable.");
        if (!m_fnOD6Power)    LOGI("AMD: ADL2_Overdrive6_CurrentPower_Get missing - power unavailable.");
        if (!m_fnMemInfo2)    LOGI("AMD: ADL2_Adapter_MemoryInfo2_Get missing - VRAM size from DXGI only.");
    }

    for (const GpuMetrics& g : probe)
    {
        LOGI("AMD: GPU '%s': load=%.0f%%, temp=%.0fC, power=%.1f/%.1f W, "
             "vram=%llu/%llu MB, fan=%.0f%%/%d rpm",
             g.name.c_str(), g.loadPercent, g.temperature, g.powerWatts,
             g.powerLimitWatts,
             static_cast<unsigned long long>(g.vramUsedBytes / (1024ULL * 1024ULL)),
             static_cast<unsigned long long>(g.vramTotalBytes / (1024ULL * 1024ULL)),
             g.fanPercent, g.fanRpm);
    }

    LOGI("AMD: monitor ready, %zu AMD GPU(s), source: %s.",
         probe.size(), adlOk ? "ADL" : "DXGI");
    return true;
}

/// Loads atiadlxx.dll, resolves the ADL2 entry points and creates the ADL context.
/// False means ADL metrics are unavailable on this system - monitoring is then
/// possible via DXGI only.
bool AmdMonitor::InitAdl()
{
    // atiadlxx.dll is the 64-bit library of modern Adrenalin drivers,
    // atiadlxy.dll is the legacy 32-bit name.
    const char* dllNames[] = { "atiadlxx.dll", "atiadlxy.dll" };
    for (const char* name : dllNames)
    {
        m_hAdl = LoadLibraryA(name);
        if (m_hAdl)
            break;
    }
    if (!m_hAdl)
    {
        LOGI("AMD: ADL library not found (no AMD display driver installed?).");
        return false;
    }

    // Required ADL2 entry points.
    m_fnCreate      = reinterpret_cast<FnAdlCreate>(
        GetProcAddress(m_hAdl, "ADL2_Main_Control_Create"));
    m_fnDestroy     = reinterpret_cast<FnAdlDestroy>(
        GetProcAddress(m_hAdl, "ADL2_Main_Control_Destroy"));
    m_fnNumAdapters = reinterpret_cast<FnNumAdaptersGet>(
        GetProcAddress(m_hAdl, "ADL2_Adapter_NumberOfAdapters_Get"));
    m_fnAdapterInfo = reinterpret_cast<FnAdapterInfoGet>(
        GetProcAddress(m_hAdl, "ADL2_Adapter_AdapterInfo_Get"));

    // Optional: without them some metrics simply are not collected.
    m_fnActiveGet   = reinterpret_cast<FnActiveGet>(
        GetProcAddress(m_hAdl, "ADL2_Adapter_Active_Get"));
    m_fnOD6Status   = reinterpret_cast<FnOD6StatusGet>(
        GetProcAddress(m_hAdl, "ADL2_Overdrive6_CurrentStatus_Get"));
    m_fnOD5Activity = reinterpret_cast<FnOD5ActivityGet>(
        GetProcAddress(m_hAdl, "ADL2_Overdrive5_CurrentActivity_Get"));
    m_fnOD6Temp     = reinterpret_cast<FnOD6TempGet>(
        GetProcAddress(m_hAdl, "ADL2_Overdrive6_Temperature_Get"));
    m_fnOD5Temp     = reinterpret_cast<FnOD5TempGet>(
        GetProcAddress(m_hAdl, "ADL2_Overdrive5_Temperature_Get"));
    m_fnOD6Fan      = reinterpret_cast<FnOD6FanGet>(
        GetProcAddress(m_hAdl, "ADL2_Overdrive6_FanSpeed_Get"));
    m_fnOD5FanInfo  = reinterpret_cast<FnOD5FanInfoGet>(
        GetProcAddress(m_hAdl, "ADL2_Overdrive5_FanSpeedInfo_Get"));
    m_fnOD5Fan      = reinterpret_cast<FnOD5FanGet>(
        GetProcAddress(m_hAdl, "ADL2_Overdrive5_FanSpeed_Get"));
    m_fnOD6Power    = reinterpret_cast<FnOD6PowerGet>(
        GetProcAddress(m_hAdl, "ADL2_Overdrive6_CurrentPower_Get"));
    m_fnMemInfo2    = reinterpret_cast<FnMemInfo2Get>(
        GetProcAddress(m_hAdl, "ADL2_Adapter_MemoryInfo2_Get"));

    if (!m_fnCreate || !m_fnDestroy || !m_fnNumAdapters || !m_fnAdapterInfo)
    {
        LOGI("AMD: ADL loaded but required ADL2 entry points are missing.");
        return false;
    }

    // iEnumConnectedAdapters = 0: enumerate all adapters (the filtering by
    // vendor and presence flags is done by us).
    int rc = m_fnCreate(reinterpret_cast<void*>(AdlMallocCallback), 0, &m_adlContext);
    if (rc != ADL_OK || !m_adlContext)
    {
        m_adlContext = nullptr;
        LOGI("AMD: ADL2_Main_Control_Create failed (code %d).", rc);
        return false;
    }
    return true;
}

/// Loads dxgi.dll and creates IDXGIFactory1. Without it VRAM stays N/A, but the
/// other metrics keep working, so the failure is not treated as fatal.
void AmdMonitor::OpenDxgi()
{
    m_hDxgi = LoadLibraryA("dxgi.dll");
    if (!m_hDxgi)
    {
        LOGI("AMD: dxgi.dll not loaded, VRAM metrics unavailable.");
        return;
    }

    using FnCreateDXGIFactory1 = HRESULT (*)(REFIID riid, void** ppFactory);
    FnCreateDXGIFactory1 createFactory = reinterpret_cast<FnCreateDXGIFactory1>(
        GetProcAddress(m_hDxgi, "CreateDXGIFactory1"));
    if (!createFactory)
    {
        LOGI("AMD: CreateDXGIFactory1 export missing, VRAM metrics unavailable.");
        return;
    }

    HRESULT hr = createFactory(IID_IDXGIFactory1,
                               reinterpret_cast<void**>(&m_dxgiFactory));
    if (FAILED(hr) || !m_dxgiFactory)
    {
        LOGI("AMD: CreateDXGIFactory1 failed, VRAM metrics unavailable.");
        m_dxgiFactory = nullptr;
    }
}

void AmdMonitor::FreeAdl()
{
    if (m_dxgiFactory)
    {
        m_dxgiFactory->Release();
        m_dxgiFactory = nullptr;
    }
    if (m_adlContext && m_fnDestroy)
    {
        m_fnDestroy(m_adlContext);
        m_adlContext = nullptr;
    }
    if (m_hAdl)
    {
        FreeLibrary(m_hAdl);
        m_hAdl = nullptr;
    }
    if (m_hDxgi)
    {
        FreeLibrary(m_hDxgi);
        m_hDxgi = nullptr;
    }
    m_fnCreate       = nullptr;
    m_fnDestroy      = nullptr;
    m_fnNumAdapters  = nullptr;
    m_fnAdapterInfo  = nullptr;
    m_fnActiveGet    = nullptr;
    m_fnOD6Status    = nullptr;
    m_fnOD5Activity  = nullptr;
    m_fnOD6Temp      = nullptr;
    m_fnOD5Temp      = nullptr;
    m_fnOD6Fan       = nullptr;
    m_fnOD5FanInfo   = nullptr;
    m_fnOD5Fan       = nullptr;
    m_fnOD6Power     = nullptr;
    m_fnMemInfo2     = nullptr;
    m_available      = false;
}

// ---------------------------------------------------------------------------
// Metrics collection
// ---------------------------------------------------------------------------

void AmdMonitor::CollectAdl(std::vector<GpuMetrics>& out)
{
    out.clear();

    // One-time diagnostics print only on the first pass (the call from
    // LoadAdl): after that the poll runs once per second and the log must not be polluted.
    const bool diag = !m_enumLogged;

    CollectViaAdl(out, diag);

    // ADL returned no cards, but the adapter is visible to the operating system:
    // show whatever DXGI can do (name and VRAM) instead of hiding
    // the card entirely.
    if (out.empty())
        CollectDxgiOnly(out, diag);

    m_enumLogged = true;
}

/// Metrics via ADL: adapter enumeration and Overdrive5/Overdrive6 queries.
/// diag = true prints a one-time dump of what ADL returned.
void AmdMonitor::CollectViaAdl(std::vector<GpuMetrics>& out, bool diag)
{
    if (!m_adlContext || !m_fnNumAdapters || !m_fnAdapterInfo)
        return;

    int numAdapters = 0;
    if (m_fnNumAdapters(m_adlContext, &numAdapters) != ADL_OK)
        return;
    if (numAdapters <= 0 || numAdapters > ADL_MAX_ADAPTERS)
        return;

    if (diag)
        LOGI("AMD: ADL reports %d OS adapter(s).", numAdapters);

    std::vector<ADLAdapterInfo> adapters(static_cast<size_t>(numAdapters));
    for (size_t i = 0; i < adapters.size(); ++i)
        adapters[i].iSize = sizeof(ADLAdapterInfo);

    const int bufBytes = static_cast<int>(adapters.size() * sizeof(ADLAdapterInfo));
    if (m_fnAdapterInfo(m_adlContext, adapters.data(), bufBytes) != ADL_OK)
        return;

    if (diag)
        LogAdlAdapters(adapters);

    // One physical adapter can occupy several records - keep
    // the first suitable one.
    struct AdapterRef
    {
        int         index;      // ADL adapter index (adapter's first record)
        int         bus;        // address on the PCI bus: identifies the adapter
        int         dev;
        int         func;
        std::string udid;       // UDID is the same for all paths of one adapter
        unsigned    ordinal;    // ordinal number among AMD adapters
        std::string name;
    };
    std::vector<AdapterRef> targets;
    size_t duplicateEntries = 0;

    for (const ADLAdapterInfo& a : adapters)
    {
        // Vendor is taken from iVendorID (allowing for ADL's "decimal" encoding); if
        // the driver left the field unset - fall back to the adapter name.
        const bool isAmd = IsAmdAdlVendor(a.iVendorID) ||
                           LooksLikeAmdName(a.strAdapterName) ||
                           LooksLikeAmdName(a.strDisplayName);
        if (!isAmd)
            continue;

        // iPresent - the adapter is active now, iExist - it is installed. A card with
        // no connected monitor (typical LLM scenario) often answers
        // present=0, so either of the two flags is enough. If the driver set
        // neither, but the adapter has an address on the PCI bus and a name - it is
        // a real device (empty placeholder records have bus=-1).
        const bool onPciBus = (a.iBusNumber >= 0) && (a.strAdapterName[0] != '\0');
        if (!a.iPresent && !a.iExist && !onPciBus)
            continue;

        // ADL lists every display path as a separate record: the R9 280 has
        // 8 records with indexes 0..7 but a single PCI address
        // (bus=3 dev=0 func=0). The physical adapter is recognized by its address on
        // the bus (and by the UDID), not by iAdapterIndex, otherwise one card
        // turns into eight.
        const std::string udid(a.strUDID, StrLenN(a.strUDID, ADL_MAX_PATH - 1));
        bool dup = false;
        for (const AdapterRef& t : targets)
        {
            const bool samePci = (a.iBusNumber >= 0) &&
                                 (t.bus  == a.iBusNumber) &&
                                 (t.dev  == a.iDeviceNumber) &&
                                 (t.func == a.iFunctionNumber);
            const bool sameUdid = !t.udid.empty() && t.udid == udid;
            if (samePci || sameUdid)
            {
                dup = true;
                break;
            }
        }
        if (dup)
        {
            ++duplicateEntries;
            continue;
        }

        AdapterRef ref{};
        ref.index   = a.iAdapterIndex;
        ref.bus     = a.iBusNumber;
        ref.dev     = a.iDeviceNumber;
        ref.func    = a.iFunctionNumber;
        ref.udid    = udid;
        ref.ordinal = static_cast<unsigned>(targets.size());
        if (a.strAdapterName[0] != '\0')
            ref.name = a.strAdapterName;
        else if (a.strDisplayName[0] != '\0')
            ref.name = a.strDisplayName;
        else
            ref.name = "AMD GPU";
        targets.push_back(ref);
    }

    if (diag)
    {
        LOGI("AMD: %zu physical adapter(s) from %zu ADL entry(ies), "
             "%zu display path(s) skipped as duplicates.",
             targets.size(), targets.size() + duplicateEntries, duplicateEntries);
    }

    for (const AdapterRef& t : targets)
    {
        // ADL2_Adapter_Active_Get is not used as a filter: an adapter that
        // ADL considers inactive (card with no connected monitor, compute mode)
        // still returns its Overdrive metrics. An adapter without any
        // data at all is dropped below, based on the query results.
        int active = 1;
        if (m_fnActiveGet &&
            m_fnActiveGet(m_adlContext, t.index, &active) != ADL_OK)
        {
            active = 1;
        }
        if (diag && !active)
            LOGI("AMD: adapter %d is reported inactive by ADL, probing anyway.",
                 t.index);

        GpuMetrics gm{};
        gm.name = t.name;
        // Explicit N/A: if neither OD6 nor OD5 returned a valid iActivityPercent,
        // the GUI hides the load graph instead of a lying "0%".
        gm.loadPercent = -1.0;

        // Load and clocks: the primary source is ADLOD6CurrentStatus
        // (clocks documented in units of 10 KHz), the fallback is OD5 activity.
        bool haveActivity = false;
        if (m_fnOD6Status)
        {
            ADLOD6CurrentStatus status{};
            if (m_fnOD6Status(m_adlContext, t.index, &status) == ADL_OK)
            {
                haveActivity = true;
                if (status.iActivityPercent >= 0 && status.iActivityPercent <= 100)
                    gm.loadPercent = static_cast<double>(status.iActivityPercent);
                if (status.iEngineClock > 0)
                    gm.gpuFreqMhz = static_cast<double>(status.iEngineClock) / 100.0;
                if (status.iMemoryClock > 0)
                    gm.vramFreqMhz = static_cast<double>(status.iMemoryClock) / 100.0;
            }
        }
        if (!haveActivity && m_fnOD5Activity)
        {
            ADLPMActivity activity{};
            activity.iSize = sizeof(ADLPMActivity);
            if (m_fnOD5Activity(m_adlContext, t.index, &activity) == ADL_OK)
            {
                // ADLPMActivity clock units are not documented in the SDK,
                // so only the load percentage is taken.
                if (activity.iActivityPercent >= 0 && activity.iActivityPercent <= 100)
                    gm.loadPercent = static_cast<double>(activity.iActivityPercent);
            }
        }

        // Temperature: OD6 first (int, millidegrees), then iterating over
        // OD5 thermal controllers (ADLTemperature.iTemperature - millidegrees).
        double tempC = -1.0;
        if (m_fnOD6Temp)
        {
            int raw = 0;
            if (m_fnOD6Temp(m_adlContext, t.index, &raw) == ADL_OK)
                tempC = NormalizeAdlTemperature(raw);
        }
        if (tempC < 0.0 && m_fnOD5Temp)
        {
            for (int c = 0; c < ADL_OD5_MAX_THERMAL_CONTROLLERS && tempC < 0.0; ++c)
            {
                ADLTemperature tr{};
                tr.iSize = sizeof(ADLTemperature);
                if (m_fnOD5Temp(m_adlContext, t.index, c, &tr) == ADL_OK)
                    tempC = NormalizeAdlTemperature(tr.iTemperature);
            }
        }
        gm.temperature = tempC;

        // Fan: OD6 (percent/RPM validity mask), then OD5
        // through the thermal controller capabilities.
        bool fanDone = false;
        if (m_fnOD6Fan)
        {
            ADLOD6FanSpeedInfo fan{};
            if (m_fnOD6Fan(m_adlContext, t.index, &fan) == ADL_OK)
            {
                if ((fan.iSpeedType & ADL_OD6_FANSPEED_TYPE_PERCENT) &&
                    fan.iFanSpeedPercent >= 0 && fan.iFanSpeedPercent <= 100)
                {
                    gm.fanPercent = static_cast<double>(fan.iFanSpeedPercent);
                    fanDone = true;
                }
                if ((fan.iSpeedType & ADL_OD6_FANSPEED_TYPE_RPM) &&
                    fan.iFanSpeedRPM > 0 && fan.iFanSpeedRPM < 30000)
                {
                    gm.fanRpm = fan.iFanSpeedRPM;
                    fanDone = true;
                }
            }
        }
        if (!fanDone && m_fnOD5Fan && m_fnOD5FanInfo)
        {
            for (int c = 0; c < ADL_OD5_MAX_THERMAL_CONTROLLERS && !fanDone; ++c)
            {
                ADLFanSpeedInfo info{};
                info.iSize = sizeof(ADLFanSpeedInfo);
                if (m_fnOD5FanInfo(m_adlContext, t.index, c, &info) != ADL_OK)
                    continue;

                if (info.iFlags & ADL_DL_FANCTRL_SUPPORTS_PERCENT_READ)
                {
                    ADLFanSpeedValue val{};
                    val.iSize = sizeof(ADLFanSpeedValue);
                    val.iSpeedType = ADL_DL_FANCTRL_SPEED_TYPE_PERCENT;
                    if (m_fnOD5Fan(m_adlContext, t.index, c, &val) == ADL_OK &&
                        val.iFanSpeed >= 0 && val.iFanSpeed <= 100)
                    {
                        gm.fanPercent = static_cast<double>(val.iFanSpeed);
                        fanDone = true;
                    }
                }
                if (info.iFlags & ADL_DL_FANCTRL_SUPPORTS_RPM_READ)
                {
                    ADLFanSpeedValue val{};
                    val.iSize = sizeof(ADLFanSpeedValue);
                    val.iSpeedType = ADL_DL_FANCTRL_SPEED_TYPE_RPM;
                    if (m_fnOD5Fan(m_adlContext, t.index, c, &val) == ADL_OK &&
                        val.iFanSpeed > 0 && val.iFanSpeed < 30000)
                    {
                        gm.fanRpm = val.iFanSpeed;
                        fanDone = true;
                    }
                }
            }
        }

        // Power: ADL2_Overdrive6_CurrentPower_Get, iPowerType = 0
        // ("ASIC Total Power"), the value is Watts with 8 fractional bits.
        // ADL does not return a public power limit in Watts (PowerControl_Get
        // returns adjustment percentages), so powerLimitWatts = N/A.
        if (m_fnOD6Power)
        {
            int rawPower = 0;
            if (m_fnOD6Power(m_adlContext, t.index, 0, &rawPower) == ADL_OK)
            {
                double watts = static_cast<double>(rawPower) / 256.0;
                if (watts >= 0.0 && watts < 1200.0)
                    gm.powerWatts = watts;
            }
        }

        // VRAM: total comes from ADL (ADLMemoryInfo2.iMemorySize, bytes), used from
        // DXGI (IDXGIAdapter3::QueryVideoMemoryInfo), ADL never reports usage.
        uint64_t adlTotal = 0;
        if (m_fnMemInfo2)
        {
            ADLMemoryInfo2 mem{};
            if (m_fnMemInfo2(m_adlContext, t.index, &mem) == ADL_OK &&
                mem.iMemorySize > 0 && mem.iMemorySize < (1LL << 40))
            {
                adlTotal = static_cast<uint64_t>(mem.iMemorySize);
            }
        }

        uint64_t vramTotal = adlTotal;
        uint64_t vramUsed  = 0;
        uint64_t dxgiTotal = 0;
        if (QueryVramDXGI(t.ordinal, adlTotal, dxgiTotal, vramUsed))
        {
            if (vramTotal == 0)
                vramTotal = dxgiTotal;
        }
        gm.vramTotalBytes = vramTotal;
        gm.vramUsedBytes  = vramUsed;
        if (gm.vramTotalBytes > 0 && gm.vramUsedBytes <= gm.vramTotalBytes)
        {
            gm.vramLoadPercent = static_cast<double>(gm.vramUsedBytes) /
                                 static_cast<double>(gm.vramTotalBytes) * 100.0;
        }

        // An adapter for which ADL answered with no valid value at all
        // (disabled device, hybrid without Overdrive support) is not shown on
        // screen: the DXGI fallback in CollectAdl handles it instead. loadPercent is
        // not counted: GpuMetrics defaults it to 0.0, which is not a sign
        // of a driver response.
        const bool noData = (gm.temperature  < 0.0) && (gm.fanPercent < 0.0) &&
                            (gm.fanRpm       < 0)   && (gm.powerWatts < 0.0) &&
                            (gm.vramTotalBytes == 0) && (gm.gpuFreqMhz <= 0.0);
        if (noData)
        {
            if (diag)
            {
                LOGI("AMD: adapter %d ('%s') answered no ADL query, skipped.",
                     t.index, t.name.c_str());
            }
            continue;
        }

        out.push_back(gm);
    }
}

/// Fallback metrics source: adapters visible to DXGI. Used when ADL does not
/// enumerate the card (basic display driver, driver without ADL2). Only the
/// name and video memory are available, all other metrics stay N/A.
void AmdMonitor::CollectDxgiOnly(std::vector<GpuMetrics>& out, bool diag)
{
    std::vector<DxgiAmdInfo> list;
    if (!EnumerateDxgiAmd(list))
        return;

    for (const DxgiAmdInfo& d : list)
    {
        GpuMetrics gm{};
        gm.name           = d.name.empty() ? std::string("AMD GPU") : d.name;
        gm.loadPercent    = -1.0;
        gm.vramTotalBytes = d.dedicatedBytes;
        gm.vramUsedBytes  = d.usedValid ? d.usedBytes : 0;
        if (gm.vramTotalBytes > 0 && gm.vramUsedBytes <= gm.vramTotalBytes)
        {
            gm.vramLoadPercent = static_cast<double>(gm.vramUsedBytes) /
                                 static_cast<double>(gm.vramTotalBytes) * 100.0;
        }
        out.push_back(gm);
    }

    if (diag && !out.empty())
    {
        LOGI("AMD: %zu adapter(s) seen by DXGI but not by ADL - VRAM metrics only.",
             out.size());
    }
}

// ---------------------------------------------------------------------------
// VRAM via DXGI
// ---------------------------------------------------------------------------

/// List of hardware DXGI AMD adapters (software WARP is dropped).
/// Returns false if DXGI is unavailable or there are no AMD adapters.
bool AmdMonitor::EnumerateDxgiAmd(std::vector<DxgiAmdInfo>& out)
{
    out.clear();
    if (!m_dxgiFactory)
        return false;

    // 16 is a safety limit: EnumAdapters1 returns DXGI_ERROR_NOT_FOUND after
    // the last adapter, but the loop is guarded in case of driver failure.
    for (UINT i = 0; i < 16; ++i)
    {
        IDXGIAdapter1* adapter = nullptr;
        HRESULT hr = m_dxgiFactory->EnumAdapters1(i, &adapter);
        if (hr == DXGI_ERROR_NOT_FOUND)
            break;
        if (FAILED(hr))
        {
            // On an enumeration failure an adapter may still have been handed out -
            // release the reference so the COM refcount is not leaked.
            if (adapter)
                adapter->Release();
            continue;
        }
        if (!adapter)
            continue;

        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter->GetDesc1(&desc)) &&
            desc.VendorId == static_cast<UINT>(AMD_PCI_VENDOR_ID) &&
            (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0)
        {
            DxgiAmdInfo item;
            item.name = WideToUtf8(desc.Description,
                                   sizeof(desc.Description) / sizeof(wchar_t));
            item.dedicatedBytes = static_cast<uint64_t>(desc.DedicatedVideoMemory);

            // Used: IDXGIAdapter3::QueryVideoMemoryInfo (Windows 10 and newer).
            IDXGIAdapter3* adapter3 = nullptr;
            if (SUCCEEDED(adapter->QueryInterface(IID_IDXGIAdapter3,
                                                  reinterpret_cast<void**>(&adapter3))))
            {
                DXGI_QUERY_VIDEO_MEMORY_INFO vmi{};
                if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(
                        0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &vmi)))
                {
                    item.usedBytes = vmi.CurrentUsage;
                    item.usedValid = true;
                }
                adapter3->Release();
            }
            out.push_back(item);
        }

        adapter->Release();
    }

    return !out.empty();
}

bool AmdMonitor::QueryVramDXGI(unsigned amdOrdinal, uint64_t adlTotalBytes,
                               uint64_t& totalBytes, uint64_t& usedBytes)
{
    totalBytes = 0;
    usedBytes  = 0;

    std::vector<DxgiAmdInfo> list;
    if (!EnumerateDxgiAmd(list))
        return false;

    // Matching against the ADL adapter. DXGI provides no PCI address, therefore:
    // 1) an unambiguous match by the dedicated memory size;
    // 2) otherwise - the ordinal number among the AMD adapters.
    const uint64_t SIZE_TOLERANCE = 64ULL * 1024ULL * 1024ULL;
    size_t pick = list.size();

    if (adlTotalBytes > 0)
    {
        size_t   best       = list.size();
        uint64_t bestDelta  = SIZE_TOLERANCE + 1;
        bool     ambiguous  = false;
        for (size_t j = 0; j < list.size(); ++j)
        {
            uint64_t delta = (list[j].dedicatedBytes > adlTotalBytes)
                           ? (list[j].dedicatedBytes - adlTotalBytes)
                           : (adlTotalBytes - list[j].dedicatedBytes);
            if (delta <= SIZE_TOLERANCE)
            {
                if (delta == bestDelta)
                    ambiguous = true;
                else if (delta < bestDelta)
                {
                    bestDelta = delta;
                    best      = j;
                    ambiguous = false;
                }
            }
        }
        if (!ambiguous && best < list.size())
            pick = best;
    }

    if (pick >= list.size() && amdOrdinal < list.size())
        pick = static_cast<size_t>(amdOrdinal);

    if (pick >= list.size())
        return false;

    totalBytes = list[pick].dedicatedBytes;
    usedBytes  = list[pick].usedValid ? list[pick].usedBytes : 0;
    return true;
}

#endif // PLATFORM_WINDOWS

// ============================================================================
// Linux: amdgpu driver sysfs/hwmon
// Interfaces are documented in Documentation/gpu/amdgpu/thermal:
//   device: gpu_busy_percent, mem_info_vram_total/used (bytes), product_name
//   hwmon (name=amdgpu): temp1_input (mC), power1_average/input (uW),
//         power1_cap (uW), fan1_input (RPM), pwm1/pwm1_max (0..255),
//         freq1_input (Hz, gfx), freq2_input (Hz, mem)
// No external processes are spawned - the polling cost is close to zero.
// ============================================================================
#ifndef PLATFORM_WINDOWS

#include <dirent.h>
#include <unistd.h>
#include <climits>
#include <cerrno>

namespace {

/// Trim whitespace characters from both ends.
std::string Trim(const std::string& s)
{
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos)
        return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

/// Read a text sysfs file. False if the file is missing or unreadable.
bool ReadSysfsStr(const std::string& path, std::string& out)
{
    FILE* f = fopen(path.c_str(), "r");
    if (!f)
        return false;
    char buf[512];
    bool ok = (fgets(buf, sizeof(buf), f) != nullptr);
    fclose(f);
    if (!ok)
        return false;
    out = Trim(buf);
    return !out.empty();
}

/// Read a numeric sysfs file. False on absence/error/garbage input.
bool ReadSysfsLL(const std::string& path, long long& out)
{
    std::string s;
    if (!ReadSysfsStr(path, s))
        return false;
    errno = 0;
    char* end = nullptr;
    long long v = strtoll(s.c_str(), &end, 10);
    if (errno != 0 || end == s.c_str())
        return false;
    out = v;
    return true;
}

/// Target name of a sysfs symlink (basename of readlink). Empty if not a symlink.
std::string LinkBaseName(const std::string& path)
{
    char buf[PATH_MAX];
    ssize_t n = readlink(path.c_str(), buf, sizeof(buf) - 1);
    if (n <= 0)
        return "";
    buf[n] = '\0';
    std::string target(buf);
    size_t pos = target.find_last_of('/');
    return (pos == std::string::npos) ? target : target.substr(pos + 1);
}

/// Canonical path (symlink resolved). Empty on error.
std::string RealPath(const std::string& path)
{
    char buf[PATH_MAX];
    if (!realpath(path.c_str(), buf))
        return "";
    return buf;
}

/// List of subdirectory names of the base directory.
std::vector<std::string> ListSubdirs(const std::string& base)
{
    std::vector<std::string> names;
    DIR* dir = opendir(base.c_str());
    if (!dir)
        return names;
    dirent* ent;
    while ((ent = readdir(dir)) != nullptr)
    {
        if (ent->d_name[0] == '.')
            continue;
        names.push_back(ent->d_name);
    }
    closedir(dir);
    return names;
}

struct AmdCard
{
    std::string drmPath;   // /sys/class/drm/cardN/device
    std::string cardName;  // cardN
    std::string bdf;       // PCI address like 0000:01:00.0 (may be empty)
    std::string device;    // PCI device ID, "0xabcd"
    std::string driver;    // driver name from device/driver (radeon/amdgpu/...)
    int cardIndex;         // numeric DRM driver index, -1 if unknown
};

/// DRM card driver name: basename of the device/driver symlink. Empty on error.
std::string GetDriverName(const std::string& devPath)
{
    return LinkBaseName(devPath + "/driver");
}

/// Find all AMD DRM cards (vendor 0x1002) in /sys/class/drm.
std::vector<AmdCard> ScanAmdCards()
{
    std::vector<AmdCard> cards;
    const std::string drmRoot = "/sys/class/drm";

    for (const std::string& entry : ListSubdirs(drmRoot))
    {
        // Only entries of the form cardN matter (ports cardN-OutputN are skipped).
        if (entry.compare(0, 4, "card") != 0 || entry.size() <= 4)
            continue;
        bool digitsOnly = true;
        for (size_t i = 4; i < entry.size(); ++i)
        {
            if (entry[i] < '0' || entry[i] > '9')
            {
                digitsOnly = false;
                break;
            }
        }
        if (!digitsOnly)
            continue;

        const std::string devPath = drmRoot + "/" + entry + "/device";
        std::string vendor;
        if (!ReadSysfsStr(devPath + "/vendor", vendor))
            continue;
        if (vendor != "0x1002")
            continue;

        AmdCard card;
        card.drmPath = devPath;
        card.cardName = entry;
        card.bdf = LinkBaseName(devPath);
        card.driver = GetDriverName(devPath);
        ReadSysfsStr(devPath + "/device", card.device);

        // Numeric DRM driver index (needed for debugfs paths).
        card.cardIndex = -1;
        errno = 0;
        char* end = nullptr;
        long idx = strtol(entry.c_str() + 4, &end, 10);
        if (errno == 0 && end && *end == '\0' && idx >= 0 && idx <= 64)
            card.cardIndex = static_cast<int>(idx);

        cards.push_back(card);
    }
    return cards;
}

/// Find hwmon directories belonging to this DRM card.
/// Primary route (documented in Documentation/gpu/amdgpu/thermal):
/// /sys/class/drm/cardN/device/hwmon/hwmonX. On modern kernels amdgpu
/// registers two hwmon devices: one for the sensors (temperature,
/// power, clocks), the second for the fan, so a list is returned.
/// Fallback route: walk /sys/class/hwmon, checking that the canonical
/// path of the sensor lies inside this card's device directory.
std::vector<std::string> FindHwmonDirsForCard(const std::string& drmPath)
{
    std::vector<std::string> dirs;

    const std::string localHwmon = drmPath + "/hwmon";
    for (const std::string& entry : ListSubdirs(localHwmon))
    {
        if (entry.compare(0, 5, "hwmon") == 0)
            dirs.push_back(localHwmon + "/" + entry);
    }
    if (!dirs.empty())
        return dirs;

    const std::string devReal = RealPath(drmPath);
    if (devReal.empty())
        return dirs;

    const std::string hwmonRoot = "/sys/class/hwmon";
    for (const std::string& entry : ListSubdirs(hwmonRoot))
    {
        const std::string hdir = hwmonRoot + "/" + entry;
        std::string name;
        if (!ReadSysfsStr(hdir + "/name", name))
            continue;
        // Driver hwmon device names: amdgpu ("amdgpu"/"gfx")
        // and radeon ("radeon").
        if (name != "amdgpu" && name != "gfx" && name != "radeon")
            continue;
        const std::string hReal = RealPath(hdir);
        if (hReal.size() > devReal.size() &&
            hReal.compare(0, devReal.size(), devReal) == 0 &&
            hReal[devReal.size()] == '/')
        {
            dirs.push_back(hReal);
        }
    }
    return dirs;
}

/// Read a numeric hwmon file, iterating over all sensor directories of the card.
bool ReadHwmonLL(const std::vector<std::string>& dirs, const char* fileName,
                 long long& value)
{
    for (const std::string& dir : dirs)
    {
        if (ReadSysfsLL(dir + "/" + fileName, value))
            return true;
    }
    return false;
}

/// Whether the file/directory exists (for sensor availability diagnostics).
bool HasFile(const std::string& path)
{
    return access(path.c_str(), F_OK) == 0;
}

// ---------------------------------------------------------------------------
// Model name from the system pci.ids database (when the driver does not report it).
// Database format: "V vvvv  Vendor Name", " dddd  Device Name" (4 hex digits),
// classes "C class" and continuation lines (" \t...") are skipped, comment
// lines "#" do not break the vendor section.
// ---------------------------------------------------------------------------

/// pci.ids directories in common distributions.
const char* const PCI_IDS_PATHS[] = {
    "/usr/share/misc/pci.ids",       // Debian/Ubuntu
    "/usr/share/hwdata/pci.ids",     // Fedora/openSUSE
};

/// "Ellesmere [Radeon RX 470/480/580/588]" -> "Radeon RX 470/480/580/588".
std::string PolishPciName(const std::string& raw)
{
    const size_t lb = raw.find('[');
    const size_t rb = raw.rfind(']');
    if (lb != std::string::npos && rb != std::string::npos && rb > lb + 1)
    {
        std::string inner = Trim(raw.substr(lb + 1, rb - lb - 1));
        if (!inner.empty())
            return inner;
    }
    return Trim(raw);
}

/// PCI device name by vendor:device from pci.ids. Empty if the database is missing.
/// File reading only - no external processes are spawned.
/// vendorId/deviceId in the form "0x1002"/"0x679a" (as in sysfs).
std::string PciIdsName(const std::string& vendorId, const std::string& deviceId)
{
    if (deviceId.compare(0, 2, "0x") != 0 || deviceId.size() < 6)
        return "";
    if (vendorId.compare(0, 2, "0x") != 0 || vendorId.size() < 6)
        return "";
    std::string wantVen = vendorId.substr(2, 4);
    std::string wantDev = deviceId.substr(2);
    auto lower = [](std::string& s) {
        for (size_t i = 0; i < s.size(); ++i)
        {
            if (s[i] >= 'A' && s[i] <= 'F')
                s[i] = static_cast<char>(s[i] - 'A' + 'a');
        }
    };
    lower(wantVen);
    lower(wantDev);

    for (const char* idsPath : PCI_IDS_PATHS)
    {
        FILE* f = fopen(idsPath, "r");
        if (!f)
            continue;

        bool inVendor = false;
        std::string result;
        char line[512];
        while (fgets(line, sizeof(line), f))
        {
            if (line[0] == '\t')
            {
                if (!inVendor)
                    continue;
                // Class-subclass ("C ...") and continuation lines (" ...") - skipped.
                if (line[1] == 'C' || line[1] == ' ')
                    continue;
                // Subsystem lines ("\t\tvvvv dddd ...") are skipped as well:
                // the device ID sits right after a single tab.
                if (line[2] == '\t')
                    continue;
                auto isHex = [](char c) {
                    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                           (c >= 'A' && c <= 'F');
                };
                if (!isHex(line[1]) || !isHex(line[2]) || !isHex(line[3]) || !isHex(line[4]))
                    continue;
                // The device ID is exactly 4 hex digits after a single tab.
                char idBuf[8];
                for (int k = 0; k < 4; ++k)
                {
                    idBuf[k] = (line[1 + k] >= 'A' && line[1 + k] <= 'F')
                        ? static_cast<char>(line[1 + k] - 'A' + 'a')
                        : line[1 + k];
                }
                idBuf[4] = '\0';
                if (wantDev == idBuf)
                {
                    size_t p = 5;
                    while (p < sizeof(line) && (line[p] == ' ' || line[p] == '\t'))
                        ++p;
                    result = Trim(line + p);
                    break;
                }
                continue;
            }
            // Vendor line: "vvvv  Name". The section of the wanted vendor
            // (1002 AMD, 10de NVIDIA etc.). "#" comments inside the
            // device block do not break the section.
            if (line[0] != '#')
                inVendor = (strncmp(line, wantVen.c_str(), 4) == 0 &&
                            (line[4] == ' ' || line[4] == '\t'));
        }
        fclose(f);
        if (!result.empty())
            return PolishPciName(result);
    }
    return "";
}

} // namespace

// Public entry point for GpuMonitor (listing NVIDIA cards without a driver).
// The pci.ids parser stays inside the file, only the result is exposed.
std::string AmdMonitor::ResolvePciName(const std::string& vendorId,
                                       const std::string& deviceId)
{
    return PciIdsName(vendorId, deviceId);
}

bool AmdMonitor::LoadSysfs()
{
    std::vector<AmdCard> cards = ScanAmdCards();
    if (cards.empty())
    {
        LOGI("AMD: No AMD GPU found in /sys/class/drm (amdgpu/radeon driver missing?).");
        return false;
    }
    LOGI("AMD: Found %zu AMD card(s) via amdgpu sysfs.", cards.size());

    // Detailed environment diagnostics (once at startup): driver, PCI ID,
    // presence of each required file. Lets the log reveal which
    // sensors the driver really does not provide and which are unreachable by path.
    for (const AmdCard& card : cards)
    {
        LOGI("AMD diag: card '%s' dev='%s' path=%s bdf='%s' driver='%s'",
             card.cardName.c_str(), card.device.c_str(), card.drmPath.c_str(),
             card.bdf.c_str(), card.driver.c_str());

        std::string p;
        std::string pretty;
        ReadSysfsStr(card.drmPath + "/product_name", p);
        ReadSysfsStr(card.drmPath + "/pretty_name", pretty);
        long long vramTotal = -1;
        ReadSysfsLL(card.drmPath + "/mem_info_vram_total", vramTotal);
        LOGI("AMD diag: product_name='%s' pretty_name='%s' vram_total=%lld",
             p.c_str(), pretty.c_str(), vramTotal);

        const std::vector<std::string> hwDirs = FindHwmonDirsForCard(card.drmPath);
        if (hwDirs.empty())
        {
            LOGI("AMD diag: no hwmon dirs for this card");
        }
        for (const std::string& h : hwDirs)
        {
            std::string hwName;
            ReadSysfsStr(h + "/name", hwName);
            LOGI("AMD diag: hwmon dir=%s name='%s'", h.c_str(), hwName.c_str());
            std::string files;
            for (const std::string& e : ListSubdirs(h))
                files += " " + e;
            if (!files.empty())
                LOGI("AMD diag: hwmon files:%s", files.c_str());
        }

        // Final list of what the code tries to read.
        bool busy = HasFile(card.drmPath + "/gpu_busy_percent");
        bool hasHwmonFile[8] = {};
        const char* hwFiles[8] = {"temp1_input", "fan1_input", "pwm1",
                                  "power1_average", "power1_input", "power1_cap",
                                  "freq1_input", "freq2_input"};
        for (const std::string& h : hwDirs)
        {
            for (int i = 0; i < 8; ++i)
            {
                if (HasFile(h + "/" + hwFiles[i]))
                    hasHwmonFile[i] = true;
            }
        }
        LOGI("AMD diag: sensors busy=%d vram_total=%d vram_used=%d temp1=%d "
             "fan1=%d pwm1=%d pwr_avg=%d pwr_in=%d pwr_cap=%d freq1=%d freq2=%d",
             busy ? 1 : 0,
             HasFile(card.drmPath + "/mem_info_vram_total") ? 1 : 0,
             HasFile(card.drmPath + "/mem_info_vram_used") ? 1 : 0,
             hasHwmonFile[0] ? 1 : 0, hasHwmonFile[1] ? 1 : 0,
             hasHwmonFile[2] ? 1 : 0, hasHwmonFile[3] ? 1 : 0,
             hasHwmonFile[4] ? 1 : 0, hasHwmonFile[5] ? 1 : 0,
             hasHwmonFile[6] ? 1 : 0, hasHwmonFile[7] ? 1 : 0);

        // The radeon driver (first-generation GCN, TeraScale) exports neither
        // load, nor video memory, nor power - the reason for N/A is explained
        // once at startup so that the logs are self-sufficient.
        if (card.driver == "radeon" && !busy &&
            !HasFile(card.drmPath + "/mem_info_vram_total") &&
            !hasHwmonFile[3] && !hasHwmonFile[4])
        {
            LOGI("AMD: driver 'radeon' exposes only temperature, fan and clocks. "
                 "Load, VRAM and power stay N/A. For GCN/SI cards they become "
                 "available with the amdgpu driver (kernel params: "
                 "radeon.si_support=0 amdgpu.si_support=1).");
        }
    }

    // One-time diagnostic print of the first values.
    std::vector<GpuMetrics> probe;
    CollectSysfs(probe);
    for (const GpuMetrics& g : probe)
    {
        LOGI("AMD: GPU '%s': load=%.0f%%, temp=%.0fC, power=%.1f/%.1f W, "
             "vram=%llu/%llu MB, fan=%.0f%%/%d rpm, clocks=%.0f/%.0f MHz",
             g.name.c_str(), g.loadPercent, g.temperature, g.powerWatts,
             g.powerLimitWatts,
             static_cast<unsigned long long>(g.vramUsedBytes / (1024ULL * 1024ULL)),
             static_cast<unsigned long long>(g.vramTotalBytes / (1024ULL * 1024ULL)),
             g.fanPercent, g.fanRpm, g.gpuFreqMhz, g.vramFreqMhz);
    }
    return true;
}

void AmdMonitor::CollectSysfs(std::vector<GpuMetrics>& out)
{
    out.clear();

    for (const AmdCard& card : ScanAmdCards())
    {
        GpuMetrics gm{};
        // Explicit N/A: if the load sensor is missing (radeon, old amdgpu),
        // the GUI hides the graph instead of an honest-looking but lying "0%".
        gm.loadPercent = -1.0;

        // Model name: product_name/pretty_name is provided by amdgpu with powerplay.
        // radeon and old amdgpu do not export them - the model is taken from the
        // system pci.ids database by PCI device ID, a standard distribution file.
        std::string name;
        if (!ReadSysfsStr(card.drmPath + "/product_name", name) &&
            !ReadSysfsStr(card.drmPath + "/pretty_name", name))
        {
            // The database is parsed once per PCI ID - see m_pciNameCache.
            std::map<std::string, std::string>::iterator it = m_pciNameCache.find(card.device);
            if (it == m_pciNameCache.end())
            {
                const std::string resolved = PciIdsName("0x1002", card.device);
                it = m_pciNameCache.insert(std::make_pair(card.device, resolved)).first;
                if (resolved.empty() && !card.device.empty())
                    LOGI("AMD: pci.ids has no entry for device %s.", card.device.c_str());
            }
            name = it->second;
        }
        if (name.empty())
            name = "AMD GPU";
        gm.name = name;

        // GPU load, %. amdgpu exposes gpu_busy_percent in the device's sysfs;
        // radeon has it only in debugfs (accessible to root only; on a normal run
        // the read silently fails and the load stays N/A).
        long long busy = 0;
        bool busyOk = ReadSysfsLL(card.drmPath + "/gpu_busy_percent", busy);
        if (!busyOk && card.cardIndex >= 0)
        {
            busyOk = ReadSysfsLL("/sys/kernel/debug/dri/" + std::to_string(card.cardIndex) +
                                 "/gpu_busy_percent", busy);
        }
        if (busyOk)
        {
            if (busy < 0) busy = 0;
            if (busy > 100) busy = 100;
            gm.loadPercent = static_cast<double>(busy);
        }

        // VRAM: the files report bytes.
        long long vramTotal = 0;
        long long vramUsed  = 0;
        const bool haveTotal = ReadSysfsLL(card.drmPath + "/mem_info_vram_total", vramTotal);
        const bool haveUsed  = ReadSysfsLL(card.drmPath + "/mem_info_vram_used", vramUsed);
        if (haveTotal && vramTotal > 0)
        {
            gm.vramTotalBytes = static_cast<uint64_t>(vramTotal);
            if (haveUsed && vramUsed >= 0 &&
                static_cast<uint64_t>(vramUsed) <= gm.vramTotalBytes)
            {
                gm.vramUsedBytes = static_cast<uint64_t>(vramUsed);
                gm.vramLoadPercent = static_cast<double>(gm.vramUsedBytes) /
                                     static_cast<double>(gm.vramTotalBytes) * 100.0;
            }
        }

        // hwmon sensors of this same card.
        const std::vector<std::string> hwmonDirs = FindHwmonDirsForCard(card.drmPath);
        if (!hwmonDirs.empty())
        {
            // Temperature, millidegrees: temp1 - edge, temp2 - junction.
            long long tempM = 0;
            if (ReadHwmonLL(hwmonDirs, "temp1_input", tempM) ||
                ReadHwmonLL(hwmonDirs, "temp2_input", tempM))
            {
                double c = static_cast<double>(tempM) / 1000.0;
                if (c > 0.0 && c < 250.0)
                    gm.temperature = c;
            }

            // Fan speed in RPM.
            long long rpm = 0;
            if (ReadHwmonLL(hwmonDirs, "fan1_input", rpm) && rpm > 0 && rpm < 30000)
                gm.fanRpm = static_cast<int>(rpm);

            // Fan PWM as a percentage (pwm1 0..255).
            long long pwm = 0;
            long long pwmMax = 0;
            if (ReadHwmonLL(hwmonDirs, "pwm1", pwm) &&
                ReadHwmonLL(hwmonDirs, "pwm1_max", pwmMax) &&
                pwmMax > 0 && pwm >= 0 && pwm <= pwmMax)
            {
                gm.fanPercent = static_cast<double>(pwm) * 100.0 /
                                static_cast<double>(pwmMax);
            }

            // Power: average first, otherwise instant; microWatts.
            long long powerUW = 0;
            if (!ReadHwmonLL(hwmonDirs, "power1_average", powerUW))
                ReadHwmonLL(hwmonDirs, "power1_input", powerUW);
            if (powerUW > 0 && powerUW < 1200LL * 1000000LL)
                gm.powerWatts = static_cast<double>(powerUW) / 1000000.0;

            // Power limit.
            long long capUW = 0;
            if (ReadHwmonLL(hwmonDirs, "power1_cap", capUW) &&
                capUW > 0 && capUW < 1200LL * 1000000LL)
            {
                gm.powerLimitWatts = static_cast<double>(capUW) / 1000000.0;
            }

            // Clocks, Hz: freq1 - graphics processor, freq2 - memory.
            long long gfxHz = 0;
            if (ReadHwmonLL(hwmonDirs, "freq1_input", gfxHz) && gfxHz > 0)
                gm.gpuFreqMhz = static_cast<double>(gfxHz) / 1000000.0;
            long long memHz = 0;
            if (ReadHwmonLL(hwmonDirs, "freq2_input", memHz) && memHz > 0)
                gm.vramFreqMhz = static_cast<double>(memHz) / 1000000.0;
        }

        // Fallback source of the core clock: devfreq (values in KHz).
        if (gm.gpuFreqMhz <= 0.0 && !card.bdf.empty())
        {
            long long clkKHz = 0;
            if (ReadSysfsLL("/sys/class/devfreq/" + card.bdf + "/cur_freq", clkKHz) &&
                clkKHz > 0)
            {
                gm.gpuFreqMhz = static_cast<double>(clkKHz) / 1000.0;
            }
        }

        // Value logging happens only at initialization (LoadSysfs) so as not to
        // pollute the log with line-by-line output every second.
        out.push_back(gm);
    }
}

#endif // !PLATFORM_WINDOWS
