// Console-only monitoring tool (htop-like text UI).
// Zero wxWidgets dependency - uses Windows API + NVML directly.
// Usage: LoadGraphTTY [-port N]
//   -port N : start HTTP server on given port, serve web dashboard

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifdef PLATFORM_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <ctime>
#include <thread>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#ifdef PLATFORM_LINUX
#include <unistd.h>
#include <sys/select.h>
#include <sys/time.h>
#endif

#include "platform.h"
#include "app_config.h"
#include "power_table.h"
#include "cpu_monitor.h"
#include "ram_monitor.h"
#include "gpu_monitor.h"
#include "metrics_api.h"
#include "http_server.h"
#include "console_colors.h"

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
// Progress bar — plain text, no colors. Label is right-padded to 12 chars
// so all bars start at the same column. Value appended after the bar.
// ============================================================================
static const char* const kBarEmpty = "\xe2\x96\x91";
static const char* const kBarFull  = "\xe2\x96\x88";

static std::string BuildProgress(const char* label, double percent, int cols)
{
    percent = std::max(0.0, std::min(100.0, percent));
    const int filled = static_cast<int>(std::round(percent / 100.0 * cols));
    std::string out;
    // Right-pad label to 14 chars so CPU and GPU bars align.
    out += label;
    for (int i = static_cast<int>(strlen(label)); i < 14; ++i) out += ' ';
    out += '[';
    for (int i = 0; i < filled;   ++i) out += kBarFull;
    for (int i = filled; i < cols; ++i) out += kBarEmpty;
    out += ']';
    return out;
}

// ============================================================================
// Console width
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
// OutputFrame — clears screen then writes frame atomically.
// Windows: Win32 API clear + WriteConsoleA (VT enabled for ANSI colors).
// Linux:   ANSI \x1b[H\x1b[2J + fputs.
// ============================================================================
static void OutputFrame(const std::string& frame)
{
#ifdef PLATFORM_WINDOWS
    // WriteConsoleA does NOT process ANSI sequences even with VT enabled.
    // Use fputs(stdout) so the console driver processes \x1b[H\x1b[2J etc.
    fputs("\x1b[H\x1b[2J", stdout);
    fputs(frame.c_str(), stdout);
#else
    fputs("\x1b[H\x1b[2J", stdout);
    fputs(frame.c_str(), stdout);
#endif
    fflush(stdout);
}

// Build a 24-bit ANSI foreground sequence from a hex color string (no '#').
static std::string AnsiColor(const char* hex)
{
    unsigned r = 0, g = 0, b = 0;
    if (hex && strlen(hex) >= 6)
        sscanf(hex, "%2x%2x%2x", &r, &g, &b);
    return std::string("\x1b[38;2;") + std::to_string(r) + ";" +
           std::to_string(g) + ";" + std::to_string(b) + "m";
}

