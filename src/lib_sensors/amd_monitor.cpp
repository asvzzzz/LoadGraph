#include "amd_monitor.h"
#include "logger.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

// ============================================================================
// Общие константы
// ============================================================================

// PCI Vendor ID компании AMD.
static const int AMD_PCI_VENDOR_ID = 0x1002;

// ============================================================================
// Каркас класса (общий для всех платформ)
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

// GUID'ы интерфейсов DXGI должны быть определены в этом TU
// (без необходимости линковать dxguid.lib).
#include <initguid.h>
#include <dxgi.h>
#include <dxgi1_6.h>

// Коды ADL (adl_defines.h).
#define ADL_OK                        0
#define ADL_MAX_PATH                  256
#define ADL_MAX_ADAPTERS              250

// Типы скорости вращения вентилятора OD6 (битовая маска iSpeedType).
#define ADL_OD6_FANSPEED_TYPE_PERCENT 0x00000001
#define ADL_OD6_FANSPEED_TYPE_RPM     0x00000002

// Маска возможностей термоконтроллера OD5 (поле iFlags у ADLFanSpeedInfo).
#define ADL_DL_FANCTRL_SUPPORTS_PERCENT_READ  1
#define ADL_DL_FANCTRL_SUPPORTS_RPM_READ      4

// Единица запроса для ADL2_Overdrive5_FanSpeed_Get (поле iSpeedType).
#define ADL_DL_FANCTRL_SPEED_TYPE_PERCENT     1
#define ADL_DL_FANCTRL_SPEED_TYPE_RPM         2

// Максимум термоконтроллеров, которые перебираем в OD5.
static const int ADL_OD5_MAX_THERMAL_CONTROLLERS = 8;

// --- Структуры ADL. Layout в точности как в adl_structures.h SDK 18.1 ---

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
    int iTemperature;   // миллиградусы Цельсия
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

// ADLOD6CurrentStatus: часы в единицах 10 КГц (документировано в SDK).
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

// ADLOD6FanSpeedInfo: iSpeedType - битовая маска валидности полей.
struct ADLOD6FanSpeedInfo
{
    int iSpeedType;
    int iFanSpeedPercent;
    int iFanSpeedRPM;
    int iExtValue;
    int iExtMask;
};

// ADLMemoryInfo2 (adl_structures.h, SDK 18.1): размеры памяти в байтах.
struct ADLMemoryInfo2
{
    long long iMemorySize;         // полный объем видеопамяти
    char      strMemoryType[ADL_MAX_PATH];
    long long iMemoryBandwidth;    // Мбайт/с на максимальном P-state
    long long iHyperMemorySize;
    long long iInvisibleMemorySize;
    long long iVisibleMemorySize;
};

/// Колбэк аллокации ADL (ADL_MAIN_MALLOC_CALLBACK).
/// ADL вызывает его для своих внутренних буферов.
void* __stdcall AdlMallocCallback(int iSize)
{
    if (iSize <= 0)
        return nullptr;
    return malloc(static_cast<size_t>(iSize));
}

/// Привести температуру ADL к градусам: и OD5 (ADLTemperature.iTemperature),
/// и OD6 (ADL2_Overdrive6_Temperature_Get) отдают миллиградусы Цельсия.
double NormalizeAdlTemperature(int raw)
{
    if (raw <= 0)
        return -1.0;
    double celsius = static_cast<double>(raw) / 1000.0;
    if (celsius < 1.0 && raw <= 250)
        celsius = static_cast<double>(raw);   // резерв: драйвер вернул градусы
    if (celsius <= 0.0 || celsius > 250.0)
        return -1.0;
    return celsius;
}

} // namespace

// ---------------------------------------------------------------------------
// Инициализация ADL/DXGI
// ---------------------------------------------------------------------------

