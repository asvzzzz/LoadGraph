// Console-only monitoring tool (htop-like text UI).
// Zero wxWidgets dependency - uses Windows API + NVML directly.
// Usage: LoadGraphTTY [-port N]
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
#endif
// Note: WMI is used internally by lib_sensors (cpu_monitor.cpp, storage_monitor.cpp).
// console_app.cpp calls MetricsGet* and does not use WMI directly.
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
#include <mutex>

// Note: /proc is read via ReadFileToString() in platform.h; no fstream needed.

#ifdef PLATFORM_LINUX
#include <unistd.h>      // read, STDIN_FILENO
#include <sys/select.h>  // select, fd_set, FD_ZERO, FD_SET
#include <sys/time.h>    // timeval
#endif

#include "platform.h"   // defines PLATFORM_WINDOWS / PLATFORM_LINUX before any guards below
#include "app_config.h"
#include "power_table.h" // CPU TDP lookup and power estimation
#include "cpu_monitor.h" // CpuMetrics (shared definition with lib_sensors)
#include "ram_monitor.h" // RamMetrics (shared definition with lib_sensors)
#include "gpu_monitor.h" // GpuMetrics (shared definition with lib_sensors)
#include "metrics_api.h" // MetricsGet* API from lib_sensors
#include "http_server.h" // StartHttpServer from lib_http
#include "console_colors.h" // GetConsoleColors / SetConsoleColor from lib_theme

// ============================================================================
// Helpers
// ============================================================================
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

