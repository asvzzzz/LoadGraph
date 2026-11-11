#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

// Simple cross-platform HTTP server for serving the LoadGraph web dashboard.
// Starts on the configured TCP port in a detached thread.
// No third-party dependencies — uses only native sockets (Winsock2 on Windows,
// POSIX sockets on Linux). Built into lib_http static library.

#include "app_config.h"
#include <string>

// Starts the HTTP web dashboard server on cfg.http_port in a detached thread.
// No-op on non-Windows platforms (Linux build stub is in http_server.cpp).
void StartHttpServer(const AppConfig& cfg);

// Returns current HTTP server status. started=true if server bound successfully, failed=true if bind failed.
bool GetHttpStatus(bool& started, bool& failed);

#endif // HTTP_SERVER_H
