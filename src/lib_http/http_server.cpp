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

// Headers sent with EVERY response (200, 401, 403, 404):
//   nosniff     - old browsers (IE on Windows 7 era) must not reinterpret the
//                 JSON metrics as HTML: device names from the drivers reach
//                 the response and would otherwise be a script vector.
//   no-store    - hardware telemetry never lands in a browser or proxy cache.
//   DENY        - the dashboard cannot be framed by another page (clickjacking).
//   no-referrer - no URL/credential leak through the Referer header.
//   CSP         - the page may only run its own inline scripts/styles and talk
//                 to this origin, so injected markup cannot fetch out.
static const char kSecurityHeaders[] =
    "X-Content-Type-Options: nosniff\r\n"
    "Cache-Control: no-store\r\n"
    "X-Frame-Options: DENY\r\n"
    "Referrer-Policy: no-referrer\r\n"
    "Content-Security-Policy: default-src 'none'; style-src 'unsafe-inline'; "
    "script-src 'unsafe-inline'; img-src 'self' data:; connect-src 'self'; "
    "frame-ancestors 'none'; base-uri 'none'\r\n";

// Resolve the http_bind config value into the IPv4 address to listen on.
// Accepted: "all" or empty (every interface - the default), "localhost" /
// "loopback" (127.0.0.1) or a dotted IPv4 literal. Returns false when the
// value cannot be parsed; the caller then falls back to every interface.
static bool ResolveBindAddress(const std::string& text, in_addr& out)
{
    std::string v = text;
    for (char& c : v)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');

    if (v.empty() || v == "all" || v == "any")
    { out.s_addr = INADDR_ANY; return true; }
    if (v == "localhost" || v == "loopback")
    { out.s_addr = htonl(0x7F000001u); return true; }

#ifdef PLATFORM_WINDOWS
    return InetPtonA(AF_INET, v.c_str(), &out) == 1;
#else
    return inet_pton(AF_INET, v.c_str(), &out) == 1;
#endif
}

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
             "\"fan_rpm\":%d,\"fan_percent\":%.1f,\"fan_max_rpm\":%d,"
             "\"power_watts\":%.1f,\"power_limit_watts\":%.1f}",
            SafeDouble(g.loadPercent), SafeDouble(g.temperature),
            (unsigned long long)g.vramUsedBytes,
            (unsigned long long)g.vramTotalBytes,
            g.fanRpm, SafeDouble(g.fanPercent), g.fanMaxRpm,
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

// Value of the named request header (exact name, case-insensitive; headers
// merely containing the word are not matched). Returns false when absent.
static bool FindHeader(const std::string& req, const char* name, std::string& value)
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
            std::string hname = line.substr(0, colon);
            for (char& c : hname)
                if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            if (hname == name)
            {
                value = line.substr(colon + 1);
                return true;
            }
        }
        lineStart = lineEnd + 1;
    }
    return false;
}

