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

#include <atomic>
#include <cstdio>    // fopen, fprintf, fflush for the access log
#include <cstring>   // strlen, strcmp for auth header parsing
#include <ctime>     // time, strftime, localtime_s/_r for log timestamps
#include <mutex>
#include <string>
#include <system_error>

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
// JSON metrics builder - calls MetricsGet* API (thread-safe).
// ============================================================================
static std::string BuildJson()
{
    CpuData  cpu{};
    MetricsGetCpu(cpu);
    RamData  ram{};
    MetricsGetRam(ram);
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

    // Storage array - separate from gpus.
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
// Socket helpers - platform-specific accept/send loop.
// ============================================================================

// ============================================================================
// Shared server state (used by BOTH platform HttpLoop variants).
// ============================================================================

// Stop flag checked by the accept loop every select() timeout (1 s).
static std::atomic<bool> g_httpStopReq{false};
// Server thread; joinable only while the server runs. Guarded by g_httpMtx.
static std::thread g_httpThread;
static std::mutex  g_httpMtx;

// ============================================================================
// HTTP Basic auth (RFC 7617). Credentials come from loadgraph.cfg
// (http_user / http_pass) and are captured once in StartHttpServer();
// edits to the file apply after the next start. Auth is skipped when
// http_auth = false or http_user is empty.
// ============================================================================

// Immutable copy of the configured credentials. Written in StartHttpServer()
// before the listener thread starts, then only read by request handling;
// the mutex makes that hand-off explicit and race-free.
struct AuthSnapshot
{
    bool        enabled = true;  // http_auth config switch
    std::string user;
    std::string pass;
};

static std::mutex   g_authMutex;
static AuthSnapshot g_auth;

static void RefreshAuthSnapshot(const AppConfig& cfg)
{
    std::lock_guard<std::mutex> lk(g_authMutex);
    g_auth.enabled = cfg.http_auth;
    g_auth.user    = cfg.http_user;
    g_auth.pass    = cfg.http_pass;
}

// Base64 decoder; padding is optional. Returns false on any character
// outside the base64 alphabet.
static bool Base64Decode(const std::string& in, std::string& out)
{
    static const char kTable[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    signed char rev[256];
    for (int i = 0; i < 256; ++i) rev[i] = -1;
    for (int i = 0; i < 64; ++i) rev[(unsigned char)kTable[i]] = (signed char)i;

    out.clear();
    int bits = 0, nbits = 0;
    for (unsigned char c : in)
    {
        if (c == '=') break;  // padding ends the payload
        const signed char v = rev[c];
        if (v < 0) return false;
        bits = (bits << 6) | v;
        nbits += 6;
        if (nbits >= 8)
        {
            nbits -= 8;
            out += (char)((bits >> nbits) & 0xFF);
        }
    }
    return true;
}

// Value of the "Authorization" request header (exact name, case-insensitive;
// headers merely containing the word are not matched).
static bool FindAuthHeader(const std::string& req, std::string& value)
{
    size_t lineStart = 0;
    while (lineStart < req.size())
    {
        size_t lineEnd = req.find('\n', lineStart);
        if (lineEnd == std::string::npos) lineEnd = req.size();
        std::string line = req.substr(lineStart, lineEnd - lineStart);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) break;  // end of headers
        const size_t colon = line.find(':');
        if (colon != std::string::npos)
        {
            std::string name = line.substr(0, colon);
            for (char& c : name)
                if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            if (name == "authorization")
            {
                value = line.substr(colon + 1);
                return true;
            }
        }
        lineStart = lineEnd + 1;
    }
    return false;
}

// Parse "<scheme> <base64(user:pass)>" from an Authorization value.
// Only the Basic scheme is accepted (scheme match is case-insensitive).
static bool ExtractBasicCreds(const std::string& value, std::string& userpass)
{
    const size_t s = value.find_first_not_of(" \t");
    if (s == std::string::npos) return false;
    const size_t sp = value.find_first_of(" \t", s);
    if (sp == std::string::npos) return false;
    std::string scheme = value.substr(s, sp - s);
    for (char& c : scheme)
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    if (scheme != "BASIC") return false;

    const size_t b = value.find_first_not_of(" \t", sp);
    const size_t e = value.find_last_not_of(" \t\r\n");
    if (b == std::string::npos || e == std::string::npos || b > e) return false;
    if (!Base64Decode(value.substr(b, e - b + 1), userpass)) return false;
    return userpass.find(':') != std::string::npos;
}

// Length-aware byte comparison without early exit on mismatch.
static bool SecureCompare(const std::string& a, const std::string& b)
{
    unsigned diff = (a.size() != b.size()) ? 1u : 0u;
    const size_t n = a.size() < b.size() ? a.size() : b.size();
    for (size_t i = 0; i < n; ++i)
        diff |= (unsigned)((unsigned char)a[i] ^ (unsigned char)b[i]);
    return diff == 0;
}

// Decide whether the request may proceed. Returns false when the caller
// must send a 401 challenge instead (socket close/send is platform code).
static bool AuthorizeRequest(const std::string& req, const char* path)
{
    AuthSnapshot auth;
    {
        std::lock_guard<std::mutex> lk(g_authMutex);
        auth = g_auth;
    }
    if (!auth.enabled || auth.user.empty())
        return true;

    std::string hdrval, userpass;
    if (FindAuthHeader(req, hdrval) && ExtractBasicCreds(hdrval, userpass))
    {
        const size_t sep = userpass.find(':');
        const std::string user = userpass.substr(0, sep);
        const std::string pass = userpass.substr(sep + 1);
        if (SecureCompare(user, auth.user) && SecureCompare(pass, auth.pass))
            return true;
    }

    fprintf(stderr, "HTTP: auth denied for %s\n", path);
    return false;
}

// ============================================================================
// HTTP access log (loadgraph_http.log, kept separate from loadgraph.log).
// Opened in StartHttpServer() with mode "w" (previous run's contents are
// discarded, same rule as the other per-user files) and written ONLY from
// the server thread, so no locking is needed. Every connection produces
// one line with the client IP:port; invalid requests (no request bytes,
// 401, 404) additionally log the raw request text.
// ============================================================================

static FILE* g_httpLogFile = nullptr;

static void HttpLogOpen(const AppConfig& cfg)
{
    if (!cfg.http_log) return;
    const std::string dir = GetAppConfigDir();
    const std::string path = dir.empty() ? std::string("./loadgraph_http.log")
                                         : dir + "/loadgraph_http.log";
    g_httpLogFile = fopen(path.c_str(), "w");
    if (!g_httpLogFile)
        fprintf(stderr, "HTTP: cannot open %s for logging\n", path.c_str());
}

static void HttpLogClose()
{
    if (!g_httpLogFile) return;
    fflush(g_httpLogFile);
    fclose(g_httpLogFile);
    g_httpLogFile = nullptr;
}

// Replace the Authorization header value with *** before logging, so a
// failed-login trace never stores real credentials (base64 is trivially
// decodable). Header name matched exactly, case-insensitive - same rule
// as FindAuthHeader.
static std::string MaskAuthorization(const std::string& req)
{
    std::string out;
    out.reserve(req.size());
    size_t lineStart = 0;
    while (lineStart < req.size())
    {
        size_t lineEnd = req.find('\n', lineStart);
        if (lineEnd == std::string::npos) lineEnd = req.size();
        std::string line = req.substr(lineStart, lineEnd - lineStart);
        const size_t colon = line.find(':');
        if (colon != std::string::npos)
        {
            std::string name = line.substr(0, colon);
            for (char& c : name)
                if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            if (name == "authorization") line = "Authorization: ***";
        }
        out += line;
        if (lineEnd < req.size()) out += '\n';
        lineStart = lineEnd + 1;
    }
    return out;
}

// Collapse a raw request into one printable ASCII log line: CR/LF/TAB are
// escaped, other control and non-ASCII bytes become \xNN, and the result is
// capped so request floods cannot grow the log without bound.
static std::string EscapeRaw(const std::string& rawReq)
{
    static const size_t kMaxLen = 1024;
    const std::string masked = MaskAuthorization(rawReq);
    std::string r;
    r.reserve(256);
    for (unsigned char c : masked)
    {
        if (r.size() >= kMaxLen) { r += "...[cut]"; break; }
        if      (c == '\r') r += "\\r";
        else if (c == '\n') r += "\\n";
        else if (c == '\t') r += "\\t";
        else if (c < 0x20 || c >= 0x7F)
        { char hex[8]; snprintf(hex, sizeof(hex), "\\x%02x", c); r += hex; }
        else r += (char)c;
    }
    return r;
}

// One line per connection: "HH:MM:SS ip:port METHOD PATH -> CODE".
// rawReq != nullptr appends the sanitized request text (invalid requests).
static void HttpLogAccess(const char* peer, const char* method, const char* path,
                          int status, const std::string* rawReq)
{
    if (!g_httpLogFile) return;
    char ts[16];
    time_t now = time(nullptr);
    struct tm tmNow{};
#ifdef PLATFORM_WINDOWS
    localtime_s(&tmNow, &now);
#else
    localtime_r(&now, &tmNow);
#endif
    if (strftime(ts, sizeof(ts), "%H:%M:%S", &tmNow) == 0) ts[0] = '\0';
    if (rawReq)
        fprintf(g_httpLogFile, "%s %s %s %s -> %d raw: %s\n",
                ts, peer, method, path, status, EscapeRaw(*rawReq).c_str());
    else
        fprintf(g_httpLogFile, "%s %s %s %s -> %d\n",
                ts, peer, method, path, status);
    fflush(g_httpLogFile);
}

// Format the accepted peer as "a.b.c.d:port" (IPv4-only listener).
static void PeerText(const sockaddr_in& peer, char* out, size_t outSz)
{
    char ip[INET_ADDRSTRLEN] = {};
#ifdef PLATFORM_WINDOWS
    InetNtopA(AF_INET, &peer.sin_addr, ip, (DWORD)sizeof(ip));
#else
    inet_ntop(AF_INET, &peer.sin_addr, ip, (socklen_t)sizeof(ip));
#endif
    snprintf(out, outSz, "%s:%u", ip[0] ? ip : "?", (unsigned)ntohs(peer.sin_port));
}

#ifdef PLATFORM_WINDOWS
// Create + bind + listen the server socket on the CALLER thread so bind()
// errors (port in use) are visible to the caller synchronously.
// Returns INVALID_SOCKET on failure; WSACleanup() is done in HttpClose().
static SOCKET HttpOpenListen(int port)
{
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2,2), &wsa) != 0)
    { fprintf(stderr, "HTTP: WSAStartup failed\n"); return INVALID_SOCKET; }

    SOCKET srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (srv == INVALID_SOCKET)
    { fprintf(stderr, "HTTP: socket() failed err=%d\n", WSAGetLastError()); WSACleanup(); return INVALID_SOCKET; }

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
        closesocket(srv); WSACleanup(); return INVALID_SOCKET;
    }
    if (listen(srv, 4) == SOCKET_ERROR)
    {
        fprintf(stderr, "HTTP: listen() failed err=%d\n", WSAGetLastError());
        closesocket(srv); WSACleanup(); return INVALID_SOCKET;
    }
    return srv;
}

