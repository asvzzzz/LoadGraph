#include "platform.h"
#include "app_config.h"
#include "logger.h"

#include <cstring>   // strncmp, strlen, strncpy
#include <cstdlib>   // strtod, atoi
#include <filesystem>
#include <fstream>
#ifdef PLATFORM_WINDOWS
#include <windows.h>
#endif

// ---------------------------------------------------------------------------
// Minimal JSON parser / emitter — no external dependencies.
// Only handles the subset we actually use: strings, numbers, booleans, objects.
// ---------------------------------------------------------------------------

namespace
{
    // Skip whitespace.
    static void SkipSpaces(const char*& p)
    {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
    }

    // Read a JSON string (assumes leading " is already consumed).
    static bool ReadJsonString(const char*& p, std::string& out)
    {
        out.clear();
        if (*p != '"') return false;
        ++p;
        while (*p && *p != '"')
        {
            if (*p == '\\' && *(p + 1))
            {
                ++p;
                switch (*p)
                {
                    case '"':  out += '"';  break;
                    case '\\': out += '\\'; break;
                    case '/':  out += '/';  break;
                    case 'n':  out += '\n'; break;
                    case 't':  out += '\t'; break;
                    case 'r':  out += '\r'; break;
                    default:   out += *p;   break;
                }
            }
            else
            {
                out += *p;
            }
            ++p;
        }
        return (*p == '"') && (++p, true);
    }

    // Read a JSON number (integer or float). Returns false on failure.
    static bool ReadJsonNumber(const char*& p, double& out)
    {
        const char* start = p;
        if (*p == '-') ++p;
        while (*p >= '0' && *p <= '9') ++p;
        if (*p == '.') { ++p; while (*p >= '0' && *p <= '9') ++p; }
        if (*p == 'e' || *p == 'E') { ++p; if (*p == '+' || *p == '-') ++p; while (*p >= '0' && *p <= '9') ++p; }
        if (p == start) return false;
        out = strtod(start, nullptr);
        return true;
    }

    // Read a JSON boolean or null. Returns false on null.
    static bool ReadJsonBool(const char*& p, bool& out)
    {
        if (strncmp(p, "true", 4) == 0)  { p += 4;  out = true;  return true; }
        if (strncmp(p, "false", 5) == 0) { p += 5;  out = false; return true; }
        if (strncmp(p, "null", 4) == 0)  { p += 4;  return false; }
        return false;
    }

    // Emit a JSON string with escaping.
    static std::string EmitJsonString(const std::string& s)
    {
        std::string out = "\"";
        for (char c : s)
        {
            unsigned char uc = static_cast<unsigned char>(c);
            switch (uc)
            {
                case '"':   out += "\\\""; break;
                case '\\':  out += "\\\\"; break;
                case '\b':  out += "\\b";  break;
                case '\f':  out += "\\f";  break;
                case '\n':  out += "\\n";  break;
                case '\r':  out += "\\r";  break;
                case '\t':  out += "\\t";  break;
                default:
                    if (uc < 0x20)
                    { char t[8]; snprintf(t, sizeof(t), "\\u%04x", uc); out += t; }
                    else out += c;
                    break;
            }
        }
        out += '"';
        return out;
    }

    // Emit a JSON number.
    static std::string EmitJsonNumber(double v)
    {
        char buf[64];
        if (v == static_cast<long long>(v))
            snprintf(buf, sizeof(buf), "%.0f", v);
        else
            snprintf(buf, sizeof(buf), "%g", v);
        return buf;
    }