// ============================================================================
// BuildFrame — assembles the entire UI into a plain-text string with ANSI colors.
// Works on both Windows (with VT enabled) and Linux via unified color strings.
// ============================================================================
static std::string BuildFrame(const CpuMetrics& cpu, const RamMetrics& ram,
                              const std::vector<GpuMetrics>& gpus, int barCols)
{
    char tmp[64];
    std::string out;

#ifdef PLATFORM_WINDOWS
    // Convert wchar_t CPU name to UTF-8 string.
    std::string cpuNameUtf8;
    if (cpu.cpuName[0] != '\0')
    {
        int len = WideCharToMultiByte(CP_UTF8, 0, cpu.cpuName, -1, nullptr, 0, nullptr, nullptr);
        if (len > 1)
        {
            cpuNameUtf8.resize(len - 1);
            WideCharToMultiByte(CP_UTF8, 0, cpu.cpuName, -1, &cpuNameUtf8[0], len, nullptr, nullptr);
        }
    }
#endif

    // Unified ANSI palette — built directly from hex values, works on both platforms.
    const std::string cReset   = "\x1b[0m";
    const std::string cBold    = "\x1b[1m";
    const std::string cDim     = "\x1b[90m";
    const std::string cTitle   = AnsiColor("e91e63");   // pink  (GPU titles)
    const std::string cCpuLoad = AnsiColor("76b900");   // green (CPU load)
    const std::string cCpuTemp = AnsiColor("00bcd4");   // cyan  (GPU temp default)
    const std::string cRam     = AnsiColor("aeb4e1");   // light blue
    const std::string cGpuLoad = AnsiColor("e91e63");   // pink
    const std::string cGpuVram = AnsiColor("aeb4e1");   // light blue
    const std::string cGpuFan  = AnsiColor("3154e1");   // blue
    const std::string cGpuPwr  = AnsiColor("ffc107");   // yellow

#ifdef PLATFORM_WINDOWS
    out += cTitle + " LoadGraph Console" + cReset;
    if (!cpuNameUtf8.empty())
        out += " \x1b[36m" + cpuNameUtf8 + cReset;
    else
        out += " " + cDim + " CPU (N/A)" + cReset;
    out += " " + cDim + "| Uptime: " + cReset + FormatUptime(cpu.uptimeMs) + "\n";
#else
    out += cBold + cTitle + " LoadGraph Console" + cReset;
    if (cpu.cpuName[0] != '\0')
        out += " \x1b[36m" + std::string(cpu.cpuName) + cReset;
    else
        out += " " + cDim + " CPU (N/A)" + cReset;
    out += " " + cDim + "| Uptime: " + cReset + FormatUptime(cpu.uptimeMs) + "\n";
#endif

    // CPU Load — bar + value on same line
    {
        const std::string bar = BuildProgress("CPU Load", cpu.loadPercent, barCols);
        out += "\n" + cCpuLoad + bar + cReset;
        snprintf(tmp, sizeof(tmp), " %5.1f%%", cpu.loadPercent);
        out += cCpuLoad + std::string(tmp) + cReset + "\n";
    }

    // CPU Power — bar (percent of TDP) + value on same line
    if (cpu.powerWatts > 0.0)
    {
        double pwrPct = (cpu.tdpWatts > 0.0) ? (cpu.powerWatts / cpu.tdpWatts * 100.0) : 0.0;
        const std::string bar = BuildProgress("CPU Power", pwrPct, barCols);
        out += "\n" + cCpuLoad + bar + cReset;
        snprintf(tmp, sizeof(tmp), " %5.1f W / %5.0f W TDP", cpu.powerWatts, cpu.tdpWatts);
        out += cDim + std::string(tmp) + cReset + "\n";
    }

    // RAM Usage — bar + value on same line
    {
        const std::string bar = BuildProgress("RAM Usage", ram.loadPercent, barCols);
        out += "\n" + cRam + bar + cReset;
        snprintf(tmp, sizeof(tmp), " %5.1f%%", ram.loadPercent);
        out += cRam + std::string(tmp) + cReset + "\n";
        double ramUsedGb = static_cast<double>(ram.totalBytes - ram.freeBytes) / (1024.0*1024*1024);
        double ramTotGb  = static_cast<double>(ram.totalBytes) / (1024.0*1024*1024);
        snprintf(tmp, sizeof(tmp), "           %4.1f GB / %4.1f GB", ramUsedGb, ramTotGb);
        out += cDim + std::string(tmp) + cReset + "\n";
    }

    // GPU section
    if (!gpus.empty())
    {
        out += "\n" + cBold + cTitle + "  GPU" + cReset;
        for (size_t i = 0; i < gpus.size(); ++i)
        {
            const GpuMetrics& g = gpus[i];
            out += "\n\n  --- GPU " + std::to_string(i) + ": " + g.name + " ---";

            if (g.loadPercent >= 0.0)
            {
                const std::string bar = BuildProgress("Load", g.loadPercent, barCols);
                out += "\n" + cGpuLoad + bar + cReset;
                snprintf(tmp, sizeof(tmp), " %5.1f%%", g.loadPercent);
                out += cGpuLoad + std::string(tmp) + cReset;
            }
            if (g.temperature >= 0.0)
            {
                double tempPct = std::min(100.0, g.temperature);
                const char* tc = (g.temperature > 80.0) ? "\x1b[31m" :
                                 (g.temperature > 65.0) ? "\x1b[33m" : cCpuTemp.c_str();
                out += "\n    Temp      ";
                out += tc;
                int tFilled = static_cast<int>(std::round(tempPct / 100.0 * barCols));
                if (tFilled < 0) tFilled = 0;
                if (tFilled > barCols) tFilled = barCols;
                out += "[";
                for (int ti = 0; ti < tFilled; ++ti) out += kBarFull;
                out += "\x1b[90m";
                for (int ti = tFilled; ti < barCols; ++ti) out += kBarEmpty;
                out += cReset + "]";
                snprintf(tmp, sizeof(tmp), " %4.0f C", g.temperature);
                out += tc + std::string(tmp) + cReset;
            }
            if (g.vramTotalBytes > 0)
            {
                double vpct = g.vramUsedBytes * 100.0 / g.vramTotalBytes;
                const std::string bar = BuildProgress("VRAM", vpct, barCols);
                out += "\n" + cGpuVram + bar + cReset;
                snprintf(tmp, sizeof(tmp), " %5.1f%%", vpct);
                out += cGpuVram + std::string(tmp) + cReset;
                double usedGb = static_cast<double>(g.vramUsedBytes) / (1024.0*1024*1024);
                double totGb  = static_cast<double>(g.vramTotalBytes) / (1024.0*1024*1024);
                snprintf(tmp, sizeof(tmp), " %4.1f GB / %4.1f GB", usedGb, totGb);
                out += cDim + std::string(tmp) + cReset;
            }
            if (g.fanPercent >= 0.0)
            {
                const std::string bar = BuildProgress("Fan", g.fanPercent, barCols);
                out += "\n" + cGpuFan + bar + cReset;
                snprintf(tmp, sizeof(tmp), " %5.0f%%", g.fanPercent);
                out += cGpuFan + std::string(tmp) + cReset;
            }
            else if (g.fanRpm > 0)
            {
                out += "\n    Fan        ";
                out += cGpuFan;
                snprintf(tmp, sizeof(tmp), "%5d RPM", g.fanRpm);
                out += std::string(tmp) + cReset;
            }
            if (g.powerWatts > 0.0)
            {
                double pwrMax = (g.powerLimitWatts > 0.0) ? g.powerLimitWatts : 400.0;
                double pwrPct = (pwrMax > 0.0) ? (g.powerWatts / pwrMax * 100.0) : 0.0;
                const std::string bar = BuildProgress("Power", pwrPct, barCols);
                out += "\n" + cGpuPwr + bar + cReset;
                snprintf(tmp, sizeof(tmp), " %5.1f W", g.powerWatts);
                out += cGpuPwr + std::string(tmp) + cReset;
            }
        }
    }

    out += "\n" + cDim + "  [Q] quit" + cReset + "\n";
    return out;
}