// 401 challenge: after the user enters credentials the browser sends the
// Authorization header on every request (including the metrics polls).
static void SendUnauthorized(SOCKET client)
{
    static const char resp[] =
        "HTTP/1.1 401 Unauthorized\r\n"
        "WWW-Authenticate: Basic realm=\"LoadGraph\", charset=\"UTF-8\"\r\n"
        "Content-Type: text/plain\r\n"
        "Connection: close\r\n"
        "Content-Length: 23\r\n\r\n"
        "Authorization required.";
    int total = (int)strlen(resp), sent = 0;
    while (sent < total)
    { int sn = send(client, resp + sent, total - sent, 0); if (sn <= 0) break; sent += sn; }
}

// Accept loop on the already-bound socket; runs on the server thread.
static void HttpServeLoop(SOCKET srv, int port, const AppConfig& cfg)
{
    DWORD rcvTimeout = 5000;
    DWORD sndTimeout = 5000;

    while (!g_httpStopReq.load())
    {
        fd_set rf; FD_ZERO(&rf); FD_SET(srv, &rf);
        timeval tv{}; tv.tv_sec = 1; tv.tv_usec = 0;
        if (select(0, &rf, nullptr, nullptr, &tv) <= 0 || !FD_ISSET(srv, &rf)) continue;

        // Capture the peer address: every connection is logged in the
        // access log, so client IPs stay visible even for failed requests.
        sockaddr_in peer{};
        int peerLen = sizeof(peer);
        SOCKET client = accept(srv, (sockaddr*)&peer, &peerLen);
        if (client == INVALID_SOCKET) continue;
        char peerTxt[40];
        PeerText(peer, peerTxt, sizeof(peerTxt));

        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, (const char*)&rcvTimeout, sizeof(rcvTimeout));
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, (const char*)&sndTimeout, sizeof(sndTimeout));

        char req[4096] = {};
        int n = recv(client, req, sizeof(req) - 1, 0);
        if (n <= 0)
        {
            // Connection with no request bytes (port scan, aborted client):
            // record the IP anyway.
            HttpLogAccess(peerTxt, "-", "-", 0, nullptr);
            closesocket(client);
            continue;
        }
        req[n] = '\0';

        char method[8] = {}, path[512] = {};
        sscanf(req, "%7s %511s", method, path);
        const std::string reqStr(req);  // full text, logged for invalid requests
        bool isMetrics   = (strcmp(method, "GET") == 0 && strcmp(path, "/api/metrics") == 0);
        bool isIndex     = (strcmp(method, "GET") == 0 && strcmp(path, "/") == 0);
        bool isFavicon   = (strcmp(method, "GET") == 0 && strcmp(path, "/favicon.ico") == 0);

        // Basic auth gate for all endpoints (metrics JSON included).
        if (!AuthorizeRequest(reqStr, path))
        {
            HttpLogAccess(peerTxt, method, path, 401, &reqStr);
            SendUnauthorized(client);
            closesocket(client);
            continue;
        }

        std::string body;
        if (isMetrics)       body = BuildJson();
        else if (isIndex)    body = RenderDashboard(port, cfg);
        else if (isFavicon)
        {
            HttpLogAccess(peerTxt, method, path, 200, nullptr);
            // Build the full header in one string to avoid partial-send over-read.
            char hdr[128];
            int hlen = snprintf(hdr, sizeof(hdr),
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: image/x-icon\r\n"
                "Connection: close\r\n"
                "Content-Length: %u\r\n\r\n",
                (unsigned int)kLoadGraphIcoSize);
            // Send header - if client disconnected mid-header, stop immediately.
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
            // Unknown path or method: invalid request, log the raw text.
            HttpLogAccess(peerTxt, method, path, 404, &reqStr);
            const char* notFound = "HTTP/1.1 404 Not Found\r\n"
                                   "Content-Type: text/plain\r\n"
                                   "Connection: close\r\n\r\nNot Found";
            int total = (int)strlen(notFound), sent = 0;
            while (sent < total)
            { int sn = send(client, notFound + sent, total - sent, 0); if (sn <= 0) break; sent += sn; }
            closesocket(client);
            continue;
        }

        // Remaining route: metrics/index answered with 200 below.
        HttpLogAccess(peerTxt, method, path, 200, nullptr);

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
    // srv is closed by the thread wrapper (HttpClose), not here.
}