bool AmdMonitor::LoadAdl()
{
    // atiadlxx.dll - 64-битная библиотека современных драйверов Adrenalin,
    // atiadlxy.dll - устаревшее 32-битное имя.
    const char* dllNames[] = { "atiadlxx.dll", "atiadlxy.dll" };
    for (const char* name : dllNames)
    {
        m_hAdl = LoadLibraryA(name);
        if (m_hAdl)
            break;
    }
    if (!m_hAdl)
    {
        LOGI("AMD: ADL library not found, AMD GPU support disabled.");
        return false;
    }

    // Обязательные точки входа ADL2.
    m_fnCreate      = reinterpret_cast<FnAdlCreate>(
        GetProcAddress(m_hAdl, "ADL2_Main_Control_Create"));
    m_fnDestroy     = reinterpret_cast<FnAdlDestroy>(
        GetProcAddress(m_hAdl, "ADL2_Main_Control_Destroy"));
    m_fnNumAdapters = reinterpret_cast<FnNumAdaptersGet>(
        GetProcAddress(m_hAdl, "ADL2_Adapter_NumberOfAdapters_Get"));
    m_fnAdapterInfo = reinterpret_cast<FnAdapterInfoGet>(
        GetProcAddress(m_hAdl, "ADL2_Adapter_AdapterInfo_Get"));

    // Необязательные: без них часть метрик просто не будет собрана.
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
        FreeAdl();
        return false;
    }

    // iEnumConnectedAdapters = 0: перечисляем все адаптеры (фильтр по
    // iVendorID/iPresent делаем сами).
    int rc = m_fnCreate(reinterpret_cast<void*>(AdlMallocCallback), 0, &m_adlContext);
    if (rc != ADL_OK || !m_adlContext)
    {
        LOGI("AMD: ADL2_Main_Control_Create failed (code %d).", rc);
        FreeAdl();
        return false;
    }

    // DXGI - источник VRAM (объем и использование). Не обязателен: без него
    // VRAM останется N/A, но прочие метрики продолжат работать.
    m_hDxgi = LoadLibraryA("dxgi.dll");
    if (m_hDxgi)
    {
        using FnCreateDXGIFactory1 = HRESULT (*)(REFIID riid, void** ppFactory);
        FnCreateDXGIFactory1 createFactory = reinterpret_cast<FnCreateDXGIFactory1>(
            GetProcAddress(m_hDxgi, "CreateDXGIFactory1"));
        if (createFactory)
        {
            HRESULT hr = createFactory(IID_IDXGIFactory1,
                                       reinterpret_cast<void**>(&m_dxgiFactory));
            if (FAILED(hr) || !m_dxgiFactory)
            {
                LOGI("AMD: CreateDXGIFactory1 failed, VRAM metrics unavailable.");
                m_dxgiFactory = nullptr;
            }
        }
    }
    if (!m_hDxgi)
        LOGI("AMD: dxgi.dll not loaded, VRAM metrics unavailable.");

    // Проверка наличия хотя бы одной AMD карты.
    std::vector<GpuMetrics> probe;
    CollectAdl(probe);
    if (probe.empty())
    {
        LOGI("AMD: No active AMD GPU found via ADL.");
        FreeAdl();
        return false;
    }

    // Диагностика необязательных точек входа: отсутствующая функция означает,
    // что соответствующая метрика будет N/A на этом драйвере.
    if (!m_fnOD6Status)   LOGI("AMD: ADL2_Overdrive6_CurrentStatus_Get missing - load/clocks unavailable.");
    if (!m_fnOD5Activity) LOGI("AMD: ADL2_Overdrive5_CurrentActivity_Get missing - no legacy load source.");
    if (!m_fnOD6Temp)     LOGI("AMD: ADL2_Overdrive6_Temperature_Get missing.");
    if (!m_fnOD5Temp)     LOGI("AMD: ADL2_Overdrive5_Temperature_Get missing.");
    if (!m_fnOD6Fan)      LOGI("AMD: ADL2_Overdrive6_FanSpeed_Get missing.");
    if (!m_fnOD5FanInfo || !m_fnOD5Fan) LOGI("AMD: ADL2_Overdrive5_FanSpeed* missing - fan unavailable.");
    if (!m_fnOD6Power)    LOGI("AMD: ADL2_Overdrive6_CurrentPower_Get missing - power unavailable.");
    if (!m_fnMemInfo2)    LOGI("AMD: ADL2_Adapter_MemoryInfo2_Get missing - VRAM size from DXGI only.");

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

    LOGI("AMD: ADL initialized, found %zu AMD GPU(s).", probe.size());
    return true;
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
// Сбор метрик
// ---------------------------------------------------------------------------

