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
// Progress bar — plain text, no colors. Caller adds color via console API.
// Returns the bar string like "  Label[████░░░░]".
// ============================================================================
static const char* const kBarEmpty  = "\xe2\x96\x91";
static const char* const kBarFull   = "\xe2\x96\x88";

static std::string BuildProgress(const char* label, double percent, int cols)
{
    percent = std::max(0.0, std::min(100.0, percent));
    const int filled = static_cast<int>(std::round(percent / 100.0 * cols));
    std::string out;
    out += label;
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
// Clear visible console area (Windows only). Linux uses ANSI in OutputFrame.
// ============================================================================
#ifdef PLATFORM_WINDOWS
static void ClearScreen()
{
    HANDLE hOut = GetConsoleOutputHandle();
    if (hOut == INVALID_HANDLE_VALUE) return;
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (!GetConsoleScreenBufferInfo(hOut, &info)) return;

    COORD zero = { 0, 0 };
    DWORD written = 0;
    const DWORD cols = static_cast<DWORD>(info.srWindow.Right - info.srWindow.Left + 1);
    const DWORD rows = static_cast<DWORD>(info.srWindow.Bottom - info.srWindow.Top + 1);
    const DWORD cells = cols * rows;

    // Reset attributes to dim white.
    FillConsoleOutputAttribute(hOut, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY,
                               cells, zero, &written);
    // Clear characters.
    FillConsoleOutputCharacterA(hOut, ' ', cells, zero, &written);
    SetConsoleCursorPosition(hOut, zero);
}
#endif

// ============================================================================
// OutputFrame — writes the built frame to the console.
// Windows: clears screen first, then WriteConsoleA (one atomic write).
// Linux:   ANSI escape sequences for cursor home + clear embedded in string.
// ============================================================================
#ifdef PLATFORM_WINDOWS
static void OutputFrame(const std::string& frame)
{
    ClearScreen();
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
}
#else
static void OutputFrame(const std::string& frame)
{
    // ANSI: move cursor to home (0,0), clear screen.
    fputs("\x1b[H\x1b[2J", stdout);
    fputs(frame.c_str(), stdout);
}
#endif

// ============================================================================
// BuildFrame — assembles the entire UI into a plain-text string (no colors).
// Colors are applied at output time via console API calls.
// ============================================================================
static std::string BuildFrame(const CpuMetrics& cpu, const RamMetrics& ram,
                              const std::vector<GpuMetrics>& gpus, int barCols)
{
    char tmp[64];
    std::string out;

#ifdef PLATFORM_WINDOWS
    const ConsoleColors& cc = GetConsoleColors();

    // Header — colors applied via SetConsoleColor before each printf-like append.
    // Since we're building a string, we can't use SetConsoleColor here.
    // Instead we embed simple markers that OutputFrame will handle... 
    // Actually: on Windows we output directly with color calls, not via string.
    // This function is only used for Linux.
    (void)cc; (void)barCols;
#else // PLATFORM_LINUX
    const ConsoleColors& cc = GetConsoleColors();

    out += cc.bold; out += cc.title; out += " LoadGraph Console"; out += cc.reset;
    if (cpu.cpuName[0] != '\0')
    { out += " \x1b[36m"; out += cpu.cpuName; out += cc.reset; }
    else
        out += " " + cc.dim + " CPU (N/A)" + cc.reset;
    out += " " + cc.dim + "| Uptime: " + cc.value + FormatUptime(cpu.uptimeMs) + cc.reset + "\n";

    // CPU section
    out += "\n" + cc.bold + "  CPU Load     " + cc.cpu_load;
    snprintf(tmp, sizeof(tmp), "%5.1f%%", cpu.loadPercent);
    out += tmp;
    out += cc.reset + "\n";
    out += BuildProgress("  CPU Load", cpu.loadPercent, barCols) + "\n";

    if (cpu.powerWatts > 0.0)
    {
        snprintf(tmp, sizeof(tmp), "%5.1f W", cpu.powerWatts);
        out += "  CPU Power    " + cc.value + " " + tmp
             + " / " + cc.dim + " " + std::to_string((int)cpu.tdpWatts) + " W TDP" + cc.reset + "\n";
    }

    // RAM section
    out += cc.bold + "  RAM Usage    " + cc.ram;
    snprintf(tmp, sizeof(tmp), "%5.1f%%", ram.loadPercent);
    out += tmp;
    out += cc.reset + "\n";
    out += BuildProgress("  RAM Usage", ram.loadPercent, barCols) + "\n";
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
                out += BuildProgress("    Load", g.loadPercent, barCols) + "\n";
            }
            if (g.temperature >= 0.0)
            {
                out += "\n" + cc.bold + "    Temp       ";
                double tempPct = std::min(100.0, g.temperature);
                const char* tempColor = (g.temperature > 80.0) ? "\x1b[31m" :
                                        (g.temperature > 65.0) ? "\x1b[33m" : cc.gpu_temp.c_str();
                out += tempColor;
                snprintf(tmp, sizeof(tmp), "%4.0f C", g.temperature);
                out += tmp;
                out += cc.reset + "\n";
                // Temp bar with color
                int tfilled = static_cast<int>(std::round(tempPct / 100.0 * barCols));
                out += "    [";
                out += tempColor;
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
                out += cc.reset + "\n";
                out += BuildProgress("    VRAM", vpct, barCols) + "\n";
                double usedGb = static_cast<double>(g.vramUsedBytes) / (1024.0*1024*1024);
                double totGb  = static_cast<double>(g.vramTotalBytes) / (1024.0*1024*1024);
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
                out += BuildProgress("    Fan", g.fanPercent, barCols) + "\n";
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
                out += BuildProgress("    Power", pwrPct, barCols) + "\n";
            }
        }
    }

    out += "\n" + cc.dim + "  [Q] quit" + cc.reset + "\n";