// Host header guard against DNS rebinding. The value is accepted when it is
// an IP literal (the normal LAN case, http://192.168.x.x:9115), "localhost"
// or a dot-free LAN name (NetBIOS/mDNS single label such as "nas-box"); a
// dotted DNS name that is not an IP is rejected, because that is exactly how
// a page on the internet points its own domain at this machine and then reads
// /api/metrics as same origin. Requests without a Host header (HTTP/1.0) are
// accepted - they cannot carry a hostile name.
static bool HostAllowed(const std::string& req)
{
    std::string host;
    if (!FindHeader(req, "host", host)) return true;

    const size_t b = host.find_first_not_of(" \t");
    if (b == std::string::npos) return true;
    const size_t e = host.find_last_not_of(" \t");
    host = host.substr(b, e - b + 1);

    // Drop the ":port" suffix, keeping an IPv6 literal [::1]:9115 intact.
    size_t cut = std::string::npos;
    if (host[0] == '[')
    {
        const size_t rb = host.find(']');
        if (rb != std::string::npos && rb + 1 < host.size() && host[rb + 1] == ':')
            cut = rb + 1;
    }
    else
    {
        cut = host.find(':');
    }
    if (cut != std::string::npos) host.resize(cut);

    // Unbracket the IPv6 literal before testing it.
    if (host.size() >= 2 && host.front() == '[' && host.back() == ']')
        host = host.substr(1, host.size() - 2);

    if (host.empty()) return true;
    for (char& c : host)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    if (host == "localhost") return true;

    in_addr  a4;
    in6_addr a6;
#ifdef PLATFORM_WINDOWS
    if (InetPtonA(AF_INET,  host.c_str(), &a4) == 1) return true;
    if (InetPtonA(AF_INET6, host.c_str(), &a6) == 1) return true;
#else
    if (inet_pton(AF_INET,  host.c_str(), &a4) == 1) return true;
    if (inet_pton(AF_INET6, host.c_str(), &a6) == 1) return true;
#endif
    return host.find('.') == std::string::npos;
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
    if (FindHeader(req, "authorization", hdrval) && ExtractBasicCreds(hdrval, userpass))
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
// Only INVALID requests are logged - connections that sent no request bytes
// (scanners), 401 auth failures and 404 unknown paths - one line each with
// the client IP:port plus the raw request text. Valid 200 responses are
// never logged, so the file stays quiet while the dashboard polls. The file
// is created lazily on the first logged event and opened in append mode:
// app restarts never truncate it. Written ONLY from the server thread, so
// no locking is needed. The log is both rate limited and size capped (see
// HttpLogAllowLine): a remote client that only opens connections or sends
// garbage cannot grow the file without bound.
// ============================================================================

static FILE* g_httpLogFile    = nullptr;
static bool  g_httpLogEnabled = false;  // cfg.http_log, set before the thread starts
static bool  g_httpLogFailed  = false;  // open error: report once, do not spam
static std::string g_httpLogPath;       // kept for the reopen on size overflow
static long long   g_httpLogBytes = 0;  // bytes currently in the file
static time_t      g_httpLogWin   = 0;  // start of the current 1 s window
static int         g_httpLogCount = 0;  // lines written in that window
static int         g_httpLogDrop  = 0;  // lines skipped in that window

// Hard limits for the access log: at most 25 lines per second and 1 MiB on
// disk. Over the size cap the file is restarted, keeping the newest entries.
static const int       kHttpLogMaxPerSec = 25;
static const long long kHttpLogMaxBytes  = 1024 * 1024;

// Records the config switch on the caller thread before the server thread
// starts (happens-before via std::thread construction), so the server
// thread can read it without synchronization.
static void HttpLogInit(const AppConfig& cfg)
{
    g_httpLogEnabled = cfg.http_log;
    g_httpLogFile    = nullptr;
    g_httpLogFailed  = false;
    g_httpLogPath.clear();
    g_httpLogBytes = 0;
    g_httpLogWin   = 0;
    g_httpLogCount = 0;
    g_httpLogDrop  = 0;
}

// Open the log on first use, in append mode: the file is not created until
// something is actually worth logging, and previous sessions' lines survive.
static bool HttpLogEnsureOpen()
{
    if (g_httpLogFile) return true;
    if (g_httpLogFailed) return false;
    const std::string dir = GetAppConfigDir();
    g_httpLogPath = dir.empty() ? std::string("./loadgraph_http.log")
                                : dir + "/loadgraph_http.log";
    g_httpLogFile = fopen(g_httpLogPath.c_str(), "a");
    if (!g_httpLogFile)
    {
        g_httpLogFailed = true;
        fprintf(stderr, "HTTP: cannot open %s for logging\n", g_httpLogPath.c_str());
    }
    else
    {
        // Length of the existing (appended) file, base for the size cap.
        const long pos = ftell(g_httpLogFile);
        g_httpLogBytes = (pos > 0) ? (long long)pos : 0;
    }
    return g_httpLogFile != nullptr;
}

// Size and rate guard for one log line. Returns false when the line must be
// skipped because the per-second budget is used up (skips are counted and
// reported by the next allowed line). When the file passes the size cap it is
// restarted here, so an attacker flooding the port cannot fill the disk; the
// newest entries are kept and the loss is marked in the log itself.
// Called only from the server thread, after HttpLogEnsureOpen() succeeded.
static bool HttpLogAllowLine()
{
    const time_t now = time(nullptr);
    if (now != g_httpLogWin)
    {
        g_httpLogWin   = now;
        g_httpLogCount = 0;
        if (g_httpLogDrop > 0)
        {
            fprintf(g_httpLogFile, "=== %d more events suppressed in the previous second ===\n",
                    g_httpLogDrop);
            g_httpLogDrop = 0;
        }
    }
    if (g_httpLogCount >= kHttpLogMaxPerSec)
    {
        ++g_httpLogDrop;
        return false;
    }
    ++g_httpLogCount;

    if (g_httpLogBytes > kHttpLogMaxBytes)
    {
        // Marker text is derived from the constant so it can never state a
        // limit that differs from the one actually applied.
        char mark[96];
        int mlen = snprintf(mark, sizeof(mark),
            "=== log exceeded %d KiB: older entries discarded ===\n",
            (int)(kHttpLogMaxBytes / 1024));
        if (mlen <= 0 || mlen >= (int)sizeof(mark))
        {
            mark[0] = '\0';
            mlen = 0;
        }
        fclose(g_httpLogFile);
        g_httpLogFile = fopen(g_httpLogPath.c_str(), "w");
        if (!g_httpLogFile)
        {
            g_httpLogFailed = true;
            fprintf(stderr, "HTTP: cannot reopen %s for logging\n", g_httpLogPath.c_str());
            return false;
        }
        if (mlen > 0) fputs(mark, g_httpLogFile);
        g_httpLogBytes = (long long)mlen;
    }
    return true;
}

// Flush and close the (possibly never opened) log file.
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
// as FindHeader.
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
// escaped, other control and non-ASCII bytes become \xNN, and the line length
// is bounded (the per-second and total size limits are in HttpLogAllowLine).
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

// One line per invalid event: "HH:MM:SS ip:port METHOD PATH -> CODE".
// rawReq != nullptr appends the sanitized request text (401/403/404); the
// no-request case has no text to log. Never called for valid 200 responses.
static void HttpLogAccess(const char* peer, const char* method, const char* path,
                          int status, const std::string* rawReq)
{
    if (!g_httpLogEnabled) return;
    if (!HttpLogEnsureOpen()) return;
    if (!HttpLogAllowLine()) return;
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
    // Track the real file length for the size cap.
    const long pos = ftell(g_httpLogFile);
    if (pos > 0) g_httpLogBytes = (long long)pos;
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
static SOCKET HttpOpenListen(int port, in_addr bindAddr)
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
    addr.sin_addr        = bindAddr;
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

// Send an already formatted response buffer in full.
static void SendBuffer(SOCKET client, const char* data, int len)
{
    int sent = 0;
    while (sent < len)
    { int sn = send(client, data + sent, len - sent, 0); if (sn <= 0) break; sent += sn; }
}

// 401 challenge: after the user enters credentials the browser sends the
// Authorization header on every request (including the metrics polls).
static void SendUnauthorized(SOCKET client)
{
    static const char body[] = "Authorization required.";
    char resp[512];
    int len = snprintf(resp, sizeof(resp),
        "HTTP/1.1 401 Unauthorized\r\n"
        "WWW-Authenticate: Basic realm=\"LoadGraph\", charset=\"UTF-8\"\r\n"
        "Content-Type: text/plain\r\n"
        "Connection: close\r\n"
        "%s"
        "Content-Length: %u\r\n\r\n%s",
        kSecurityHeaders, (unsigned int)strlen(body), body);
    if (len <= 0 || len >= (int)sizeof(resp)) return;
    SendBuffer(client, resp, len);
}

// 403: the Host header names a dotted DNS domain instead of an IP address or
// localhost (DNS rebinding pattern). Nothing about the host is revealed.
static void SendForbidden(SOCKET client)
{
    static const char body[] = "Invalid Host header.";
    char resp[512];
    int len = snprintf(resp, sizeof(resp),
        "HTTP/1.1 403 Forbidden\r\n"
        "Content-Type: text/plain\r\n"
        "Connection: close\r\n"
        "%s"
        "Content-Length: %u\r\n\r\n%s",
        kSecurityHeaders, (unsigned int)strlen(body), body);
    if (len <= 0 || len >= (int)sizeof(resp)) return;
    SendBuffer(client, resp, len);
}

// 404 for unknown paths and methods.
static void SendNotFound(SOCKET client)
{
    static const char body[] = "Not Found";
    char resp[512];
    int len = snprintf(resp, sizeof(resp),
        "HTTP/1.1 404 Not Found\r\n"
        "Content-Type: text/plain\r\n"
        "Connection: close\r\n"
        "%s"
        "Content-Length: %u\r\n\r\n%s",
        kSecurityHeaders, (unsigned int)strlen(body), body);
    if (len <= 0 || len >= (int)sizeof(resp)) return;
    SendBuffer(client, resp, len);
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

        // Host guard first: a rebinding request is refused before the server
        // even reveals that it needs credentials.
        if (!HostAllowed(reqStr))
        {
            HttpLogAccess(peerTxt, method, path, 403, &reqStr);
            SendForbidden(client);
            closesocket(client);
            continue;
        }

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
            // Build the full header in one string to avoid partial-send over-read.
            char hdr[1024];
            int hlen = snprintf(hdr, sizeof(hdr),
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: image/x-icon\r\n"
                "Connection: close\r\n"
                "%s"
                "Content-Length: %u\r\n\r\n",
                kSecurityHeaders, (unsigned int)kLoadGraphIcoSize);
            if (hlen <= 0 || hlen >= (int)sizeof(hdr)) { closesocket(client); continue; }
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
            SendNotFound(client);
            closesocket(client);
            continue;
        }

        char hdr[1024] = {};
        int hlen = snprintf(hdr, sizeof(hdr),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: %s\r\n"
            "Connection: close\r\n"
            "%s"
            "Content-Length: %u\r\n"
            "\r\n",
            isMetrics ? "application/json" : "text/html; charset=utf-8",
            kSecurityHeaders,
            (unsigned int)body.size());
        if (hlen <= 0 || hlen >= (int)sizeof(hdr)) { closesocket(client); continue; }

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
static int HttpOpenListen(int port, in_addr bindAddr)
{
    int srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (srv < 0) { fprintf(stderr, "HTTP: socket() failed\n"); return -1; }

    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr        = bindAddr;
    addr.sin_port        = htons(static_cast<unsigned short>(port));

    if (bind(srv, (sockaddr*)&addr, sizeof(addr)) < 0)
    { fprintf(stderr, "HTTP: bind() failed port=%d\n", port); close(srv); return -1; }
    if (listen(srv, 4) < 0)
    { fprintf(stderr, "HTTP: listen() failed\n"); close(srv); return -1; }
    return srv;
}

// Send an already formatted response buffer in full. MSG_NOSIGNAL: a client
// that hangs up mid-response must not raise SIGPIPE and kill the application.
static void SendBuffer(int client, const char* data, int len)
{
    int sent = 0;
    while (sent < len)
    { int sn = send(client, data + sent, len - sent, MSG_NOSIGNAL); if (sn <= 0) break; sent += sn; }
}

// 401 challenge: after the user enters credentials the browser sends the
// Authorization header on every request (including the metrics polls).
static void SendUnauthorized(int client)
{
    static const char body[] = "Authorization required.";
    char resp[512];
    int len = snprintf(resp, sizeof(resp),
        "HTTP/1.1 401 Unauthorized\r\n"
        "WWW-Authenticate: Basic realm=\"LoadGraph\", charset=\"UTF-8\"\r\n"
        "Content-Type: text/plain\r\n"
        "Connection: close\r\n"
        "%s"
        "Content-Length: %u\r\n\r\n%s",
        kSecurityHeaders, (unsigned int)strlen(body), body);
    if (len <= 0 || len >= (int)sizeof(resp)) return;
    SendBuffer(client, resp, len);
}

// 403: the Host header names a dotted DNS domain instead of an IP address or
// localhost (DNS rebinding pattern). Nothing about the host is revealed.
static void SendForbidden(int client)
{
    static const char body[] = "Invalid Host header.";
    char resp[512];
    int len = snprintf(resp, sizeof(resp),
        "HTTP/1.1 403 Forbidden\r\n"
        "Content-Type: text/plain\r\n"
        "Connection: close\r\n"
        "%s"
        "Content-Length: %u\r\n\r\n%s",
        kSecurityHeaders, (unsigned int)strlen(body), body);
    if (len <= 0 || len >= (int)sizeof(resp)) return;
    SendBuffer(client, resp, len);
}

// 404 for unknown paths and methods.
static void SendNotFound(int client)
{
    static const char body[] = "Not Found";
    char resp[512];
    int len = snprintf(resp, sizeof(resp),
        "HTTP/1.1 404 Not Found\r\n"
        "Content-Type: text/plain\r\n"
        "Connection: close\r\n"
        "%s"
        "Content-Length: %u\r\n\r\n%s",
        kSecurityHeaders, (unsigned int)strlen(body), body);
    if (len <= 0 || len >= (int)sizeof(resp)) return;
    SendBuffer(client, resp, len);
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

        // Bounded I/O on the accepted socket (same 5 s as on Windows): without
        // it a client that connects and sends nothing blocks this loop in
        // recv() forever, which freezes the dashboard AND makes
        // StopHttpServer() hang in thread::join() during shutdown.
        timeval rcvTo{}; rcvTo.tv_sec  = 5; rcvTo.tv_usec = 0;
        timeval sndTo{}; sndTo.tv_sec  = 5; sndTo.tv_usec = 0;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &rcvTo, sizeof(rcvTo));
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &sndTo, sizeof(sndTo));

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

        // Host guard first: a rebinding request is refused before the server
        // even reveals that it needs credentials.
        if (!HostAllowed(reqStr))
        {
            HttpLogAccess(peerTxt, method, path, 403, &reqStr);
            SendForbidden(client);
            close(client);
            continue;
        }

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
            char hdr[1024];
            int hlen = snprintf(hdr, sizeof(hdr),
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: image/x-icon\r\n"
                "Connection: close\r\n"
                "%s"
                "Content-Length: %u\r\n\r\n",
                kSecurityHeaders, (unsigned int)kLoadGraphIcoSize);
            if (hlen <= 0 || hlen >= (int)sizeof(hdr)) { close(client); continue; }
            // MSG_NOSIGNAL in every send below: SIGPIPE is not ignored in this
            // application, so writing to a socket the browser already closed
            // would terminate the whole program.
            int sent = 0;
            while (sent < hlen)
            { int sn = send(client, hdr + sent, hlen - sent, MSG_NOSIGNAL); if (sn <= 0) break; sent += sn; }
            if (sent < hlen) { close(client); continue; }
            const unsigned char* p = kLoadGraphIcoBytes;
            size_t rem = kLoadGraphIcoSize;
            while (rem > 0)
            {
                int chunk = (int)(rem > 4096 ? 4096 : rem);
                int sn = send(client, (const char*)p, chunk, MSG_NOSIGNAL);
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
            SendNotFound(client);
            close(client);
            continue;
        }

        char hdr[1024] = {};
        int hlen = snprintf(hdr, sizeof(hdr),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: %s\r\n"
            "Connection: close\r\n"
            "%s"
            "Content-Length: %u\r\n"
            "\r\n",
            isMetrics ? "application/json" : "text/html; charset=utf-8",
            kSecurityHeaders,
            (unsigned int)body.size());
        if (hlen <= 0 || hlen >= (int)sizeof(hdr)) { close(client); continue; }

        int total = hlen, sent = 0;
        while (sent < total)
        { int sn = send(client, hdr + sent, total - sent, MSG_NOSIGNAL); if (sn <= 0) break; sent += sn; }
        total = (int)body.size(); sent = 0;
        while (sent < total)
        { int sn = send(client, body.c_str() + sent, total - sent, MSG_NOSIGNAL); if (sn <= 0) break; sent += sn; }

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

    // Listen interface from cfg.http_bind. An unparseable value falls back to
    // every interface (the documented default) with a warning.
    in_addr bindAddr{};
    if (!ResolveBindAddress(cfg.http_bind, bindAddr))
    {
        fprintf(stderr, "HTTP: unknown http_bind value, listening on all interfaces\n");
        fflush(stderr);  // stderr may be fully buffered when redirected to a file
        bindAddr.s_addr = INADDR_ANY;
    }

#ifdef PLATFORM_WINDOWS
    SOCKET srv = HttpOpenListen(cfg.http_port, bindAddr);
    if (srv == INVALID_SOCKET)
        return false;
#else
    int srv = HttpOpenListen(cfg.http_port, bindAddr);
    if (srv < 0)
        return false;
#endif

    // Report what is actually reachable: an all-interface bind means every
    // host on the network can reach the dashboard (and brute force the login).
    char bindTxt[INET_ADDRSTRLEN] = {};
#ifdef PLATFORM_WINDOWS
    InetNtopA(AF_INET, &bindAddr, bindTxt, (DWORD)sizeof(bindTxt));
#else
    inet_ntop(AF_INET, &bindAddr, bindTxt, (socklen_t)sizeof(bindTxt));
#endif
    fprintf(stdout, "HTTP: dashboard on http://%s:%d (%s)\n",
            bindTxt[0] ? bindTxt : "0.0.0.0", cfg.http_port,
            bindAddr.s_addr == INADDR_ANY ? "all interfaces" : "single interface");

    g_httpStopReq = false;
    // Snapshot credentials before the thread starts: request handling then
    // never touches the caller's AppConfig object.
    RefreshAuthSnapshot(cfg);
    // Record the access-log switch; the file itself is opened lazily on the
    // first invalid request (append mode, never truncated on restart).
    HttpLogInit(cfg);
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
