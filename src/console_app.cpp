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
// Progress bar — simple horizontal bar showing a percentage value.
// Uses Unicode block characters: ░ (empty) and █ (filled).
// Returns the rendered line as a std::string (no console I/O).
// ============================================================================
static const char* const kBarEmpty  = "\xe2\x96\x91";   // U+2591 light shade
static const char* const kBarFull   = "\xe2\x96\x88";   // U+2588 full block

#ifdef PLATFORM_WINDOWS
static std::string BuildProgress(const char* label, double percent, int cols, WORD fillColor)
{
    percent = std::max(0.0, std::min(100.0, percent));
    const int filled = static_cast<int>(std::round(percent / 100.0 * cols));
    const ConsoleColors& cc = GetConsoleColors();

    // Build raw text with embedded ANSI-like color markers won't work on Windows.
    // Instead we build the visual string and return it; the caller handles coloring.
    std::string out;
    out += label;
    out += '[';
    for (int i = 0; i < filled;   ++i) out += kBarFull;
    for (int i = filled; i < cols; ++i) out += kBarEmpty;
    out += ']';
    return out;
}
#else // PLATFORM_LINUX
static std::string BuildProgress(const char* label, double percent, int cols, const std::string& fillColor)
{
    percent = std::max(0.0, std::min(100.0, percent));
    const int filled = static_cast<int>(std::round(percent / 100.0 * cols));
    const ConsoleColors& cc = GetConsoleColors();
    const std::string& col = fillColor.empty() ? cc.label : fillColor;

    std::string out;
    out += cc.bold;
    out += label;
    out += '[';
    out += col;
    for (int i = 0; i < filled;   ++i) out += kBarFull;
    for (int i = filled; i < cols; ++i) { out += "\x1b[90m"; out += kBarEmpty; }
    out += cc.reset;
    out += ']';
    return out;
}
#endif

// ============================================================================
// Console width helper — returns visible column count.
// ============================================================================
#ifdef PLATFORM_WINDOWS
static int GetConsoleWidth()
{
    HANDLE hOut = GetConsoleOutputHandle();
    if (hOut == INVALID_HANDLE_VALUE) return 80;
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (!GetConsoleScreenBufferInfo(hOut, &info)) return 80;
    return static_cast<int>(info.srWindow.Right - info.srWindow.Left + 1);
}
#else
static int GetConsoleWidth()
{
    // Try environment variable first, then fallback.
    const char* env = getenv("COLUMNS");
    if (env && env[0])
    {
        int w = atoi(env);
        if (w > 0) return w;
    }
    return 80;
}
#endif