// ============================================================================
// Quit-key polling
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
    AppConfig cfg = LoadAppConfig();

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
    InitConsoleHandle();
    SetConsoleColor(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
    if (!SetConsoleOutputCP(CP_UTF8))
        fprintf(stderr, "WARN: SetConsoleOutputCP failed err=%lu\n", GetLastError());
    if (!SetConsoleCP(CP_UTF8))
        fprintf(stderr, "WARN: SetConsoleCP failed err=%lu\n", GetLastError());

    // Enable VT processing so ANSI escape sequences work with WriteConsoleA / printf.
    {
        HANDLE hOut = GetConsoleOutputHandle();
        if (hOut != INVALID_HANDLE_VALUE)
        {
            DWORD mode = 0;
            if (GetConsoleMode(hOut, &mode))
            {
                mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
                SetConsoleMode(hOut, mode);
            }
        }
    }

    ApplyColors(cfg);
    // VT is already enabled above; use ANSI sequences for all output.
    printf("\x1b[1;38;2;233;30;99mLoadGraph Console - system monitoring\x1b[0m\n\n");
    printf("  Press Q to quit. Updates every 1 second.\n");
#else
    ApplyColors(cfg);
    const ConsoleColors& cc = GetConsoleColors();
    printf("%sLoadGraph Console%s - system monitoring\n\n"
           "  Press Q to quit. Updates every 1 second.%s\n",
           cc.bold.c_str(), cc.title.c_str(), cc.reset.c_str());
#endif

    const int consW = GetConsoleWidth();
    // Bar width: ~60% of available columns, min 20, max 60.
    const int barCols = std::min(60, std::max(20, (consW - 18) * 60 / 100));

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

    int effectivePort = (cliPort >= 0) ? cliPort : cfg.http_port;
    bool httpActive = cfg.http_enabled || cliPort >= 0;

    printf("  config: port=%d effective=%d http=%s log=%s bar_cols=%d console_w=%d\n\n",
           cfg.http_port, effectivePort,
           cfg.http_enabled ? "on" : "off",
           cfg.log_file ? "on"  : "off",
           barCols, consW);
    fflush(stdout);

    MetricsInit();
    if (httpActive)
        StartHttpServer(cfg);

    auto lastRender = std::chrono::steady_clock::now();

#ifdef PLATFORM_WINDOWS
    while (!PollQuitKey())
    {
        CpuData  cpuData{};
        MetricsGetCpu(cpuData);
        RamData  ramData{};
        MetricsGetRam(ramData);
        const auto& gpuDatas = MetricsGetGpus();

        CpuMetrics cpu{};
        wcsncpy(cpu.cpuName, cpuData.cpuName.c_str(), sizeof(cpu.cpuName) / sizeof(wchar_t) - 1);
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
            gm.name            = gd.name;
            gm.loadPercent     = gd.loadPercent;
            gm.temperature     = gd.temperature;
            gm.vramTotalBytes  = gd.vramTotalBytes;
            gm.vramUsedBytes   = gd.vramUsedBytes;
            gm.fanPercent      = gd.fanPercent;
            gm.fanRpm          = gd.fanRpm;
            gm.powerWatts      = gd.powerWatts;
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
        const auto& gpuDatas = MetricsGetGpus();

        CpuMetrics cpu{};
        strncpy(cpu.cpuName, cpuData.cpuName.c_str(), sizeof(cpu.cpuName) - 1);
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
            gm.name            = gd.name;
            gm.loadPercent     = gd.loadPercent;
            gm.temperature     = gd.temperature;
            gm.vramTotalBytes  = gd.vramTotalBytes;
            gm.vramUsedBytes   = gd.vramUsedBytes;
            gm.fanPercent      = gd.fanPercent;
            gm.fanRpm          = gd.fanRpm;
            gm.powerWatts      = gd.powerWatts;
            gm.powerLimitWatts = gd.powerLimitWatts;
            gpus.push_back(gm);
        }

        // Non-blocking stdin check for 'q' or 'Q'.
        {
            fd_set rf; FD_ZERO(&rf); FD_SET(STDIN_FILENO, &rf);
            timeval tv{}; tv.tv_sec = 0; tv.tv_usec = 80000;
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
    printf("\x1b[90m\n  Goodbye.\x1b[0m\n");
#else
    const ConsoleColors& ccc = GetConsoleColors();
    printf("\n%s  Goodbye.%s\n", ccc.dim.c_str(), ccc.reset.c_str());
#endif

    if (s_logFp) { fclose(s_logFp); s_logFp = nullptr; }
    return 0;
}
