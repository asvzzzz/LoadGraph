#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include <map>
#include <string>

// Minimal JSON-like config loader for loadgraph.cfg.
// Stores chart colors, HTTP server port, window position/size and view menu state.
// File is created with defaults if missing.

struct AppConfig
{
    // Chart colors (hex without #).
    std::string cpu_load_color   = "76B900";
    std::string cpu_temp_color   = "00bcd4";
    std::string cpu_power_color  = "ffc107";
    std::string cpu_name_color   = "00bcd4";  // CPU model name in header
    std::string ram_color        = "AEB4E1";
    std::string gpu_load_color   = "e91e63";
    std::string gpu_temp_color   = "00bcd4";
    std::string gpu_vram_color   = "AEB4E1";
    std::string gpu_name_color   = "76B900";  // GPU title labels (green, matches web dashboard)
    std::string gpu_fan_color    = "3154E1";
    std::string gpu_power_color  = "ffc107";
    std::string total_power_color = "ff5722";

    // Background colors (hex without #).
    std::string page_bg_color    = "1a1a2e";
    std::string section_bg_color = "16213e";
    std::string chart_bg_color   = "0f3460";

    int     http_port      = 9115;
    bool    http_enabled   = false;
    bool    log_file       = false; // write loadgraph.log when true
    // Window position and size saved between sessions.
    // (0,0,0,0 means "let OS decide" — default placement).
    int     window_x       = 0;
    int     window_y       = 0;
    int     window_w       = 0;
    int     window_h       = 0;
    // View menu visibility state: key = toggle label, value = visible.
    std::map<std::string, bool> view_menu;
};

// Load config from <cwd>/loadgraph.cfg. Creates file with defaults if missing.
AppConfig LoadAppConfig();

// Serialize current AppConfig back to loadgraph.cfg (call after user edits).
void SaveAppConfig(const AppConfig& cfg);

#endif // APP_CONFIG_H