// Render a waveform row: `label` on left, values from `hist`, width columns.
// ttyplot-style: Unicode block chars (U+2591-U+2588) with per-cell color.
static void DrawWave(const char* label, const RingBuffer<double>& hist, int cols
#ifdef PLATFORM_WINDOWS
    , ConsoleColor cellColor = 0   // 0 = use cc.label
#else
    , const std::string& cellColor = std::string()   // empty = use cc.label
#endif
)
{
#ifdef PLATFORM_WINDOWS
    const ConsoleColors& cc = GetConsoleColors();
    if (cellColor == 0) cellColor = cc.label;
    SetConsoleColor(cellColor);
    printf("%-12s", label);

    const int steps = (int)hist.Size();
    for (int c = 0; c < cols; ++c)
    {
        const int hi = steps - cols + c;
        double v = (hi >= 0) ? hist.Get(static_cast<size_t>(hi)) : 0.0;
        v = std::max(0.0, std::min(100.0, v));
        int lv = static_cast<int>(v / 100.0 * 4.0 + 0.5); // 0..4
        lv = std::max(0, std::min(4, lv));

        if (lv == 0)
        {
            SetConsoleColor(cc.dim);
            putchar(' ');   // empty cell: dim gray space, always visible
        }
        else
        {
            SetConsoleColor(cellColor);
            if (g_useUtf8Blocks)
                fputs(kUtf8Blocks[lv], stdout);
            else
                putchar(" .oO@"[lv]);  // ASCII fallback: 4 levels
        }
    }
    SetConsoleColor(cc.value);
    putchar('\n');
#else // PLATFORM_LINUX — ANSI + UTF-8 via printf
    const ConsoleColors& cc = GetConsoleColors();
    const std::string& col = cellColor.empty() ? cc.label : cellColor;
    printf("%s%-12s%s", col.c_str(), label, cc.reset.c_str());

    const int steps = (int)hist.Size();
    for (int c = 0; c < cols; ++c)
    {
        const int hi = steps - cols + c;
        double v = (hi >= 0) ? hist.Get(static_cast<size_t>(hi)) : 0.0;
        v = std::max(0.0, std::min(100.0, v));
        int lv = static_cast<int>(v / 100.0 * 4.0 + 0.5);
        lv = std::max(0, std::min(4, lv));
        if (lv == 0) fputs("\x1b[90m ", stdout);
        else         fputs(kUtf8Blocks[lv], stdout);
    }
    printf("%s\n", cc.reset.c_str());
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
    // Use cached handle from InitConsoleHandle() instead of calling GetStdHandle again.
    HANDLE hOut = GetConsoleOutputHandle();
    if (hOut != INVALID_HANDLE_VALUE)
    {
        COORD zero = { 0, 0 };
        SetConsoleCursorPosition(hOut, zero);
        CONSOLE_SCREEN_BUFFER_INFO info;
        if (GetConsoleScreenBufferInfo(hOut, &info))
        {
            DWORD written = 0;
            const DWORD fill = static_cast<DWORD>(
                (info.dwSize.Y + 2) * info.dwSize.X);
            FillConsoleOutputCharacterA(hOut, ' ', fill, zero, &written);
            SetConsoleCursorPosition(hOut, zero);
        }
    }
#endif

#ifdef PLATFORM_WINDOWS
    const ConsoleColors& cc = GetConsoleColors();
    // Header
    SetConsoleColor(cc.title | FOREGROUND_INTENSITY);
    printf(" LoadGraph Console");
    if (cpu.cpuName[0] != '\0')
    {
        SetConsoleColor(FOREGROUND_BLUE | FOREGROUND_GREEN); // cyan for CPU name
#ifdef PLATFORM_WINDOWS
        printf(" %ls", cpu.cpuName);
#else
        printf(" %s", cpu.cpuName);
#endif
    }
    else
    {
        SetConsoleColor(cc.dim);
        printf(" CPU (N/A)");
    }
    SetConsoleColor(cc.dim);
    printf(" | Uptime: ");
    SetConsoleColor(cc.value);
    printf("%s\n", FormatUptime(cpu.uptimeMs).c_str());

    // Push current samples into history rings.
    g_cpuHist.Push(cpu.loadPercent);
    g_ramHist.Push(ram.loadPercent);

    const int kW = 40; // waveform width in chars

    // CPU section
    SetConsoleColor(FOREGROUND_INTENSITY);
    printf("\n  CPU Load     ");
    SetConsoleColor(cc.cpu_load);
    printf("%5.1f%%\n", cpu.loadPercent);
    DrawWave("", g_cpuHist, kW, cc.cpu_load);

    if (cpu.powerWatts > 0.0)
    {
        SetConsoleColor(cc.value);
        printf("  CPU Power    %5.1f W / ", cpu.powerWatts);
        SetConsoleColor(cc.dim);
        printf("%5.0f W TDP\n", cpu.tdpWatts);
    }

    // RAM section
    SetConsoleColor(FOREGROUND_INTENSITY);
    printf("\n  RAM Usage    ");
    SetConsoleColor(cc.ram);
    printf("%5.1f%%\n", ram.loadPercent);
    DrawWave("", g_ramHist, kW, cc.ram);
    SetConsoleColor(cc.dim);
    printf("           %4.1f GB / %4.1f GB\n",
           static_cast<double>(ram.totalBytes - ram.freeBytes) / (1024.0*1024*1024),
           static_cast<double>(ram.totalBytes) / (1024.0*1024*1024));

    // GPU section
    if (!gpus.empty())
    {
        SetConsoleColor(cc.title | FOREGROUND_INTENSITY);
        printf("\n  GPU");
        for (size_t i = 0; i < gpus.size(); ++i)
        {
            const GpuMetrics& g = gpus[i];
            // Explicit color reset: otherwise the header inherits the color from
            // the previous printf call (Load/Temp/VRAM/Fan/Power).
            SetConsoleColor(cc.title | FOREGROUND_INTENSITY);
            printf("\n\n  --- GPU %zu: %s ---\n", i, g.name.c_str());
            if (g.loadPercent >= 0.0)
            {
                SetConsoleColor(FOREGROUND_INTENSITY);
                printf("    Load       ");
                SetConsoleColor(cc.gpu_load);
                printf("%5.1f%%\n", g.loadPercent);
            }
            if (g.temperature >= 0.0)
            {
                SetConsoleColor(FOREGROUND_INTENSITY);
                printf("    Temp       ");
                SetConsoleColor(TempAttrs(g.temperature));
                printf("%4.0f C\n", g.temperature);
            }
            if (g.vramTotalBytes > 0)
            {
                double pct = g.vramUsedBytes * 100.0 / g.vramTotalBytes;
                SetConsoleColor(FOREGROUND_INTENSITY);
                printf("    VRAM       ");
                SetConsoleColor(cc.gpu_vram);
                printf("%5.1f%%\n", pct);
                double usedGb = static_cast<double>(g.vramUsedBytes) / (1024.0*1024*1024);
                double totGb  = static_cast<double>(g.vramTotalBytes) / (1024.0*1024*1024);
                SetConsoleColor(cc.dim);
                printf("%4.1f GB / %4.1f GB\n", usedGb, totGb);
            }
            if (g.fanPercent >= 0.0)
            {
                SetConsoleColor(FOREGROUND_INTENSITY);
                printf("    Fan        ");
                SetConsoleColor(cc.gpu_fan);
                printf("%5.0f%%\n", g.fanPercent);
            }
            if (g.powerWatts > 0.0)
            {
                SetConsoleColor(FOREGROUND_INTENSITY);
                printf("    Power      ");
                SetConsoleColor(cc.gpu_power);
                printf("%5.1f W\n", g.powerWatts);
            }
        }
    }

    SetConsoleColor(cc.dim);
    printf("\n  [Q] quit");
    SetConsoleColor(cc.value);
#else // PLATFORM_LINUX — ANSI output
    const ConsoleColors& cc = GetConsoleColors();
    printf("%s%s LoadGraph Console%s", cc.bold.c_str(), cc.title.c_str(), cc.reset.c_str());
    if (cpu.cpuName[0] != '\0')
        printf(" %s%s%s", "\x1b[36m", cpu.cpuName, cc.reset.c_str()); // cyan
    else
        printf(" %sCPU (N/A)%s", cc.dim.c_str(), cc.reset.c_str());
    printf(" %s| Uptime: %s%s\n", cc.dim.c_str(), cc.value.c_str(), FormatUptime(cpu.uptimeMs).c_str());

    // Push current samples into history rings.
    g_cpuHist.Push(cpu.loadPercent);
    g_ramHist.Push(ram.loadPercent);

    const int kW = 40; // waveform width in chars

    // CPU section
    printf("\n%s  CPU Load     %s%5.1f%%%s\n",
           cc.bold.c_str(), cc.cpu_load.c_str(), cpu.loadPercent, cc.reset.c_str());
    DrawWave("", g_cpuHist, kW, cc.cpu_load);

    if (cpu.powerWatts > 0.0)
        printf("  CPU Power    %s%5.1f W / %s%5.0f W TDP%s\n",
               cc.value.c_str(), cpu.powerWatts,
               cc.dim.c_str(), cpu.tdpWatts, cc.reset.c_str());

    // RAM section
    printf("%s  RAM Usage    %s%5.1f%%%s\n",
           cc.bold.c_str(), cc.ram.c_str(), ram.loadPercent, cc.reset.c_str());
    DrawWave("", g_ramHist, kW, cc.ram);
    printf("%s           %4.1f GB / %4.1f GB%s\n",
           cc.dim.c_str(),
           static_cast<double>(ram.totalBytes - ram.freeBytes) / (1024.0*1024*1024),
           static_cast<double>(ram.totalBytes) / (1024.0*1024*1024),
           cc.reset.c_str());

    // GPU section
    if (!gpus.empty())
    {
        printf("%s%s\n  GPU%s", cc.bold.c_str(), cc.title.c_str(), cc.reset.c_str());
        for (size_t i = 0; i < gpus.size(); ++i)
        {
            const GpuMetrics& g = gpus[i];
            // Explicit color reset: otherwise the header inherits from a previous row.
            printf("%s", cc.title.c_str());
            printf("\n\n  --- GPU %zu: %s%s\n", i, g.name.c_str(), cc.reset.c_str());
            if (g.loadPercent >= 0.0)
                printf("%s    Load       %s%5.1f%%\n",
                       cc.bold.c_str(), cc.gpu_load.c_str(), g.loadPercent);
            if (g.temperature >= 0.0)
                printf("%s    Temp       %s%4.0f C%s\n",
                       cc.bold.c_str(), TempAnsi(g.temperature), g.temperature, cc.reset.c_str());
            if (g.vramTotalBytes > 0)
            {
                double pct = g.vramUsedBytes * 100.0 / g.vramTotalBytes;
                printf("%s    VRAM       %s%5.1f%%\n",
                       cc.bold.c_str(), cc.gpu_vram.c_str(), pct);
                double usedGb = static_cast<double>(g.vramUsedBytes) / (1024.0*1024*1024);
                double totGb  = static_cast<double>(g.vramTotalBytes) / (1024.0*1024*1024);
                printf("%s%4.1f GB / %4.1f GB%s\n",
                       cc.dim.c_str(), usedGb, totGb, cc.reset.c_str());
            }
            if (g.fanPercent >= 0.0)
                printf("%s    Fan        %s%5.0f%%\n",
                       cc.bold.c_str(), cc.gpu_fan.c_str(), g.fanPercent);
            if (g.powerWatts > 0.0)
                printf("%s    Power      %s%5.1f W%s\n",
                       cc.bold.c_str(), cc.gpu_power.c_str(), g.powerWatts, cc.reset.c_str());
        }
    }

    printf("\n%s  [Q] quit%s\n", cc.dim.c_str(), cc.reset.c_str());
#endif // PLATFORM_WINDOWS / PLATFORM_LINUX
    fflush(stdout);
}

