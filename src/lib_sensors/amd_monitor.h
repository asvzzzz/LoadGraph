#ifndef AMD_MONITOR_H
#define AMD_MONITOR_H

#include "gpu_monitor.h"
#include "platform.h"

#ifdef PLATFORM_WINDOWS
#include <windows.h>
struct IDXGIFactory1;
#endif

#include <string>
#include <vector>
#include <map>
#include <cstdint>

/// AMD GPU monitoring.
/// Windows: ADL2 API (atiadlxx.dll is loaded dynamically), VRAM - via DXGI.
/// Linux:   amdgpu driver sysfs/hwmon (no external processes).
class AmdMonitor
{
public:
    AmdMonitor();
    ~AmdMonitor();

    /// Collect current metrics of all AMD GPUs.
    /// Empty vector if the library/driver is unavailable or no cards exist.
    std::vector<GpuMetrics> GetMetrics();

    /// True if ADL/amdgpu is initialized and at least one card was found.
    bool IsAvailable() const;

#ifndef PLATFORM_WINDOWS
    /// Generic helper (not only for AMD): PCI device model name
    /// by vendor:device ("0x1002", "0x679a") from the system pci.ids database.
    /// Used by GpuMonitor to show NVIDIA cards without the proprietary
    /// driver when NVML is unavailable. Empty string if there is no entry.
    static std::string ResolvePciName(const std::string& vendorId,
                                      const std::string& deviceId);
#endif

private:
#ifdef PLATFORM_WINDOWS
    // ADL2 signatures (adl_sdk.h / adapter.h / overdrive5.h / overdrive6.h).
    // ADL_CONTEXT_HANDLE is a void*.
    using FnAdlCreate      = int (*)(void* callback, int iEnumConnectedAdapters, void** ppContext);
    using FnAdlDestroy     = int (*)(void* pContext);
    using FnNumAdaptersGet = int (*)(void* pContext, int* lpNumAdapters);
    using FnAdapterInfoGet = int (*)(void* pContext, void* lpInfo, int iInputSize);
    using FnActiveGet      = int (*)(void* pContext, int iAdapterIndex, int* lpStatus);
    using FnOD6StatusGet   = int (*)(void* pContext, int iAdapterIndex, void* lpCurrentStatus);
    using FnOD5ActivityGet = int (*)(void* pContext, int iAdapterIndex, void* lpActivity);
    using FnOD6TempGet     = int (*)(void* pContext, int iAdapterIndex, int* lpTemperature);
    using FnOD5TempGet     = int (*)(void* pContext, int iAdapterIndex, int iThermalControllerIndex, void* lpTemperature);
    using FnOD6FanGet      = int (*)(void* pContext, int iAdapterIndex, void* lpFanSpeedInfo);
    using FnOD5FanInfoGet  = int (*)(void* pContext, int iAdapterIndex, int iThermalControllerIndex, void* lpFanSpeedInfo);
    using FnOD5FanGet      = int (*)(void* pContext, int iAdapterIndex, int iThermalControllerIndex, void* lpFanSpeedValue);
    using FnOD6PowerGet    = int (*)(void* pContext, int iAdapterIndex, int iPowerType, int* lpCurrentValue);
    using FnMemInfo2Get    = int (*)(void* pContext, int iAdapterIndex, void* lpMemoryInfo2);

    /// Hardware AMD adapter enumerated by DXGI (no ADL).
    struct DxgiAmdInfo
    {
        std::string name;                 // Description from DXGI_ADAPTER_DESC
        uint64_t    dedicatedBytes = 0;   // dedicated video memory size
        uint64_t    usedBytes      = 0;   // usage, valid when usedValid
        bool        usedValid      = false;
    };

    bool LoadAdl();
    void FreeAdl();

    /// ADL initialization: load atiadlxx.dll, resolve ADL2 entry points and
    /// create the context. false means ADL metrics are unavailable.
    bool InitAdl();

