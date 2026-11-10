#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include "app_config.h"

// Simple HTTP server for serving the LoadGraph web dashboard.
// Starts on the configured TCP port in a detached thread.
// No third-party dependencies - uses only Winsock2.

#ifdef PLATFORM_WINDOWS
void StartHttpServer(const AppConfig& cfg);
#else
inline void StartHttpServer(const AppConfig&) {}
#endif

// HTML rendering helpers — exposed so console_app.cpp can reuse them.
extern const char* kHtmlPageHead;
std::string GetHtmlBody();
std::string RenderHtml(const char* head, const char* body, int port, const AppConfig& cfg);

#endif // HTTP_SERVER_H
