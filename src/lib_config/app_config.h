#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include <map>
#include <string>

// Minimal JSON-like config loader for loadgraph.cfg.
// Stores chart colors, HTTP server port/credentials, window position/size
// and view menu state.
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

    // HTTP Basic auth for the web dashboard (RFC 7617). Stored in plaintext
    // in loadgraph.cfg: protect the file via user/profile permissions.
    // Empty http_user disables auth entirely.
    std::string http_user  = "load";
    std::string http_pass  = "graph";

    // Auth on/off switch: false serves the dashboard with no login prompt.
    bool    http_auth      = true;
    // Separate HTTP access log (loadgraph_http.log in the config dir):
    // invalid requests only (no request bytes / 401 / 404) with client
    // IP:port and raw request text. Created lazily on the first event and
    // appended to across restarts, never truncated.
    bool    http_log       = true;
    bool    log_file       = false; // GUI and console (TTY): write loadgraph.jsonl snapshots when true
    bool    superio_fan    = false; // enable Super I/O fan probe via WinRing0
    // One time notice shown the first time the probe is switched on from the
    // View menu. Set to true once the user confirmed it, so the dialog does
    // not repeat on every start.
    bool    superio_warn_ok = false;
    // Window position and size saved between sessions.
    // (0,0,0,0 means "let OS decide" - default placement).
    int     window_x       = 0;
    int     window_y       = 0;
    int     window_w       = 0;
    int     window_h       = 0;
    // View menu visibility state: key = toggle label, value = visible.
    std::map<std::string, bool> view_menu;
};

// Load config from the per-user config directory (same layout on both OS):
//   Windows: %USERPROFILE%\.config\loadgraph\loadgraph.cfg
//   Linux:   $XDG_CONFIG_HOME/loadgraph/loadgraph.cfg (default ~/.config/...)
// The directory is created on demand; the file is created with defaults if missing.
AppConfig LoadAppConfig();

// Serialize current AppConfig back to loadgraph.cfg (call after user edits).
void SaveAppConfig(const AppConfig& cfg);

// Per-user directory that holds loadgraph.cfg and the log files. Created on
// demand. Returns an empty string only when no user directory could be
// determined, in which case callers should fall back to the current directory.
std::string GetAppConfigDir();

#endif // APP_CONFIG_H
