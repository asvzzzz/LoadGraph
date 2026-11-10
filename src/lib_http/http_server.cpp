#include "http_server.h"
#include "dashboard_html.h"
#include "ico_favicon.h"
#include "metrics_api.h"
#include "platform.h"
#include <cmath>   // std::isfinite

#ifdef PLATFORM_WINDOWS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <thread>
#pragma comment(lib, "ws2_32.lib")
#else // PLATFORM_LINUX
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <thread>
#endif

// ============================================================================
// Cross-platform helpers (pure C++, no platform APIs).
// ============================================================================

static std::string JsonEscape(const char* s)
{
    std::string r;
    r.reserve(strlen(s) + 8);
    for (const unsigned char* p = (const unsigned char*)s; *p; ++p)
    {
        switch (*p)
        {
            case '"':   r += "\\\""; break;
            case '\\':  r += "\\\\"; break;
            case '\b':  r += "\\b";  break;
            case '\f':  r += "\\f";  break;
            case '\n':  r += "\\n";  break;
            case '\r':  r += "\\r";  break;
            case '\t':  r += "\\t"; break;
            default:
                if (*p < 0x20)
                { char t[8]; snprintf(t, sizeof(t), "\\u%04x", *p); r += t; }
                else r += (char)*p;
        }
    }
    return r;
}

static double SafeDouble(double v) { return std::isfinite(v) ? v : 0.0; }

#ifdef PLATFORM_WINDOWS
// Convert a wchar_t string (UTF-16 on Windows) to UTF-8.
static std::string WcharToUtf8(const std::wstring& w)
{
    if (w.empty()) return "";
    int bytes = WideCharToMultiByte(CP_UTF8, 0, w.c_str(),
                                     static_cast<int>(w.size()),
                                     nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return "";
    std::string out;
    out.resize(static_cast<size_t>(bytes));
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(),
                        static_cast<int>(w.size()),
                        &out[0], bytes, nullptr, nullptr);
    return out;
}
#endif

// ============================================================================
// JSON metrics builder — calls MetricsGet* API (thread-safe).
// ============================================================================
static std::string BuildJson()
{
    const CpuData  cpu  = MetricsGetCpu();
    const RamData  ram  = MetricsGetRam();
    const auto&    gpus = MetricsGetGpus();

#ifdef PLATFORM_WINDOWS
    std::string cpuNameUtf8 = JsonEscape(WcharToUtf8(cpu.cpuName).c_str());
#else
    std::string cpuNameUtf8 = JsonEscape(cpu.cpuName.c_str());
#endif

    std::string out;
    out.reserve(1024 + gpus.size() * 256);

    char buf[512];
    snprintf(buf, sizeof(buf),
        "{\"cpu_load\":%.1f,\"cpu_temp\":%.1f,\"cpu_power\":%.1f,"
         "\"fan_rpm\":%d,\"tdp_watts\":%.1f,"
         "\"ram_load\":%.1f,\"ram_power\":%.1f,"
         "\"ram_used_bytes\":%llu,\"ram_total_bytes\":%llu,"
         "\"uptime_ms\":%llu,\"cpu_name\":\"",
        SafeDouble(cpu.loadPercent),
        SafeDouble(cpu.temperature),
        SafeDouble(cpu.powerWatts),
        cpu.fanRpm,
        SafeDouble(cpu.tdpWatts),
        SafeDouble(ram.loadPercent),
        SafeDouble(ram.powerWatts),
        (unsigned long long)(ram.totalBytes > ram.freeBytes
                             ? ram.totalBytes - ram.freeBytes : 0),
        (unsigned long long)ram.totalBytes,
        (unsigned long long)cpu.uptimeMs);
    out += buf;
    out += cpuNameUtf8;
    out += "\",\"gpus\":[";

    for (size_t i = 0; i < gpus.size(); ++i)
    {
        const GpuData& g = gpus[i];
        if (i > 0) out += ',';
        out += "{\"name\":\"";
        out += JsonEscape(g.name.c_str());
        snprintf(buf, sizeof(buf),
            "\",\"load_percent\":%.1f,\"temperature\":%.1f,"
             "\"vram_used_bytes\":%llu,\"vram_total_bytes\":%llu,"
             "\"fan_rpm\":%d,\"fan_percent\":%.1f,"
             "\"power_watts\":%.1f,\"power_limit_watts\":%.1f}",
            SafeDouble(g.loadPercent), SafeDouble(g.temperature),
            (unsigned long long)g.vramUsedBytes,
            (unsigned long long)g.vramTotalBytes,
            g.fanRpm, SafeDouble(g.fanPercent),
            SafeDouble(g.powerWatts), SafeDouble(g.powerLimitWatts));
        out += buf;
    }

    // Storage array — separate from gpus.
    out += "],\"storage\":[";
    auto storage = MetricsGetStorage();
    for (size_t i = 0; i < storage.size(); ++i)
    {
        if (i > 0) out += ',';
        const StorageData& sd = storage[i];
        out += "{\"name\":\"";
        out += JsonEscape(sd.name.c_str());
        out += "\",\"interface\":\"";
        out += JsonEscape(sd.iface.c_str());
        snprintf(buf, sizeof(buf),
            "\",\"total_bytes\":%llu,\"power_watts\":%.1f}",
            (unsigned long long)sd.totalBytes, SafeDouble(sd.powerWatts));
        out += buf;
    }

    out += "]}";
    return out;
}