// Release socket + Winsock state (paired with HttpOpenListen).
static void HttpClose(SOCKET srv)
{
    closesocket(srv);
    WSACleanup();
}

#else // PLATFORM_LINUX - POSIX sockets
// Create + bind + listen the server socket on the CALLER thread (see the
// Windows variant above). Returns -1 on failure; close() is done in HttpClose.
static int HttpOpenListen(int port)
{
    int srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (srv < 0) { fprintf(stderr, "HTTP: socket() failed\n"); return -1; }

    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(static_cast<unsigned short>(port));

    if (bind(srv, (sockaddr*)&addr, sizeof(addr)) < 0)
    { fprintf(stderr, "HTTP: bind() failed port=%d\n", port); close(srv); return -1; }
    if (listen(srv, 4) < 0)
    { fprintf(stderr, "HTTP: listen() failed\n"); close(srv); return -1; }
    return srv;
}

// 401 challenge: after the user enters credentials the browser sends the
// Authorization header on every request (including the metrics polls).
static void SendUnauthorized(int client)
{
    static const char resp[] =
        "HTTP/1.1 401 Unauthorized\r\n"
        "WWW-Authenticate: Basic realm=\"LoadGraph\", charset=\"UTF-8\"\r\n"
        "Content-Type: text/plain\r\n"
        "Connection: close\r\n"
        "Content-Length: 23\r\n\r\n"
        "Authorization required.";
    int total = (int)strlen(resp), sent = 0;
    // MSG_NOSIGNAL: a client that hangs up mid-response must not raise
    // SIGPIPE and kill the whole application.
    while (sent < total)
    { int sn = send(client, resp + sent, total - sent, MSG_NOSIGNAL); if (sn <= 0) break; sent += sn; }
}

