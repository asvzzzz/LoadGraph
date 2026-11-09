// Console-only monitoring tool (htop-like text UI).
// Zero wxWidgets dependency - uses Windows API + NVML directly.
// Usage: loadgraph-console [-port N]
//   -port N : start HTTP server on given port, serve web dashboard

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
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
#include <string>
#include <vector>
#include <windows.h>
#include <psapi.h>

#pragma comment(lib, "psapi.lib")

// NVML dynamic loading - function pointer types (matching nvml.dll exports).
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

static HMODULE              g_hNvml     = nullptr;
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
// CPU monitoring via GetSystemTimes (no PDH dependency).
// Computes average CPU usage across all cores from idle/kernel/user deltas.
// ============================================================================
static ULONGLONG g_prevIdle   = 0;
static ULONGLONG g_prevKernel = 0;
static ULONGLONG g_prevUser   = 0;
static bool      g_prevValid  = false;

static double SampleCpu()
{
    FILETIME ftIdle, ftKernel, ftUser;
    if (!GetSystemTimes(&ftIdle, &ftKernel, &ftUser)) return 0.0;
    ULONGLONG idle   = ((ULONGLONG)ftIdle.dwHighDateTime << 32) | ftIdle.dwLowDateTime;
    ULONGLONG kernel = ((ULONGLONG)ftKernel.dwHighDateTime << 32) | ftKernel.dwLowDateTime;
    ULONGLONG user   = ((ULONGLONG)ftUser.dwHighDateTime   << 32) | ftUser.dwLowDateTime;
    if (g_prevValid)
    {
        ULONGLONG dk = kernel - g_prevKernel;
        ULONGLONG du = user  - g_prevUser;
        ULONGLONG di = idle  - g_prevIdle;
        ULONGLONG total = dk + du - di; // time spent on non-idle work
        if (total > 0)
            return static_cast<double>((total) * 100.0 / (total + di));
    }
    g_prevIdle   = idle;
    g_prevKernel = kernel;
    g_prevUser   = user;
    g_prevValid  = true;
    return 0.0; // first sample always returns 0
}

// InitCpu is a no-op with this approach.
static bool InitCpu() { return true; }

static void QueryCpuName(std::string& out)
{
    HKEY hKey = nullptr;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
        "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
        0, KEY_READ, &hKey) == ERROR_SUCCESS)
    {
        char name[256] = {};
        DWORD size = sizeof(name);
        if (RegQueryValueExA(hKey, "ProcessorNameString", nullptr, nullptr,
                             reinterpret_cast<LPBYTE>(name), &size) == ERROR_SUCCESS)
            out = name;
        RegCloseKey(hKey);
    }
}

// ============================================================================
// RAM monitoring
// ============================================================================
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

// ============================================================================
// GPU monitoring via NVML
// ============================================================================
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
    if (g_fnShutdown) g_fnShutdown();
    if (g_hNvml) { FreeLibrary(g_hNvml); g_hNvml = nullptr; }
}

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

        struct { uint64_t total; uint64_t used; uint64_t free_; } mem{};
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
// ANSI colors
// ============================================================================
static const char* C_GREEN   = "\x1b[32m";
static const char* C_CYAN    = "\x1b[36m";
static const char* C_YELLOW  = "\x1b[33m";
static const char* C_RED     = "\x1b[31m";
static const char* C_WHITE   = "\x1b[37m";
static const char* C_GRAY    = "\x1b[90m";
static const char* C_BOLD    = "\x1b[1m";
static const char* C_NVGREEN = "\x1b[38;5;148m";
static const char* C_RESET   = "\x1b[0m";
static const char* C_CLEAR   = "\x1b[2J";
static const char* C_HOME    = "\x1b[H";

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

static const char* TempColor(double t)
{
    if (t > 80.0) return C_RED;
    if (t > 65.0) return C_YELLOW;
    return C_GREEN;
}