void AmdMonitor::CollectAdl(std::vector<GpuMetrics>& out)
{
    out.clear();
    if (!m_adlContext || !m_fnNumAdapters || !m_fnAdapterInfo)
        return;

    int numAdapters = 0;
    if (m_fnNumAdapters(m_adlContext, &numAdapters) != ADL_OK)
        return;
    if (numAdapters <= 0 || numAdapters > ADL_MAX_ADAPTERS)
        return;

    if (!m_enumLogged)
        LOGI("AMD: ADL reports %d OS adapter(s).", numAdapters);

    std::vector<ADLAdapterInfo> adapters(static_cast<size_t>(numAdapters));
    for (size_t i = 0; i < adapters.size(); ++i)
        adapters[i].iSize = sizeof(ADLAdapterInfo);

    const int bufBytes = static_cast<int>(adapters.size() * sizeof(ADLAdapterInfo));
    if (m_fnAdapterInfo(m_adlContext, adapters.data(), bufBytes) != ADL_OK)
        return;

    // Один физический адаптер может занимать несколько записей с одинаковым
    // iAdapterIndex - оставляем первую с нужным вендором.
    struct AdapterRef
    {
        int         index;      // ADL adapter index
        unsigned    ordinal;    // порядковый номер среди AMD-адаптеров
        std::string name;
    };
    std::vector<AdapterRef> targets;

    for (const ADLAdapterInfo& a : adapters)
    {
        if (a.iVendorID != AMD_PCI_VENDOR_ID || !a.iPresent)
            continue;

        bool dup = false;
        for (const AdapterRef& t : targets)
        {
            if (t.index == a.iAdapterIndex)
            {
                dup = true;
                break;
            }
        }
        if (dup)
            continue;

        AdapterRef ref{};
        ref.index   = a.iAdapterIndex;
        ref.ordinal = static_cast<unsigned>(targets.size());
        if (a.strAdapterName[0] != '\0')
            ref.name = a.strAdapterName;
        else if (a.strDisplayName[0] != '\0')
            ref.name = a.strDisplayName;
        else
            ref.name = "AMD GPU";
        targets.push_back(ref);
    }

    // Одноразовая диагностика инициализации - дальше не засоряем лог.
    if (!m_enumLogged)
    {
        LOGI("AMD: %zu adapter(s) matched PCI vendor 0x%04X.",
             targets.size(), static_cast<unsigned>(AMD_PCI_VENDOR_ID));
        m_enumLogged = true;
    }

    for (const AdapterRef& t : targets)
    {
        if (m_fnActiveGet)
        {
            int active = 0;
            if (m_fnActiveGet(m_adlContext, t.index, &active) == ADL_OK && !active)
                continue;
        }

        GpuMetrics gm{};
        gm.name = t.name;

        // Загрузка и частоты: основной источник - ADLOD6CurrentStatus
        // (часы документированы в 10 КГц), запасной - OD5 activity.
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
                // Единицы частот ADLPMActivity в SDK не документированы,
                // поэтому берем только процент загрузки.
                if (activity.iActivityPercent >= 0 && activity.iActivityPercent <= 100)
                    gm.loadPercent = static_cast<double>(activity.iActivityPercent);
            }
        }

        // Температура: сначала OD6 (int, миллиградусы), затем перебор
        // термоконтроллеров OD5 (ADLTemperature.iTemperature - миллиградусы).
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

        // Вентилятор: OD6 (маска валидности percent/RPM), затем OD5
        // через возможности термоконтроллера.
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

        // Мощность: ADL2_Overdrive6_CurrentPower_Get, iPowerType = 0
        // ("ASIC Total Power"), значение - Ватты с 8 дробными битами.
        // Публичный лимит мощности в Ваттах ADL не отдает (PowerControl_Get
        // возвращает проценты регулировки), поэтому powerLimitWatts = N/A.
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

        // VRAM: total - из ADL (ADLMemoryInfo2.iMemorySize, байты), used - из
        // DXGI (IDXGIAdapter3::QueryVideoMemoryInfo), ADL использование не отдает.
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

        // Логирование значений - только при инициализации (LoadAdl), чтобы
        // не засорять лог построчным выводом каждую секунду.
        out.push_back(gm);
    }
}

