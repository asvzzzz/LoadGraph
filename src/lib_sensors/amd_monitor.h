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

/// Мониторинт AMD GPU.
/// Windows: ADL2 API (atiadlxx.dll грузится динамически), VRAM - через DXGI.
/// Linux:   sysfs/hwmon драйвера amdgpu (без внешних процессов).
class AmdMonitor
{
public:
    AmdMonitor();
    ~AmdMonitor();

    /// Собрать актуальные метрики всех AMD GPU.
    /// Пустой вектор, если библиотека/драйвер недоступны или карт нет.
    std::vector<GpuMetrics> GetMetrics();

    /// True, если ADL/amdgpu инициализирован и найдена хотя бы одна карта.
    bool IsAvailable() const;

#ifndef PLATFORM_WINDOWS
    /// Универсальный помощник (не только для AMD): имя модели PCI-устройства
    /// по vendor:device ("0x1002", "0x679a") из системной базы pci.ids.
    /// Используется GpuMonitor для показа NVIDIA-карт без проприетарного
    /// драйвера, когда NVML недоступен. Пустая строка, если записи нет.
    static std::string ResolvePciName(const std::string& vendorId,
                                      const std::string& deviceId);
#endif

private:
#ifdef PLATFORM_WINDOWS
    // ADL2 сигнатуры (adl_sdk.h / adapter.h / overdrive5.h / overdrive6.h).
    // ADL_CONTEXT_HANDLE - это void*.
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

    bool LoadAdl();
    void FreeAdl();
    void CollectAdl(std::vector<GpuMetrics>& out);

    /// VRAM по DXGI: ADL не отдает использование видеопамяти, поэтому used
    /// берется из IDXGIAdapter3::QueryVideoMemoryInfo. Сопоставление адаптеров
    /// идет по порядковому номеру среди AMD-адаптеров (если их количество
    /// совпадает), иначе - по объему выделенной памяти.
    /// Возвращает false, если DXGI недоступен или адаптер не найден.
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
#else
    bool LoadSysfs();
    void CollectSysfs(std::vector<GpuMetrics>& out);

    /// Кэш имени модели из pci.ids по PCI device ID. Разбор базы (~1 МБ
    /// текста) делается один раз на ID, чтобы опрос раз в секунду не читал
    /// файл. Кэш трогает только поток опроса (GpuMonitor::Collect), гонок нет.
    std::map<std::string, std::string> m_pciNameCache;
#endif

    bool m_available = false;
#ifdef PLATFORM_WINDOWS
    bool m_enumLogged = false;   // флаг одноразовой диагностики перечисления ADL
#endif
};

#endif // AMD_MONITOR_H