// ============================================================================
// Socket helpers — platform-specific accept/send loop.
// ============================================================================

#ifdef PLATFORM_WINDOWS
static void HttpLoop(int port, const AppConfig& cfg)
{
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2,2), &wsa) != 0)
    { fprintf(stderr, "HTTP: WSAStartup failed\n"); return; }

    SOCKET srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (srv == INVALID_SOCKET)
    { fprintf(stderr, "HTTP: socket() failed err=%d\n", WSAGetLastError()); WSACleanup(); return; }

    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR,   (const char*)&opt, sizeof(opt));
    setsockopt(srv, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(static_cast<unsigned short>(port));

    if (bind(srv, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR)
    {
        fprintf(stderr, "HTTP: bind() failed port=%d err=%d\n", port, WSAGetLastError());
        closesocket(srv); WSACleanup(); return;
    }
    if (listen(srv, 4) == SOCKET_ERROR)
    {
        fprintf(stderr, "HTTP: listen() failed err=%d\n", WSAGetLastError());
        closesocket(srv); WSACleanup(); return;
    }

    printf("HTTP server listening on port %d\nOpen http://localhost:%d in your browser\n", port, port);
    fflush(stdout);

    DWORD rcvTimeout = 5000;
    DWORD sndTimeout = 5000;

    while (true)
    {
        fd_set rf; FD_ZERO(&rf); FD_SET(srv, &rf);
        timeval tv{}; tv.tv_sec = 1; tv.tv_usec = 0;
        if (select(0, &rf, nullptr, nullptr, &tv) <= 0 || !FD_ISSET(srv, &rf)) continue;

        SOCKET client = accept(srv, nullptr, nullptr);
        if (client == INVALID_SOCKET) continue;

        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, (const char*)&rcvTimeout, sizeof(rcvTimeout));
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, (const char*)&sndTimeout, sizeof(sndTimeout));

        char req[4096] = {};
        int n = recv(client, req, sizeof(req) - 1, 0);
        if (n <= 0) { closesocket(client); continue; }
        req[n] = '\0';

        char method[8] = {}, path[512] = {};
        sscanf(req, "%7s %511s", method, path);
        bool isMetrics   = (strcmp(method, "GET") == 0 && strcmp(path, "/api/metrics") == 0);
        bool isIndex     = (strcmp(method, "GET") == 0 && strcmp(path, "/") == 0);
        bool isFavicon   = (strcmp(method, "GET") == 0 && strcmp(path, "/favicon.ico") == 0);

        std::string body;
        if (isMetrics)       body = BuildJson();
        else if (isIndex)    body = RenderDashboard(port, cfg);
        else if (isFavicon)
        {
            // Build the full header in one string to avoid partial-send over-read.
            char hdr[128];
            int hlen = snprintf(hdr, sizeof(hdr),
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: image/x-icon\r\n"
                "Connection: close\r\n"
                "Content-Length: %u\r\n\r\n",
                (unsigned int)kLoadGraphIcoSize);
            // Send header — if client disconnected mid-header, stop immediately.
            int sent = 0;
            while (sent < hlen)
            { int sn = send(client, hdr + sent, hlen - sent, 0); if (sn <= 0) break; sent += sn; }
            if (sent < hlen) { closesocket(client); continue; }
            // Send body.
            const unsigned char* p = kLoadGraphIcoBytes;
            size_t rem = kLoadGraphIcoSize;
            while (rem > 0)
            {
                int chunk = (int)(rem > 4096 ? 4096 : rem);
                int sn = send(client, (const char*)p, chunk, 0);
                if (sn <= 0) break;
                p += sn; rem -= sn;
            }
            closesocket(client);
            continue;
        }
        else
        {
            const char* notFound = "HTTP/1.1 404 Not Found\r\n"
                                   "Content-Type: text/plain\r\n"
                                   "Connection: close\r\n\r\nNot Found";
            int total = (int)strlen(notFound), sent = 0;
            while (sent < total)
            { int sn = send(client, notFound + sent, total - sent, 0); if (sn <= 0) break; sent += sn; }
            closesocket(client);
            continue;
        }

        char hdr[512] = {};
        int hlen = snprintf(hdr, sizeof(hdr),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: %s\r\n"
            "Connection: close\r\n"
            "Content-Length: %u\r\n"
            "\r\n",
            isMetrics ? "application/json" : "text/html; charset=utf-8",
            (unsigned int)body.size());

        int total = hlen, sent = 0;
        while (sent < total)
        { int sn = send(client, hdr + sent, total - sent, 0); if (sn <= 0) break; sent += sn; }
        total = (int)body.size(); sent = 0;
        while (sent < total)
        { int sn = send(client, body.c_str() + sent, total - sent, 0); if (sn <= 0) break; sent += sn; }

        closesocket(client);
    }
    closesocket(srv); WSACleanup();
}

