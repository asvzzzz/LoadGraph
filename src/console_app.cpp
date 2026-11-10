// Console-only monitoring tool (htop-like text UI).
// Zero wxWidgets dependency - uses Windows API + NVML directly.
// Usage: loadgraph-console [-port N]
//   -port N : start HTTP server on given port, serve web dashboard

// Platform headers first — must precede all standard library headers.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifdef PLATFORM_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#undef WIN32_LEAN_AND_MEAN  // restore full API for WMI (wbemidl needs excluded headers)
#include <objbase.h>
#include <oleauto.h>
#include <wbemidl.h>
#pragma comment(lib, "wbemuuid.lib")
#endif
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <ctime>
#include <thread>
#include <atomic>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>
#include <mutex>   // g_cpuMutex — protects SampleCpu static state from two threads
#ifdef PLATFORM_WINDOWS
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
// WMI library linked via wmi_helper.cpp (#pragma comment in that TU).
#endif
#include <fstream> // std::ifstream — used on both platforms for /proc (Linux) and general IO

#ifdef PLATFORM_LINUX
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <dirent.h>
#endif

#include "platform.h"   // defines PLATFORM_WINDOWS / PLATFORM_LINUX before any guards below
#include "app_config.h"
#include "power_table.h" // CPU TDP lookup and power estimation

// NVML dynamic loading - function pointer types.
#ifdef PLATFORM_WINDOWS
typedef int (__cdecl *FnNvmlInit)(void);
typedef int (__cdecl *FnNvmlShutdown)(void);
typedef int (__cdecl *FnNvmlDeviceGetCount)(unsigned int*);
typedef int (__cdecl *FnNvmlDeviceGetHandleByIndex)(unsigned int, void**);
typedef int (__cdecl *FnNvmlDeviceGetName)(void*, char*, unsigned int);
typedef int (__cdecl *FnNvmlDeviceGetUtilizationRates)(void*, void*);
typedef int (__cdecl *FnNvmlDeviceGetMemoryInfo)(void*, void*);
typedef int (__cdecl *FnNvmlDeviceGetTemperature)(void*, unsigned int, unsigned int*);
typedef int (__cdecl *FnNvmlDeviceGetFanSpeed)(void*, unsigned int*);
typedef int (__cdecl *FnNvmlDeviceGetPowerUsage)(void*, unsigned int*);
#endif
#ifdef PLATFORM_LINUX
typedef int (*FnNvmlInit)(void);
typedef int (*FnNvmlShutdown)(void);
typedef int (*FnNvmlDeviceGetCount)(unsigned int*);
typedef int (*FnNvmlDeviceGetHandleByIndex)(unsigned int, void**);
typedef int (*FnNvmlDeviceGetName)(void*, char*, unsigned int);
typedef int (*FnNvmlDeviceGetUtilizationRates)(void*, void*);
typedef int (*FnNvmlDeviceGetMemoryInfo)(void*, void*);
typedef int (*FnNvmlDeviceGetTemperature)(void*, unsigned int, unsigned int*);
typedef int (*FnNvmlDeviceGetFanSpeed)(void*, unsigned int*);
typedef int (*FnNvmlDeviceGetPowerUsage)(void*, unsigned int*);
#endif

#ifdef PLATFORM_WINDOWS
static HMODULE              g_hNvml     = nullptr;
#else
// Linux: dlopen handle.
static void*                g_hNvml     = nullptr;
#endif

// NVML function pointers — available on both platforms.
static FnNvmlInit           g_fnInit    = nullptr;
static FnNvmlShutdown       g_fnShutdown= nullptr;
static FnNvmlDeviceGetCount          g_fnGetCount      = nullptr;
static FnNvmlDeviceGetHandleByIndex  g_fnGetHandle     = nullptr;
static FnNvmlDeviceGetName           g_fnGetName       = nullptr;
static FnNvmlDeviceGetUtilizationRates g_fnGetUsage    = nullptr;
static FnNvmlDeviceGetMemoryInfo     g_fnGetMemory     = nullptr;
static FnNvmlDeviceGetTemperature    g_fnGetTemp       = nullptr;
static FnNvmlDeviceGetFanSpeed       g_fnGetFan        = nullptr;
static FnNvmlDeviceGetPowerUsage     g_fnGetPower       = nullptr;

// ============================================================================
// Metrics structures
// ============================================================================
struct CpuMetrics
{
    double   loadPercent  = 0.0;
    double   temperature  = -1.0; // degrees C, -1 = N/A
    double   powerWatts   = -1.0;
    double   tdpWatts     = -1.0;
    uint64_t uptimeMs     = 0;
    std::string cpuName;
};

struct RamMetrics
{
    uint64_t totalBytes  = 0;
    uint64_t freeBytes   = 0;
    double   loadPercent = 0.0;
};

struct GpuMetrics
{
    std::string name;
    double      loadPercent     = 0.0;
    double      temperature     = -1.0;
    uint64_t    vramTotalBytes  = 0;
    uint64_t    vramUsedBytes   = 0;
    double      fanPercent      = -1.0;
    double      powerWatts      = -1.0;
};

// ============================================================================
// CPU monitoring
// Windows: GetSystemTimes (kernel/user/idle deltas).
// Linux:   read /proc/stat (jiffies, user/nice/system/idle/iowait/irq/softirq).
// ============================================================================
#ifdef PLATFORM_WINDOWS
static std::mutex g_cpuMutex;
static ULONGLONG g_prevIdle   = 0;
static ULONGLONG g_prevKernel = 0;
static ULONGLONG g_prevUser   = 0;
static bool      g_prevValid  = false;
#endif
#ifdef PLATFORM_LINUX
static std::mutex g_cpuMutex;
static long long g_prevTotal = 0;
static long long g_prevIdleJ = 0;
#endif

// Called from two threads: main render loop and HTTP server thread.
// Static state is protected by g_cpuMutex to prevent Data Race / UB.
static double SampleCpu()
{
#ifdef PLATFORM_WINDOWS
    FILETIME ftIdle, ftKernel, ftUser;
    if (!GetSystemTimes(&ftIdle, &ftKernel, &ftUser)) return 0.0;
    ULONGLONG idle   = ((ULONGLONG)ftIdle.dwHighDateTime << 32) | ftIdle.dwLowDateTime;
    ULONGLONG kernel = ((ULONGLONG)ftKernel.dwHighDateTime << 32) | ftKernel.dwLowDateTime;
    ULONGLONG user   = ((ULONGLONG)ftUser.dwHighDateTime   << 32) | ftUser.dwLowDateTime;

    std::lock_guard<std::mutex> lock(g_cpuMutex);
    if (g_prevValid)
    {
        ULONGLONG dk = kernel - g_prevKernel;
        ULONGLONG du = user  - g_prevUser;
        ULONGLONG di = idle  - g_prevIdle;
        ULONGLONG total = dk + du - di;
        if (total > 0)
            return static_cast<double>(total * 100.0 / (total + di));
    }
    g_prevIdle   = idle;
    g_prevKernel = kernel;
    g_prevUser   = user;
    g_prevValid  = true;
    return 0.0;
#else // PLATFORM_LINUX
    std::ifstream f("/proc/stat");
    if (!f) return 0.0;
    std::string label; long long v[10] = {};
    f >> label; // cpu
    for (int i = 0; i < 8 && f >> v[i]; ++i) {}
    if (!f) return 0.0;
    long long total   = 0;
    for (int i = 0; i < 8; ++i) total += v[i];
    long long idleJ   = v[3] + v[4]; // idle + iowait

    std::lock_guard<std::mutex> lock(g_cpuMutex);
    if (g_prevTotal > 0)
    {
        long long dt = total - g_prevTotal;
        long long di = idleJ - g_prevIdleJ;
        if (dt > 0)
            return static_cast<double>((dt - di) * 100.0 / dt);
    }
    g_prevTotal = total;
    g_prevIdleJ = idleJ;
    return 0.0;
#endif
}