// ---------------------------------------------------------------------------
// VRAM через DXGI
// ---------------------------------------------------------------------------

bool AmdMonitor::QueryVramDXGI(unsigned amdOrdinal, uint64_t adlTotalBytes,
                               uint64_t& totalBytes, uint64_t& usedBytes)
{
    totalBytes = 0;
    usedBytes  = 0;
    if (!m_dxgiFactory)
        return false;

    // Собираем все аппаратные AMD-адаптеры DXGI (софтверный WARP отбрасываем).
    struct DxgiAmd
    {
        uint64_t dedicated;
        uint64_t used;
        bool     usedValid;
    };
    std::vector<DxgiAmd> list;

    for (UINT i = 0;; ++i)
    {
        IDXGIAdapter1* adapter = nullptr;
        HRESULT hr = m_dxgiFactory->EnumAdapters1(i, &adapter);
        if (hr == DXGI_ERROR_NOT_FOUND)
            break;
        if (FAILED(hr) || !adapter)
            continue;

        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter->GetDesc1(&desc)) &&
            desc.VendorId == static_cast<UINT>(AMD_PCI_VENDOR_ID) &&
            (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0)
        {
            DxgiAmd item{};
            item.dedicated = static_cast<uint64_t>(desc.DedicatedVideoMemory);
            item.used      = 0;
            item.usedValid = false;

            // Used: IDXGIAdapter3::QueryVideoMemoryInfo (Windows 10 и новее).
            IDXGIAdapter3* adapter3 = nullptr;
            if (SUCCEEDED(adapter->QueryInterface(IID_IDXGIAdapter3,
                                                  reinterpret_cast<void**>(&adapter3))))
            {
                DXGI_QUERY_VIDEO_MEMORY_INFO vmi{};
                if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(
                        0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &vmi)))
                {
                    item.used      = vmi.CurrentUsage;
                    item.usedValid = true;
                }
                adapter3->Release();
            }
            list.push_back(item);
        }

        adapter->Release();
    }

    if (list.empty())
        return false;

    // Сопоставление с адаптером ADL. DXGI не предоставляет адрес PCI, поэтому:
    // 1) однозначное совпадение по объему выделенной памяти;
    // 2) иначе - порядковый номер среди AMD-адаптеров.
    const uint64_t SIZE_TOLERANCE = 64ULL * 1024ULL * 1024ULL;
    size_t pick = list.size();

    if (adlTotalBytes > 0)
    {
        size_t  best    = list.size();
        uint64_t bestDelta = SIZE_TOLERANCE + 1;
        bool     ambiguous = false;
        for (size_t j = 0; j < list.size(); ++j)
        {
            uint64_t delta = (list[j].dedicated > adlTotalBytes)
                           ? (list[j].dedicated - adlTotalBytes)
                           : (adlTotalBytes - list[j].dedicated);
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

    totalBytes = list[pick].dedicated;
    usedBytes  = list[pick].usedValid ? list[pick].used : 0;
    return true;
}

#endif // PLATFORM_WINDOWS

// ============================================================================
// Linux: sysfs/hwmon драйвера amdgpu
// Интерфейсы документированы в Documentation/gpu/amdgpu/thermal:
//   device: gpu_busy_percent, mem_info_vram_total/used (байты), product_name
//   hwmon (name=amdgpu): temp1_input (mC), power1_average/input (uW),
//         power1_cap (uW), fan1_input (RPM), pwm1/pwm1_max (0..255),
//         freq1_input (Гц, gfx), freq2_input (Гц, mem)
// Внешние процессы не запускаются - стоимость опроса близка к нулю.
// ============================================================================
#ifndef PLATFORM_WINDOWS

#include <dirent.h>
#include <unistd.h>
#include <climits>
#include <cerrno>

namespace {

/// Обрезка пробельных символов с обеих сторон.
std::string Trim(const std::string& s)
{
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos)
        return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

/// Прочитать текстовый sysfs-файл. False, если файла нет или он нечитаем.
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

/// Прочитать числовой sysfs-файл. False при отсутствии/ошибке/мусоре.
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

/// Имя цели симлинка sysfs (basename от readlink). Пусто, если не симлинк.
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

/// Канонический путь (разыменованный симлинк). Пусто при ошибке.
std::string RealPath(const std::string& path)
{
    char buf[PATH_MAX];
    if (!realpath(path.c_str(), buf))
        return "";
    return buf;
}

/// Список имён подкаталогов каталога base.
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
    std::string bdf;       // PCI address like 0000:01:00.0 (may be empty)
};

/// Найти все DRM-карты AMD (vendor 0x1002) в /sys/class/drm.
std::vector<AmdCard> ScanAmdCards()
{
    std::vector<AmdCard> cards;
    const std::string drmRoot = "/sys/class/drm";

    for (const std::string& entry : ListSubdirs(drmRoot))
    {
        // Интересуют только карточки вида cardN (ports cardN-OutputN пропускаем).
        if (entry.compare(0, 4, "card") != 0)
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
        card.bdf = LinkBaseName(devPath);
        cards.push_back(card);
    }
    return cards;
}

/// Найти каталоги hwmon, относящиеся к данной DRM-карте.
/// Основной вариант (документирован в Documentation/gpu/amdgpu/thermal):
/// /sys/class/drm/cardN/device/hwmon/hwmonX. У современных ядер amdgpu
/// регистрирует два hwmon-устройства: одно для датчиков (температура,
/// мощность, частоты), второе для вентилятора, поэтому возвращаем список.
/// Резервный вариант: обход /sys/class/hwmon с проверкой, что канонический
/// путь датчика лежит внутри каталога устройства этой карты.
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
        // Драйвер amdgpu регистрирует hwmon с именем "amdgpu"
        // (на старых ядрах - "gfx").
        if (name != "amdgpu" && name != "gfx")
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

/// Прочитать числовой файл hwmon, перебирая все каталоги датчиков карты.
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

} // namespace