    // Get the directory containing the running executable.
    // Falls back to cwd on failure.
    static std::string GetExeDir()
    {
#ifdef PLATFORM_WINDOWS
        wchar_t path[MAX_PATH] = {};
        DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH);
        if (n > 0 && n < MAX_PATH)
        {
            std::wstring wstr(path);
            size_t pos = wstr.find_last_of(L"\\/");
            if (pos != std::wstring::npos)
                return std::string(wstr.begin(), wstr.begin() + static_cast<std::wstring::iterator::difference_type>(pos));
        }
#endif
        try { return std::filesystem::current_path().string(); }
        catch (...) { return "."; }
    }

    // Read entire file into string. Returns false on failure.
    static bool ReadFile(const std::string& path, std::string& out)
    {
        std::error_code ec;
        auto sz = std::filesystem::file_size(path, ec);
        if (ec || sz == 0 || sz > 1024 * 1024) return false;
        std::ifstream f(path, std::ios::binary);
        if (!f) return false;
        out.resize(static_cast<size_t>(sz));
        f.read(&out[0], static_cast<std::streamsize>(sz));
        return static_cast<size_t>(f.gcount()) == sz;
    }

    // Write string to file atomically (write to tmp then rename).
    static bool WriteFile(const std::string& path, const std::string& content)
    {
        std::error_code ec;
        const std::string tmp = path + ".tmp";
        std::ofstream f(tmp, std::ios::binary);
        if (!f) return false;
        f.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!f.good()) { std::filesystem::remove(tmp, ec); return false; }
        f.close();
        std::filesystem::rename(tmp, path, ec);
        return !ec;
    }
} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

// Return the path to loadgraph.cfg for the current platform.
// Windows: looks in exe directory first (config travels with binary).
static std::string GetConfigPath()
{
#ifdef PLATFORM_WINDOWS
    return GetExeDir() + "/loadgraph.cfg";
#else
    // Linux: use XDG_CONFIG_HOME/loadgraph/loadgraph.cfg
    const char* xdg = getenv("XDG_CONFIG_HOME");
    std::string dir;
    if (xdg && xdg[0])
        dir = xdg;
    else
    {
        const char* home = getenv("HOME");
        if (home && home[0])
            dir = std::string(home) + "/.config";
    }
    if (dir.empty()) return "./loadgraph.cfg";
    std::string path = dir + "/loadgraph/loadgraph.cfg";
    // Ensure directory exists.
    std::error_code ec;
    std::filesystem::create_directories(dir + "/loadgraph", ec);
    return path;
#endif
}