// InitCpu is a no-op with this approach.
static bool InitCpu() { return true; }

#ifdef PLATFORM_WINDOWS
static void QueryCpuName(std::string& out)
{
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
        L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
        0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return;
    wchar_t wname[256] = {};
    DWORD size = sizeof(wname);
    if (RegQueryValueExW(hKey, L"ProcessorNameString", nullptr, nullptr,
                          reinterpret_cast<LPBYTE>(wname), &size) == ERROR_SUCCESS &&
        size > sizeof(wchar_t))
    {
        int bytes = WideCharToMultiByte(CP_UTF8, 0, wname, -1, nullptr, 0, nullptr, nullptr);
        if (bytes > 1)
        {
            out.resize(static_cast<size_t>(bytes - 1));
            WideCharToMultiByte(CP_UTF8, 0, wname, -1, &out[0], bytes, nullptr, nullptr);
        }
    }
    RegCloseKey(hKey);
}
#else // PLATFORM_LINUX
static void QueryCpuName(std::string& out)
{
    std::ifstream f("/proc/cpuinfo");
    std::string line;
    while (std::getline(f, line))
    {
        if (line.find("model name") == 0)
        {
            auto pos = line.find(":");
            if (pos != std::string::npos)
            {
                out = line.substr(pos + 1);
                if (!out.empty() && out[0] == ' ') out.erase(out.begin());
                return;
            }
        }
    }
}
#endif

// ============================================================================
// RAM monitoring
// Windows: GlobalMemoryStatusEx. Linux: /proc/meminfo.
// ============================================================================
#ifdef PLATFORM_WINDOWS
static void SampleRam(RamMetrics& out)
{
    MEMORYSTATUSEX st{};
    st.dwLength = sizeof(st);
    if (GlobalMemoryStatusEx(&st))
    {
        out.totalBytes = st.ullTotalPhys;
        out.freeBytes  = st.ullAvailPhys;
        out.loadPercent = (out.totalBytes > 0)
            ? ((out.totalBytes - out.freeBytes) * 100.0 / out.totalBytes) : 0.0;
    }
}
#else // PLATFORM_LINUX
static void SampleRam(RamMetrics& out)
{
    std::ifstream f("/proc/meminfo");
    if (!f) return;
    long long total = 0, freeMem = 0, buffers = 0, cached = 0;
    std::string key; long long val;
    while (f >> key >> val)
    {
        if (key == "MemTotal:")      total   = val * 1024;
        else if (key == "MemFree:")   freeMem = val * 1024;
        else if (key == "Buffers:")   buffers = val * 1024;
        else if (key == "Cached:")    cached  = val * 1024;
    }
    out.totalBytes = static_cast<uint64_t>(total);
    out.freeBytes  = static_cast<uint64_t>(freeMem + buffers + cached);
    out.loadPercent = (out.totalBytes > 0)
        ? ((out.totalBytes - out.freeBytes) * 100.0 / out.totalBytes) : 0.0;
}
#endif

// WMI helper declared in wmi_helper.cpp (separate TU to avoid header-order issues).
#ifdef PLATFORM_WINDOWS
bool QueryWmiValueRaw(const wchar_t* ns, const wchar_t* query,
                      const wchar_t* prop, double& outValue);
#endif

// ============================================================================
// CPU temperature — returns degrees C, or -1.0 if unavailable.
// Windows: WMI (MSAcpi_ThermalZoneTemperature in root\wmi).
// Linux:   first thermal zone under /sys/class/thermal with type containing
//           "coretemp" or "x86_pkg_temp".
// ============================================================================
static double SampleCpuTemp()
{
#ifdef PLATFORM_WINDOWS
    // Source 1: ACPI thermal zone (root\wmi) — milli-Kelvin, often admin-only.
    double raw = -1.0;
    if (QueryWmiValueRaw(L"root\\wmi",
                         L"SELECT CurrentTemperature FROM MSAcpi_ThermalZoneTemperature",
                         L"CurrentTemperature", raw) && raw > 0.0)
    {
        double temp = raw / 1000.0 - 273.15;
        if (temp > 0.0 && temp < 150.0) return temp;
    }
    // Source 2: Win32_TemperatureProbe (root\cimv2) — generic probe class.
    if (QueryWmiValueRaw(L"root\\cimv2",
                         L"SELECT CurrentReading FROM Win32_TemperatureProbe",
                         L"CurrentReading", raw) && raw > 0.0 && raw < 150.0)
        return raw;
    // Source 3: OpenHardwareMonitor / LibreHardwareMonitor WMI namespace (needs app running).
    if (QueryWmiValueRaw(L"root\\OpenHardwareMonitor",
                         L"SELECT Value FROM Sensor WHERE SensorType='Temperature' AND Name='CPU Package'",
                         L"Value", raw) && raw > 0.0 && raw < 150.0)
        return raw;
    // Source 4: any Temperature sensor in LHM (fallback if CPU Package not found).
    if (QueryWmiValueRaw(L"root\\OpenHardwareMonitor",
                         L"SELECT Value FROM Sensor WHERE SensorType='Temperature'",
                         L"Value", raw) && raw > 0.0 && raw < 150.0)
        return raw;
    return -1.0;
#else
    DIR* d = opendir("/sys/class/thermal");
    if (!d) return -1.0;
    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr)
    {
        if (ent->d_name[0] == '.') continue;
        char path[256];
        snprintf(path, sizeof(path), "/sys/class/thermal/%s/type", ent->d_name);
        FILE* f = fopen(path, "r");
        if (!f) continue;
        char zoneType[32] = {};
        fscanf(f, "%31s", zoneType);
        fclose(f);
        bool isCpu = (strstr(zoneType, "coretemp") != nullptr ||
                      strstr(zoneType, "x86_pkg_temp") != nullptr);
        if (!isCpu) continue;
        snprintf(path, sizeof(path), "/sys/class/thermal/%s/temp", ent->d_name);
        f = fopen(path, "r");
        if (!f) continue;
        int raw = 0;
        if (fscanf(f, "%d", &raw) == 1 && raw > 0)
        {
            closedir(d);
            return static_cast<double>(raw) / 1000.0;
        }
        fclose(f);
    }
    closedir(d);
    return -1.0;
#endif
}

