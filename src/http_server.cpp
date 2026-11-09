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

// HTML dashboard with canvas-based rolling charts (no progress bars).
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
  .row { display: flex; align-items: center; margin: 5px 0; gap: 10px; }
  .label { color: #888; font-size: 13px; width: 90px; flex-shrink: 0; }
  .value { color: #fff; font-weight: bold; font-size: 14px; margin-right: 10px; min-width: 80px; text-align: right; }
  canvas.chart { background: #0f3460; border-radius: 4px; display: block; width: 100%; height: 50px; }
  .chart-row { display: flex; align-items: center; gap: 10px; margin: 3px 0; }
  .gpu-title { color: #76B900; font-size: 14px; margin: 8px 0 4px; }
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
  <div class="row">
    <span class="label">CPU Load</span>
    <span class="value" id="cpuLoadVal">--%</span>
  </div>
  <canvas id="cpuChart" class="chart"></canvas>
  <div class="row" style="margin-top:6px">
    <span class="label">CPU Temp</span>
    <span class="value" id="cpuTempVal">N/A</span>
    <span class="label" style="width:auto;margin-left:12px">Power</span>
    <span class="value" id="cpuPowerVal">N/A</span>
  </div>
  <div class="row">
    <span class="label">Uptime</span>
    <span class="value" id="uptime">--</span>
  </div>
</div>

<div class="section">
  <div class="row">
    <span class="label">RAM Usage</span>
    <span class="value" id="ramLoadVal">--%</span>
  </div>
  <canvas id="ramChart" class="chart"></canvas>
  <div class="row" style="margin-top:4px">
    <span class="label">RAM Used</span>
    <span class="value" id="ramUsed">N/A</span>
    <span class="label" id="ramTotal"></span>
  </div>
</div>

<div id="gpuSections"></div>

<div class="footer">Auto-refresh every 1s | LoadGraph</div>

<script>
const GPU_COLORS = ['#76B900','#00bcd4','#ffc107','#f44336','#e91e63'];
const MAX_POINTS = 60;

// --- Mini rolling chart (canvas) ---
function makeChart(id, color) {
    const c = document.getElementById(id);
    c.width  = c.offsetWidth * 2 || 400;
    c.height = 100;
    const ctx = c.getContext('2d');
    const data = [];
    return {
        push(v) { data.push(Math.max(0, Math.min(100, v))); if (data.length > MAX_POINTS) data.shift(); draw(); },
        clear() { data.length = 0; draw(); }
    };
    function draw() {
        const W = c.width, H = c.height;
        ctx.clearRect(0, 0, W, H);
        // Grid lines
        ctx.strokeStyle = 'rgba(255,255,255,0.07)';
        ctx.lineWidth = 1;
        for (let y = 0; y <= 4; y++) {
            const yy = H - (y / 4) * H;
            ctx.beginPath(); ctx.moveTo(0, yy); ctx.lineTo(W, yy); ctx.stroke();
        }
        if (data.length < 2) return;
        const step = W / (MAX_POINTS - 1);
        // Fill gradient
        ctx.beginPath();
        ctx.moveTo(0, H);
        for (let i = 0; i < data.length; i++)
            ctx.lineTo(i * step, H - (data[i] / 100) * H);
        ctx.lineTo((data.length - 1) * step, H);
        ctx.closePath();
        const grad = ctx.createLinearGradient(0, 0, 0, H);
        grad.addColorStop(0, color + 'cc');
        grad.addColorStop(1, color + '22');
        ctx.fillStyle = grad;
        ctx.fill();
        // Line
        ctx.beginPath();
        for (let i = 0; i < data.length; i++) {
            const x = i * step, y = H - (data[i] / 100) * H;
            i === 0 ? ctx.moveTo(x, y) : ctx.lineTo(x, y);
        }
        ctx.strokeStyle = color;
        ctx.lineWidth = 2;
        ctx.stroke();
    }
}

let cpuChart = makeChart('cpuChart', '#76B900');
let ramChart = makeChart('ramChart', '#00bcd4');

function esc(s) { return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;'); }
function fmtBytes(b) {
  if (!b || b < 1024) return '0 B';
  if (b < 1024*1024) return (b/1024).toFixed(0) + ' KB';
  if (b < 1024*1024*1024) return (b/(1024*1024*1024)).toFixed(1) + ' GB';
  return (b/(1024*1024*1024*1024)).toFixed(1) + ' TB';
}
function fmtUptime(ms) {
  var s = Math.floor(ms/1000), d = Math.floor(s/86400); s %= 86400;
  var h = Math.floor(s/3600); s %= 3600; var m = Math.floor(s/60); s %= 60;
  if (d > 0) return d+'d '+h+'h '+m+'m';
  if (h > 0) return h+'h '+m+'m '+s+'s';
  return m+'m '+s+'s';
}
function tempColor(t) { return t > 80 ? 'red' : t > 65 ? 'yellow' : 'green'; }

// Per-GPU charts
const gpuCharts = {};

async function fetchMetrics() {
  try { var r = await fetch('/api/metrics'); var d = await r.json(); updateDashboard(d); } catch(e) {}
}

function updateDashboard(d) {
  document.getElementById('cpuName').textContent = d.cpu_name || '';
  var cl = d.cpu_load.toFixed(1);
  document.getElementById('cpuLoadVal').textContent = cl + '%';
  cpuChart.push(d.cpu_load);

  if (d.cpu_temp >= 0) {
    var tc = tempColor(d.cpu_temp);
    document.getElementById('cpuTempVal').innerHTML = '<span class="'+tc+'">'+d.cpu_temp.toFixed(0)+' C</span>';
  } else {
    document.getElementById('cpuTempVal').textContent = 'N/A';
  }
  if (d.cpu_power > 0) {
    document.getElementById('cpuPowerVal').textContent = d.cpu_power.toFixed(1) + ' W';
  } else {
    document.getElementById('cpuPowerVal').textContent = 'N/A';
  }
  document.getElementById('uptime').textContent = fmtUptime(d.uptime_ms);

  var rl = d.ram_load.toFixed(1);
  document.getElementById('ramLoadVal').textContent = rl + '%';
  ramChart.push(d.ram_load);
  document.getElementById('ramUsed').textContent = fmtBytes(d.ram_used_bytes);
  document.getElementById('ramTotal').textContent = '/ ' + fmtBytes(d.ram_total_bytes);

  var gpuDiv = document.getElementById('gpuSections');
  if (!d.gpus || d.gpus.length === 0) { gpuDiv.innerHTML = ''; return; }
  var html = '';
  for (var i = 0; i < d.gpus.length; i++) {
    var g = d.gpus[i]; var col = GPU_COLORS[i % GPU_COLORS.length];
    var cid = 'gpu' + i + 'chart';
    if (!gpuCharts[cid]) gpuCharts[cid] = makeChart(cid, col);
    html += '<div class="section">';
    html += '<div class="gpu-title" style="color:'+col+'">GPU '+i+': '+esc(g.name)+'</div>';
    if (g.load_percent >= 0) {
      gpuCharts[cid].push(g.load_percent);
      html += '<canvas id="'+cid+'" class="chart"></canvas>';
      html += '<div class="row"><span class="label">Load</span><span class="value">'+g.load_percent.toFixed(1)+'%</span></div>';
    }
    if (g.temperature >= 0) {
      var tc2 = tempColor(g.temperature);
      html += '<div class="row"><span class="label">Temp</span><span class="'+tc2+' value">'+g.temperature.toFixed(0)+' C</span></div>';
    }
    if (g.vram_total_bytes > 0) {
      var vp = g.vram_used_bytes*100/g.vram_total_bytes;
      html += '<div class="row"><span class="label">VRAM</span><span class="value">'+vp.toFixed(1)+'%</span></div>';
    }
    if (g.power_watts > 0) {
      html += '<div class="row"><span class="label">Power</span><span class="value">'+g.power_watts.toFixed(1)+' W</span></div>';
    }
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
            (unsigned long long)g.vramTotalBytes, g.powerWatts);
    }
    off += snprintf(buf + off, sizeof(buf) - off, "]}");
    return std::string(buf, static_cast<size_t>(off));
}

static void HttpLoop(int port)
{
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) { fprintf(stderr, "HTTP: WSAStartup failed\n"); return; }
    SOCKET srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (srv == INVALID_SOCKET) { fprintf(stderr, "HTTP: socket() failed err=%d\n", WSAGetLastError()); WSACleanup(); return; }
    int opt = 1; setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
    sockaddr_in addr{}; addr.sin_family=AF_INET; addr.sin_addr.s_addr=INADDR_ANY; addr.sin_port=htons((unsigned short)port);
    if (bind(srv, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR)
    { fprintf(stderr, "HTTP: bind() failed port=%d err=%d\n", port, WSAGetLastError()); closesocket(srv); WSACleanup(); return; }
    if (listen(srv, 4) == SOCKET_ERROR) { fprintf(stderr, "HTTP: listen() failed err=%d\n", WSAGetLastError()); closesocket(srv); WSACleanup(); return; }
    printf("HTTP server listening on port %d\nOpen http://localhost:%d in your browser\n", port, port);
    fflush(stdout);
    while (true)
    {
        fd_set rf; FD_ZERO(&rf); FD_SET(srv, &rf);
        timeval tv{}; tv.tv_sec=1; tv.tv_usec=0;
        if (select(0, &rf, nullptr, nullptr, &tv) <= 0 || !FD_ISSET(srv, &rf)) continue;
        SOCKET client = accept(srv, nullptr, nullptr);
        if (client == INVALID_SOCKET) continue;
        char req[2048] = {}; recv(client, req, sizeof(req)-1, 0);
        std::string resp;
        if (strstr(req, "GET /api/metrics") != nullptr)
        {
            CpuData cpu = MetricsGetCpu();
            RamData ram = MetricsGetRam();
            auto g2 = MetricsGetGpus();
            // Build JSON with current snapshot
            char buf[8192]; int off = 0;
            std::string es;
            for (wchar_t c : cpu.cpuName)
            { if (c == L'\\' || c == L'"') es += '\\'; else if (c < 128) es += (char)c; }
            off = snprintf(buf, sizeof(buf),
                "{\"cpu_load\":%.1f,\"cpu_temp\":%.1f,\"cpu_power\":%.1f,"
                 "\"ram_load\":%.1f,\"ram_used_bytes\":%llu,\"ram_total_bytes\":%llu,"
                 "\"uptime_ms\":%llu,\"cpu_name\":\"%s\",\"gpus\":[",
                cpu.loadPercent, cpu.temperature, cpu.powerWatts,
                ram.loadPercent,
                (unsigned long long)(ram.totalBytes > ram.freeBytes ? ram.totalBytes - ram.freeBytes : 0),
                (unsigned long long)ram.totalBytes,
                (unsigned long long)cpu.uptimeMs, es.c_str());
            for (size_t i = 0; i < g2.size(); ++i)
            {
                if (i > 0) off += snprintf(buf+off, sizeof(buf)-off, ",");
                off += snprintf(buf+off, sizeof(buf)-off,
                    "{\"name\":\"%s\",\"load_percent\":%.1f,\"temperature\":%.1f,"
                     "\"vram_used_bytes\":%llu,\"vram_total_bytes\":%llu,"
                     "\"power_watts\":%.1f}",
                    g2[i].name.c_str(), g2[i].loadPercent, g2[i].temperature,
                    (unsigned long long)g2[i].vramUsedBytes,
                    (unsigned long long)g2[i].vramTotalBytes, g2[i].powerWatts);
            }
            off += snprintf(buf+off, sizeof(buf)-off, "]}");
            resp = std::string("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n") + std::string(buf, (size_t)off);
        }
        else if (strstr(req, "GET /") != nullptr)
        {
            resp = std::string("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n") + kHtmlPage;
        }
        else { resp = "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\nNot Found"; }
        send(client, resp.c_str(), (int)resp.size(), 0);
        closesocket(client);
    }
    closesocket(srv); WSACleanup();
}

void StartHttpServer(int port) { std::thread(HttpLoop, port).detach(); }
#else
static void StartHttpServer(int) {}
#endif // PLATFORM_WINDOWS