AppConfig LoadAppConfig()
{
    AppConfig cfg;

    const std::string path = GetConfigPath();

    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
    {
        // Create default config file.
        SaveAppConfig(cfg);
        LOGI("Created default loadgraph.cfg");
        return cfg;
    }

    std::string data;
    if (!ReadFile(path, data))
    {
        LOGE("Failed to read loadgraph.cfg, using defaults");
        return cfg;
    }


    // Parse top-level object.
    const char* p = data.c_str();
    SkipSpaces(p);
    if (*p != '{') { LOGE("loadgraph.cfg: expected {"); return cfg; }
    ++p;

    while (*p && *p != '}')
    {
        SkipSpaces(p);
        if (*p == ',') { ++p; continue; }
        if (*p == '}') break;

        std::string key;
        if (!ReadJsonString(p, key)) break;

        SkipSpaces(p);
        if (*p != ':') break;
        ++p;
        SkipSpaces(p);

        // Parse value based on key.
        if (key == "http_port")
        {
            double v = 0.0;
            if (ReadJsonNumber(p, v) && v >= 1 && v <= 65535)
                cfg.http_port = static_cast<int>(v);
        }
        else if (key == "http_enabled")
        {
            bool v = false;
            SkipSpaces(p);
            if (strncmp(p, "true", 4) == 0)       { p += 4;  v = true;  }
            else if (strncmp(p, "false", 5) == 0) { p += 5;  v = false; }
            else if (*p == '"')
            {
                std::string sv;
                if (ReadJsonString(p, sv))
                    v = (sv == "true");
            }
            cfg.http_enabled = v;
        }
        else if (key == "log_file")
        {
            bool v = false;
            SkipSpaces(p);
            if (strncmp(p, "true", 4) == 0)       { p += 4;  v = true;  }
            else if (strncmp(p, "false", 5) == 0) { p += 5;  v = false; }
            else if (*p == '"')
            {
                std::string sv;
                if (ReadJsonString(p, sv))
                    v = (sv == "true");
            }
            cfg.log_file = v;
        }
        else if (key == "window_x")
        {
            double v = 0.0;
            if (ReadJsonNumber(p, v) && v >= -32000 && v <= 32000)
                cfg.window_x = static_cast<int>(v);
        }
        else if (key == "window_y")
        {
            double v = 0.0;
            if (ReadJsonNumber(p, v) && v >= -32000 && v <= 32000)
                cfg.window_y = static_cast<int>(v);
        }
        else if (key == "window_w")
        {
            double v = 0.0;
            if (ReadJsonNumber(p, v) && v >= 0 && v <= 10000)
                cfg.window_w = static_cast<int>(v);
        }
        else if (key == "window_h")
        {
            double v = 0.0;
            if (ReadJsonNumber(p, v) && v >= 0 && v <= 10000)
                cfg.window_h = static_cast<int>(v);
        }
        else if (key == "colors")
        {
            // Parse inner object.
            if (*p != '{') break;
            ++p;
            while (*p && *p != '}')
            {
                SkipSpaces(p);
                if (*p == ',') { ++p; continue; }
                if (*p == '}') break;

                std::string ck;
                if (!ReadJsonString(p, ck)) break;

                SkipSpaces(p);
                if (*p != ':') break;
                ++p;
                SkipSpaces(p);

                std::string cv;
                if (!ReadJsonString(p, cv)) break;

                // Validate: must be 6-hex-char color.
                if (cv.size() == 6)
                {
                    bool ok = true;
                    for (char c : cv)
                    {
                        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
                        { ok = false; break; }
                    }
                    if (!ok) continue;
                }
                else
                { continue; } // skip invalid color

                if      (ck == "cpu_load")     cfg.cpu_load_color     = cv;
                else if (ck == "cpu_temp")     cfg.cpu_temp_color     = cv;
                else if (ck == "cpu_power")    cfg.cpu_power_color    = cv;
                else if (ck == "ram")          cfg.ram_color          = cv;
                else if (ck == "gpu_load")     cfg.gpu_load_color     = cv;
                else if (ck == "gpu_temp")     cfg.gpu_temp_color     = cv;
                else if (ck == "gpu_vram")     cfg.gpu_vram_color     = cv;
                else if (ck == "gpu_fan")      cfg.gpu_fan_color      = cv;
                else if (ck == "gpu_power")    cfg.gpu_power_color    = cv;
                else if (ck == "total_power")  cfg.total_power_color  = cv;
            }
            if (*p == '}') ++p;
        }
        else if (key == "backgrounds")
        {
            if (*p != '{') break;
            ++p;
            while (*p && *p != '}')
            {
                SkipSpaces(p);
                if (*p == ',') { ++p; continue; }
                if (*p == '}') break;

                std::string ck;
                if (!ReadJsonString(p, ck)) break;

                SkipSpaces(p);
                if (*p != ':') break;
                ++p;
                SkipSpaces(p);

                std::string cv;
                if (!ReadJsonString(p, cv)) break;

                if (cv.size() == 6)
                {
                    bool ok = true;
                    for (char c : cv)
                    {
                        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
                        { ok = false; break; }
                    }
                    if (!ok) continue;
                }
                else { continue; }

                if      (ck == "page")       cfg.page_bg_color      = cv;
                else if (ck == "section")    cfg.section_bg_color   = cv;
                else if (ck == "chart")      cfg.chart_bg_color     = cv;
            }
            if (*p == '}') ++p;
        }
        else if (key == "view_menu")
        {
            // Parse view menu visibility state: {"cpu_load":true,"ram":false,...}
            if (*p != '{') break;
            ++p;
            while (*p && *p != '}')
            {
                SkipSpaces(p);
                if (*p == ',') { ++p; continue; }
                if (*p == '}') break;

                std::string ck;
                if (!ReadJsonString(p, ck)) break;

                SkipSpaces(p);
                if (*p != ':') break;
                ++p;
                SkipSpaces(p);

                bool cv = false;
                if (strncmp(p, "true", 4) == 0)       { p += 4;  cv = true; }
                else if (strncmp(p, "false", 5) == 0) { p += 5;  cv = false; }
                else
                {
                    // Skip unknown value and continue.
                    if (*p == '"') { std::string dummy; ReadJsonString(p, dummy); }
                    else if (*p == '{')
                    { int depth = 1; ++p; while (*p && depth > 0) { if (*p == '{') ++depth; else if (*p == '}') --depth; ++p; } }
                    else if (*p == '[')
                    { int depth = 1; ++p; while (*p && depth > 0) { if (*p == '[') ++depth; else if (*p == ']') --depth; ++p; } }
                    else { while (*p && *p != ',' && *p != '}') ++p; }
                    continue;
                }
                cfg.view_menu[ck] = cv;
            }
            if (*p == '}') ++p;
        }
        else
        {
            // Skip unknown value.
            if (*p == '"')
            { std::string dummy; ReadJsonString(p, dummy); }
            else if (*p == '{')
            {
                int depth = 1; ++p;
                while (*p && depth > 0)
                {
                    if (*p == '{') ++depth;
                    else if (*p == '}') --depth;
                    ++p;
                }
            }
            else if (*p == '[')
            {
                int depth = 1; ++p;
                while (*p && depth > 0)
                {
                    if (*p == '[') ++depth;
                    else if (*p == ']') --depth;
                    ++p;
                }
            }
            else
            {
                // Number, bool, or null — skip to next comma or }.
                while (*p && *p != ',' && *p != '}') ++p;
            }
        }
    }

    LOGI("Config: port=%d http=%s log_file=%s win=(%d,%d) sz=%dx%d",
         cfg.http_port, cfg.http_enabled ? "on" : "off",
         cfg.log_file ? "on" : "off",
         cfg.window_x, cfg.window_y, cfg.window_w, cfg.window_h);

    return cfg;
}