// ============================================================================
// GPU monitoring via NVML
// ============================================================================
#ifdef PLATFORM_WINDOWS
static bool LoadNvml()
{
    g_hNvml = LoadLibraryA("nvml.dll");
    if (!g_hNvml) return false;
#define X(name, ptr) \
    ptr = reinterpret_cast<FnNvml##name>(GetProcAddress(g_hNvml, "nvml" #name)); \
    if (!ptr) { FreeLibrary(g_hNvml); g_hNvml = nullptr; return false; }
    X(Init,           g_fnInit);
    X(Shutdown,       g_fnShutdown);
    X(DeviceGetCount,          g_fnGetCount);
    X(DeviceGetHandleByIndex,  g_fnGetHandle);
    X(DeviceGetName,           g_fnGetName);
    X(DeviceGetUtilizationRates, g_fnGetUsage);
    X(DeviceGetMemoryInfo,     g_fnGetMemory);
    X(DeviceGetTemperature,    g_fnGetTemp);
    X(DeviceGetFanSpeed,       g_fnGetFan);
    X(DeviceGetPowerUsage,     g_fnGetPower);
#undef X
    return (g_fnInit() == 0);
}

static void FreeNvml()
{
    if (g_fnShutdown)
    {
        g_fnShutdown();
        g_fnShutdown = nullptr;
    }
    if (g_hNvml)
    {
        FreeLibrary(g_hNvml);
        g_hNvml = nullptr;
    }
    g_fnInit      = nullptr;
    g_fnGetCount  = nullptr;
    g_fnGetHandle = nullptr;
    g_fnGetName   = nullptr;
    g_fnGetUsage  = nullptr;
    g_fnGetMemory = nullptr;
    g_fnGetTemp   = nullptr;
    g_fnGetFan    = nullptr;
    g_fnGetPower  = nullptr;
}
#else // PLATFORM_LINUX
#include <dlfcn.h>
static bool LoadNvml()
{
    g_hNvml = dlopen("libnvidia-ml.so", RTLD_LAZY);
    if (!g_hNvml) return false;
#define X(name, ptr) \
    ptr = reinterpret_cast<FnNvml##name>(dlsym(g_hNvml, "nvml" #name)); \
    if (!ptr) { dlclose(g_hNvml); g_hNvml = nullptr; return false; }
    X(Init,           g_fnInit);
    X(Shutdown,       g_fnShutdown);
    X(DeviceGetCount,          g_fnGetCount);
    X(DeviceGetHandleByIndex,  g_fnGetHandle);
    X(DeviceGetName,           g_fnGetName);
    X(DeviceGetUtilizationRates, g_fnGetUsage);
    X(DeviceGetMemoryInfo,     g_fnGetMemory);
    X(DeviceGetTemperature,    g_fnGetTemp);
    X(DeviceGetFanSpeed,       g_fnGetFan);
    X(DeviceGetPowerUsage,     g_fnGetPower);
#undef X
    return (g_fnInit() == 0);
}

static void FreeNvml()
{
    if (g_fnShutdown)
    {
        g_fnShutdown();
        g_fnShutdown = nullptr;
    }
    if (g_hNvml)
    {
        dlclose(g_hNvml);
        g_hNvml = nullptr;
    }
    g_fnInit      = nullptr;
    g_fnGetCount  = nullptr;
    g_fnGetHandle = nullptr;
    g_fnGetName   = nullptr;
    g_fnGetUsage  = nullptr;
    g_fnGetMemory = nullptr;
    g_fnGetTemp   = nullptr;
    g_fnGetFan    = nullptr;
    g_fnGetPower  = nullptr;
}
#endif

static std::vector<GpuMetrics> SampleGpus()
{
    std::vector<GpuMetrics> out;
    if (!g_hNvml) return out;
    unsigned int count = 0;
    if (g_fnGetCount(&count) != 0 || count == 0) return out;
    for (unsigned int i = 0; i < count; ++i)
    {
        GpuMetrics m{};
        void* dev = nullptr;
        if (g_fnGetHandle(i, &dev) != 0) continue;
        char name[256] = {};
        g_fnGetName(dev, name, sizeof(name));
        m.name = name;

        struct { unsigned int gpu; unsigned int mem; } util{};
        if (g_fnGetUsage(dev, &util) == 0)
            m.loadPercent = static_cast<double>(util.gpu);

        unsigned int temp = 0;
        if (g_fnGetTemp(dev, 0, &temp) == 0)
            m.temperature = static_cast<double>(temp);

        // NVML memory info layout: total, free, used (in that exact order).
        struct { uint64_t total; uint64_t free_; uint64_t used; } mem{};
        if (g_fnGetMemory(dev, reinterpret_cast<void*>(&mem)) == 0)
        {
            m.vramTotalBytes = mem.total;
            m.vramUsedBytes  = mem.used;
        }

        unsigned int fan = 0;
        if (g_fnGetFan(dev, &fan) == 0)
            m.fanPercent = static_cast<double>(fan);

        unsigned int pw = 0;
        if (g_fnGetPower(dev, &pw) == 0)
            m.powerWatts = pw / 1000.0;

        out.push_back(m);
    }
    return out;
}

// ============================================================================
// Console colors — loaded from loadgraph.cfg at startup.
// Windows: SetConsoleTextAttribute WORD values.
// Linux:   ANSI escape sequences (\x1b[38;2;r;g;b m).
// ============================================================================
#ifdef PLATFORM_WINDOWS
static HANDLE  g_hOut = INVALID_HANDLE_VALUE;

struct ConsoleColors
{
    WORD title       = FOREGROUND_GREEN | FOREGROUND_INTENSITY; // NVGREEN default
    WORD label       = FOREGROUND_RED   | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY; // white bold
    WORD value       = FOREGROUND_RED   | FOREGROUND_GREEN | FOREGROUND_BLUE; // white
    WORD dim         = FOREGROUND_BLUE  | FOREGROUND_GREEN;           // gray/cyan dim
    WORD cpu_load    = FOREGROUND_GREEN | FOREGROUND_INTENSITY;
    WORD cpu_temp    = FOREGROUND_BLUE  | FOREGROUND_GREEN;            // cyan
    WORD cpu_power   = FOREGROUND_RED   | FOREGROUND_GREEN;            // yellow
    WORD ram         = 0x0F; // white on black (default)
    WORD gpu_load    = FOREGROUND_RED   | FOREGROUND_INTENSITY;        // red/magenta
    WORD gpu_temp    = 0; // set from cfg
    WORD gpu_vram    = 0;
    WORD gpu_fan     = 0;
    WORD gpu_power   = 0;
};

static ConsoleColors g_colors{};

static void SetColor(WORD attrs)
{
    if (g_hOut != INVALID_HANDLE_VALUE)
        SetConsoleTextAttribute(g_hOut, attrs);
}

