#include "platform.h"
#include "logger.h"

#ifdef PLATFORM_LINUX

#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <errno.h>

namespace
{
    // Simple JSON-like parser (no external dependencies).
    // Extracts integer value for a given key from a simple config file.
    bool ParseIntValue(const char* content, const char* key, int& out)
    {
        std::string search = "\"";
        search += key;
        search += "\"";

        size_t pos = strstr(content, search.c_str()) - content;
        if (pos == std::string::npos) return false;

        // Find the colon after the key.
        pos = content.find(':', pos);
        if (pos == std::string::npos) return false;

        // Skip whitespace and find the number.
        size_t numStart = content.find_first_of("0123456789-", pos + 1);
        if (numStart == std::string::npos) return false;

        try {
            out = std::stoi(content.substr(numStart));
            return true;
        } catch (...) {
            return false;
        }
    }
}

bool LoadWindowPosition(int& x, int& y)
{
    std::string path = GetAppConfigPath("position.json");
    if (path.empty()) return false;

    std::string content;
    if (!ReadFileToString(path.c_str(), content))
        return false;

    int w = 0, h = 0;
    bool hasX = ParseIntValue(content.c_str(), "window_x", x);
    bool hasY = ParseIntValue(content.c_str(), "window_y", y);
    bool hasW = ParseIntValue(content.c_str(), "window_w", w);
    bool hasH = ParseIntValue(content.c_str(), "window_h", h);

    // Validate range.
    if (x < -1000 || x > 5000) x = 100;
    if (y < -1000 || y > 5000) y = 100;
    if (w < 480 || w > 3840) w = 640;
    if (h < 400 || h > 2160) h = 720;

    LOGI("Loaded position: (%d,%d) size=%dx%d", x, y, w, h);
    return hasX && hasY;
}

bool LoadWindowSize(int& w, int& h)
{
    std::string path = GetAppConfigPath("position.json");
    if (path.empty()) return false;

    std::string content;
    if (!ReadFileToString(path.c_str(), content))
        return false;

    int x = 0, y = 0;
    bool hasX = ParseIntValue(content.c_str(), "window_x", x);
    bool hasY = ParseIntValue(content.c_str(), "window_y", y);
    bool hasW = ParseIntValue(content.c_str(), "window_w", w);
    bool hasH = ParseIntValue(content.c_str(), "window_h", h);

    // Validate range.
    if (w < 480 || w > 3840) w = 640;
    if (h < 400 || h > 2160) h = 720;

    return hasW && hasH;
}

void SaveWindowPosition(int x, int y, int w, int h)
{
    std::string path = GetAppConfigPath("position.json");
    if (path.empty()) return;

    EnsureDirectoryExists(path);

    std::ofstream f(path.c_str());
    if (!f.is_open())
    {
        LOGE("Failed to save position to %s", path.c_str());
        return;
    }

    f << "{\n";
    f << "    \"window_x\": " << x << ",\n";
    f << "    \"window_y\": " << y << ",\n";
    f << "    \"window_w\": " << w << ",\n";
    f << "    \"window_h\": " << h << "\n";
    f << "}\n";

    LOGI("Saved position: (%d,%d) size=%dx%d", x, y, w, h);
}

#endif // PLATFORM_LINUX