// ============================================================================
// Render
// ============================================================================
static void Render(const CpuMetrics& cpu, const RamMetrics& ram,
                   const std::vector<GpuMetrics>& gpus)
{
    printf("%s%s", C_CLEAR, C_HOME);

    // Header
    printf("%s%s LoadGraph Console %s", C_BOLD, C_NVGREEN, C_RESET);
    if (!cpu.cpuName.empty())
        printf(" %s%s%s", C_CYAN, cpu.cpuName.c_str(), C_RESET);
    else
        printf(" %sCPU (N/A)%s", C_GRAY, C_RESET);
    printf(" %s| Uptime: %s%s\n", C_GRAY, C_WHITE, FormatUptime(cpu.uptimeMs).c_str());

    // CPU
    printf("%s  CPU Load     %s%6.1f%%  %s[%s]%s\n",
           C_BOLD, C_RESET, cpu.loadPercent, C_GRAY,
           ProgressBar(cpu.loadPercent, 30).c_str(), C_RESET);

    // RAM
    printf("%s\n  RAM Usage    %s%6.1f%%  %s[%s]%s\n",
           C_BOLD, C_RESET, ram.loadPercent, C_GRAY,
           ProgressBar(ram.loadPercent, 30).c_str(), C_RESET);
    printf("%s%4.1f GB used / %4.1f GB total%s\n",
           C_GRAY,
           static_cast<double>(ram.totalBytes - ram.freeBytes) / (1024.0*1024*1024),
           static_cast<double>(ram.totalBytes) / (1024.0*1024*1024),
           C_RESET);

    // GPU
    if (!gpus.empty())
    {
        printf("%s%s\n  GPU%s", C_BOLD, C_NVGREEN, C_RESET);
        for (size_t i = 0; i < gpus.size(); ++i)
        {
            const GpuMetrics& g = gpus[i];
            printf("\n\n  --- GPU %zu: %s ---\n", i, g.name.c_str());
            if (g.loadPercent >= 0.0)
            {
                printf("%s    Load       %s%6.1f%%  %s[%s]%s\n",
                       C_BOLD, C_RESET, g.loadPercent, C_GRAY,
                       ProgressBar(g.loadPercent, 30).c_str(), C_RESET);
            }
            if (g.temperature >= 0.0)
                printf("%s    Temp       %s%4.0f C%s\n",
                       C_BOLD, TempColor(g.temperature), g.temperature, C_RESET);
            if (g.vramTotalBytes > 0)
            {
                double pct = g.vramUsedBytes * 100.0 / g.vramTotalBytes;
                printf("%s    VRAM       %s%6.1f%%  %s[%s]%s\n",
                       C_BOLD, C_RESET, pct, C_GRAY,
                       ProgressBar(pct, 30).c_str(), C_RESET);
                double usedGb = static_cast<double>(g.vramUsedBytes) / (1024.0*1024*1024);
                double totGb  = static_cast<double>(g.vramTotalBytes) / (1024.0*1024*1024);
                printf("%s               %s%.1f GB / %.1f GB%s\n",
                       C_GRAY, C_RESET, usedGb, totGb, C_RESET);
            }
            if (g.fanPercent >= 0.0)
                printf("%s    Fan        %s%5.0f%%\n", C_BOLD, C_RESET, g.fanPercent);
            if (g.powerWatts > 0.0)
                printf("%s    Power      %s%5.1f W\n", C_BOLD, C_RESET, g.powerWatts);
        }
    }

    printf("\n%s  [Q] quit                                          %s\n", C_GRAY, C_RESET);
    fflush(stdout);
}

// ============================================================================
// HTTP server (minimal, single-threaded)
// ============================================================================
#ifdef USE_HTTP_SERVER
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

