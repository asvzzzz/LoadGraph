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
#include <sstream>
#include <vector>

#ifdef PLATFORM_WINDOWS
#include <io.h>
#include <fcntl.h>
#endif
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
static const char* const kBarFull = "\xe2\x96\x88";

// Build a progress bar string. Empty cells are spaces (invisible on dark bg).
// Label right-padded to 14 chars so all bars align at the same column.
static std::string BuildProgress(const char* label, double percent, int cols)
{
    percent = std::max(0.0, std::min(100.0, percent));
    const int filled = static_cast<int>(std::round(percent / 100.0 * cols));
    std::string out;
    out += label;
    for (int i = static_cast<int>(strlen(label)); i < 14; ++i) out += ' ';
    out += '[';
    for (int i = 0; i < filled;   ++i) out += kBarFull;
    // Empty cells: spaces (background already dark, no need for ░).
    for (int i = filled; i < cols; ++i) out += ' ';
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
// OutputFrame — unified ANSI output on both platforms.
// Windows: VT enabled, write frame atomically without full clear to avoid flicker.
// Linux:   clear screen then write.
// ============================================================================
static void OutputFrame(const std::string& frame)
{
#ifdef PLATFORM_WINDOWS
    // Move cursor home and overwrite. No 2J clear = no flicker/growth.
    fputs("\x1b[H", stdout);
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
// BuildFrame — unified ANSI output for both Windows (VT enabled) and Linux.
// ============================================================================
static std::string BuildFrame(const CpuMetrics& cpu, const RamMetrics& ram,
                              const std::vector<GpuMetrics>& gpus, int barCols)
{
    char tmp[64];
    std::string out;

#ifdef PLATFORM_WINDOWS
    // On Windows we use ANSI sequences via VT, so colors are strings.
    // Read colors from config to match Linux behavior.
    static AppConfig cfg = LoadAppConfig();
    const std::string cReset   = "\x1b[0m";
    const std::string cBold    = "\x1b[1m";
    const std::string cDim     = "\x1b[90m";
    const std::string cTitle   = AnsiColor(cfg.cpu_name_color.c_str());
    const std::string cCpuLoad = AnsiColor(cfg.cpu_load_color.c_str());
    const std::string cCpuTemp = AnsiColor(cfg.cpu_temp_color.c_str());
    const std::string cRam     = AnsiColor(cfg.ram_color.c_str());
    const std::string cGpuLoad = AnsiColor(cfg.gpu_load_color.c_str());
    const std::string cGpuVram = AnsiColor(cfg.gpu_vram_color.c_str());
    const std::string cGpuFan  = AnsiColor(cfg.gpu_fan_color.c_str());
    const std::string cGpuPwr  = AnsiColor(cfg.gpu_power_color.c_str());
#else
    const ConsoleColors& cc = GetConsoleColors();
    const std::string cReset   = cc.reset;
    const std::string cBold    = cc.bold;
    const std::string cDim     = cc.dim;
    const std::string cTitle   = cc.title.empty()  ? AnsiColor("e91e63") : cc.title;
    const std::string cCpuLoad = cc.cpu_load.empty()? AnsiColor("76b900") : cc.cpu_load;
    const std::string cCpuTemp = cc.cpu_temp.empty()? AnsiColor("00bcd4") : cc.cpu_temp;
    const std::string cRam     = cc.ram.empty()     ? AnsiColor("aeb4e1") : cc.ram;
    const std::string cGpuLoad = cc.gpu_load.empty()? AnsiColor("e91e63") : cc.gpu_load;
    const std::string cGpuVram = cc.gpu_vram.empty()? AnsiColor("aeb4e1") : cc.gpu_vram;
    const std::string cGpuFan  = cc.gpu_fan.empty() ? AnsiColor("3154e1") : cc.gpu_fan;
    const std::string cGpuPwr  = cc.gpu_power.empty()? AnsiColor("ffc107"): cc.gpu_power;
#endif

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

    // Header — same style as GPU header.
    std::string cpuNameStr;
#ifdef PLATFORM_WINDOWS
    cpuNameStr = cpuNameUtf8.empty() ? "CPU (N/A)" : cpuNameUtf8;
#else
    cpuNameStr = (cpu.cpuName[0] != '\0') ? std::string(cpu.cpuName) : "CPU (N/A)";
#endif
    out += cBold + AnsiColor(cfg.cpu_name_color.c_str()) + " --- CPU0: " + cpuNameStr + " ---" + cReset;
    // HTTP status
    {
        bool httpStarted = false, httpFailed = false;
        GetHttpStatus(httpStarted, httpFailed);
        std::string httpTag;
        if (cfg.http_enabled)
            httpTag = httpFailed ? "HTTP failed" : (httpStarted ? "HTTP Server" : "HTTP starting");
        else
            httpTag = "HTTP offline";
        out += " " + cDim + "Uptime: " + cReset + FormatUptime(cpu.uptimeMs);
        out += " " + cDim + "| " + cReset + (httpFailed ? "\x1b[31m" : "\x1b[32m") + httpTag + cReset;
    }
    out += "\n";

    // CPU Load — bar + value
    {
        const std::string bar = BuildProgress("CPU Load", cpu.loadPercent, barCols);
        out += "\n" + cCpuLoad + bar + " " + cReset;
        snprintf(tmp, sizeof(tmp), "%5.1f%%", cpu.loadPercent);
        out += cCpuLoad + std::string(tmp) + cReset;
    }

    // CPU Power — bar (percent of TDP) + value
    if (cpu.powerWatts > 0.0)
    {
        double pwrPct = (cpu.tdpWatts > 0.0) ? (cpu.powerWatts / cpu.tdpWatts * 100.0) : 0.0;
        const std::string bar = BuildProgress("CPU Power", pwrPct, barCols);
        out += "\n" + AnsiColor(cfg.cpu_power_color.c_str()) + bar + " " + cReset;
        snprintf(tmp, sizeof(tmp), "%5.1f W / %5.0f W TDP", cpu.powerWatts, cpu.tdpWatts);
        out += cDim + std::string(tmp) + cReset;
    }

    // RAM Usage — bar + value
    {
        const std::string bar = BuildProgress("RAM Usage", ram.loadPercent, barCols);
        out += "\n" + cRam + bar + " " + cReset;
        snprintf(tmp, sizeof(tmp), "%5.1f%%", ram.loadPercent);
        out += cRam + std::string(tmp) + cReset;
        double ramUsedGb = static_cast<double>(ram.totalBytes - ram.freeBytes) / (1024.0*1024*1024);
        double ramTotGb  = static_cast<double>(ram.totalBytes) / (1024.0*1024*1024);
        snprintf(tmp, sizeof(tmp), "%4.1f GB / %4.1f GB", ramUsedGb, ramTotGb);
        out += cDim + std::string(tmp) + cReset;
    }

    // GPU section
    if (!gpus.empty())
    {
        for (size_t i = 0; i < gpus.size(); ++i)
        {
            const GpuMetrics& g = gpus[i];
            out += "\n\n" + cBold + AnsiColor(cfg.gpu_name_color.c_str()) + " --- GPU " + std::to_string(i) + ": " + g.name + " ---" + cReset;

            if (g.loadPercent >= 0.0)
            {
                const std::string bar = BuildProgress("Load", g.loadPercent, barCols);
                out += "\n" + cGpuLoad + bar + " " + cReset;
                snprintf(tmp, sizeof(tmp), "%5.1f%%", g.loadPercent);
                out += cGpuLoad + std::string(tmp) + cReset;
            }
            if (g.temperature >= 0.0)
            {
                double tempPct = std::min(100.0, g.temperature);
                const char* tc = (g.temperature > 80.0) ? "\x1b[31m" :
                                 (g.temperature > 65.0) ? "\x1b[33m" : cCpuTemp.c_str();
                const std::string bar = BuildProgress("Temp", tempPct, barCols);
                out += "\n" + std::string(tc) + bar + " " + cReset;
                snprintf(tmp, sizeof(tmp), "%4.0f C", g.temperature);
                out += std::string(tc) + std::string(tmp) + cReset;
            }
            if (g.vramTotalBytes > 0)
            {
                double vpct = g.vramUsedBytes * 100.0 / g.vramTotalBytes;
                const std::string bar = BuildProgress("VRAM", vpct, barCols);
                out += "\n" + cGpuVram + bar + " " + cReset;
                snprintf(tmp, sizeof(tmp), "%5.1f%%", vpct);
                out += cGpuVram + std::string(tmp) + cReset;
                double usedGb = static_cast<double>(g.vramUsedBytes) / (1024.0*1024*1024);
                double totGb  = static_cast<double>(g.vramTotalBytes) / (1024.0*1024*1024);
                snprintf(tmp, sizeof(tmp), "%4.1f GB / %4.1f GB", usedGb, totGb);
                out += cDim + std::string(tmp) + cReset;
            }
            if (g.fanPercent >= 0.0)
            {
                const std::string bar = BuildProgress("Fan", g.fanPercent, barCols);
                out += "\n" + cGpuFan + bar + " " + cReset;
                snprintf(tmp, sizeof(tmp), "%5.0f%%", g.fanPercent);
                out += cGpuFan + std::string(tmp) + cReset;
            }
            else if (g.fanRpm > 0)
            {
                const std::string bar = BuildProgress("Fan", static_cast<double>(g.fanRpm), barCols);
                out += "\n" + cGpuFan + bar + " " + cReset;
                snprintf(tmp, sizeof(tmp), "%5d RPM", g.fanRpm);
                out += cGpuFan + std::string(tmp) + cReset;
            }
            if (g.powerWatts > 0.0)
            {
                double pwrMax = (g.powerLimitWatts > 0.0) ? g.powerLimitWatts : 400.0;
                double pwrPct = (pwrMax > 0.0) ? (g.powerWatts / pwrMax * 100.0) : 0.0;
                const std::string bar = BuildProgress("Power", pwrPct, barCols);
                out += "\n" + cGpuPwr + bar + " " + cReset;
                snprintf(tmp, sizeof(tmp), "%5.1f W", g.powerWatts);
                out += cGpuPwr + std::string(tmp) + cReset;
            }
        }
    }

    out += "\n\n" + cDim + "  [Q] quit" + cReset;
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
    if (!SetConsoleOutputCP(CP_UTF8))
        fprintf(stderr, "WARN: SetConsoleOutputCP failed err=%lu\n", GetLastError());
    if (!SetConsoleCP(CP_UTF8))
        fprintf(stderr, "WARN: SetConsoleCP failed err=%lu\n", GetLastError());

    // Enable VT processing so ANSI escape sequences work with printf/fputs.
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

    // Set console window title with HTTP server info.
    {
        int portToUse = (cliPort >= 0) ? cliPort : cfg.http_port;
        bool httpActive = cfg.http_enabled || cliPort >= 0;
        char title[256];
        if (httpActive)
            snprintf(title, sizeof(title), "LoadGraphTTY - HTTP server listening on port %d", portToUse);
        else
            snprintf(title, sizeof(title), "LoadGraphTTY");
        SetConsoleTitleA(title);
    }
#else
    ApplyColors(cfg);
#endif

    const int consW = GetConsoleWidth();
    // Bar width: ~60% of available columns, min 20, max 60.
    const int barCols = std::min(60, std::max(20, (consW - 18) * 60 / 100));

    static FILE* s_logFp = nullptr;
    if (cfg.log_file)
    {
        char logPath[1024] = "loadgraph.log";
#ifdef PLATFORM_WINDOWS
        DWORD len = GetCurrentDirectoryA(sizeof(logPath), logPath);
        if (len > 0 && len < sizeof(logPath))
            strncat_s(logPath, sizeof(logPath), "\\loadgraph.log", 14);
        else
            strncpy_s(logPath, sizeof(logPath), "loadgraph.log", _TRUNCATE);
        fopen_s(&s_logFp, logPath, "a");
#else
        s_logFp = fopen("loadgraph.log", "a");
#endif
        if (s_logFp)
        {
            fprintf(s_logFp, "console: logging enabled file=%s\n", logPath);
            fflush(s_logFp);
        }
    }

    bool httpActive = cfg.http_enabled || cliPort >= 0;

    MetricsInit();
    if (httpActive)
    {
        StartHttpServer(cfg);
    }

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