// Parse a 6-char hex color string into a WORD console attribute.
// Tries to match the closest standard console color; falls back to intensity-on for bright.
static WORD ParseColorHex(const char* hex)
{
    unsigned int r = 0, g = 0, b = 0;
    if (sscanf(hex, "%2x%2x%2x", &r, &g, &b) != 3) return FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
    // Map to nearest console color. Console has only 8 base colors + intensity.
    bool bright = (r > 0x7F || g > 0x7F || b > 0x7F);
    WORD w = 0;
    if (r & 0x80) w |= FOREGROUND_RED;
    if (g & 0x80) w |= FOREGROUND_GREEN;
    if (b & 0x80) w |= FOREGROUND_BLUE;
    if (bright) w |= FOREGROUND_INTENSITY;
    return w ? w : (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
}

static WORD TempAttrs(double t)
{
    if (t > 80.0) return FOREGROUND_RED   | FOREGROUND_INTENSITY;
    if (t > 65.0) return FOREGROUND_RED   | FOREGROUND_GREEN | FOREGROUND_INTENSITY; // yellow
    return FOREGROUND_GREEN | FOREGROUND_INTENSITY;
}

static void ApplyColors(const AppConfig& cfg)
{
    g_colors.title      = ParseColorHex(cfg.gpu_load_color.c_str()); // use gpu_load as title (green-ish)
    g_colors.label      = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
    g_colors.value      = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
    g_colors.dim        = FOREGROUND_BLUE  | FOREGROUND_GREEN;
    g_colors.cpu_load   = ParseColorHex(cfg.cpu_load_color.c_str());
    g_colors.cpu_temp   = ParseColorHex(cfg.cpu_temp_color.c_str());
    g_colors.cpu_power  = ParseColorHex(cfg.cpu_power_color.c_str());
    g_colors.ram        = ParseColorHex(cfg.ram_color.c_str());
    g_colors.gpu_load   = ParseColorHex(cfg.gpu_load_color.c_str());
    g_colors.gpu_temp   = ParseColorHex(cfg.gpu_temp_color.c_str());
    g_colors.gpu_vram   = ParseColorHex(cfg.gpu_vram_color.c_str());
    g_colors.gpu_fan    = ParseColorHex(cfg.gpu_fan_color.c_str());
    g_colors.gpu_power  = ParseColorHex(cfg.gpu_power_color.c_str());
}

#else // PLATFORM_LINUX — ANSI escape sequences
struct ConsoleColors
{
    std::string title;   // e.g. "\x1b[38;2;118;185;0m" (green)
    std::string label;   // bold white
    std::string value;   // reset
    std::string dim;     // gray
    std::string cpu_load;
    std::string cpu_temp;
    std::string cpu_power;
    std::string ram;
    std::string gpu_load;
    std::string gpu_temp;
    std::string gpu_vram;
    std::string gpu_fan;
    std::string gpu_power;
    std::string reset    = "\x1b[0m";
    std::string bold     = "\x1b[1m";
};

static ConsoleColors g_colors{};

// Build an ANSI 24-bit color escape sequence from a hex string.
static std::string AnsiColor(const char* hex)
{
    unsigned int r = 0, g = 0, b = 0;
    if (sscanf(hex, "%2x%2x%2x", &r, &g, &b) != 3)
        return "\x1b[37m"; // fallback white
    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[38;2;%u;%u;%um", r, g, b);
    return std::string(buf);
}

static void ApplyColors(const AppConfig& cfg)
{
    g_colors.title      = AnsiColor(cfg.gpu_load_color.c_str()); // green-ish for title
    g_colors.label      = "\x1b[1m";
    g_colors.value      = "\x1b[0m";
    g_colors.dim        = "\x1b[90m";
    g_colors.cpu_load   = AnsiColor(cfg.cpu_load_color.c_str());
    g_colors.cpu_temp   = AnsiColor(cfg.cpu_temp_color.c_str());
    g_colors.cpu_power  = AnsiColor(cfg.cpu_power_color.c_str());
    g_colors.ram        = AnsiColor(cfg.ram_color.c_str());
    g_colors.gpu_load   = AnsiColor(cfg.gpu_load_color.c_str());
    g_colors.gpu_temp   = AnsiColor(cfg.gpu_temp_color.c_str());
    g_colors.gpu_vram   = AnsiColor(cfg.gpu_vram_color.c_str());
    g_colors.gpu_fan    = AnsiColor(cfg.gpu_fan_color.c_str());
    g_colors.gpu_power  = AnsiColor(cfg.gpu_power_color.c_str());
}

// Temperature color on Linux — returns ANSI string.
static const char* TempAnsi(double t)
{
    if (t > 80.0) return "\x1b[31m";   // red
    if (t > 65.0) return "\x1b[33m";   // yellow
    return "\x1b[32m";                 // green
}
#endif // PLATFORM_WINDOWS / PLATFORM_LINUX

// ============================================================================
// Helpers
// ============================================================================
static std::string ProgressBar(double pct, int width)
{
    pct = std::max(0.0, std::min(100.0, pct));
    int filled = static_cast<int>(pct / 100.0 * width + 0.5);
    filled = std::max(0, std::min(width, filled));
    std::string bar;
    bar.reserve(static_cast<size_t>(width) + 1);
    for (int i = 0; i < width; ++i)
        bar += (i < filled) ? '#' : '-';
    return bar;
}

static std::string FormatUptime(uint64_t ms)
{
    uint64_t s = ms / 1000;
    uint64_t d = s / 86400;  s %= 86400;
    uint64_t h = s / 3600;   s %= 3600;
    uint64_t m = s / 60;     s %= 60;
    char buf[64];
    if (d > 0) snprintf(buf, sizeof(buf), "%llu u %llu h %llu m",
                        (unsigned long long)d, (unsigned long long)h, (unsigned long long)m);
    else if (h > 0) snprintf(buf, sizeof(buf), "%llu h %llu m %llu s",
                             (unsigned long long)h, (unsigned long long)m, (unsigned long long)s);
    else snprintf(buf, sizeof(buf), "%llu m %llu s",
                  (unsigned long long)m, (unsigned long long)s);
    return std::string(buf);
}

static std::string BytesToStr(uint64_t bytes)
{
    char buf[32];
    if (bytes >= 1024ULL * 1024 * 1024 * 1024)
        snprintf(buf, sizeof(buf), "%.1f GB", bytes / (1024.0*1024*1024*1024));
    else if (bytes >= 1024ULL * 1024 * 1024)
        snprintf(buf, sizeof(buf), "%.1f MB", bytes / (1024.0*1024*1024));
    else
        snprintf(buf, sizeof(buf), "%.0f KB", bytes / 1024.0);
    return std::string(buf);
}



// ============================================================================
// ASCII waveform (scrolling history, like an oscilloscope).
// Uses Unicode block characters: ░▒▓█ for 4 levels of height.
// Each column represents one sample; `history` holds the last N values [0,100].
// ============================================================================
template <typename T>
class RingBuffer
{
    std::vector<T> buf_;
    size_t head_ = 0, count_ = 0;
public:
    explicit RingBuffer(size_t cap) : buf_(cap, T{}) {}
    void Push(const T& v) { buf_[head_] = v; head_ = (head_ + 1) % buf_.size(); if (count_ < buf_.size()) ++count_; }
    size_t Size() const { return count_; }
    T Get(size_t i) const // i=0 is oldest
    { if (count_ == 0) return T{}; size_t idx = (head_ + buf_.size() - count_ + i) % buf_.size(); return buf_[idx]; }
};

static RingBuffer<double> g_cpuHist(60), g_ramHist(60);

// Controls whether DrawWave uses Unicode block chars or ASCII fallback.
// Set in main() after confirming console CP is UTF-8; overridable via env var.
static bool g_useUtf8Blocks = true;

// Platform-agnostic color handle: WORD on Windows (SetConsoleTextAttribute),
// const char* on Linux (ANSI escape sequence).
#ifdef PLATFORM_WINDOWS
using ConsoleColor = WORD;
#else
using ConsoleColor = const char*;
#endif

// UTF-8 block-char table — CP is already set to CP_UTF8 via SetConsoleOutputCP.
// printf with these literals renders correctly in any real console.
// ASCII fallback (LOADGRAPH_ASCII_BARS=1) always works, even on raster fonts
// and when stdout is redirected to a pipe.
static const char* const kUtf8Blocks[] = {
    " ",      // level 0 — empty / dim
    "\xe2\x96\x91",   // U+2591 light shade
    "\xe2\x96\x92",   // U+2592 medium shade
    "\xe2\x96\x93",   // U+2593 dark shade
    "\xe2\x96\x88"    // U+2588 full block
};
static const char kAsciiBlocks[] = " .oO@";

// Render a waveform row: `label` on left, values from `hist`, width columns.
// ttyplot-style: Unicode block chars (U+2591-U+2588) with per-cell color.
// cellColor is the Windows WORD attribute / Linux ANSI sequence for filled cells;
// 0/default = g_colors.label.
static void DrawWave(const char* label, const RingBuffer<double>& hist, int cols, ConsoleColor cellColor = 0)
{
#ifdef PLATFORM_WINDOWS
    if (cellColor == 0) cellColor = g_colors.label;
    SetColor(cellColor);
    printf("%-12s", label);

    const int steps = (int)hist.Size();
    for (int c = 0; c < cols; ++c)
    {
        const int hi = steps - cols + c;
        double v = (hi >= 0) ? hist.Get(static_cast<size_t>(hi)) : 0.0;
        v = std::max(0.0, std::min(100.0, v));
        int lv = static_cast<int>(v / 100.0 * 4.0 + 0.5); // 0..4
        // lv=4 (U+2588 FULL BLOCK) is the "full" level at 100%.
        lv = std::max(0, std::min(4, lv));

        if (lv == 0)
        {
            SetColor(g_colors.dim);
            putchar(' ');   // empty cell: dim gray space, always visible
        }
        else
        {
            SetColor(cellColor);
            fputs(kUtf8Blocks[lv], stdout);
        }
    }
    SetColor(g_colors.value);
    putchar('\n');
#else // PLATFORM_LINUX — ANSI + UTF-8 via printf
    const char* colStr = (cellColor != nullptr) ? cellColor : g_colors.label.c_str();
    printf("%s%-12s%s", colStr, label, g_colors.value.c_str());

    static const char* nblocks[] = { " ", "\xe2\x96\x91", "\xe2\x96\x92", "\xe2\x96\x93", "\xe2\x96\x88" };
    const int steps = (int)hist.Size();
    for (int c = 0; c < cols; ++c)
    {
        const int hi = steps - cols + c;
        double v = (hi >= 0) ? hist.Get(static_cast<size_t>(hi)) : 0.0;
        v = std::max(0.0, std::min(100.0, v));
        int lv = static_cast<int>(v / 100.0 * 4.0 + 0.5);
        lv = std::max(0, std::min(4, lv));
        if (lv == 0) fputs("\x1b[90m ", stdout);
        else         fputs(nblocks[lv], stdout);
    }
    printf("%s\n", g_colors.reset.c_str());
#endif
}

// ============================================================================
// Render
// ============================================================================
static void Render(const CpuMetrics& cpu, const RamMetrics& ram,
                   const std::vector<GpuMetrics>& gpus)
{
#ifdef PLATFORM_WINDOWS
    // Move cursor to home and clear screen via WinAPI.
    if (g_hOut != INVALID_HANDLE_VALUE)
    {
        COORD zero = { 0, 0 };
        SetConsoleCursorPosition(g_hOut, zero);
        CONSOLE_SCREEN_BUFFER_INFO info;
        if (GetConsoleScreenBufferInfo(g_hOut, &info))
        {
            DWORD written = 0;
            const DWORD fill = static_cast<DWORD>(
                (info.dwSize.Y + 2) * info.dwSize.X);
            FillConsoleOutputCharacterA(g_hOut, ' ', fill, zero, &written);
            SetConsoleCursorPosition(g_hOut, zero);
        }
    }
#endif

#ifdef PLATFORM_WINDOWS
    // Header
    SetColor(g_colors.title | FOREGROUND_INTENSITY);
    printf(" LoadGraph Console");
    if (!cpu.cpuName.empty())
    {
        SetColor(FOREGROUND_BLUE | FOREGROUND_GREEN); // cyan for CPU name
        printf(" %s", cpu.cpuName.c_str());
    }
    else
    {
        SetColor(g_colors.dim);
        printf(" CPU (N/A)");
    }
    SetColor(g_colors.dim);
    printf(" | Uptime: ");
    SetColor(g_colors.value);
    printf("%s\n", FormatUptime(cpu.uptimeMs).c_str());

    // Push current samples into history rings.
    g_cpuHist.Push(cpu.loadPercent);
    g_ramHist.Push(ram.loadPercent);

    const int kW = 40; // waveform width in chars

    // CPU section
    SetColor(FOREGROUND_INTENSITY);
    printf("\n  CPU Load     ");
    SetColor(g_colors.cpu_load);
    printf("%5.1f%%\n", cpu.loadPercent);
    DrawWave("", g_cpuHist, kW, g_colors.cpu_load);

    if (cpu.powerWatts > 0.0)
    {
        SetColor(g_colors.value);
        printf("  CPU Power    %5.1f W / ", cpu.powerWatts);
        SetColor(g_colors.dim);
        printf("%5.0f W TDP\n", cpu.tdpWatts);
    }

    // RAM section
    SetColor(FOREGROUND_INTENSITY);
    printf("\n  RAM Usage    ");
    SetColor(g_colors.ram);
    printf("%5.1f%%\n", ram.loadPercent);
    DrawWave("", g_ramHist, kW, g_colors.ram);
    SetColor(g_colors.dim);
    printf("           %4.1f GB / %4.1f GB\n",
           static_cast<double>(ram.totalBytes - ram.freeBytes) / (1024.0*1024*1024),
           static_cast<double>(ram.totalBytes) / (1024.0*1024*1024));

    // GPU section
    if (!gpus.empty())
    {
        SetColor(g_colors.title | FOREGROUND_INTENSITY);
        printf("\n  GPU");
        for (size_t i = 0; i < gpus.size(); ++i)
        {
            const GpuMetrics& g = gpus[i];
            // Explicit color reset: otherwise the header inherits the color from
            // the previous printf call (Load/Temp/VRAM/Fan/Power).
            SetColor(g_colors.title | FOREGROUND_INTENSITY);
            printf("\n\n  --- GPU %zu: %s ---\n", i, g.name.c_str());
            if (g.loadPercent >= 0.0)
            {
                SetColor(FOREGROUND_INTENSITY);
                printf("    Load       ");
                SetColor(g_colors.gpu_load);
                printf("%5.1f%%\n", g.loadPercent);
            }
            if (g.temperature >= 0.0)
            {
                SetColor(FOREGROUND_INTENSITY);
                printf("    Temp       ");
                SetColor(TempAttrs(g.temperature));
                printf("%4.0f C\n", g.temperature);
            }
            if (g.vramTotalBytes > 0)
            {
                double pct = g.vramUsedBytes * 100.0 / g.vramTotalBytes;
                SetColor(FOREGROUND_INTENSITY);
                printf("    VRAM       ");
                SetColor(g_colors.gpu_vram);
                printf("%5.1f%%\n", pct);
                double usedGb = static_cast<double>(g.vramUsedBytes) / (1024.0*1024*1024);
                double totGb  = static_cast<double>(g.vramTotalBytes) / (1024.0*1024*1024);
                SetColor(g_colors.dim);
                printf("%4.1f GB / %4.1f GB\n", usedGb, totGb);
            }
            if (g.fanPercent >= 0.0)
            {
                SetColor(FOREGROUND_INTENSITY);
                printf("    Fan        ");
                SetColor(g_colors.gpu_fan);
                printf("%5.0f%%\n", g.fanPercent);
            }
            if (g.powerWatts > 0.0)
            {
                SetColor(FOREGROUND_INTENSITY);
                printf("    Power      ");
                SetColor(g_colors.gpu_power);
                printf("%5.1f W\n", g.powerWatts);
            }
        }
    }

    SetColor(g_colors.dim);
    printf("\n  [Q] quit");
    SetColor(g_colors.value);
#else // PLATFORM_LINUX — ANSI output
    printf("%s%s LoadGraph Console%s", g_colors.bold.c_str(), g_colors.title.c_str(), g_colors.reset.c_str());
    if (!cpu.cpuName.empty())
        printf(" %s%s%s", "\x1b[36m", cpu.cpuName.c_str(), g_colors.reset.c_str()); // cyan
    else
        printf(" %sCPU (N/A)%s", g_colors.dim.c_str(), g_colors.reset.c_str());
    printf(" %s| Uptime: %s%s\n", g_colors.dim.c_str(), g_colors.value.c_str(), FormatUptime(cpu.uptimeMs).c_str());

    // Push current samples into history rings.
    g_cpuHist.Push(cpu.loadPercent);
    g_ramHist.Push(ram.loadPercent);

    const int kW = 40; // waveform width in chars

    // CPU section
    printf("\n%s  CPU Load     %s%s%5.1f%%\n",
           g_colors.bold.c_str(), g_colors.cpu_load.c_str(), g_colors.reset.c_str(), cpu.loadPercent);
    DrawWave("", g_cpuHist, kW, g_colors.cpu_load);

    if (cpu.powerWatts > 0.0)
        printf("  CPU Power    %s%5.1f W / %s%5.0f W TDP%s\n",
               g_colors.value.c_str(), cpu.powerWatts,
               g_colors.dim.c_str(), cpu.tdpWatts, g_colors.reset.c_str());

    // RAM section
    printf("%s  RAM Usage    %s%5.1f%%\n",
           g_colors.bold.c_str(), g_colors.ram.c_str(), ram.loadPercent);
    DrawWave("", g_ramHist, kW, g_colors.ram);
    printf("%s           %4.1f GB / %4.1f GB%s\n",
           g_colors.dim.c_str(),
           static_cast<double>(ram.totalBytes - ram.freeBytes) / (1024.0*1024*1024),
           static_cast<double>(ram.totalBytes) / (1024.0*1024*1024),
           g_colors.reset.c_str());

    // GPU section
    if (!gpus.empty())
    {
        printf("%s%s\n  GPU%s", g_colors.bold.c_str(), g_colors.title.c_str(), g_colors.reset.c_str());
        for (size_t i = 0; i < gpus.size(); ++i)
        {
            const GpuMetrics& g = gpus[i];
            // Explicit color reset: otherwise the header inherits from a previous row.
            printf("%s", g_colors.title.c_str());
            printf("\n\n  --- GPU %zu: %s%s\n", i, g.name.c_str(), g_colors.reset.c_str());
            if (g.loadPercent >= 0.0)
                printf("%s    Load       %s%5.1f%%\n",
                       g_colors.bold.c_str(), g_colors.gpu_load.c_str(), g.loadPercent);
            if (g.temperature >= 0.0)
                printf("%s    Temp       %s%4.0f C%s\n",
                       g_colors.bold.c_str(), TempAnsi(g.temperature), g.temperature, g_colors.reset.c_str());
            if (g.vramTotalBytes > 0)
            {
                double pct = g.vramUsedBytes * 100.0 / g.vramTotalBytes;
                printf("%s    VRAM       %s%5.1f%%\n",
                       g_colors.bold.c_str(), g_colors.gpu_vram.c_str(), pct);
                double usedGb = static_cast<double>(g.vramUsedBytes) / (1024.0*1024*1024);
                double totGb  = static_cast<double>(g.vramTotalBytes) / (1024.0*1024*1024);
                printf("%s%4.1f GB / %4.1f GB%s\n",
                       g_colors.dim.c_str(), usedGb, totGb, g_colors.reset.c_str());
            }
            if (g.fanPercent >= 0.0)
                printf("%s    Fan        %s%5.0f%%\n",
                       g_colors.bold.c_str(), g_colors.gpu_fan.c_str(), g.fanPercent);
            if (g.powerWatts > 0.0)
                printf("%s    Power      %s%5.1f W%s\n",
                       g_colors.bold.c_str(), g_colors.gpu_power.c_str(), g.powerWatts, g_colors.reset.c_str());
        }
    }

    printf("\n%s  [Q] quit%s\n", g_colors.dim.c_str(), g_colors.reset.c_str());
#endif // PLATFORM_WINDOWS / PLATFORM_LINUX
    fflush(stdout);
}

// ============================================================================
// HTTP server (minimal, single-threaded)
// ============================================================================
#ifdef USE_HTTP_SERVER
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

// Reuse GUI web dashboard HTML — ensures identical appearance.
#include "dashboard_html.h"

// Escape a C string for safe embedding in a JSON literal.
static std::string JsonEscape(const char* s)
{
    std::string r;
    r.reserve(strlen(s) + 8);
    for (const unsigned char* p = (const unsigned char*)s; *p; ++p)
    {
        switch (*p)
        {
            case '"':   r += "\\\""; break;
            case '\\':  r += "\\\\"; break;
            case '\b':  r += "\\b";  break;
            case '\f':  r += "\\f";  break;
            case '\n':  r += "\\n";  break;
            case '\r':  r += "\\r";  break;
            case '\t':  r += "\\t";  break;
            default:
                if (*p < 0x20)
                { char t[8]; snprintf(t, sizeof(t), "\\u%04x", *p); r += t; }
                else r += (char)*p;
        }
    }
    return r;
}

static double SafeDouble(double v) { return std::isfinite(v) ? v : 0.0; }

static std::string BuildJson(const CpuMetrics& cpu, const RamMetrics& ram,
                              const std::vector<GpuMetrics>& gpus)
{
    std::string out;
    out.reserve(1024 + gpus.size() * 256);

    char buf[512];
    // Build prefix up to "cpu_name":" — keep it separate from the gpus array
    // so that cpu_name can be appended safely regardless of length.
    snprintf(buf, sizeof(buf),
        "{\"cpu_load\":%.1f,\"cpu_temp\":%.1f,\"cpu_power\":%.1f,"
         "\"tdp_watts\":%.1f,"
         "\"ram_load\":%.1f,\"ram_used_bytes\":%llu,\"ram_total_bytes\":%llu,"
         "\"uptime_ms\":%llu,\"cpu_name\":\"",
        SafeDouble(cpu.loadPercent),
        SafeDouble(cpu.temperature),
        SafeDouble(cpu.powerWatts),
        SafeDouble(cpu.tdpWatts),
        SafeDouble(ram.loadPercent),
        (unsigned long long)(ram.totalBytes > ram.freeBytes
                             ? ram.totalBytes - ram.freeBytes : 0),
        (unsigned long long)ram.totalBytes);
    out += buf;
    out += JsonEscape(cpu.cpuName.c_str());
    out += "\",\"gpus\":[";

    for (size_t i = 0; i < gpus.size(); ++i)
    {
        const GpuMetrics& g = gpus[i];
        if (i > 0) out += ',';
        out += "{\"name\":\"";
        out += JsonEscape(g.name.c_str());   // safe JSON escaping for GPU name
        snprintf(buf, sizeof(buf),
            "\",\"load_percent\":%.1f,\"temperature\":%.1f,"
             "\"vram_used_bytes\":%llu,\"vram_total_bytes\":%llu,"
             "\"fan_percent\":%.1f,\"power_watts\":%.1f}",
            SafeDouble(g.loadPercent), SafeDouble(g.temperature),
            (unsigned long long)g.vramUsedBytes,
            (unsigned long long)g.vramTotalBytes,
            SafeDouble(g.fanPercent), SafeDouble(g.powerWatts));
        out += buf;
    }
    out += "]}";
    return out;
}

// Cached CPU name — populated once at startup, shared between the render loop
// and the HTTP server thread (which has no other way to access it).
static std::string s_cachedCpuName;

static void HttpLoop(int port, const AppConfig& cfg)
{
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) { fprintf(stderr, "HTTP: WSAStartup failed\n"); return; }
    SOCKET srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (srv == INVALID_SOCKET) { fprintf(stderr, "HTTP: socket() failed err=%d\n", WSAGetLastError()); WSACleanup(); return; }
    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR,   (const char*)&opt, sizeof(opt));
#ifdef PLATFORM_WINDOWS
    // Prevent another process from stealing the port after a crash.
    setsockopt(srv, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&opt, sizeof(opt));
#endif
    sockaddr_in addr{}; addr.sin_family=AF_INET; addr.sin_addr.s_addr=INADDR_ANY; addr.sin_port=htons((unsigned short)port);
    if (bind(srv, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR)
    { fprintf(stderr, "HTTP: bind() failed port=%d err=%d\n", port, WSAGetLastError()); closesocket(srv); WSACleanup(); return; }
    if (listen(srv, 4) == SOCKET_ERROR) { fprintf(stderr, "HTTP: listen() failed err=%d\n", WSAGetLastError()); closesocket(srv); WSACleanup(); return; }
    printf("HTTP server listening on port %d\nOpen http://localhost:%d in your browser\n", port, port);
    fflush(stdout);

    DWORD rcvTimeout = 5000; // 5 s - prevents a slow client from blocking the accept loop.
    DWORD sndTimeout = 5000;
    while (true)
    {
        fd_set rf; FD_ZERO(&rf); FD_SET(srv, &rf);
        timeval tv{}; tv.tv_sec=1; tv.tv_usec=0;
        if (select(0, &rf, nullptr, nullptr, &tv) <= 0 || !FD_ISSET(srv, &rf)) continue;
        SOCKET client = accept(srv, nullptr, nullptr);
        if (client == INVALID_SOCKET) continue;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, (const char*)&rcvTimeout, sizeof(rcvTimeout));
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, (const char*)&sndTimeout, sizeof(sndTimeout));
        char req[4096] = {};
        int n = recv(client, req, sizeof(req)-1, 0);
        if (n <= 0) { closesocket(client); continue; }
        req[n] = '\0';

        // Parse method + path properly - strstr matches substrings anywhere.
        char method[8] = {}, path[512] = {};
        sscanf(req, "%7s %511s", method, path);
        bool isMetrics = (strcmp(method, "GET") == 0 && strcmp(path, "/api/metrics") == 0);
        bool isIndex   = (strcmp(method, "GET") == 0 && strcmp(path, "/") == 0);

        std::string body;
        if (isMetrics)
        {
            CpuMetrics cpu{};
            cpu.cpuName    = s_cachedCpuName;
            cpu.loadPercent = SampleCpu();
            cpu.temperature = SampleCpuTemp();
            // Estimate CPU power from load% using TDP table.
            {
                uint32_t tdp = PowerTable::GetCpuTdpByName(s_cachedCpuName);
                if (tdp == 0) tdp = 125;
                cpu.tdpWatts    = static_cast<double>(tdp);
                cpu.powerWatts  = PowerTable::EstimateCpuPowerWatts(
                                      cpu.loadPercent, tdp, /* physicalCores=0 */ 0);
            }
            RamMetrics ram{}; SampleRam(ram);
            auto g2 = SampleGpus();
            body = BuildJson(cpu, ram, g2);
        }
        else if (isIndex)
        {
            body = RenderDashboard(port, cfg);
        }
        else
        {
            const char* notFound = "HTTP/1.1 404 Not Found\r\n"
                                   "Content-Type: text/plain\r\n"
                                   "Connection: close\r\n\r\nNot Found";
            int total = (int)strlen(notFound), sent = 0;
            while (sent < total)
            {
                int sn = send(client, notFound + sent, total - sent, 0);
                if (sn <= 0) break;
                sent += sn;
            }
            closesocket(client);
            continue;
        }

        // No CORS header: local-only API, no need to expose hardware details.
        char hdr[512] = {};
        int hlen = snprintf(hdr, sizeof(hdr),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: %s\r\n"
            "Connection: close\r\n"
            "Content-Length: %u\r\n"
            "\r\n",
            isMetrics ? "application/json" : "text/html; charset=utf-8",
            (unsigned int)body.size());
        // Send headers in a loop to handle short writes.
        int total = hlen, sent = 0;
        while (sent < total)
        {
            int sn = send(client, hdr + sent, total - sent, 0);
            if (sn <= 0) break;
            sent += sn;
        }
        // Send body in a loop to handle short writes.
        total = (int)body.size(); sent = 0;
        while (sent < total)
        {
            int sn = send(client, body.c_str() + sent, total - sent, 0);
            if (sn <= 0) break;
            sent += sn;
        }
        closesocket(client);
    }
    closesocket(srv); WSACleanup();
}
static void StartHttpServer(int port, const AppConfig& cfg) { std::thread(HttpLoop, port, cfg).detach(); }
#else
static void StartHttpServer(int) {}
#endif

