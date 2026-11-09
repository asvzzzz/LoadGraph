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

// HTML dashboard with canvas-based rolling charts.
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
  .value { color: #fff; font-weight: bold; font-size: 14px; min-width: 80px; text-align: right; }
  canvas.chart { background: #0f3460; border-radius: 4px; display: block; width: 100%; height: 50px; margin: 4px 0; }
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
  <div class="row">
    <span class="label">CPU Temp</span>
    <span class="value" id="cpuTempVal">N/A</span>
  </div>
  <canvas id="tempChart" class="chart"></canvas>
  <div class="row">
    <span class="label">Power</span>
    <span class="value" id="cpuPowerVal">N/A</span>
  </div>
  <canvas id="powerChart" class="chart"></canvas>
  <div class="row">
    <span class="label">Uptime</span>
    <span class="value" id="uptime">--</span>
  </div>
</div>

<div class="section">
  <div class="row">
    <span class="label">RAM Used</span>
    <span class="value" id="ramUsedVal">-- GB</span>
  </div>
  <canvas id="ramChart" class="chart"></canvas>
  <div class="row">
    <span class="label">RAM Total</span>
    <span class="value" id="ramTotalVal">-- GB</span>
  </div>
</div>

<div id="gpuSections"></div>

<div class="footer">Auto-refresh every 1s | LoadGraph</div>

<script>
const GPU_COLORS = ['#76B900','#00bcd4','#ffc107','#f44336','#e91e63'];
const MAX_P = 60;

// Rolling chart — stores raw values and scales to 0-100% for display.
// yMax defines the max value for scaling (temp in C, power in W, load in %).
function makeChart(id, color, yMax) {
    const c = document.getElementById(id);
    if (!c) return null;
    c.width  = (c.parentElement.offsetWidth || 400) * 2;
    c.height = 100;
    const ctx = c.getContext('2d');
    const data = [];
    const ymax = yMax || 100;
    return {
        push(v) {
            data.push(Math.max(0, v));
            if (data.length > MAX_P) data.shift();
            draw();
        },
        el: c
    };
    function draw() {
        const W = c.width, H = c.height;
        ctx.clearRect(0, 0, W, H);
        ctx.strokeStyle = 'rgba(255,255,255,0.07)';
        ctx.lineWidth = 1;
        for (let y = 0; y <= 4; y++) {
            const yy = H - (y / 4) * H;
            ctx.beginPath(); ctx.moveTo(0, yy); ctx.lineTo(W, yy); ctx.stroke();
        }
        if (data.length < 2) return;
        const step = W / (MAX_P - 1);
        ctx.beginPath();
        ctx.moveTo(0, H);
        for (let i = 0; i < data.length; i++)
            ctx.lineTo(i * step, H - (data[i] / ymax) * H);
        ctx.lineTo((data.length - 1) * step, H);
        ctx.closePath();
        const grad = ctx.createLinearGradient(0, 0, 0, H);
        grad.addColorStop(0, color + 'cc');
        grad.addColorStop(1, color + '22');
        ctx.fillStyle = grad;
        ctx.fill();
        ctx.beginPath();
        for (let i = 0; i < data.length; i++) {
            const x = i * step, y = H - (data[i] / ymax) * H;
            i === 0 ? ctx.moveTo(x, y) : ctx.lineTo(x, y);
        }
        ctx.strokeStyle = color;
        ctx.lineWidth = 2;
        ctx.stroke();
    }
}

let cpuChart   = makeChart('cpuChart',  '#76B900', 100);
let tempChart  = makeChart('tempChart', '#00bcd4',  100);
let powerChart = makeChart('powerChart','#ffc107',  250);
let ramChart   = makeChart('ramChart',  '#e91e63',  64);

const gpuCharts = {};

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

async function fetchMetrics() {
  try { var r = await fetch('/api/metrics'); var d = await r.json(); updateDashboard(d); } catch(e) {}
}

function getOrCreateGpuSection(i, name, col) {
  var div = document.getElementById('gpusec'+i);
  if (div) return div;
  div = document.createElement('div');
  div.className = 'section';
  div.id = 'gpusec'+i;
  div.innerHTML = '<div class="gpu-title" style="color:'+col+'">GPU '+i+': '+esc(name)+'</div>';
  document.getElementById('gpuSections').appendChild(div);
  return div;
}

function updateDashboard(d) {
  document.getElementById('cpuName').textContent = d.cpu_name || '';
  document.getElementById('cpuLoadVal').textContent = d.cpu_load.toFixed(1) + '%';
  if (cpuChart) cpuChart.push(d.cpu_load);

  if (d.cpu_temp >= 0) {
    var tc = tempColor(d.cpu_temp);
    document.getElementById('cpuTempVal').innerHTML = '<span class="'+tc+'">'+d.cpu_temp.toFixed(0)+' C</span>';
    if (tempChart) tempChart.push(d.cpu_temp);
  } else {
    document.getElementById('cpuTempVal').textContent = 'N/A';
  }

  if (d.cpu_power > 0) {
    document.getElementById('cpuPowerVal').textContent = d.cpu_power.toFixed(1) + ' W';
    if (powerChart) powerChart.push(d.cpu_power);
  } else {
    document.getElementById('cpuPowerVal').textContent = 'N/A';
  }

  document.getElementById('uptime').textContent = fmtUptime(d.uptime_ms);

  var ramGb = (d.ram_used_bytes / (1024*1024*1024));
  var totGb = (d.ram_total_bytes / (1024*1024*1024));
  document.getElementById('ramUsedVal').textContent = ramGb.toFixed(1) + ' GB';
  document.getElementById('ramTotalVal').textContent = totGb.toFixed(1) + ' GB';
  if (ramChart) { ramChart.push(ramGb); ramChart.ymax = Math.max(totGb, ramChart.ymax || totGb); }

  // GPU sections — preserve DOM, update in place
  var gpuDiv = document.getElementById('gpuSections');
  var existingIds = {};
  gpuDiv.querySelectorAll('.section').forEach(function(el) {
    var m = el.id.match(/^gpusec(\d+)$/);
    if (m) existingIds[parseInt(m[1])] = el;
  });

  if (!d.gpus || d.gpus.length === 0) { gpuDiv.innerHTML = ''; return; }

  for (var i = 0; i < d.gpus.length; i++) {
    var g = d.gpus[i];
    var col = GPU_COLORS[i % GPU_COLORS.length];
    var sec = existingIds[i] || getOrCreateGpuSection(i, g.name, col);

    // Remove old chart canvases to rebuild
    var oldCanvases = sec.querySelectorAll('canvas');
    oldCanvases.forEach(function(c) { c.remove(); });
    // Remove old metric rows
    var oldRows = sec.querySelectorAll('.row');
    oldRows.forEach(function(r) { r.remove(); });

    var cid = 'gpu'+i+'chart';
    if (!gpuCharts[cid]) gpuCharts[cid] = makeChart(cid, col, 100);

    if (g.load_percent >= 0) {
      var c1 = document.createElement('canvas');
      c1.id = cid; c1.className = 'chart';
      sec.appendChild(c1);
      if (gpuCharts[cid]) gpuCharts[cid].push(g.load_percent);
      var r1 = document.createElement('div');
      r1.className = 'row';
      r1.innerHTML = '<span class="label">Load</span><span class="value">'+g.load_percent.toFixed(1)+'%</span>';
      sec.appendChild(r1);
    }
    if (g.temperature >= 0) {
      var tc2 = tempColor(g.temperature);
      var r2 = document.createElement('div');
      r2.className = 'row';
      r2.innerHTML = '<span class="label">Temp</span><span class="'+tc2+' value">'+g.temperature.toFixed(0)+' C</span>';
      sec.appendChild(r2);
    }
    if (g.vram_total_bytes > 0) {
      var vp = g.vram_used_bytes*100/g.vram_total_bytes;
      var r3 = document.createElement('div');
      r3.className = 'row';
      r3.innerHTML = '<span class="label">VRAM</span><span class="value">'+vp.toFixed(1)+'% ('+fmtBytes(g.vram_used_bytes)+'/'+fmtBytes(g.vram_total_bytes)+')</span>';
      sec.appendChild(r3);
    }
    if (g.power_watts > 0) {
      var r4 = document.createElement('div');
      r4.className = 'row';
      r4.innerHTML = '<span class="label">Power</span><span class="value">'+g.power_watts.toFixed(1)+' W</span>';
      sec.appendChild(r4);
    }
  }

  // Remove stale sections
  var allSecs = gpuDiv.querySelectorAll('.section');
  allSecs.forEach(function(el) {
    var m = el.id.match(/^gpusec(\d+)$/);
    if (m && parseInt(m[1]) >= d.gpus.length) el.remove();
  });
}

setInterval(fetchMetrics, 1000);
fetchMetrics();
</script>
</body>
</html>)";

static std::string BuildJson()
{
    // Sample CPU load NOW so powerWatts estimate is fresh (SampleLoad also
    // reads the PDH counter and estimates power from TDP + core count).
    MetricsSampleCpuLoad();

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
            resp = std::string("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n") + BuildJson();
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
