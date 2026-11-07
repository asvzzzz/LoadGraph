#ifndef PLATFORM_H
#define PLATFORM_H

#include <string>
#include <cstdio>

// Platform detection macros.
#ifdef __linux__
    #define PLATFORM_LINUX 1
    #define PLATFORM_NAME "Linux"
#elif defined(_WIN32)
    #define PLATFORM_WINDOWS 1
    #define PLATFORM_NAME "Windows"
#else
    #error "Unsupported platform"
#endif

// Common types for cross-platform compatibility.
#ifdef PLATFORM_WINDOWS
    // Suppress windows.h warnings about min/max macros.
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
#endif

#ifdef PLATFORM_LINUX
    #include <unistd.h>
    #include <fcntl.h>
    #include <string.h>
    #include <sys/stat.h>
    #include <errno.h>
#endif

// Platform-specific path separator.
#ifdef PLATFORM_WINDOWS
    #define PATH_SEP '\\'
    #define PATH_SEP_STR "\\"
#else
    #define PATH_SEP '/'
    #define PATH_SEP_STR "/"
#endif

// Helper to read an entire text file into a string.
// Returns true on success, false if the file cannot be opened.
inline bool ReadFileToString(const char* path, std::string& out)
{
    out.clear();
#ifdef PLATFORM_WINDOWS
    FILE* f = nullptr;
    fopen_s(&f, path, "r");
    if (!f) return false;
#else
    FILE* f = fopen(path, "r");
    if (!f) return false;
#endif

    // Read until EOF — works correctly for virtual filesystems like /proc.
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        out.append(buf, n);
    fclose(f);
    return !out.empty();
}

// Helper to get the XDG config directory on Linux.
// On Windows returns empty string (uses registry instead).
inline std::string GetConfigDir()
{
#ifdef PLATFORM_LINUX
    const char* xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && xdg[0]) return xdg;
    const char* home = getenv("HOME");
    if (home && home[0]) return std::string(home) + PATH_SEP_STR + ".config";
    return "";
#else
    return "";
#endif
}

// Helper to get the app config path.
inline std::string GetAppConfigPath(const char* filename)
{
    std::string dir = GetConfigDir();
    if (dir.empty()) return "";
    return dir + PATH_SEP_STR + "loadgraph" + PATH_SEP_STR + filename;
}

// Ensure directory exists (Linux only). Recursively creates parent dirs.
#ifdef PLATFORM_LINUX
#include <sys/stat.h>
inline void EnsureDirectoryExists(const std::string& path)
{
    // Walk up the path creating each missing component, like mkdir -p.
    std::string cur;
    for (size_t i = 0; i <= path.size(); ++i)
    {
        if (i == path.size() || path[i] == '/')
        {
            if (!cur.empty())
            {
                mkdir(cur.c_str(), 0755);
                cur += '/';
            }
        }
        else
        {
            cur += path[i];
        }
    }
}
#endif

#endif // PLATFORM_H
