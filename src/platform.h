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

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    out.resize(static_cast<size_t>(len));
    size_t read = fread(&out[0], 1, static_cast<size_t>(len), f);
    fclose(f);
    return read == static_cast<size_t>(len);
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

// Ensure directory exists (Linux only).
#ifdef PLATFORM_LINUX
inline void EnsureDirectoryExists(const std::string& path)
{
    // Simple implementation: try to create with mkdir -p logic.
    // For now just ensure parent dir exists.
    size_t pos = path.find_last_of('/');
    if (pos == std::string::npos) return;
    std::string dir = path.substr(0, pos);
    mkdir(dir.c_str(), 0755);
}
#endif

#endif // PLATFORM_H
