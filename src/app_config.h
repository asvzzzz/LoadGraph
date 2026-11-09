#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include <string>

// Minimal JSON-like config loader for loadgraph.json.
// Stores chart colors, HTTP server port and enabled flag.
// File is created with defaults if missing.

struct AppConfig
{
    // Chart colors (hex without #).
    std::string cpu_load_color   = "76B900";
    std::string cpu_temp_color   = "00bcd4";
    std::string cpu_power_color  = "ffc107";
    std::string ram_color        = "AEB4E1";
    std::string gpu_load_color   = "e91e63";
    std::string gpu_temp_color   = "00bcd4";
    std::string gpu_vram_color   = "AEB4E1";
    std::string gpu_fan_color    = "3154E1";
    std::string gpu_power_color  = "ffc107";
    std::string total_power_color = "ff5722";

    // Background colors (hex without #).
    std::string page_bg_color    = "1a1a2e";
    std::string section_bg_color = "16213e";
    std::string chart_bg_color   = "0f3460";

    int     http_port   = 9115;
    bool    http_enabled = false;
};

// Load config from <cwd>/loadgraph.json. Creates file with defaults if missing.
AppConfig LoadAppConfig();

// Serialize current AppConfig back to loadgraph.json (call after user edits).
void SaveAppConfig(const AppConfig& cfg);

#endif // APP_CONFIG_H
