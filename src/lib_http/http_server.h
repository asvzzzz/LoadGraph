#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

// Simple cross-platform HTTP server for serving the LoadGraph web dashboard.
// Runs an accept loop in a worker thread; the listening socket is created
// synchronously in StartHttpServer so a port conflict is reported instantly.
// No third-party dependencies - uses only native sockets (Winsock2 on Windows,
// POSIX sockets on Linux). Built into lib_http static library.

#include "app_config.h"
#include <string>

// Starts the HTTP web dashboard server on cfg.http_port.
// All endpoints are protected with HTTP Basic auth using cfg.http_user /
// cfg.http_pass captured at this moment (cfg.http_auth = false or empty
// user = auth disabled); edits to the credentials in loadgraph.cfg apply
// after the next start. When cfg.http_log is set, opens the separate
// access log loadgraph_http.log (client IP:port per request, raw text for
// invalid requests).
// Returns true if the socket was bound and the accept thread is running;
// false on bind/listen failure or invalid port. Idempotent: if the server is
// already running, returns true without restarting it.
bool StartHttpServer(const AppConfig& cfg);

// Stops the server if running: signals the accept loop and joins the thread.
// Blocks up to ~1 second (loop select timeout) plus time to finish sending
// the current response. Safe to call when the server is not running.
void StopHttpServer();

// True while the server accept thread is running.
bool IsHttpServerRunning();

#endif // HTTP_SERVER_H