    /// Loads dxgi.dll and creates IDXGIFactory1. The failure is not fatal:
    /// without DXGI only VRAM is unavailable.
    void OpenDxgi();

    /// Metrics collection: ADL first; if it returns nothing - the DXGI fallback.
    void CollectAdl(std::vector<GpuMetrics>& out);

    /// ADL-only metrics. diag = print the one-time ADL diagnostics.
    void CollectViaAdl(std::vector<GpuMetrics>& out, bool diag);

    /// Maximum tachometer reading of one adapter, from
    /// ADL2_Overdrive5_FanSpeedInfo_Get (iMaxRPM). -1 when the card reports no
    /// such number. The result is cached per adapter index: it is a static
    /// property of the card, and walking 8 thermal controllers every second
    /// would be needless driver traffic.
    int QueryFanMaxRpm(int adapterIndex);

    /// Fallback source: adapters visible to DXGI (name and VRAM), when ADL does
    /// not enumerate the card. diag = print the message only once.
    void CollectDxgiOnly(std::vector<GpuMetrics>& out, bool diag);

    /// List of hardware DXGI AMD adapters (software WARP dropped).
    bool EnumerateDxgiAmd(std::vector<DxgiAmdInfo>& out);

    /// VRAM via DXGI: ADL never reports video memory usage, so used
    /// comes from IDXGIAdapter3::QueryVideoMemoryInfo. Adapter matching
    /// goes by ordinal number among AMD adapters (when their counts
    /// match), otherwise - by the dedicated memory size.
    /// Returns false if DXGI is unavailable or the adapter is not found.
    bool QueryVramDXGI(unsigned amdOrdinal, uint64_t adlTotalBytes,
                       uint64_t& totalBytes, uint64_t& usedBytes);

    HMODULE          m_hAdl        = nullptr;
    HMODULE          m_hDxgi       = nullptr;
    void*            m_adlContext  = nullptr;   // ADL_CONTEXT_HANDLE
    IDXGIFactory1*   m_dxgiFactory = nullptr;

    FnAdlCreate      m_fnCreate       = nullptr;
    FnAdlDestroy     m_fnDestroy      = nullptr;
    FnNumAdaptersGet m_fnNumAdapters  = nullptr;
    FnAdapterInfoGet m_fnAdapterInfo  = nullptr;
    FnActiveGet      m_fnActiveGet    = nullptr;
    FnOD6StatusGet   m_fnOD6Status    = nullptr;
    FnOD5ActivityGet m_fnOD5Activity  = nullptr;
    FnOD6TempGet     m_fnOD6Temp      = nullptr;
    FnOD5TempGet     m_fnOD5Temp      = nullptr;
    FnOD6FanGet      m_fnOD6Fan       = nullptr;
    FnOD5FanInfoGet  m_fnOD5FanInfo   = nullptr;
    FnOD5FanGet      m_fnOD5Fan       = nullptr;
    FnOD6PowerGet    m_fnOD6Power     = nullptr;
    FnMemInfo2Get    m_fnMemInfo2     = nullptr;

    /// Fan maximum (RPM) per ADL adapter index, -1 = no such number. Written
    /// and read only by the poll thread (GpuMonitor::Collect), no mutex needed.
    std::map<int, int> m_fanMaxRpmCache;
#else
    bool LoadSysfs();
    void CollectSysfs(std::vector<GpuMetrics>& out);

    /// Cache of pci.ids model names keyed by PCI device ID. Parsing the database
    /// (~1 MB of text) is done once per ID so that the once-per-second poll does
    /// not read the file. Only the poll thread (GpuMonitor::Collect) touches it.
    std::map<std::string, std::string> m_pciNameCache;
#endif

    bool m_available = false;
#ifdef PLATFORM_WINDOWS
    bool m_enumLogged = false;   // one-time ADL enumeration diagnostics flag
#endif
};

#endif // AMD_MONITOR_H
