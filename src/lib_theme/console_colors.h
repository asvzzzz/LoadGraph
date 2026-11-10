#ifndef CONSOLE_COLORS_H
#define CONSOLE_COLORS_H

// Cross-platform terminal color management for LoadGraphTTY.
// Windows: WORD console attributes (SetConsoleTextAttribute).
// Linux:   ANSI 24-bit escape sequences (\x1b[38;2;r;g;b m).
//
// Usage:
//   ApplyColors(cfg);           // call once at startup with AppConfig
//   SetColor(g_colors.cpu_load); // apply color before printf on Windows
//   printf("%s", g_colors.ram);  // print escape sequence on Linux

#include "platform.h"   // defines PLATFORM_WINDOWS / PLATFORM_LINUX before any guards below
#include "app_config.h"
#include <cstdint>
#include <string>

#ifdef PLATFORM_WINDOWS
#include <windows.h>
using ConsoleColor = WORD;
#else
using ConsoleColor = std::string;   // ANSI escape sequence per cell on Linux
#endif

struct ConsoleColors
{
    // Palette entries — always populated by ApplyColors().
    // = {} zero-initializes WORD to 0 and default-constructs std::string to "".
    ConsoleColor title       = {};
    ConsoleColor label       = {};
    ConsoleColor value       = {};
    ConsoleColor dim         = {};
    ConsoleColor cpu_load    = {};
    ConsoleColor cpu_temp    = {};
    ConsoleColor cpu_power   = {};
    ConsoleColor ram         = {};
    ConsoleColor gpu_load    = {};
    ConsoleColor gpu_temp    = {};
    ConsoleColor gpu_vram    = {};
    ConsoleColor gpu_fan     = {};
    ConsoleColor gpu_power   = {};

#ifdef PLATFORM_LINUX
    // Linux-only helpers stored inline.
    std::string reset    = "\x1b[0m";
    std::string bold     = "\x1b[1m";
#endif
};

// Parse cfg and fill g_colors with platform-appropriate values.
// Safe to call multiple times (e.g. after config reload).
void ApplyColors(const AppConfig& cfg);

// Get a pointer to the singleton color state (for DrawWave, Render, etc.).
const ConsoleColors& GetConsoleColors();

#ifdef PLATFORM_WINDOWS
// Cache the console output handle (call once at startup, before any coloring).
void InitConsoleHandle();
// Return the cached console output handle (for Render() screen-clearing).
HANDLE GetConsoleOutputHandle();
// Set console text attribute. Flushes stdout first to sync with CRT buffering.
void SetConsoleColor(WORD attrs);
#endif

// Return ANSI escape for temperature thresholds (Linux).
const char* TempAnsi(double t);

#ifdef PLATFORM_WINDOWS
// Return WORD console attribute for temperature thresholds.
WORD TempAttrs(double t);
#endif

#endif // CONSOLE_COLORS_H
