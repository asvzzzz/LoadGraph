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

// HTML dashboard — layout matches the GUI exactly:
//   CPU section : Row1=CPU Load | RAM Usage, Row2=Power(full), Row3=Temp(±Fan)
//   GPU per card: Title, Row1=Load|Temp, Row2=VRAM|Fan, Row3=Power(full)
static const char* kHtmlPage = R"(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>LoadGraph</title>
<style>
  * { margin: 0; padding: 0; box-sizing: border-box; }
  body { background: #1a1a2e; color: #e0e0e0; font-family: Consolas, monospace;
         padding: 16px; height: 100vh; overflow-y: auto; }

  h1  { color: #76B900; margin-bottom: 8px; font-size: 18px; }
  .cpu-name { color: #00bcd4; }

  /* Each section — fixed height rows, no vertical growth per tick. */
  .section { background: #16213e; border-radius: 8px; padding: 10px 12px;
             margin-bottom: 8px; display: flex; flex-direction: column; gap: 6px; }

  /* Row of two charts — each takes half the width, fixed height. */
  .chart-row { display: flex; gap: 8px; }
  .chart-cell { flex: 1 1 0; display: flex; flex-direction: column; min-width: 0; }

  /* Single full-width chart row. */
  .full-row { }

  /* Value label above each canvas — fixed height, no growth. */
  .val-row { display: flex; align-items: center; gap: 8px; height: 22px; flex-shrink: 0; }
  .label  { color: #888; font-size: 12px; width: 80px; flex-shrink: 0; }
  .value  { color: #fff; font-weight: bold; font-size: 13px; flex: 1; text-align: right; }

  /* Fixed-height canvas — never grows beyond CHART_H. */
  canvas.chart { background: #0f3460; border-radius: 4px; display: block;
                 width: 100%; height: 50px; flex-shrink: 0; }

  .gpu-title { color: #76B900; font-size: 13px; font-weight: bold;
               margin-bottom: 4px; flex-shrink: 0; }
  .footer    { text-align: center; color: #555; font-size: 11px; margin-top: 8px; }

  .green  { color: #76B900; }
  .cyan   { color: #00bcd4; }
  .yellow { color: #ffc107; }
  .red    { color: #f44336; }
</style>
</head>
<body>
<h1>NVIDIA LoadGraph <span id="cpuName" class="cpu-name"></span></h1>

<!-- CPU section — 3 chart rows matching GUI layout -->
<div class="section" id="cpuSection">
  <!-- Row 1: CPU Load | RAM Usage -->
  <div class="chart-row">
    <div class="chart-cell">
      <div class="val-row"><span class="label">CPU Load</span><span class="value" id="cpuLoadVal">--%</span></div>
      <canvas id="cpuChart" class="chart"></canvas>
    </div>
    <div class="chart-cell">
      <div class="val-row"><span class="label">RAM Usage</span><span class="value" id="ramUsedVal">-- GB</span></div>
      <canvas id="ramChart" class="chart"></canvas>
    </div>
  </div>
  <!-- Row 2: CPU Power (full width) -->
  <div class="full-row" id="cpuPowerRow" style="display:none">
    <div class="val-row"><span class="label">CPU Power</span><span class="value" id="cpuPowerVal">N/A</span></div>
    <canvas id="powerChart" class="chart"></canvas>
  </div>
  <!-- Row 3: CPU Temp | Fan -->
  <div class="chart-row" id="cpuTempRow">
    <div class="chart-cell">
      <div class="val-row"><span class="label">CPU Temp</span><span class="value" id="cpuTempVal">N/A</span></div>
      <canvas id="tempChart" class="chart"></canvas>
    </div>
    <div class="chart-cell" id="fanCell" style="display:none">
      <div class="val-row"><span class="label">CPU Fan</span><span class="value" id="fanVal">N/A</span></div>
      <canvas id="fanChart" class="chart"></canvas>
    </div>
  </div>
</div>

<div id="gpuSections"></div>

<div class="footer">Auto-refresh every 1s | LoadGraph</div>

<script>
const GPU_COLORS = ['#76B900','#00bcd4','#ffc107','#f44336','#e91e63'];
const MAX_P = 60;

// Rolling chart — looks up canvas by ID on each draw() so it works after DOM re-build.
// curYmax is mutable via .ymax setter so RAM/VRAM can grow dynamically.
function makeChart(id, color, yMax) {
    const defYmax = yMax || 100;
    const data = [];
    var curYmax = defYmax;
    return {
        get ymax() { return curYmax; },
        set ymax(v) { curYmax = Math.max(curYmax, v); },
        push(v) {
            data.push(Math.max(0, v));
            if (data.length > MAX_P) data.shift();
            draw();
        }
    };
    function draw() {
        const c = document.getElementById(id);
        if (!c || !c.parentElement) return;
        // HiDPI + read actual rendered size so resize propagates.
        var pw = c.parentElement.offsetWidth  || 400;
        var ph = c.parentElement.offsetHeight || 80;
        c.width  = pw * 2;
        c.height = ph * 2;
        const ctx = c.getContext('2d');
        const W = c.width, H = c.height, ymax = curYmax;
        ctx.clearRect(0, 0, W, H);
        // Grid lines.
        ctx.strokeStyle = 'rgba(255,255,255,0.07)'; ctx.lineWidth = 1;
        for (let y = 0; y <= 4; y++) {
            const yy = H - (y / 4) * H;
            ctx.beginPath(); ctx.moveTo(0, yy); ctx.lineTo(W, yy); ctx.stroke();
        }
        if (data.length < 2) return;
        const step = W / (MAX_P - 1);
        // Filled area.
        ctx.beginPath(); ctx.moveTo(0, H);
        for (let i = 0; i < data.length; i++)
            ctx.lineTo(i * step, H - (data[i] / ymax) * H);
        ctx.lineTo((data.length - 1) * step, H); ctx.closePath();
        const grad = ctx.createLinearGradient(0, 0, 0, H);
        grad.addColorStop(0, color + 'cc'); grad.addColorStop(1, color + '22');
        ctx.fillStyle = grad; ctx.fill();
        // Line.
        ctx.beginPath();
        for (let i = 0; i < data.length; i++) {
            const x = i * step, y = H - (data[i] / ymax) * H;
            i === 0 ? ctx.moveTo(x, y) : ctx.lineTo(x, y);
        }
        ctx.strokeStyle = color; ctx.lineWidth = 2; ctx.stroke();
    }
}

// ── Charts ────────────────────────────────────────────────────────────────
let cpuChart   = makeChart('cpuChart', '#76B900', 100);
let ramChart   = makeChart('ramChart', '#e91e63', 128); // grows dynamically
let tempChart  = makeChart('tempChart','#00bcd4', 100);
let powerChart = null;       // created only when TDP > 0
let fanChart   = null;       // created only when CPU fan exists

const gpuCharts = {};        // key → chart object

// ── Helpers ───────────────────────────────────────────────────────────────
function esc(s)  { return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;'); }
function gb(b)   { return (b / (1024*1024*1024)).toFixed(1); }
function fmtBytes(b) {
  if (!b||b<1024) return '0 B';
  if (b<1048576)  return (b/1024).toFixed(0)+' KB';
  if (b<1073741824) return gb(b)+' GB';
  return (b/1099511627776).toFixed(1)+' TB';
}
function fmtUptime(ms) {
  var s=Math.floor(ms/1000), d=Math.floor(s/86400); s%=86400;
  var h=Math.floor(s/3600); s%=3600; var m=Math.floor(s/60); s%=60;
  if (d>0) return d+'d '+h+'h '+m+'m';
  if (h>0) return h+'h '+m+'m '+s+'s';
  return m+'m '+s+'s';
}
function tempColor(t) { return t > 80 ? 'red' : t > 65 ? 'yellow' : 'green'; }

// ── CPU section update ────────────────────────────────────────────────────
function updateCpu(d) {
  document.getElementById('cpuName').textContent = d.cpu_name || '';
  document.getElementById('cpuLoadVal').textContent = d.cpu_load.toFixed(1) + '%';
  cpuChart.push(d.cpu_load);

  // RAM — yMax grows with total RAM so the chart is always proportional.
  var ramGb = d.ram_used_bytes / (1024*1024*1024);
  var totGb = d.ram_total_bytes   / (1024*1024*1024);
  document.getElementById('ramUsedVal').textContent = ramGb.toFixed(1) + ' GB';
  if (ramChart) { ramChart.push(ramGb); ramChart.ymax = Math.max(totGb, ramChart.ymax || totGb); }

  // CPU Power — row shown only when TDP > 0 (matches GUI).
  var ppRow = document.getElementById('cpuPowerRow');
  if (d.cpu_power > 0) {
    if (!powerChart && d.tdp_watts > 0) {
      powerChart = makeChart('powerChart', '#ffc107', Math.max(250, (d.tdp_watts||150)*1.1));
      ppRow.style.display = '';
    }
    document.getElementById('cpuPowerVal').textContent = d.cpu_power.toFixed(1) + ' W';
    if (powerChart) powerChart.push(d.cpu_power);
  } else {
    ppRow.style.display = 'none';
  }

  // CPU Temp.
  var tcEl = document.getElementById('cpuTempVal');
  if (d.cpu_temp >= 0) {
    tcEl.innerHTML = '<span class="'+tempColor(d.cpu_temp)+'">'+d.cpu_temp.toFixed(0)+' C</span>';
    tempChart.push(d.cpu_temp);
  } else { tcEl.textContent = 'N/A'; }

  // CPU Fan.
  if (d.fan_rpm > 0) {
    document.getElementById('fanCell').style.display = '';
    if (!fanChart) fanChart = makeChart('fanChart', '#9c27b0', 3000);
    document.getElementById('fanVal').textContent = d.fan_rpm + ' RPM';
    fanChart.push(d.fan_rpm);
  } else {
    document.getElementById('fanCell').style.display = 'none';
  }
}

// ── GPU sections — preserve DOM, update in place ──────────────────────────
function getOrCreateGpuSec(i, name, col) {
  var div = document.getElementById('gpusec'+i);
  if (div) return div;
  div = document.createElement('div');
  div.className = 'section';
  div.id = 'gpusec'+i;
  // Title stays static inside the section.
  div.innerHTML = '<div class="gpu-title" style="color:'+col+'">GPU '+i+': '+esc(name)+'</div>';
  document.getElementById('gpuSections').appendChild(div);
  return div;
}

function clearChartsFrom(sec) {
  // Remove old canvases but keep the title.
  var rows = sec.querySelectorAll('.chart-row, .full-row');
  rows.forEach(function(r) { r.remove(); });
}

function updateGpu(d) {
  var gpuDiv = document.getElementById('gpuSections');
  var existing = {};
  gpuDiv.querySelectorAll('.section').forEach(function(el) {
    var m = el.id.match(/^gpusec(\d+)$/);
    if (m) existing[parseInt(m[1])] = el;
  });

  if (!d.gpus || d.gpus.length === 0) { gpuDiv.innerHTML = ''; return; }

  for (var i = 0; i < d.gpus.length; i++) {
    var g   = d.gpus[i];
    var col = GPU_COLORS[i % GPU_COLORS.length];
    var sec = existing[i] || getOrCreateGpuSec(i, g.name, col);

    // Keep title, rebuild chart rows.
    var title = sec.querySelector('.gpu-title');
    clearChartsFrom(sec);
    sec.appendChild(title);

    function addChartRow(labelId, valId, canvasId, color, yMax) {
      var row = document.createElement('div');
      row.className = 'full-row';
      row.innerHTML = '<div class="val-row"><span class="label">'+labelId+'</span><span class="value" id="'+valId+'">--</span></div>';
      var c = document.createElement('canvas');
      c.id = canvasId; c.className = 'chart';
      row.appendChild(c); sec.appendChild(row);
      return { valEl: document.getElementById(valId), chartId: canvasId, color: color, yMax: yMax };
    }
    function addSideBySide(leftLabel, leftValId, leftCanvas, rightLabel, rightValId, rightCanvas) {
      var row = document.createElement('div');
      row.className = 'chart-row';
      row.innerHTML =
        '<div class="chart-cell"><div class="val-row"><span class="label">'+leftLabel+'</span><span class="value" id="'+leftValId+'">--</span></div>' +
         '<canvas id="'+leftCanvas+'" class="chart"></canvas></div>' +
        '<div class="chart-cell"><div class="val-row"><span class="label">'+rightLabel+'</span><span class="value" id="'+rightValId+'">--</span></div>' +
         '<canvas id="'+rightCanvas+'" class="chart"></canvas></div>';
      sec.appendChild(row);
    }

    // Row 1: Load | Temp.
    addSideBySide('Load','gpu'+i+'LoadVal','gpu'+i+'LoadChart',
                  'Temp', 'gpu'+i+'TempVal','gpu'+i+'TempChart');
    var lcKey = 'gpu'+i+'LoadChart';
    if (!gpuCharts[lcKey]) gpuCharts[lcKey] = makeChart(lcKey, col, 100);
    document.getElementById('gpu'+i+'LoadVal').textContent = g.load_percent.toFixed(1)+'%';
    gpuCharts[lcKey].push(g.load_percent);

    var tvEl = document.getElementById('gpu'+i+'TempVal');
    if (g.temperature >= 0) {
      tvEl.innerHTML = '<span class="'+tempColor(g.temperature)+'">'+g.temperature.toFixed(0)+' C</span>';
      var tcKey = 'gpu'+i+'TempChart';
      if (!gpuCharts[tcKey]) gpuCharts[tcKey] = makeChart(tcKey, col, 100);
      gpuCharts[tcKey].push(g.temperature);
    } else { tvEl.textContent = 'N/A'; }

    // Row 2: VRAM | Fan.
    addSideBySide('VRAM','gpu'+i+'VramVal','gpu'+i+'VramChart',
                  'Fan',  'gpu'+i+'FanVal', 'gpu'+i+'FanChart');
    var vKey = 'gpu'+i+'VramChart';
    if (g.vram_total_bytes > 0) {
      var vGb = g.vram_used_bytes / (1024*1024*1024);
      var vtGb = g.vram_total_bytes / (1024*1024*1024);
      document.getElementById('gpu'+i+'VramVal').textContent = vGb.toFixed(1)+' GB';
      if (!gpuCharts[vKey]) gpuCharts[vKey] = makeChart(vKey, col, 64);
      gpuCharts[vKey].push(vGb);
      gpuCharts[vKey].ymax = Math.max(vtGb, gpuCharts[vKey].ymax || vtGb);
    } else { document.getElementById('gpu'+i+'VramVal').textContent = 'N/A'; }

    var fvEl = document.getElementById('gpu'+i+'FanVal');
    if (g.fan_rpm > 0) {
      fvEl.textContent = g.fan_rpm + ' RPM';
      var fKey = 'gpu'+i+'FanChart';
      if (!gpuCharts[fKey]) gpuCharts[fKey] = makeChart(fKey, '#9c27b0', 3000);
      gpuCharts[fKey].push(g.fan_rpm);
    } else if (g.fan_percent >= 0) {
      fvEl.textContent = g.fan_percent.toFixed(0) + '%';
      var fKey = 'gpu'+i+'FanChart';
      if (!gpuCharts[fKey]) gpuCharts[fKey] = makeChart(fKey, '#9c27b0', 100);
      gpuCharts[fKey].push(g.fan_percent);
    } else { fvEl.textContent = 'N/A'; }

    // Row 3: Power (full width).
    var ppRow = document.createElement('div');
    ppRow.className = 'full-row';
    if (g.power_watts > 0 || g.power_limit_watts > 0) {
      var limit = g.power_limit_watts;
      if (limit <= 0) limit = 400; // fallback
      ppRow.innerHTML = '<div class="val-row"><span class="label">Power</span><span class="value" id="gpu'+i+'PowVal">-- W</span></div>';
      var pc = document.createElement('canvas');
      pc.id = 'gpu'+i+'PowChart'; pc.className = 'chart'; ppRow.appendChild(pc);
      sec.appendChild(ppRow);
      var pKey = 'gpu'+i+'PowChart';
      if (!gpuCharts[pKey]) gpuCharts[pKey] = makeChart(pKey, col, Math.max(250, limit*1.1));
      document.getElementById('gpu'+i+'PowVal').textContent = g.power_watts.toFixed(1)+' W';
      gpuCharts[pKey].push(g.power_watts);
    } else {
      ppRow.innerHTML = '<div class="val-row"><span class="label">Power</span><span class="value" id="gpu'+i+'PowVal">N/A</span></div>';
      sec.appendChild(ppRow);
    }
  }

  // Remove stale sections.
  Object.keys(existing).forEach(function(k) {
    if (parseInt(k) >= d.gpus.length) existing[k].remove();
  });
}

// ── Main loop ─────────────────────────────────────────────────────────────
async function fetchMetrics() {
  try { var r = await fetch('/api/metrics'); var d = await r.json(); updateCpu(d); updateGpu(d); } catch(e) {}
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
         "\"fan_rpm\":%d,\"tdp_watts\":%.1f,"
         "\"ram_load\":%.1f,\"ram_used_bytes\":%llu,\"ram_total_bytes\":%llu,"
         "\"uptime_ms\":%llu,\"cpu_name\":\"%s\",\"gpus\":[",
        cpu.loadPercent,
        cpu.temperature,
        cpu.powerWatts,
        cpu.fanRpm,
        cpu.tdpWatts,
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
             "\"fan_rpm\":%d,\"fan_percent\":%.1f,"
             "\"power_watts\":%.1f,\"power_limit_watts\":%.1f}",
            g.name.c_str(), g.loadPercent, g.temperature,
            (unsigned long long)g.vramUsedBytes,
            (unsigned long long)g.vramTotalBytes, g.fanRpm, g.fanPercent,
            g.powerWatts, g.powerLimitWatts);
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