// ============================================================================
// Quit-key polling — reliable on Windows via _kbhit() + _getwch().
// Reads directly from the console input buffer, no window needed.
// Returns true when Q or q is pressed (consumes the key).
// ============================================================================
#ifdef PLATFORM_WINDOWS
#include <conio.h>
static bool PollQuitKey()
{
    // _kbhit() returns non-zero if a keystroke is waiting in the console buffer.
    if (!_kbhit()) return false;
    // _getwch() reads a wide-character key without echoing — works in any console.
    wchar_t ch = _getwch();
    return (ch == L'Q' || ch == L'q');
}
#endif

// ============================================================================
// Main
// ============================================================================
int main(int argc, char* argv[])
{
    // Load config first — colors and HTTP port come from loadgraph.cfg.
    AppConfig cfg = LoadAppConfig();

    int httpPort = cfg.http_port;
    for (int i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "-port") == 0 && i + 1 < argc)
        {
            httpPort = atoi(argv[i+1]);
            if (httpPort <= 0 || httpPort > 65535)
            { fprintf(stderr, "Invalid port: %s\n", argv[i+1]); return 1; }
            ++i;
        }
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0)
        {
            printf("LoadGraph Console - system monitoring (htop-like)\n"
                   "Usage: loadgraph-console [-port N]\n"
                   "  -port N   Start HTTP server on port N\n"
                   "  -h        Show this help\n");
            return 0;
        }
    }

