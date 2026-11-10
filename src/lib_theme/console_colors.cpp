#include "console_colors.h"
#include "color_utils.h"   // HexToRgb from lib_config
#include <cstdio>          // fflush, snprintf, stdout

namespace
{
    ConsoleColors g_colors{};

#ifdef PLATFORM_WINDOWS
    HANDLE g_hOut = INVALID_HANDLE_VALUE;
#endif
} // namespace

// ---------------------------------------------------------------------------
// Windows: map hex RGB to nearest console attribute WORD.
// ---------------------------------------------------------------------------
#ifdef PLATFORM_WINDOWS
static WORD HexToWord(unsigned int r, unsigned int g, unsigned int b)
{
    bool bright = (r > 0x7F || g > 0x7F || b > 0x7F);
    WORD w = 0;
    if (r & 0x80) w |= FOREGROUND_RED;
    if (g & 0x80) w |= FOREGROUND_GREEN;
    if (b & 0x80) w |= FOREGROUND_BLUE;
    if (bright)   w |= FOREGROUND_INTENSITY;
    return w ? w : (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
}

static WORD ParseColorHex(const char* hex)
{
    const uint32_t rgb = HexToRgb(hex);
    if (rgb == 0xFFFFFFFFu)
        return FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE; // white
    unsigned int r = static_cast<unsigned int>((rgb >> 16) & 0xFF);
    unsigned int g = static_cast<unsigned int>((rgb >> 8)  & 0xFF);
    unsigned int b = static_cast<unsigned int>( rgb        & 0xFF);
    return HexToWord(r, g, b);
}

WORD TempAttrs(double t)
{
    if (t > 80.0)  return FOREGROUND_RED   | FOREGROUND_INTENSITY;
    if (t > 65.0)  return FOREGROUND_RED   | FOREGROUND_GREEN | FOREGROUND_INTENSITY; // yellow
    return FOREGROUND_GREEN | FOREGROUND_INTENSITY;
}

const char* TempAnsi(double /* t */) { return ""; } // N/A on Windows
#else // PLATFORM_LINUX — ANSI escape sequences
static std::string HexToAnsi(unsigned int r, unsigned int g, unsigned int b)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[38;2;%u;%u;%um", r, g, b);
    return std::string(buf);
}

static std::string ParseColorHex(const char* hex)
{
    const uint32_t rgb = HexToRgb(hex);
    if (rgb == 0xFFFFFFFFu)
        return "\x1b[37m"; // fallback white
    unsigned int r = static_cast<unsigned int>((rgb >> 16) & 0xFF);
    unsigned int g = static_cast<unsigned int>((rgb >> 8)  & 0xFF);
    unsigned int b = static_cast<unsigned int>( rgb        & 0xFF);
    return HexToAnsi(r, g, b);
}

const char* TempAnsi(double t)
{
    if (t > 80.0) return "\x1b[31m";   // red
    if (t > 65.0) return "\x1b[33m";   // yellow
    return "\x1b[32m";                 // green
}

#endif

void ApplyColors(const AppConfig& cfg)
{
#ifdef PLATFORM_WINDOWS
    g_colors.title      = ParseColorHex(cfg.gpu_load_color.c_str());     // green-ish title
    g_colors.label      = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
    g_colors.value      = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
    g_colors.dim        = FOREGROUND_BLUE  | FOREGROUND_GREEN;            // cyan/gray dim
    g_colors.cpu_load   = ParseColorHex(cfg.cpu_load_color.c_str());
    g_colors.cpu_temp   = ParseColorHex(cfg.cpu_temp_color.c_str());
    g_colors.cpu_power  = ParseColorHex(cfg.cpu_power_color.c_str());
    g_colors.ram        = ParseColorHex(cfg.ram_color.c_str());
    g_colors.gpu_load   = ParseColorHex(cfg.gpu_load_color.c_str());
    g_colors.gpu_temp   = ParseColorHex(cfg.gpu_temp_color.c_str());
    g_colors.gpu_vram   = ParseColorHex(cfg.gpu_vram_color.c_str());
    g_colors.gpu_fan    = ParseColorHex(cfg.gpu_fan_color.c_str());
    g_colors.gpu_power  = ParseColorHex(cfg.gpu_power_color.c_str());
#else
    g_colors.title      = ParseColorHex(cfg.gpu_load_color.c_str());     // green-ish title
    g_colors.label      = "\x1b[1m";                                      // bold white
    g_colors.value      = "\x1b[0m";                                      // reset
    g_colors.dim        = "\x1b[90m";                                     // gray
    g_colors.cpu_load   = ParseColorHex(cfg.cpu_load_color.c_str());
    g_colors.cpu_temp   = ParseColorHex(cfg.cpu_temp_color.c_str());
    g_colors.cpu_power  = ParseColorHex(cfg.cpu_power_color.c_str());
    g_colors.ram        = ParseColorHex(cfg.ram_color.c_str());
    g_colors.gpu_load   = ParseColorHex(cfg.gpu_load_color.c_str());
    g_colors.gpu_temp   = ParseColorHex(cfg.gpu_temp_color.c_str());
    g_colors.gpu_vram   = ParseColorHex(cfg.gpu_vram_color.c_str());
    g_colors.gpu_fan    = ParseColorHex(cfg.gpu_fan_color.c_str());
    g_colors.gpu_power  = ParseColorHex(cfg.gpu_power_color.c_str());
#endif
}

const ConsoleColors& GetConsoleColors()
{
    return g_colors;
}

#ifdef PLATFORM_WINDOWS
void InitConsoleHandle()
{
    g_hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    // Flush any buffered printf output so the first color change applies cleanly.
    fflush(stdout);
}

HANDLE GetConsoleOutputHandle()
{
    return g_hOut;
}

void SetConsoleColor(WORD attrs)
{
    if (g_hOut == INVALID_HANDLE_VALUE) return;
    // Sync with CRT line-buffering: without this flush, text already emitted
    // before this call would render with the previous color instead of attrs.
    fflush(stdout);
    SetConsoleTextAttribute(g_hOut, attrs);
}
#endif