void StartHttpServer(const AppConfig& cfg)
{
    std::thread([cfg] { HttpLoop(cfg.http_port, cfg); }).detach();
}

#else // PLATFORM_LINUX — POSIX sockets
static void HttpLoop(int port, const AppConfig& cfg)
{
    int srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (srv < 0) { fprintf(stderr, "HTTP: socket() failed\n"); return; }

    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(static_cast<unsigned short>(port));

    if (bind(srv, (sockaddr*)&addr, sizeof(addr)) < 0)
    { fprintf(stderr, "HTTP: bind() failed port=%d\n", port); close(srv); return; }
    if (listen(srv, 4) < 0)
    { fprintf(stderr, "HTTP: listen() failed\n"); close(srv); return; }

    printf("HTTP server listening on port %d\nOpen http://localhost:%d in your browser\n", port, port);
    fflush(stdout);

    while (true)
    {
        fd_set rf; FD_ZERO(&rf); FD_SET(srv, &rf);
        timeval tv{}; tv.tv_sec = 1; tv.tv_usec = 0;
        if (select(srv + 1, &rf, nullptr, nullptr, &tv) <= 0 || !FD_ISSET(srv, &rf)) continue;

        int client = accept(srv, nullptr, nullptr);
        if (client < 0) continue;

        char req[4096] = {};
        int n = recv(client, req, sizeof(req) - 1, 0);
        if (n <= 0) { close(client); continue; }
        req[n] = '\0';

        char method[8] = {}, path[512] = {};
        sscanf(req, "%7s %511s", method, path);
        bool isMetrics   = (strcmp(method, "GET") == 0 && strcmp(path, "/api/metrics") == 0);
        bool isIndex     = (strcmp(method, "GET") == 0 && strcmp(path, "/") == 0);
        bool isFavicon   = (strcmp(method, "GET") == 0 && strcmp(path, "/favicon.ico") == 0);

        std::string body;
        if (isMetrics)       body = BuildJson();
        else if (isIndex)    body = RenderDashboard(port, cfg);
        else if (isFavicon)
        {
            char hdr[128];
            int hlen = snprintf(hdr, sizeof(hdr),
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: image/x-icon\r\n"
                "Connection: close\r\n"
                "Content-Length: %u\r\n\r\n",
                (unsigned int)kLoadGraphIcoSize);
            int sent = 0;
            while (sent < hlen)
            { int sn = send(client, hdr + sent, hlen - sent, 0); if (sn <= 0) break; sent += sn; }
            if (sent < hlen) { close(client); continue; }
            const unsigned char* p = kLoadGraphIcoBytes;
            size_t rem = kLoadGraphIcoSize;
            while (rem > 0)
            {
                int chunk = (int)(rem > 4096 ? 4096 : rem);
                int sn = send(client, (const char*)p, chunk, 0);
                if (sn <= 0) break;
                p += sn; rem -= sn;
            }
            close(client);
            continue;
        }
        else
        {
            const char* notFound = "HTTP/1.1 404 Not Found\r\n"
                                   "Content-Type: text/plain\r\n"
                                   "Connection: close\r\n\r\nNot Found";
            int total = (int)strlen(notFound), sent = 0;
            while (sent < total)
            { int sn = send(client, notFound + sent, total - sent, 0); if (sn <= 0) break; sent += sn; }
            close(client);
            continue;
        }

        char hdr[512] = {};
        int hlen = snprintf(hdr, sizeof(hdr),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: %s\r\n"
            "Connection: close\r\n"
            "Content-Length: %u\r\n"
            "\r\n",
            isMetrics ? "application/json" : "text/html; charset=utf-8",
            (unsigned int)body.size());

        int total = hlen, sent = 0;
        while (sent < total)
        { int sn = send(client, hdr + sent, total - sent, 0); if (sn <= 0) break; sent += sn; }
        total = (int)body.size(); sent = 0;
        while (sent < total)
        { int sn = send(client, body.c_str() + sent, total - sent, 0); if (sn <= 0) break; sent += sn; }

        close(client);
    }
    close(srv);
}

void StartHttpServer(const AppConfig& cfg)
{
    std::thread([cfg] { HttpLoop(cfg.http_port, cfg); }).detach();
}
#endif // PLATFORM_WINDOWS / PLATFORM_LINUX