#ifdef PLATFORM_WINDOWS
    // Cache console handle early — used by SetColor() in Render().
    g_hOut = GetStdHandle(STD_OUTPUT_HANDLE);

    // Enable VT processing on stdout and WINDOW_INPUT on stdin — CP_UTF8 can
    // interfere with how the ESC byte is emitted if VT is not yet active.
    if (g_hOut != INVALID_HANDLE_VALUE)
    {
        DWORD mode = 0;
        if (GetConsoleMode(g_hOut, &mode))
        {
            if (SetConsoleMode(g_hOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING))
                ; // VT mode enabled on output.
            else
                fprintf(stderr, "WARN: SetConsoleMode(VT/out) failed err=%lu\n",
                        GetLastError());
        }
    }
    // Do NOT enable ENABLE_WINDOW_INPUT: it floods the input buffer with
    // non-keyboard events (resize, focus, mouse), causing _kbhit() to return
    // for non-keystrokes and _getwch() to block waiting for a character.
    // The console app does not process window events, so the mode is unnecessary.
    if (!SetConsoleOutputCP(CP_UTF8))
        fprintf(stderr, "WARN: SetConsoleOutputCP failed err=%lu\n", GetLastError());
    if (!SetConsoleCP(CP_UTF8))
        fprintf(stderr, "WARN: SetConsoleCP failed err=%lu\n", GetLastError());

    // UTF-8 block chars only when CP is confirmed UTF-8; otherwise use ASCII.
    // LOADGRAPH_ASCII_BARS=1 forces ASCII regardless of CP setting.
    g_useUtf8Blocks = (GetConsoleOutputCP() == CP_UTF8);
    {
        const char* asciiEnv = getenv("LOADGRAPH_ASCII_BARS");
        if (asciiEnv && asciiEnv[0] != '\0' && asciiEnv[0] != '0')
            g_useUtf8Blocks = false;
    }

    // Apply colors from config.
    ApplyColors(cfg);

    SetColor(g_colors.title | FOREGROUND_INTENSITY);
    printf("LoadGraph Console - system monitoring\n\n");
    SetColor(g_colors.value);
    printf("  Press Q to quit. Updates every 1 second.\n");