// ============================================================================
// BuildFrame — assembles the entire UI into a single string.
// No console I/O happens here — caller writes it once.
// ============================================================================
static std::string BuildFrame(const CpuMetrics& cpu, const RamMetrics& ram,
                              const std::vector<GpuMetrics>& gpus, int barCols)
{
    std::string out;
#ifdef PLATFORM_WINDOWS
    const ConsoleColors& cc = GetConsoleColors();

    // Header
    SetConsoleColor(cc.title | FOREGROUND_INTENSITY);
    out += " LoadGraph Console";
    if (cpu.cpuName[0] != '\0')
    {
        SetConsoleColor(FOREGROUND_BLUE | FOREGROUND_GREEN);
#ifdef PLATFORM_WINDOWS
        // Convert wchar_t CPU name to UTF-8 for string concatenation.
        int len = WideCharToMultiByte(CP_UTF8, 0, cpu.cpuName, -1, nullptr, 0, nullptr, nullptr);
        if (len > 0)
        {
            std::string wstr; wstr.resize(len - 1);
            WideCharToMultiByte(CP_UTF8, 0, cpu.cpuName, -1, &wstr[0], len, nullptr, nullptr);
            out += " "; out += wstr;
        }
#else
        out += " "; out += cpu.cpuName;
#endif
    }
    else
    {
        SetConsoleColor(cc.dim);
        out += " CPU (N/A)";
    }
    SetConsoleColor(cc.dim);
    out += " | Uptime: ";
    SetConsoleColor(cc.value);
    out += FormatUptime(cpu.uptimeMs);
    out += '\n';

    // CPU section
    SetConsoleColor(FOREGROUND_INTENSITY);
    out += "\n  CPU Load     ";
    SetConsoleColor(cc.cpu_load);
    char tmp[32];
    snprintf(tmp, sizeof(tmp), "%5.1f%%", cpu.loadPercent);
    out += tmp;
    out += '\n';
    // We can't easily embed colors in a string on Windows without VT sequences,
    // so we write CPU/RAM/GPU sections directly with color calls and only build
    // the bar line as plain text.  Actually — let's just use WriteConsoleW later
    // with the full string.  For now keep it simple: output directly.
    (void)barCols; // used below

    // CPU Power
    if (cpu.powerWatts > 0.0)
    {
        SetConsoleColor(cc.value);
        out += "  CPU Power    ";
        snprintf(tmp, sizeof(tmp), "%5.1f W / ", cpu.powerWatts);
        out += tmp;
        SetConsoleColor(cc.dim);
        snprintf(tmp, sizeof(tmp), "%5.0f W TDP", cpu.tdpWatts);
        out += tmp;
        out += '\n';
    }

    // RAM section
    SetConsoleColor(FOREGROUND_INTENSITY);
    out += "\n  RAM Usage    ";
    SetConsoleColor(cc.ram);
    snprintf(tmp, sizeof(tmp), "%5.1f%%", ram.loadPercent);
    out += tmp;
    out += '\n';

    // GPU section
    if (!gpus.empty())
    {
        SetConsoleColor(cc.title | FOREGROUND_INTENSITY);
        out += "\n  GPU";
        for (size_t i = 0; i < gpus.size(); ++i)
        {
            const GpuMetrics& g = gpus[i];
            SetConsoleColor(cc.title | FOREGROUND_INTENSITY);
            snprintf(tmp, sizeof(tmp), "\n\n  --- GPU %zu: %s ---", i, g.name.c_str());
            out += tmp;
            if (g.loadPercent >= 0.0)
            {
                SetConsoleColor(FOREGROUND_INTENSITY);
                out += "\n    Load       ";
                SetConsoleColor(cc.gpu_load);
                snprintf(tmp, sizeof(tmp), "%5.1f%%", g.loadPercent);
                out += tmp;
            }
            if (g.temperature >= 0.0)
            {
                SetConsoleColor(FOREGROUND_INTENSITY);
                out += "\n    Temp       ";
                SetConsoleColor(TempAttrs(g.temperature));
                snprintf(tmp, sizeof(tmp), "%4.0f C", g.temperature);
                out += tmp;
            }
            if (g.vramTotalBytes > 0)
            {
                double pct = g.vramUsedBytes * 100.0 / g.vramTotalBytes;
                SetConsoleColor(FOREGROUND_INTENSITY);
                out += "\n    VRAM       ";
                SetConsoleColor(cc.gpu_vram);
                snprintf(tmp, sizeof(tmp), "%5.1f%%", pct);
                out += tmp;
                double usedGb = static_cast<double>(g.vramUsedBytes) / (1024.0*1024*1024);
                double totGb  = static_cast<double>(g.vramTotalBytes) / (1024.0*1024*1024);
                SetConsoleColor(cc.dim);
                snprintf(tmp, sizeof(tmp), "  %4.1f GB / %4.1f GB", usedGb, totGb);
                out += tmp;
            }
            if (g.fanPercent >= 0.0)
            {
                SetConsoleColor(FOREGROUND_INTENSITY);
                out += "\n    Fan        ";
                SetConsoleColor(cc.gpu_fan);
                snprintf(tmp, sizeof(tmp), "%5.0f%%", g.fanPercent);
                out += tmp;
            }
            else if (g.fanRpm > 0)
            {
                SetConsoleColor(FOREGROUND_INTENSITY);
                out += "\n    Fan        ";
                SetConsoleColor(cc.gpu_fan);
                snprintf(tmp, sizeof(tmp), "%5d RPM", g.fanRpm);
                out += tmp;
            }
            if (g.powerWatts > 0.0)
            {
                SetConsoleColor(FOREGROUND_INTENSITY);
                out += "\n    Power      ";
                SetConsoleColor(cc.gpu_power);
                snprintf(tmp, sizeof(tmp), "%5.1f W", g.powerWatts);
                out += tmp;
            }
        }
    }

    SetConsoleColor(cc.dim);
    out += "\n  [Q] quit";
    SetConsoleColor(cc.value);
#else // PLATFORM_LINUX — ANSI output
    const ConsoleColors& cc = GetConsoleColors();
    out += cc.bold;
    out += cc.title;
    out += " LoadGraph Console";
    out += cc.reset;
    if (cpu.cpuName[0] != '\0')
    { out += " \x1b[36m"; out += cpu.cpuName; out += cc.reset; } // cyan
    else
        out += " " + cc.dim + " CPU (N/A)" + cc.reset;
    out += " " + cc.dim + "| Uptime: " + cc.value + FormatUptime(cpu.uptimeMs) + cc.reset + "\n";

    char tmp[64];
    // CPU section
    out += "\n" + cc.bold + "  CPU Load     " + cc.cpu_load;
    snprintf(tmp, sizeof(tmp), "%5.1f%%", cpu.loadPercent);
    out += tmp;
    out += cc.reset + "\n";
    out += BuildProgress("  CPU Load", cpu.loadPercent, barCols, cc.cpu_load) + "\n";

    if (cpu.powerWatts > 0.0)
    {
        snprintf(tmp, sizeof(tmp), "%5.1f W / ", cpu.powerWatts);
        out += "  CPU Power    " + cc.value + " " + tmp
             + " W / " + cc.dim + " " + std::to_string((int)cpu.tdpWatts) + " W TDP" + cc.reset + "\n";
    }

    // RAM section
    out += cc.bold + "  RAM Usage    " + cc.ram;
    snprintf(tmp, sizeof(tmp), "%5.1f%%", ram.loadPercent);
    out += tmp;
    out += cc.reset + "\n";
    out += BuildProgress("  RAM Usage", ram.loadPercent, barCols, cc.ram) + "\n";
    double ramUsedGb = static_cast<double>(ram.totalBytes - ram.freeBytes) / (1024.0*1024*1024);
    double ramTotGb  = static_cast<double>(ram.totalBytes) / (1024.0*1024*1024);
    out += cc.dim;
    snprintf(tmp, sizeof(tmp), "           %4.1f GB / %4.1f GB", ramUsedGb, ramTotGb);
    out += tmp;
    out += cc.reset + "\n";

    // GPU section
    if (!gpus.empty())
    {
        out += cc.bold + cc.title + "\n  GPU" + cc.reset;
        for (size_t i = 0; i < gpus.size(); ++i)
        {
            const GpuMetrics& g = gpus[i];
            out += "\n\n  --- GPU " + std::to_string(i) + ": " + g.name + " ---";
            if (g.loadPercent >= 0.0)
            {
                out += "\n" + cc.bold + "    Load       " + cc.gpu_load;
                snprintf(tmp, sizeof(tmp), "%5.1f%%", g.loadPercent);
                out += tmp;
                out += cc.reset + "\n";
                out += BuildProgress("    Load", g.loadPercent, barCols, cc.gpu_load) + "\n";
            }
            if (g.temperature >= 0.0)
            {
                out += "\n" + cc.bold + "    Temp       ";
                // Temperature bar: 0-100 C scale
                double tempPct = std::min(100.0, g.temperature);
                out += TempAnsi(g.temperature);
                snprintf(tmp, sizeof(tmp), "%4.0f C", g.temperature);
                out += tmp;
                out += cc.reset + "\n";
                // Color for temp bar: green < 65, yellow 65-80, red > 80
                std::string tempBarColor = (g.temperature > 80.0) ? "\x1b[31m" :
                                           (g.temperature > 65.0) ? "\x1b[33m" : cc.gpu_temp;
                out += "    ";
                // Build temp bar manually with color
                int tfilled = static_cast<int>(std::round(tempPct / 100.0 * barCols));
                out += '[';
                out += tempBarColor;
                for (int ti = 0; ti < tfilled; ++ti) out += kBarFull;
                out += "\x1b[90m";
                for (int ti = tfilled; ti < barCols; ++ti) out += kBarEmpty;
                out += cc.reset + "]\n";
            }
            if (g.vramTotalBytes > 0)
            {
                double vpct = g.vramUsedBytes * 100.0 / g.vramTotalBytes;
                out += "\n" + cc.bold + "    VRAM       " + cc.gpu_vram;
                snprintf(tmp, sizeof(tmp), "%5.1f%%", vpct);
                out += tmp;
                double usedGb = static_cast<double>(g.vramUsedBytes) / (1024.0*1024*1024);
                double totGb  = static_cast<double>(g.vramTotalBytes) / (1024.0*1024*1024);
                out += cc.reset + "\n";
                out += BuildProgress("    VRAM", vpct, barCols, cc.gpu_vram) + "\n";
                out += cc.dim;
                snprintf(tmp, sizeof(tmp), "           %4.1f GB / %4.1f GB", usedGb, totGb);
                out += tmp;
                out += cc.reset + "\n";
            }
            if (g.fanPercent >= 0.0)
            {
                out += "\n" + cc.bold + "    Fan        " + cc.gpu_fan;
                snprintf(tmp, sizeof(tmp), "%5.0f%%", g.fanPercent);
                out += tmp;
                out += cc.reset + "\n";
                out += BuildProgress("    Fan", g.fanPercent, barCols, cc.gpu_fan) + "\n";
            }
            else if (g.fanRpm > 0)
            {
                out += "\n" + cc.bold + "    Fan        " + cc.gpu_fan;
                snprintf(tmp, sizeof(tmp), "%5d RPM", g.fanRpm);
                out += tmp;
                out += cc.reset + "\n";
            }
            if (g.powerWatts > 0.0)
            {
                double pwrMax = (g.powerLimitWatts > 0.0) ? g.powerLimitWatts : 400.0;
                double pwrPct = (pwrMax > 0.0) ? (g.powerWatts / pwrMax * 100.0) : 0.0;
                out += "\n" + cc.bold + "    Power      " + cc.gpu_power;
                snprintf(tmp, sizeof(tmp), "%5.1f W", g.powerWatts);
                out += tmp;
                out += cc.reset + "\n";
                out += BuildProgress("    Power", pwrPct, barCols, cc.gpu_power) + "\n";
            }
        }
    }

    out += "\n" + cc.dim + "  [Q] quit" + cc.reset + "\n";
#endif
    return out;
}