static const char* kHtmlPage = R"(<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8"><title>LoadGraph</title>
<style>*{margin:0;padding:0;box-sizing:border-box}
body{background:#1a1a2e;color:#e0e0e0;font-family:Consolas,monospace;padding:16px}
h1{color:#76B900;margin-bottom:8px;font-size:18px}
.section{background:#16213e;border-radius:8px;padding:12px;margin-bottom:12px}
.row{display:flex;justify-content:space-between;align-items:center;margin:4px 0}
.label{color:#888;font-size:13px}.value{color:#fff;font-weight:bold;font-size:14px}
.bar-bg{background:#0f3460;border-radius:4px;height:16px;width:200px;overflow:hidden}
.bar-fill{height:100%;border-radius:4px;transition:width .5s}
.gpu-title{color:#76B900;font-size:14px;margin-top:8px;margin-bottom:4px}
.footer{text-align:center;color:#555;font-size:11px;margin-top:16px}
.green{color:#76B900}.cyan{color:#00bcd4}.yellow{color:#ffc107}.red{color:#f44336}</style>
</head><body>
<h1>NVIDIA LoadGraph <span id="cpuName" class="cyan"></span></h1>
<div class="section">
<div class="row"><span class="label">CPU Load</span><span><span id="cpuLoad" class="value">--</span>%<div class="bar-bg"><div id="cpuBar" class="bar-fill" style="width:0;background:#76B900"></div></div></span></div>
<div class="row"><span class="label">Uptime</span><span id="uptime" class="value">--</span></div>
</div>
<div class="section">
<div class="row"><span class="label">RAM Usage</span><span><span id="ramLoad" class="value">--</span>%<div class="bar-bg"><div id="ramBar" class="bar-fill" style="width:0;background:#00bcd4"></div></div></span></div>
<div class="row"><span class="label">RAM Used</span><span id="ramUsed" class="value">N/A</span><span class="label" id="ramTotal"></span></div>
</div>
<div id="gpuSections"></div>
<div class="footer">Auto-refresh every 1s | LoadGraph Console</div>
<script>
const GC=['#76B900','#00bcd4','#ffc107','#f44336','#e91e63'];
function esc(s){return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;')}
function fb(b){if(!b||b<1024)return'0 B';if(b<1048576)return(b/1024).toFixed(0)+' KB';if(b<1073741824)return(b/1073741824).toFixed(1)+' GB';return(b/1099511627776).toFixed(1)+' TB'}
function fu(ms){var s=Math.floor(ms/1000),d=Math.floor(s/86400);s%=86400;var h=Math.floor(s/3600);s%=3600;var m=Math.floor(s/60);s%=60;if(d>0)return d+'d '+h+'h '+m+'m';if(h>0)return h+'h '+m+'m '+s+'s';return m+'m '+s+'s'}
function tc(t){return t>80?'red':t>65?'yellow':'green'}
async function fm(){try{var r=await fetch('/api/metrics'),d=await r.json();ud(d)}catch(e){}}
function ud(d){
document.getElementById('cpuName').textContent=d.cpu_name||'';
document.getElementById('cpuLoad').textContent=d.cpu_load.toFixed(1);
document.getElementById('cpuBar').style.width=Math.min(100,d.cpu_load)+'%';
document.getElementById('cpuBar').style.background=d.cpu_load>80?'#f44336':'#76B900';
document.getElementById('uptime').textContent=fu(d.uptime_ms);
document.getElementById('ramLoad').textContent=d.ram_load.toFixed(1);
document.getElementById('ramBar').style.width=Math.min(100,d.ram_load)+'%';
document.getElementById('ramUsed').textContent=fb(d.ram_used_bytes);
document.getElementById('ramTotal').textContent='/ '+fb(d.ram_total_bytes);
var gd=document.getElementById('gpuSections');if(!d.gpus||!d.gpus.length){gd.innerHTML='';return}
var h='';for(var i=0;i<d.gpus.length;i++){var g=d.gpus[i],c=GC[i%GC.length];
h+='<div class="section"><div class="gpu-title" style="color:'+c+'">GPU '+i+': '+esc(g.name)+'</div>';
if(g.load_percent>=0)h+='<div class="row"><span class="label">Load</span><span>'+g.load_percent.toFixed(1)+'%<div class="bar-bg"><div style="width:'+Math.min(100,g.load_percent)+'%;background:'+c+';height:100%;border-radius:4px"></div></div></span></div>';
if(g.temperature>=0){var t=tc(g.temperature);h+='<div class="row"><span class="label">Temp</span><span class="'+t+'">'+g.temperature.toFixed(0)+' C</span></div>'}
if(g.vram_total_bytes>0){var p=g.vram_used_bytes*100/g.vram_total_bytes;h+='<div class="row"><span class="label">VRAM</span><span>'+p.toFixed(1)+'% ('+fb(g.vram_used_bytes)+'/'+fb(g.vram_total_bytes)+')<div class="bar-bg"><div style="width:'+Math.min(100,p)+'%;background:'+c+';height:100%;border-radius:4px"></div></div></span></div>'}
if(g.power_watts>0)h+='<div class="row"><span class="label">Power</span><span>'+g.power_watts.toFixed(1)+' W</span></div>';
h+='</div>'}gd.innerHTML=h}
setInterval(fm,1000);fm();
</script></body></html>)";

static std::string BuildJson(const CpuMetrics& cpu, const RamMetrics& ram,
                              const std::vector<GpuMetrics>& gpus)
{
    char buf[8192]; int off = 0;
    off = snprintf(buf, sizeof(buf),
        "{\"cpu_load\":%.1f,\"uptime_ms\":%llu,\"cpu_name\":\"%s\","
         "\"ram_load\":%.1f,\"ram_used_bytes\":%llu,\"ram_total_bytes\":%llu,"
         "\"gpus\":[",
        cpu.loadPercent, (unsigned long long)cpu.uptimeMs, cpu.cpuName.c_str(),
        ram.loadPercent,
        (unsigned long long)(ram.totalBytes > ram.freeBytes ? ram.totalBytes - ram.freeBytes : 0),
        (unsigned long long)ram.totalBytes);
    for (size_t i = 0; i < gpus.size(); ++i)
    {
        const GpuMetrics& g = gpus[i];
        if (i > 0) off += snprintf(buf+off, sizeof(buf)-off, ",");
        off += snprintf(buf+off, sizeof(buf)-off,
            "{\"name\":\"%s\",\"load_percent\":%.1f,\"temperature\":%.1f,"
             "\"vram_used_bytes\":%llu,\"vram_total_bytes\":%llu,"
             "\"power_watts\":%.1f}",
            g.name.c_str(), g.loadPercent, g.temperature,
            (unsigned long long)g.vramUsedBytes,
            (unsigned long long)g.vramTotalBytes, g.powerWatts);
    }
    off += snprintf(buf+off, sizeof(buf)-off, "]}");
    return std::string(buf, static_cast<size_t>(off));
}

static void HttpLoop(int port)
{
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) { fprintf(stderr, "HTTP: WSAStartup failed\n"); return; }
    SOCKET srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (srv == INVALID_SOCKET) { fprintf(stderr, "HTTP: socket() failed err=%d\n", WSAGetLastError()); WSACleanup(); return; }
    int opt = 1; setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
    sockaddr_in addr{}; addr.sin_family=AF_INET; addr.sin_addr.s_addr=INADDR_ANY; addr.sin_port=htons((unsigned short)port);
    if (bind(srv, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR)
    { fprintf(stderr, "HTTP: bind() failed port=%d err=%d\n", port, WSAGetLastError()); closesocket(srv); WSACleanup(); return; }
    if (listen(srv, 4) == SOCKET_ERROR) { fprintf(stderr, "HTTP: listen() failed err=%d\n", WSAGetLastError()); closesocket(srv); WSACleanup(); return; }
    printf("HTTP server listening on port %d\nOpen http://localhost:%d in your browser\n", port, port);
    fflush(stdout);
    while (true)
    {
        fd_set rf; FD_ZERO(&rf); FD_SET(srv, &rf);
        timeval tv{}; tv.tv_sec=1; tv.tv_usec=0;
        if (select(0, &rf, nullptr, nullptr, &tv) <= 0 || !FD_ISSET(srv, &rf)) continue;
        SOCKET client = accept(srv, nullptr, nullptr);
        if (client == INVALID_SOCKET) continue;
        char req[2048] = {}; recv(client, req, sizeof(req)-1, 0);
        std::string resp;
        if (strstr(req, "GET /api/metrics") != nullptr)
        {
            CpuMetrics cpu{}; cpu.loadPercent = SampleCpu();
            RamMetrics ram{}; SampleRam(ram);
            auto g2 = SampleGpus();
            resp = std::string("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n") + BuildJson(cpu, ram, g2);
        }
        else if (strstr(req, "GET /") != nullptr)
        {
            resp = std::string("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n") + kHtmlPage;
        }
        else { resp = "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\nNot Found"; }
        send(client, resp.c_str(), (int)resp.size(), 0);
        closesocket(client);
    }
    closesocket(srv); WSACleanup();
}

static void StartHttpServer(int port) { std::thread(HttpLoop, port).detach(); }
#else
static void StartHttpServer(int) {}
#endif

// ============================================================================
// Main
// ============================================================================
int main(int argc, char* argv[])
{
    int httpPort = 0;
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

    SetConsoleOutputCP(CP_UTF8);
    printf("%s%s\n LoadGraph Console%s - system monitoring\n\n"
           "  Press Q to quit. Updates every 1 second.\n", C_BOLD, C_NVGREEN, C_RESET);
    fflush(stdout);

    // Initialize data sources.
    InitCpu();
    { std::string n; QueryCpuName(n); }
    bool nvmlOk = LoadNvml();

    if (httpPort > 0)
        StartHttpServer(httpPort);

    auto lastRender = std::chrono::steady_clock::now();
    while (!(GetAsyncKeyState((int)'Q') & 0x8000))
    {
        CpuMetrics cpu{};
        cpu.loadPercent = SampleCpu();
        cpu.uptimeMs = GetTickCount64();
        RamMetrics ram{}; SampleRam(ram);
        auto gpus = nvmlOk ? SampleGpus() : std::vector<GpuMetrics>();

        auto now = std::chrono::steady_clock::now();
        if (now - lastRender >= std::chrono::seconds(1))
        { Render(cpu, ram, gpus); lastRender = now; }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    FreeNvml();
    printf("\n%sGoodbye.%s\n", C_GRAY, C_RESET);
    return 0;
}