// ============================================================================
// Quit-key polling — reliable on Windows via _kbhit() + _getwch().
// Returns true when Q or q is pressed (consumes the key).
// ============================================================================
#ifdef PLATFORM_WINDOWS
#include <conio.h>
static bool PollQuitKey()
{
    if (!_kbhit()) return false;
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

    // -1 means "not specified on command line"; 0+ means CLI override.
    int cliPort = -1;
    for (int i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "-port") == 0 && i + 1 < argc)
        {
            cliPort = atoi(argv[i+1]);
            if (cliPort <= 0 || cliPort > 65535)
            { fprintf(stderr, "Invalid port: %s\n", argv[i+1]); return 1; }
            ++i;
        }
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0)
        {
            printf("LoadGraph Console - system monitoring (htop-like)\n"
                   "Usage: LoadGraphTTY [-port N]\n"
                   "  -port N   Start HTTP server on port N\n"
                   "  -h        Show this help\n");
            return 0;
        }
    }

#ifdef PLATFORM_WINDOWS
    // Cache console handle early — used by SetConsoleColor() in Render().
    InitConsoleHandle();
    SetConsoleColor(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
    if (!SetConsoleOutputCP(CP_UTF8))
        fprintf(stderr, "WARN: SetConsoleOutputCP failed err=%lu\n", GetLastError());
    if (!SetConsoleCP(CP_UTF8))
        fprintf(stderr, "WARN: SetConsoleCP failed err=%lu\n", GetLastError());

    // UTF-8 block chars only when CP is confirmed UTF-8; otherwise use ASCII.
    g_useUtf8Blocks = (GetConsoleOutputCP() == CP_UTF8);
    {
        const char* asciiEnv = getenv("LOADGRAPH_ASCII_BARS");
        if (asciiEnv && asciiEnv[0] != '\0' && asciiEnv[0] != '0')
            g_useUtf8Blocks = false;
    }

    // Apply colors from config.
    ApplyColors(cfg);

    const ConsoleColors& cc = GetConsoleColors();
    SetConsoleColor(cc.title | FOREGROUND_INTENSITY);
    printf("LoadGraph Console - system monitoring\n\n");
    SetConsoleColor(cc.value);
    printf("  Press Q to quit. Updates every 1 second.\n");
#else // PLATFORM_LINUX
    ApplyColors(cfg);

    const ConsoleColors& cc = GetConsoleColors();
    printf("%sLoadGraph Console%s - system monitoring\n\n"
           "  Press Q to quit. Updates every 1 second.%s\n",
           cc.bold.c_str(), cc.title.c_str(), cc.reset.c_str());
#endif

    // Enable file logging if configured.
    static FILE* s_logFp = nullptr;
    char logPath[1024] = "loadgraph.log";
    int rc = 0;
#ifdef PLATFORM_WINDOWS
    DWORD len = GetCurrentDirectoryA(sizeof(logPath), logPath);
    if (len > 0 && len < sizeof(logPath))
        strncat_s(logPath, sizeof(logPath), "\\loadgraph.log", 14);
    else
        strncpy_s(logPath, sizeof(logPath), "loadgraph.log", _TRUNCATE);
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

    // Only start HTTP server when explicitly enabled in config or -port given.
    // cliPort == -1 means no -port flag was passed; use cfg.http_port only then.
    int effectivePort = (cliPort >= 0) ? cliPort : cfg.http_port;
    bool httpActive = cfg.http_enabled || cliPort >= 0;

    printf("  config: port=%d effective=%d http=%s log=%s\n\n",
           cfg.http_port, effectivePort,
           cfg.http_enabled ? "on" : "off",
           cfg.log_file ? "on"  : "off");
    fflush(stdout);

    // Initialize metrics system (NVML, PDH, thermal sensors, etc.).
    MetricsInit();

    if (httpActive)
        StartHttpServer(cfg);

    auto lastRender = std::chrono::steady_clock::now();

#ifdef PLATFORM_WINDOWS
    // Accept plain Q or q to quit (polls console input, works reliably).
    while (!PollQuitKey())
    {
        CpuData  cpuData{};
        MetricsGetCpu(cpuData);
        RamData  ramData{};
        MetricsGetRam(ramData);
        const auto&    gpuDatas = MetricsGetGpus();

        CpuMetrics cpu{};
#ifdef PLATFORM_WINDOWS
        wcsncpy(cpu.cpuName, cpuData.cpuName.c_str(), sizeof(cpu.cpuName) / sizeof(wchar_t) - 1);
#else
        strncpy(cpu.cpuName, cpuData.cpuName.c_str(), sizeof(cpu.cpuName) - 1);
#endif
        cpu.loadPercent  = cpuData.loadPercent;
        cpu.temperature  = cpuData.temperature;
        cpu.uptimeMs     = cpuData.uptimeMs;
        cpu.powerWatts   = cpuData.powerWatts;
        cpu.tdpWatts     = cpuData.tdpWatts;

        RamMetrics ram{};
        ram.totalBytes   = ramData.totalBytes;
        ram.freeBytes    = ramData.freeBytes;
        ram.loadPercent  = ramData.loadPercent;

        std::vector<GpuMetrics> gpus;
        for (const auto& gd : gpuDatas)
        {
            GpuMetrics gm{};
            gm.name           = gd.name;
            gm.loadPercent    = gd.loadPercent;
            gm.temperature    = gd.temperature;
            gm.vramTotalBytes = gd.vramTotalBytes;
            gm.vramUsedBytes  = gd.vramUsedBytes;
            gm.fanPercent     = gd.fanPercent;
            gm.powerWatts     = gd.powerWatts;
            gpus.push_back(gm);
        }

        auto now = std::chrono::steady_clock::now();
        if (now - lastRender >= std::chrono::seconds(1))
        { Render(cpu, ram, gpus); lastRender = now; }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
#else // PLATFORM_LINUX
    bool running = true;
    while (running)
    {
        CpuData  cpuData{};
        MetricsGetCpu(cpuData);
        RamData  ramData{};
        MetricsGetRam(ramData);
        const auto&    gpuDatas = MetricsGetGpus();

        CpuMetrics cpu{};
#ifdef PLATFORM_WINDOWS
        wcsncpy(cpu.cpuName, cpuData.cpuName.c_str(), sizeof(cpu.cpuName) / sizeof(wchar_t) - 1);
#else
        strncpy(cpu.cpuName, cpuData.cpuName.c_str(), sizeof(cpu.cpuName) - 1);
#endif
        cpu.loadPercent  = cpuData.loadPercent;
        cpu.temperature  = cpuData.temperature;
        cpu.uptimeMs     = cpuData.uptimeMs;
        cpu.powerWatts   = cpuData.powerWatts;
        cpu.tdpWatts     = cpuData.tdpWatts;

        RamMetrics ram{};
        ram.totalBytes   = ramData.totalBytes;
        ram.freeBytes    = ramData.freeBytes;
        ram.loadPercent  = ramData.loadPercent;

        std::vector<GpuMetrics> gpus;
        for (const auto& gd : gpuDatas)
        {
            GpuMetrics gm{};
            gm.name           = gd.name;
            gm.loadPercent    = gd.loadPercent;
            gm.temperature    = gd.temperature;
            gm.vramTotalBytes = gd.vramTotalBytes;
            gm.vramUsedBytes  = gd.vramUsedBytes;
            gm.fanPercent     = gd.fanPercent;
            gm.powerWatts     = gd.powerWatts;
            gpus.push_back(gm);
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

        auto now = std::chrono::steady_clock::now();
        if (now - lastRender >= std::chrono::seconds(1))
        { Render(cpu, ram, gpus); lastRender = now; }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
#endif

    MetricsShutdown();

#ifdef PLATFORM_WINDOWS
    const ConsoleColors& ccc = GetConsoleColors();
    SetConsoleColor(ccc.dim);
    printf("\n  Goodbye.");
    SetConsoleColor(ccc.value);
#else
    const ConsoleColors& ccc = GetConsoleColors();
    printf("\n%s  Goodbye.%s\n", ccc.dim.c_str(), ccc.reset.c_str());
#endif

    if (s_logFp) { fclose(s_logFp); s_logFp = nullptr; }
    return 0;
}
