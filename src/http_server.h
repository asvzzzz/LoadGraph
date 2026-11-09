#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

// Simple HTTP server for serving the LoadGraph web dashboard.
// Starts on the given TCP port in a detached thread.
// No third-party dependencies - uses only Winsock2.

#ifdef PLATFORM_WINDOWS
void StartHttpServer(int port);
#else
inline void StartHttpServer(int) {}
#endif

#endif // HTTP_SERVER_H