#else // PLATFORM_LINUX
    // Apply ANSI colors from config.
    ApplyColors(cfg);

    printf("%sLoadGraph Console%s - system monitoring\n\n"
           "  Press Q to quit. Updates every 1 second.%s\n",
           g_colors.bold.c_str(), g_colors.title.c_str(), g_colors.reset.c_str());
#endif

    // Enable file logging if configured.
    static FILE* s_logFp = nullptr;
    char logPath[1024] = "loadgraph.log";
    if (cfg.log_file)
    {
        int rc = 0; // declared before platform branches for both Windows and Linux
#ifdef PLATFORM_WINDOWS
        // Use current working directory (same as GUI wxGetCwd()).
        DWORD len = GetCurrentDirectoryA(sizeof(logPath), logPath);
        if (len > 0 && len < sizeof(logPath))
        {
            strncat_s(logPath, sizeof(logPath), "\\loadgraph.log", 14);
        }
        else { strncpy_s(logPath, sizeof(logPath), "loadgraph.log", _TRUNCATE); }
        rc = fopen_s(&s_logFp, logPath, "a");
#else
        s_logFp = fopen("loadgraph.log", "a");
        if (!s_logFp) rc = 1;
#endif
        if (s_logFp)
        {
            fprintf(s_logFp, "console: logging enabled file=%s\n", logPath);
            fflush(s_logFp);
        }
        else
        {
            printf("WARN: cannot open log err=%d\n", rc);
            fflush(stdout);
        }
    }

    printf("  config: port=%d http=%s log=%s\n\n",
           cfg.http_port, cfg.http_enabled ? "on" : "off",
           cfg.log_file ? "on"  : "off");
    fflush(stdout);

    // Initialize data sources.
    InitCpu();
    QueryCpuName(s_cachedCpuName);
    bool nvmlOk = LoadNvml();

    if (httpPort > 0)
        StartHttpServer(httpPort, cfg);

    auto lastRender = std::chrono::steady_clock::now();
    // Accept plain Q or q to quit (polls console input, works reliably).
