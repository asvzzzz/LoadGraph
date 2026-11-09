#include "platform.h"
#include "http_server.h"
#include "metrics_api.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <thread>
#include <mutex>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "ws2_32.lib")

#ifdef PLATFORM_WINDOWS

// HTML dashboard template - inline for zero external dependencies.
static const char* kHtmlPage = R"(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>LoadGraph</title>
<style>
  * { margin: 0; padding: 0; box-sizing: border-box; }
  body { background: #1a1a2e; color: #e0e0e0; font-family: Consolas, monospace; padding: 16px; }
  h1 { color: #76B900; margin-bottom: 8px; font-size: 18px; }
  .section { background: #16213e; border-radius: 8px; padding: 12px; margin-bottom: 12px; }
  .row { display: flex; justify-content: space-between; align-items: center; margin: 4px 0; }
  .label { color: #888; font-size: 13px; }
  .value { color: #fff; font-weight: bold; font-size: 14px; }
  .bar-bg { background: #0f3460; border-radius: 4px; height: 16px; width: 200px; overflow: hidden; }
  .bar-fill { height: 100%; border-radius: 4px; transition: width 0.5s; }
  .gpu-title { color: #76B900; font-size: 14px; margin-top: 8px; margin-bottom: 4px; }
  .footer { text-align: center; color: #555; font-size: 11px; margin-top: 16px; }
  .green { color: #76B900; }
  .cyan  { color: #00bcd4; }
  .yellow{ color: #ffc107; }
  .red   { color: #f44336; }
</style>
</head>
<body>
<h1>NVIDIA LoadGraph <span id="cpuName" class="cyan"></span></h1>

<div class="section">
  <div class="row"><span class="label">CPU Load</span>
    <span><span id="cpuLoad" class="value">--</span>%
      <div class="bar-bg"><div id="cpuBar" class="bar-fill" style="width:0;background:#76B900"></div></div>
    </span></div>
  <div class="row"><span class="label">CPU Temp</span><span id="cpuTemp" class="value">N/A</span></div>
  <div class="row"><span class="label">CPU Power</span><span id="cpuPower" class="value">N/A</span></div>
  <div class="row"><span class="label">Uptime</span><span id="uptime" class="value">--</span></div>
</div>

<div class="section">
  <div class="row"><span class="label">RAM Usage</span>
    <span><span id="ramLoad" class="value">--</span>%
      <div class="bar-bg"><div id="ramBar" class="bar-fill" style="width:0;background:#00bcd4"></div></div>
    </span></div>
  <div class="row"><span class="label">RAM Used</span><span id="ramUsed" class="value">N/A</span>
    <span class="label" id="ramTotal"></span></div>
</div>

<div id="gpuSections"></div>

<div class="footer">Auto-refresh every 1s | LoadGraph Console</div>

<script>
const GPU_COLORS = ['#76B900','#00bcd4','#ffc107','#f44336','#e91e63'];
function esc(s) { return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;'); }
function fmtBytes(b) {
  if (!b || b < 1024) return '0 B';
  if (b < 1024*1024) return (b/1024).toFixed(0) + ' KB';
  if (b < 1024*1024*1024) return (b/(1024*1024*1024)).toFixed(1) + ' GB';
  return (b/(1024*1024*1024*1024)).toFixed(1) + ' TB';
}
function fmtUptime(ms) {
  var s = Math.floor(ms/1000);
  var d = Math.floor(s/86400); s %= 86400;
  var h = Math.floor(s/3600);  s %= 3600;
  var m = Math.floor(s/60);    s %= 60;
  if (d > 0) return d+'d '+h+'h '+m+'m';
  if (h > 0) return h+'h '+m+'m '+s+'s';
  return m+'m '+s+'s';
}
function tempColor(t) {
  if (t > 80) return 'red';
  if (t > 65) return 'yellow';
  return 'green';
}
async function fetchMetrics() {
  try { var r = await fetch('/api/metrics'); var d = await r.json(); updateDashboard(d); } catch(e) {}
}
function updateDashboard(d) {
  document.getElementById('cpuName').textContent = d.cpu_name || '';
  document.getElementById('cpuLoad').textContent = d.cpu_load.toFixed(1);
  document.getElementById('cpuBar').style.width = Math.min(100, d.cpu_load) + '%';
  document.getElementById('cpuBar').style.background = d.cpu_load > 80 ? '#f44336' : '#76B900';
  if (d.cpu_temp >= 0) { var tc = tempColor(d.cpu_temp); document.getElementById('cpuTemp').innerHTML = '<span class="'+tc+'">'+d.cpu_temp.toFixed(0)+' C</span>'; }
  else { document.getElementById('cpuTemp').textContent = 'N/A'; }
  document.getElementById('cpuPower').textContent = d.cpu_power > 0 ? d.cpu_power.toFixed(1)+' W' : 'N/A';
  document.getElementById('uptime').textContent = fmtUptime(d.uptime_ms);
  document.getElementById('ramLoad').textContent = d.ram_load.toFixed(1);
  document.getElementById('ramBar').style.width = Math.min(100, d.ram_load) + '%';
  document.getElementById('ramUsed').textContent = fmtBytes(d.ram_used_bytes);
  document.getElementById('ramTotal').textContent = '/ ' + fmtBytes(d.ram_total_bytes);
  var gpuDiv = document.getElementById('gpuSections');
  if (!d.gpus || d.gpus.length === 0) { gpuDiv.innerHTML = ''; return; }
  var html = '';
  for (var i = 0; i < d.gpus.length; i++) {
    var g = d.gpus[i]; var col = GPU_COLORS[i % GPU_COLORS.length];
    html += '<div class="section"><div class="gpu-title" style="color:'+col+'">GPU '+i+': '+esc(g.name)+'</div>';
    if (g.load_percent >= 0) html += '<div class="row"><span class="label">Load</span><span>'+g.load_percent.toFixed(1)+'% <div class="bar-bg"><div style="width:'+Math.min(100,g.load_percent)+'%;background:'+col+';height:100%;border-radius:4px"></div></div></span></div>';
    if (g.temperature >= 0) { var tc2 = tempColor(g.temperature); html += '<div class="row"><span class="label">Temp</span><span class="'+tc2+'">'+g.temperature.toFixed(0)+' C</span></div>'; }
    if (g.vram_total_bytes > 0) { var vp = g.vram_used_bytes*100/g.vram_total_bytes; html += '<div class="row"><span class="label">VRAM</span><span>'+vp.toFixed(1)+'% ('+fmtBytes(g.vram_used_bytes)+'/'+fmtBytes(g.vram_total_bytes)+') <div class="bar-bg"><div style="width:'+Math.min(100,vp)+'%;background:'+col+';height:100%;border-radius:4px"></div></div></span></div>'; }
    if (g.power_watts > 0) html += '<div class="row"><span class="label">Power</span><span>'+g.power_watts.toFixed(1)+' W</span></div>';
    html += '</div>';
  }
  gpuDiv.innerHTML = html;
}
setInterval(fetchMetrics, 1000);
fetchMetrics();
</script>
</body>
</html>)";

static std::string BuildJson()
{
    const CpuData  cpu  = MetricsGetCpu();
    const RamData  ram  = MetricsGetRam();
    const auto&    gpus = MetricsGetGpus();

    char buf[8192];
    int off = 0;
    // Escape the CPU name for JSON (replace backslash and quote)
    std::string escaped;
    for (wchar_t c : cpu.cpuName)
    {
        if (c == L'\\' || c == L'"') escaped += '\\';
        else if (c < 128) escaped += (char)c;
    }

    off = snprintf(buf, sizeof(buf),
        "{\"cpu_load\":%.1f,\"cpu_temp\":%.1f,\"cpu_power\":%.1f,"
         "\"ram_load\":%.1f,\"ram_used_bytes\":%llu,\"ram_total_bytes\":%llu,"
         "\"uptime_ms\":%llu,\"cpu_name\":\"%s\",\"gpus\":[",
        cpu.loadPercent,
        cpu.temperature,
        cpu.powerWatts,
        ram.loadPercent,
        (unsigned long long)(ram.totalBytes > ram.freeBytes
                             ? ram.totalBytes - ram.freeBytes : 0),
        (unsigned long long)ram.totalBytes,
        (unsigned long long)cpu.uptimeMs,
        escaped.c_str());

    for (size_t i = 0; i < gpus.size(); ++i)
    {
        const GpuData& g = gpus[i];
        if (i > 0) off += snprintf(buf + off, sizeof(buf) - off, ",");
        off += snprintf(buf + off, sizeof(buf) - off,
            "{\"name\":\"%s\",\"load_percent\":%.1f,\"temperature\":%.1f,"
             "\"vram_used_bytes\":%llu,\"vram_total_bytes\":%llu,"
             "\"power_watts\":%.1f}",
            g.name.c_str(), g.loadPercent, g.temperature,
            (unsigned long long)g.vramUsedBytes,
            (unsigned long long)g.vramTotalBytes,
            g.powerWatts);
    }
    off += snprintf(buf + off, sizeof(buf) - off, "]}");
    return std::string(buf, (size_t)off);
}

static void HttpLoop(int port)
{
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        fprintf(stderr, "HTTP: WSAStartup failed\n");
        return;
    }

    SOCKET srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (srv == INVALID_SOCKET)
    {
        fprintf(stderr, "HTTP: socket() failed err=%d\n", WSAGetLastError());
        WSACleanup();
        return;
    }

    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons((unsigned short)port);

    if (bind(srv, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR)
    {
        fprintf(stderr, "HTTP: bind() failed port=%d err=%d\n", port, WSAGetLastError());
        closesocket(srv);
        WSACleanup();
        return;
    }
    if (listen(srv, 4) == SOCKET_ERROR)
    {
        fprintf(stderr, "HTTP: listen() failed err=%d\n", WSAGetLastError());
        closesocket(srv);
        WSACleanup();
        return;
    }

    printf("HTTP server listening on port %d\n", port);
    printf("Open http://localhost:%d in your browser\n", port);
    fflush(stdout);

    while (true)
    {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(srv, &readfds);
        timeval tv{};
        tv.tv_sec  = 1;
        tv.tv_usec = 0;

        int ret = select(0, &readfds, nullptr, nullptr, &tv);
        if (ret <= 0 || !FD_ISSET(srv, &readfds)) continue;

        SOCKET client = accept(srv, nullptr, nullptr);
        if (client == INVALID_SOCKET) continue;

        char reqBuf[2048] = {};
        recv(client, reqBuf, sizeof(reqBuf) - 1, 0);

        std::string resp;
        if (strstr(reqBuf, "GET /api/metrics") != nullptr)
        {
            resp = "HTTP/1.1 200 OK\r\n"
                   "Content-Type: application/json\r\n"
                   "Access-Control-Allow-Origin: *\r\n"
                   "Connection: close\r\n"
                   "\r\n" + BuildJson();
        }
        else if (strstr(reqBuf, "GET /") != nullptr)
        {
            resp = std::string("HTTP/1.1 200 OK\r\n"
                               "Content-Type: text/html; charset=utf-8\r\n"
                               "Access-Control-Allow-Origin: *\r\n"
                               "Connection: close\r\n"
                               "\r\n") + kHtmlPage;
        }
        else
        {
            resp = "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\nNot Found";
        }

        send(client, resp.c_str(), (int)resp.size(), 0);
        closesocket(client);
    }

    closesocket(srv);
    WSACleanup();
}

void StartHttpServer(int port)
{
    std::thread(HttpLoop, port).detach();
}

#endif // PLATFORM_WINDOWS
