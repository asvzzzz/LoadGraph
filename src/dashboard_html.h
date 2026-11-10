#ifndef DASHBOARD_HTML_H
#define DASHBOARD_HTML_H

#include "app_config.h"
#include <string>

// Web dashboard HTML strings and rendering helpers.
// Shared between GUI (http_server.cpp) and console_app — no wxWidgets dependency.

// Convenience: renders the full dashboard with colors substituted from cfg.
std::string RenderDashboard(int port, const AppConfig& cfg);

std::string GetHtmlBody();
std::string RenderHtml(const char* head, const char* body, int port, const AppConfig& cfg);

#endif // DASHBOARD_HTML_H
