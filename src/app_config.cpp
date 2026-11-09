#include "platform.h"
#include "app_config.h"
#include "logger.h"

#ifdef PLATFORM_WINDOWS
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>
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
            switch (c)
            {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n";  break;
                case '\t': out += "\\t";  break;
                case '\r': out += "\\r";  break;
                default:   out += c;      break;
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

#ifdef PLATFORM_WINDOWS
    // Get current working directory path as std::string.
    static std::string GetCwdPath()
    {
        char buf[4096] = {};
        if (_getcwd(buf, sizeof(buf)) == nullptr) return ".";
        return std::string(buf);
    }

    // Check if a file exists.
    static bool FileExists(const std::string& path)
    {
        struct stat st;
        return stat(path.c_str(), &st) == 0;
    }

    // Read entire file into string. Returns false on failure.
    static bool ReadFile(const std::string& path, std::string& out)
    {
        FILE* f = fopen(path.c_str(), "r");
        if (!f) return false;
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        rewind(f);
        if (sz < 0 || sz > 1024 * 1024) { fclose(f); return false; } // sanity
        out.resize(static_cast<size_t>(sz));
        size_t n = fread(&out[0], 1, static_cast<size_t>(sz), f);
        fclose(f);
        return n == static_cast<size_t>(sz);
    }

    // Write string to file atomically (write to tmp then rename).
    static bool WriteFile(const std::string& path, const std::string& content)
    {
        std::string tmp = path + ".tmp";
        FILE* f = fopen(tmp.c_str(), "w");
        if (!f) return false;
        if (fprintf(f, "%s", content.c_str()) < (int)content.size())
        { fclose(f); remove(tmp.c_str()); return false; }
        fclose(f);
        return rename(tmp.c_str(), path.c_str()) == 0;
    }
#endif // PLATFORM_WINDOWS
} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

AppConfig LoadAppConfig()
{
    AppConfig cfg;

#ifdef PLATFORM_WINDOWS
    std::string cwd   = GetCwdPath();
    std::string path  = cwd + "/loadgraph.json";

    if (!FileExists(path))
    {
        // Create default config file.
        SaveAppConfig(cfg);
        LOGI("Created default loadgraph.json");
        return cfg;
    }

    std::string data;
    if (!ReadFile(path, data))
    {
        LOGE("Failed to read loadgraph.json, using defaults");
        return cfg;
    }

    // Parse top-level object.
    const char* p = data.c_str();
    SkipSpaces(p);
    if (*p != '{') { LOGE("loadgraph.json: expected {"); return cfg; }
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

    LOGI("Config: port=%d enabled=%s", cfg.http_port, cfg.http_enabled ? "on" : "off");
#endif // PLATFORM_WINDOWS

    return cfg;
}

void SaveAppConfig(const AppConfig& cfg)
{
#ifdef PLATFORM_WINDOWS
    std::string cwd  = GetCwdPath();
    std::string path = cwd + "/loadgraph.json";

    std::string out;
    out += "{\n";
    out += "  \"http_port\": "   + EmitJsonNumber((double)cfg.http_port)   + ",\n";
    out += "  \"http_enabled\": " + std::string(cfg.http_enabled ? "true" : "false") + ",\n";
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
    out += "  }\n";
    out += "}\n";

    if (!WriteFile(path, out))
        LOGE("Failed to write loadgraph.json");
    else
        LOGI("Saved loadgraph.json");
#endif // PLATFORM_WINDOWS
}