#ifdef PLATFORM_WINDOWS
    while (!PollQuitKey())
    {
        CpuMetrics cpu{};
        cpu.cpuName    = s_cachedCpuName;
        cpu.loadPercent = SampleCpu();
        cpu.uptimeMs   = GetTickCount64();
        cpu.temperature = SampleCpuTemp();
        // Estimate CPU power from load% using TDP table when no RAPL sensor.
        {
            uint32_t tdp = PowerTable::GetCpuTdpByName(s_cachedCpuName);
            if (tdp == 0) tdp = 125; // conservative default for unknown CPU
            cpu.tdpWatts    = static_cast<double>(tdp);
            cpu.powerWatts  = PowerTable::EstimateCpuPowerWatts(
                                  cpu.loadPercent, tdp, /* physicalCores=0 -> auto */ 0);
        }
#else // PLATFORM_LINUX
    bool running = true;
    while (running)
    {
        CpuMetrics cpu{};
        cpu.cpuName    = cachedCpuName;
        cpu.loadPercent  = SampleCpu();
        cpu.temperature  = SampleCpuTemp();
        {
            uint32_t tdp = PowerTable::GetCpuTdpByName(s_cachedCpuName);
            if (tdp == 0) tdp = 125;
            cpu.tdpWatts    = static_cast<double>(tdp);
            cpu.powerWatts  = PowerTable::EstimateCpuPowerWatts(
                                  cpu.loadPercent, tdp, /* physicalCores=0 */ 0);
        }
        // Read uptime from /proc/uptime (seconds since boot).
        {
            std::ifstream up("/proc/uptime");
            double secs = 0.0;
            if (up >> secs) cpu.uptimeMs = static_cast<uint64_t>(secs * 1000.0);
        }
        // Non-blocking stdin check for 'q' or 'Q'.
        {
            fd_set rf; FD_ZERO(&rf); FD_SET(STDIN_FILENO, &rf);
            timeval tv{}; tv.tv_sec = 0; tv.tv_usec = 80000; // 80ms
            if (select(STDIN_FILENO + 1, &rf, nullptr, nullptr, &tv) > 0)
            {
                char ch = 0;
                if (read(STDIN_FILENO, &ch, 1) == 1 && (ch == 'q' || ch == 'Q'))
                    running = false;
            }
        }
#endif
        RamMetrics ram{}; SampleRam(ram);
        auto gpus = nvmlOk ? SampleGpus() : std::vector<GpuMetrics>();

        auto now = std::chrono::steady_clock::now();
        if (now - lastRender >= std::chrono::seconds(1))
        { Render(cpu, ram, gpus); lastRender = now; }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    FreeNvml();
#ifdef PLATFORM_WINDOWS
    SetColor(g_colors.dim);
    printf("\n  Goodbye.");
    SetColor(g_colors.value);
#else
    printf("\n%s  Goodbye.%s\n", g_colors.dim.c_str(), g_colors.reset.c_str());
#endif
    if (s_logFp) { fclose(s_logFp); s_logFp = nullptr; }
    return 0;
}