void SaveAppConfig(const AppConfig& cfg)
{
    const std::string path = GetConfigPath();

    std::string out;
    out += "{\n";
    out += "  \"http_port\": "   + EmitJsonNumber((double)cfg.http_port)   + ",\n";
    out += "  \"http_enabled\": " + std::string(cfg.http_enabled ? "true" : "false") + ",\n";
    out += "  \"log_file\": "    + std::string(cfg.log_file     ? "true"  : "false") + ",\n";
    out += "  \"window_x\": "    + EmitJsonNumber((double)cfg.window_x)     + ",\n";
    out += "  \"window_y\": "    + EmitJsonNumber((double)cfg.window_y)     + ",\n";
    out += "  \"window_w\": "    + EmitJsonNumber((double)cfg.window_w)     + ",\n";
    out += "  \"window_h\": "    + EmitJsonNumber((double)cfg.window_h)     + ",\n";
    out += "  \"colors\": {\n";
    out += "    \"cpu_load\":     " + EmitJsonString(cfg.cpu_load_color)      + ",\n";
    out += "    \"cpu_temp\":     " + EmitJsonString(cfg.cpu_temp_color)      + ",\n";
    out += "    \"cpu_power\":    " + EmitJsonString(cfg.cpu_power_color)     + ",\n";
    out += "    \"ram\":          " + EmitJsonString(cfg.ram_color)           + ",\n";
    out += "    \"gpu_load\":     " + EmitJsonString(cfg.gpu_load_color)      + ",\n";
    out += "    \"gpu_temp\":     " + EmitJsonString(cfg.gpu_temp_color)      + ",\n";
    out += "    \"gpu_vram\":     " + EmitJsonString(cfg.gpu_vram_color)      + ",\n";
    out += "    \"gpu_fan\":      " + EmitJsonString(cfg.gpu_fan_color)       + ",\n";
    out += "    \"gpu_power\":    " + EmitJsonString(cfg.gpu_power_color)     + ",\n";
    out += "    \"total_power\":  " + EmitJsonString(cfg.total_power_color)   + "\n";
    out += "  },\n";
    out += "  \"backgrounds\": {\n";
    out += "    \"page\":       " + EmitJsonString(cfg.page_bg_color)       + ",\n";
    out += "    \"section\":    " + EmitJsonString(cfg.section_bg_color)    + ",\n";
    out += "    \"chart\":      " + EmitJsonString(cfg.chart_bg_color)      + "\n";
    out += "  },\n";
    // View menu state.
    if (!cfg.view_menu.empty())
    {
        out += "  \"view_menu\": {\n";
        size_t idx = 0;
        for (const auto& kv : cfg.view_menu)
        {
            out += "    \"" + kv.first + "\": " + std::string(kv.second ? "true" : "false");
            if (++idx < cfg.view_menu.size()) out += ",";
            out += "\n";
        }
        out += "  },\n";
    }
    out += "}\n";

    if (!WriteFile(path, out))
        LOGE("Failed to write loadgraph.cfg");
    else
        LOGI("Saved loadgraph.cfg");
}