bool AmdMonitor::LoadSysfs()
{
    std::vector<AmdCard> cards = ScanAmdCards();
    if (cards.empty())
    {
        LOGI("AMD: No AMD GPU found in /sys/class/drm (amdgpu driver missing?).");
        return false;
    }
    LOGI("AMD: Found %zu AMD card(s) via amdgpu sysfs.", cards.size());

    // Одноразовый диагностический вывод первых значений.
    std::vector<GpuMetrics> probe;
    CollectSysfs(probe);
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
    return true;
}

void AmdMonitor::CollectSysfs(std::vector<GpuMetrics>& out)
{
    out.clear();

    for (const AmdCard& card : ScanAmdCards())
    {
        GpuMetrics gm{};

        // Имя модели.
        std::string name;
        if (!ReadSysfsStr(card.drmPath + "/product_name", name) &&
            !ReadSysfsStr(card.drmPath + "/pretty_name", name))
        {
            name = "AMD GPU";
        }
        gm.name = name;

        // Загрузка GPU, %.
        long long busy = 0;
        if (ReadSysfsLL(card.drmPath + "/gpu_busy_percent", busy))
        {
            if (busy < 0) busy = 0;
            if (busy > 100) busy = 100;
            gm.loadPercent = static_cast<double>(busy);
        }

        // VRAM: файлы отдают байты.
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

        // Датчики hwmon этой же карты.
        const std::vector<std::string> hwmonDirs = FindHwmonDirsForCard(card.drmPath);
        if (!hwmonDirs.empty())
        {
            // Температура, миллиградусы: temp1 - edge, temp2 - junction.
            long long tempM = 0;
            if (ReadHwmonLL(hwmonDirs, "temp1_input", tempM) ||
                ReadHwmonLL(hwmonDirs, "temp2_input", tempM))
            {
                double c = static_cast<double>(tempM) / 1000.0;
                if (c > 0.0 && c < 250.0)
                    gm.temperature = c;
            }

            // Обороты вентилятора.
            long long rpm = 0;
            if (ReadHwmonLL(hwmonDirs, "fan1_input", rpm) && rpm > 0 && rpm < 30000)
                gm.fanRpm = static_cast<int>(rpm);

            // ШИМ вентилятора в проценты (pwm1 0..255).
            long long pwm = 0;
            long long pwmMax = 0;
            if (ReadHwmonLL(hwmonDirs, "pwm1", pwm) &&
                ReadHwmonLL(hwmonDirs, "pwm1_max", pwmMax) &&
                pwmMax > 0 && pwm >= 0 && pwm <= pwmMax)
            {
                gm.fanPercent = static_cast<double>(pwm) * 100.0 /
                                static_cast<double>(pwmMax);
            }

            // Мощность: средняя, иначе мгновенная; микроВатты.
            long long powerUW = 0;
            if (!ReadHwmonLL(hwmonDirs, "power1_average", powerUW))
                ReadHwmonLL(hwmonDirs, "power1_input", powerUW);
            if (powerUW > 0 && powerUW < 1200LL * 1000000LL)
                gm.powerWatts = static_cast<double>(powerUW) / 1000000.0;

            // Лимит мощности.
            long long capUW = 0;
            if (ReadHwmonLL(hwmonDirs, "power1_cap", capUW) &&
                capUW > 0 && capUW < 1200LL * 1000000LL)
            {
                gm.powerLimitWatts = static_cast<double>(capUW) / 1000000.0;
            }

            // Частоты, Гц: freq1 - графический процессор, freq2 - память.
            long long gfxHz = 0;
            if (ReadHwmonLL(hwmonDirs, "freq1_input", gfxHz) && gfxHz > 0)
                gm.gpuFreqMhz = static_cast<double>(gfxHz) / 1000000.0;
            long long memHz = 0;
            if (ReadHwmonLL(hwmonDirs, "freq2_input", memHz) && memHz > 0)
                gm.vramFreqMhz = static_cast<double>(memHz) / 1000000.0;
        }

        // Резервный источник частоты ядра: devfreq (значения в КГц).
        if (gm.gpuFreqMhz <= 0.0 && !card.bdf.empty())
        {
            long long clkKHz = 0;
            if (ReadSysfsLL("/sys/class/devfreq/" + card.bdf + "/cur_freq", clkKHz) &&
                clkKHz > 0)
            {
                gm.gpuFreqMhz = static_cast<double>(clkKHz) / 1000.0;
            }
        }

        // Логирование значений - только при инициализации (LoadSysfs), чтобы
        // не засорять лог построчным выводом каждую секунду.
        out.push_back(gm);
    }
}

#endif // !PLATFORM_WINDOWS