#endif
    return out;
}

// ============================================================================
// RenderWin — direct Windows console output with color (no string buffering).
// Clears screen first, then writes each line with proper colors.
// ============================================================================
#ifdef PLATFORM_WINDOWS
static void RenderWin(const CpuMetrics& cpu, const RamMetrics& ram,
                      const std::vector<GpuMetrics>& gpus, int barCols)
{
    ClearScreen();
    const ConsoleColors& cc = GetConsoleColors();

    // Header
    SetConsoleColor(cc.title | FOREGROUND_INTENSITY);
    printf(" LoadGraph Console");
    if (cpu.cpuName[0] != '\0')
    {
        SetConsoleColor(FOREGROUND_BLUE | FOREGROUND_GREEN);
        int len = WideCharToMultiByte(CP_UTF8, 0, cpu.cpuName, -1, nullptr, 0, nullptr, nullptr);
        if (len > 1)
        {
            std::string wstr; wstr.resize(len - 1);
            WideCharToMultiByte(CP_UTF8, 0, cpu.cpuName, -1, &wstr[0], len, nullptr, nullptr);
            printf(" %s", wstr.c_str());
        }
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

    // CPU section
    SetConsoleColor(FOREGROUND_INTENSITY);
    printf("\n  CPU Load     ");
    SetConsoleColor(cc.cpu_load);
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "%5.1f%%", cpu.loadPercent);
    printf("%s\n", tmp);
    // Bar: plain text, color applied via console attribute on the bar chars
    {
        const std::string bar = BuildProgress("  CPU Load", cpu.loadPercent, barCols);
        SetConsoleColor(cc.cpu_load);
        fputs(bar.c_str(), stdout);
        printf("\n");
    }

    if (cpu.powerWatts > 0.0)
    {
        SetConsoleColor(cc.value);
        snprintf(tmp, sizeof(tmp), "%5.1f W", cpu.powerWatts);
        printf("  CPU Power    %s / ", tmp);
        SetConsoleColor(cc.dim);
        snprintf(tmp, sizeof(tmp), "%5.0f W TDP", cpu.tdpWatts);
        printf("%s\n", tmp);
    }

    // RAM section
    SetConsoleColor(FOREGROUND_INTENSITY);
    printf("\n  RAM Usage    ");
    SetConsoleColor(cc.ram);
    snprintf(tmp, sizeof(tmp), "%5.1f%%", ram.loadPercent);
    printf("%s\n", tmp);
    {
        const std::string bar = BuildProgress("  RAM Usage", ram.loadPercent, barCols);
        SetConsoleColor(cc.ram);
        fputs(bar.c_str(), stdout);
        printf("\n");
    }
    SetConsoleColor(cc.dim);
    double ramUsedGb = static_cast<double>(ram.totalBytes - ram.freeBytes) / (1024.0*1024*1024);
    double ramTotGb  = static_cast<double>(ram.totalBytes) / (1024.0*1024*1024);
    snprintf(tmp, sizeof(tmp), "           %4.1f GB / %4.1f GB", ramUsedGb, ramTotGb);
    printf("%s\n", tmp);

    // GPU section
    if (!gpus.empty())
    {
        SetConsoleColor(cc.title | FOREGROUND_INTENSITY);
        printf("\n  GPU");
        for (size_t i = 0; i < gpus.size(); ++i)
        {
            const GpuMetrics& g = gpus[i];
            SetConsoleColor(cc.title | FOREGROUND_INTENSITY);
            snprintf(tmp, sizeof(tmp), "\n\n  --- GPU %zu: ", i);
            printf("%s", tmp);
            // CPU-name-style wide char output
            int len = WideCharToMultiByte(CP_UTF8, 0, g.name.empty() ? L"" : nullptr, 0, nullptr, 0, nullptr, nullptr);
            // name is already std::string (UTF-8), just print it
            printf("%s---\n", g.name.c_str());

            if (g.loadPercent >= 0.0)
            {
                SetConsoleColor(FOREGROUND_INTENSITY);
                printf("    Load       ");
                SetConsoleColor(cc.gpu_load);
                snprintf(tmp, sizeof(tmp), "%5.1f%%", g.loadPercent);
                printf("%s\n", tmp);
                const std::string bar = BuildProgress("    Load", g.loadPercent, barCols);
                SetConsoleColor(cc.gpu_load);
                fputs(bar.c_str(), stdout);
                printf("\n");
            }
            if (g.temperature >= 0.0)
            {
                SetConsoleColor(FOREGROUND_INTENSITY);
                printf("    Temp       ");
                SetConsoleColor(TempAttrs(g.temperature));
                snprintf(tmp, sizeof(tmp), "%4.0f C", g.temperature);
                printf("%s\n", tmp);
                // Temp bar
                double tempPct = std::min(100.0, g.temperature);
                int tfilled = static_cast<int>(std::round(tempPct / 100.0 * barCols));
                SetConsoleColor(TempAttrs(g.temperature));
                printf("    [");
                for (int ti = 0; ti < tfilled; ++ti) fputs(kBarFull, stdout);
                SetConsoleColor(cc.dim);
                for (int ti = tfilled; ti < barCols; ++ti) fputs(kBarEmpty, stdout);
                printf("]\n");
            }
            if (g.vramTotalBytes > 0)
            {
                double vpct = g.vramUsedBytes * 100.0 / g.vramTotalBytes;
                SetConsoleColor(FOREGROUND_INTENSITY);
                printf("    VRAM       ");
                SetConsoleColor(cc.gpu_vram);
                snprintf(tmp, sizeof(tmp), "%5.1f%%", vpct);
                printf("%s\n", tmp);
                const std::string bar = BuildProgress("    VRAM", vpct, barCols);
                SetConsoleColor(cc.gpu_vram);
                fputs(bar.c_str(), stdout);
                printf("\n");
                double usedGb = static_cast<double>(g.vramUsedBytes) / (1024.0*1024*1024);
                double totGb  = static_cast<double>(g.vramTotalBytes) / (1024.0*1024*1024);
                SetConsoleColor(cc.dim);
                snprintf(tmp, sizeof(tmp), "           %4.1f GB / %4.1f GB", usedGb, totGb);
                printf("%s\n", tmp);
            }
            if (g.fanPercent >= 0.0)
            {
                SetConsoleColor(FOREGROUND_INTENSITY);
                printf("    Fan        ");
                SetConsoleColor(cc.gpu_fan);
                snprintf(tmp, sizeof(tmp), "%5.0f%%", g.fanPercent);
                printf("%s\n", tmp);
                const std::string bar = BuildProgress("    Fan", g.fanPercent, barCols);
                SetConsoleColor(cc.gpu_fan);
                fputs(bar.c_str(), stdout);
                printf("\n");
            }
            else if (g.fanRpm > 0)
            {
                SetConsoleColor(FOREGROUND_INTENSITY);
                printf("    Fan        ");
                SetConsoleColor(cc.gpu_fan);
                snprintf(tmp, sizeof(tmp), "%5d RPM", g.fanRpm);
                printf("%s\n", tmp);
            }
            if (g.powerWatts > 0.0)
            {
                double pwrMax = (g.powerLimitWatts > 0.0) ? g.powerLimitWatts : 400.0;
                double pwrPct = (pwrMax > 0.0) ? (g.powerWatts / pwrMax * 100.0) : 0.0;
                SetConsoleColor(FOREGROUND_INTENSITY);
                printf("    Power      ");
                SetConsoleColor(cc.gpu_power);
                snprintf(tmp, sizeof(tmp), "%5.1f W", g.powerWatts);
                printf("%s\n", tmp);
                const std::string bar = BuildProgress("    Power", pwrPct, barCols);
                SetConsoleColor(cc.gpu_power);
                fputs(bar.c_str(), stdout);
                printf("\n");
            }
        }
    }

    SetConsoleColor(cc.dim);
    printf("\n  [Q] quit");
    SetConsoleColor(cc.value);
}
#endif // PLATFORM_WINDOWS

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
    ApplyColors(cfg);

    const ConsoleColors& cc = GetConsoleColors();
    SetConsoleColor(cc.title | FOREGROUND_INTENSITY);
    printf("LoadGraph Console - system monitoring\n\n");
    SetConsoleColor(cc.value);
    printf("  Press Q to quit. Updates every 1 second.\n");
#else
    ApplyColors(cfg);
    const ConsoleColors& cc = GetConsoleColors();
    printf("%sLoadGraph Console%s - system monitoring\n\n"
           "  Press Q to quit. Updates every 1 second.%s\n",
           cc.bold.c_str(), cc.title.c_str(), cc.reset.c_str());
#endif

    const int consW = GetConsoleWidth();
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
            RenderWin(cpu, ram, gpus, barCols);
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
