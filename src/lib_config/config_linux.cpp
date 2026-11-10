#include "platform.h"
#ifdef PLATFORM_LINUX
#include <fstream>
#include <filesystem>
#include <system_error>
#include "config.h"
#include "logger.h"

// Linux: window position persisted to ~/.config/loadgraph/position.ini
namespace
{
    static std::string GetPosPath()
    {
        const char* home = getenv("HOME");
        if (!home || !home[0]) return "";
        return std::string(home) + "/.config/loadgraph/position.ini";
    }
}

bool Config::LoadWindowPosition(int& x, int& y)
{
    std::ifstream f(GetPosPath());
    if (!f) return false;
    if (!(f >> x >> y)) return false;
    if (x < -1000 || x > 5000) x = 100;
    if (y < -1000 || y > 5000) y = 100;
    LOGI("Loaded position: (%d,%d)", x, y);
    return true;
}

bool Config::LoadWindowSize(int& w, int& h)
{
    std::ifstream f(GetPosPath());
    if (!f) return false;
    int x = 0, y = 0;
    if (!(f >> x >> y >> w >> h)) return false;
    if (w < 480 || w > 3840) w = 640;
    if (h < 540 || h > 2160) h = 720;
    LOGI("Loaded size: %dx%d", w, h);
    return true;
}

void Config::SaveWindowPosition(int x, int y, int w, int h)
{
    const std::string path = GetPosPath();
    // Ensure parent directory exists.
    std::error_code ec;
    std::filesystem::create_directories(
        std::filesystem::path(path).parent_path(), ec);
    std::ofstream f(path, std::ios::trunc);
    if (!f) { LOGE("Cannot write position to %s", path.c_str()); return; }
    f << x << " " << y << " " << w << " " << h << "\n";
    LOGI("Saved position: (%d,%d) size=%dx%d", x, y, w, h);
}

bool Config::LoadViewMenuState(std::map<std::string, bool>& out)
{
    // Linux: view menu state stored as key=value lines in position.ini after the first line.
    std::ifstream f(GetPosPath());
    if (!f) return false;
    int x = 0, y = 0, w = 0, h = 0;
    if (!(f >> x >> y >> w >> h)) return false;
    std::string line;
    bool any = false;
    while (std::getline(f, line))
    {
        if (line.empty() || line[0] == '#') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        out[key] = (val == "1" || val == "true");
        any = true;
    }
    return any;
}

void Config::SaveViewMenuState(const std::map<std::string, bool>& state)
{
    const std::string path = GetPosPath();
    // Read existing position line first.
    int x = 0, y = 0, w = 0, h = 0;
    std::ifstream f(path);
    if (f) f >> x >> y >> w >> h;
    f.close();
    std::ofstream out(path, std::ios::trunc);
    if (!out) { LOGE("Cannot write position to %s", path.c_str()); return; }
    out << x << " " << y << " " << w << " " << h << "\n";
    for (const auto& kv : state)
        out << kv.first << "=" << (kv.second ? "1" : "0") << "\n";
}

#endif // PLATFORM_LINUX