// Accept loop on the already-bound socket; runs on the server thread.
static void HttpServeLoop(int srv, int port, const AppConfig& cfg)
{
    while (!g_httpStopReq.load())
    {
        fd_set rf; FD_ZERO(&rf); FD_SET(srv, &rf);
        timeval tv{}; tv.tv_sec = 1; tv.tv_usec = 0;
        if (select(srv + 1, &rf, nullptr, nullptr, &tv) <= 0 || !FD_ISSET(srv, &rf)) continue;

        // Capture the peer address: every connection is logged in the
        // access log, so client IPs stay visible even for failed requests.
        sockaddr_in peer{};
        socklen_t peerLen = sizeof(peer);
        int client = accept(srv, (sockaddr*)&peer, &peerLen);
        if (client < 0) continue;
        char peerTxt[40];
        PeerText(peer, peerTxt, sizeof(peerTxt));

        char req[4096] = {};
        int n = recv(client, req, sizeof(req) - 1, 0);
        if (n <= 0)
        {
            // Connection with no request bytes (port scan, aborted client):
            // record the IP anyway.
            HttpLogAccess(peerTxt, "-", "-", 0, nullptr);
            close(client);
            continue;
        }
        req[n] = '\0';

        char method[8] = {}, path[512] = {};
        sscanf(req, "%7s %511s", method, path);
        const std::string reqStr(req);  // full text, logged for invalid requests
        bool isMetrics   = (strcmp(method, "GET") == 0 && strcmp(path, "/api/metrics") == 0);
        bool isIndex     = (strcmp(method, "GET") == 0 && strcmp(path, "/") == 0);
        bool isFavicon   = (strcmp(method, "GET") == 0 && strcmp(path, "/favicon.ico") == 0);

        // Basic auth gate for all endpoints (metrics JSON included).
        if (!AuthorizeRequest(reqStr, path))
        {
            HttpLogAccess(peerTxt, method, path, 401, &reqStr);
            SendUnauthorized(client);
            close(client);
            continue;
        }

        std::string body;
        if (isMetrics)       body = BuildJson();
        else if (isIndex)    body = RenderDashboard(port, cfg);
        else if (isFavicon)
        {
            HttpLogAccess(peerTxt, method, path, 200, nullptr);
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
            // Unknown path or method: invalid request, log the raw text.
            HttpLogAccess(peerTxt, method, path, 404, &reqStr);
            const char* notFound = "HTTP/1.1 404 Not Found\r\n"
                                   "Content-Type: text/plain\r\n"
                                   "Connection: close\r\n\r\nNot Found";
            int total = (int)strlen(notFound), sent = 0;
            while (sent < total)
            { int sn = send(client, notFound + sent, total - sent, 0); if (sn <= 0) break; sent += sn; }
            close(client);
            continue;
        }

        // Remaining route: metrics/index answered with 200 below.
        HttpLogAccess(peerTxt, method, path, 200, nullptr);

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
    // srv is closed by the thread wrapper (HttpClose), not here.
}

// Release the listening socket (paired with HttpOpenListen).
static void HttpClose(int srv)
{
    close(srv);
}
#endif // PLATFORM_WINDOWS / PLATFORM_LINUX

// ============================================================================
// Public API (common for both platforms).
// ============================================================================

bool StartHttpServer(const AppConfig& cfg)
{
    std::lock_guard<std::mutex> lock(g_httpMtx);

    // Already running - do not rebind, the caller just gets the status.
    if (g_httpThread.joinable())
        return true;

    if (cfg.http_port <= 0 || cfg.http_port > 65535)
    { fprintf(stderr, "HTTP: invalid port %d\n", cfg.http_port); return false; }

#ifdef PLATFORM_WINDOWS
    SOCKET srv = HttpOpenListen(cfg.http_port);
    if (srv == INVALID_SOCKET)
        return false;
#else
    int srv = HttpOpenListen(cfg.http_port);
    if (srv < 0)
        return false;
#endif

    g_httpStopReq = false;
    // Snapshot credentials before the thread starts: request handling then
    // never touches the caller's AppConfig object.
    RefreshAuthSnapshot(cfg);
    // Open the access log on the caller thread (bind already succeeded);
    // only the server thread writes it, so plain FILE* is enough.
    HttpLogOpen(cfg);
    try
    {
        g_httpThread = std::thread([srv, cfg]
        {
            HttpServeLoop(srv, cfg.http_port, cfg);
            HttpClose(srv);
            HttpLogClose();
        });
    }
    catch (const std::system_error& e)
    {
        fprintf(stderr, "HTTP: cannot create server thread: %s\n", e.what());
        HttpClose(srv);
        HttpLogClose();
        return false;
    }
    return true;
}

void StopHttpServer()
{
    std::lock_guard<std::mutex> lock(g_httpMtx);
    if (!g_httpThread.joinable())
        return;
    // The loop checks the flag after every select() timeout (1 s), so the
    // join returns quickly without touching the socket from another thread.
    g_httpStopReq = true;
    g_httpThread.join();
}

bool IsHttpServerRunning()
{
    std::lock_guard<std::mutex> lock(g_httpMtx);
    return g_httpThread.joinable();
}