// ============================================================================
// OutputFrame — writes the built frame to the console in one shot.
// ============================================================================
static void OutputFrame(const std::string& frame)
{
#ifdef PLATFORM_WINDOWS
    HANDLE hOut = GetConsoleOutputHandle();
    if (hOut != INVALID_HANDLE_VALUE)
    {
        DWORD written = 0;
        WriteConsoleA(hOut, frame.c_str(), static_cast<DWORD>(frame.size()), &written, nullptr);
    }
    else
    {
        fputs(frame.c_str(), stdout);
    }
#else
    fputs(frame.c_str(), stdout);
#endif
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
    // Cache console handle early — used by SetConsoleColor() and OutputFrame().
    InitConsoleHandle();
    SetConsoleColor(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
    if (!SetConsoleOutputCP(CP_UTF8))
        fprintf(stderr, "WARN: SetConsoleOutputCP failed err=%lu\n", GetLastError());
    if (!SetConsoleCP(CP_UTF8))
        fprintf(stderr, "WARN: SetConsoleCP failed err=%lu\n", GetLastError());

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

    // Compute bar width from console size — ~60% of available columns, min 20, max 60.
    const int consW = GetConsoleWidth();
    const int barCols = std::min(60, std::max(20, (consW - 18) * 60 / 100));

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

    printf("  config: port=%d effective=%d http=%s log=%s bar_cols=%d console_w=%d\n\n",
           cfg.http_port, effectivePort,
           cfg.http_enabled ? "on" : "off",
           cfg.log_file ? "on"  : "off",
           barCols, consW);
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
            gm.fanRpm         = gd.fanRpm;
            gm.powerWatts     = gd.powerWatts;
            gm.powerLimitWatts = gd.powerLimitWatts;
            gpus.push_back(gm);
        }

        auto now = std::chrono::steady_clock::now();
        if (now - lastRender >= std::chrono::seconds(1))
        {
            std::string frame = BuildFrame(cpu, ram, gpus, barCols);
            OutputFrame(frame);
            lastRender = now;
        }
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
            gm.fanRpm         = gd.fanRpm;
            gm.powerWatts     = gd.powerWatts;
            gm.powerLimitWatts = gd.powerLimitWatts;
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
        {
            std::string frame = BuildFrame(cpu, ram, gpus, barCols);
            OutputFrame(frame);
            lastRender = now;
        }
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
