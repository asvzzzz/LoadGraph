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

// HTML dashboard — layout matches the GUI exactly.
//   CPU section : Row1=CPU Load | RAM Usage, Row2=Power(full), Row3=Temp(±Fan)
//   GPU per card: Title, Row1=Load|Temp, Row2=VRAM|Fan, Row3=Power(full)
// All value labels are inline: "Label 42" instead of label left / value right.
static const char* kHtmlPage = R"(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>LoadGraph</title>
<style>
  * { margin: 0; padding: 0; box-sizing: border-box; }
  html, body { background: #1a1a2e; color: #e0e0e0; font-family: Consolas, monospace;
               height: 100vh; overflow: hidden; }

  h1  { color: #76B900; margin: 0 0 8px; font-size: 18px; padding: 0 16px; flex-shrink: 0; }
  .cpu-name { color: #00bcd4; }

  /* Main scrollable content area — fills remaining viewport height. */
  .content { display: flex; flex-direction: column; flex: 1 1 auto; min-height: 0;
             padding: 0 16px 16px; overflow-y: auto; }

  /* Sections stack vertically. Each section grows proportionally via flex. */
  .section { background: #16213e; border-radius: 8px; padding: 10px 12px;
             margin-bottom: 8px; display: flex; flex-direction: column; gap: 4px;
             /* CPU section gets proportional share of available height. */
             flex: 2 2 auto; min-height: 0; }

  /* GPU sections also grow proportionally. */
  .gpu-section { background: #16213e; border-radius: 8px; padding: 8px 12px;
                 margin-bottom: 8px; display: flex; flex-direction: column; gap: 4px;
                 flex: 1 1 auto; min-height: 0; }

  /* Row of two charts side by side — each cell grows to fill row height. */
  .chart-row { display: flex; gap: 8px; flex: 1 1 auto; min-height: 0; }
  .chart-cell { flex: 1 1 0; display: flex; flex-direction: column; min-width: 0; min-height: 0; }

  /* Single full-width chart row — grows to fill available height. */
  .full-row { flex: 1 1 auto; min-height: 0; display: flex; flex-direction: column; }

  /* Inline value label — fixed height, no growth. */
  .val-row { display: block; height: 20px; line-height: 20px; flex-shrink: 0;
             font-size: 12px; color: #aaa; white-space: nowrap; overflow: hidden;
             text-overflow: ellipsis; }
  .val-val { color: #fff; font-weight: bold; font-size: 13px; }

  /* Canvas fills remaining cell height — proportional resize. */
  canvas.chart { background: #0f3460; border-radius: 4px; display: block;
                 width: 100%; flex: 1 1 auto; min-height: 0; }

  .gpu-title { color: #76B900; font-size: 13px; font-weight: bold;
               margin-bottom: 2px; flex-shrink: 0; white-space: nowrap;
               overflow: hidden; text-overflow: ellipsis; }
  .green  { color: #76B900; }
  .cyan   { color: #00bcd4; }
  .yellow { color: #ffc107; }
  .red    { color: #f44336; }
</style>
</head>
<body>
<h1>LoadGraph <span id="cpuName" class="cpu-name"></span></h1>
<div class="content">

<!-- CPU section — rows mirror GUI layout -->
<div class="section">
  <!-- Row 1: CPU Load | RAM Usage -->
  <div class="chart-row">
    <div class="chart-cell">
      <div class="val-row">CPU Load <span class="val-val" id="cpuLoadVal">--%</span></div>
      <canvas id="cpuChart" class="chart"></canvas>
    </div>
    <div class="chart-cell">
      <div class="val-row">RAM Usage <span class="val-val" id="ramUsedVal">-- GB</span></div>
      <canvas id="ramChart" class="chart"></canvas>
    </div>
  </div>
  <!-- Row 2: CPU Power (full width, shown only when TDP > 0) -->
  <div class="full-row" id="cpuPowerRow" style="display:none">
    <div class="val-row">CPU Power <span class="val-val" id="cpuPowerVal">N/A</span></div>
    <canvas id="powerChart" class="chart"></canvas>
  </div>
  <!-- Row 3: Total Power (full width) -->
  <div class="full-row" id="totalPowerRow" style="display:none">
    <div class="val-row">Total Power <span class="val-val" id="totalPowerVal">N/A</span></div>
    <canvas id="totalPowerChart" class="chart"></canvas>
  </div>
  <!-- Row 4: CPU Temp | Fan -->
  <div class="chart-row">
    <div class="chart-cell">
      <div class="val-row">CPU Temp <span class="val-val" id="cpuTempVal">N/A</span></div>
      <canvas id="tempChart" class="chart"></canvas>
    </div>
    <div class="chart-cell" id="fanCell" style="display:none">
      <div class="val-row">CPU Fan <span class="val-val" id="fanVal">N/A</span></div>
      <canvas id="fanChart" class="chart"></canvas>
    </div>
  </div>
</div>

<div id="gpuSections"></div>

</div>

<script>
const GPU_COLORS = ['#76B900','#00bcd4','#ffc107','#f44336','#e91e63'];
const MAX_P = 60;

// Rolling chart — looks up canvas by ID on each draw() so it works after DOM re-build.
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
let cpuChart      = makeChart('cpuChart',       '#76B900', 100);
let ramChart      = makeChart('ramChart',       '#e91e63', 128);
let tempChart     = makeChart('tempChart',      '#00bcd4', 100);
let powerChart    = null;
let fanChart      = null;
let totalPowerChart = null;

const gpuCharts  = {};

// ── Helpers ───────────────────────────────────────────────────────────────
function esc(s)  { return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;'); }
function gb(b)   { return (b / (1024*1024*1024)).toFixed(1); }
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

  // CPU Power — row shown only when TDP > 0.
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

  // Total Power — CPU + all GPUs with valid power readings.
  var totalW = -1;
  if (d.cpu_power > 0) totalW += d.cpu_power;
  if (d.gpus && d.gpus.length > 0) {
    for (var gi = 0; gi < d.gpus.length; gi++) {
      if (d.gpus[gi].power_watts > 0) totalW += d.gpus[gi].power_watts;
    }
  }
  var tpRow = document.getElementById('totalPowerRow');
  if (totalW > 0) {
    tpRow.style.display = '';
    if (!totalPowerChart) totalPowerChart = makeChart('totalPowerChart', '#ff5722', 500);
    document.getElementById('totalPowerVal').textContent = totalW.toFixed(1) + ' W';
    totalPowerChart.push(totalW);
  } else {
    tpRow.style.display = 'none';
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
  div.className = 'gpu-section';
  div.id = 'gpusec'+i;
  div.innerHTML = '<div class="gpu-title" style="color:'+col+'">GPU '+i+': '+esc(name)+'</div>';
  document.getElementById('gpuSections').appendChild(div);
  return div;
}

function clearChartsFrom(sec) {
  var rows = sec.querySelectorAll('.chart-row, .full-row');
  rows.forEach(function(r) { r.remove(); });
}

function updateGpu(d) {
  var gpuDiv = document.getElementById('gpuSections');
  var existing = {};
  gpuDiv.querySelectorAll('.gpu-section').forEach(function(el) {
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

    function addSideBySide(lLabel, lValId, lCanvas, rLabel, rValId, rCanvas) {
      var row = document.createElement('div');
      row.className = 'chart-row';
      row.innerHTML =
        '<div class="chart-cell"><div class="val-row">'+esc(lLabel)+' <span class="val-val" id="'+lValId+'">--</span></div>' +
         '<canvas id="'+lCanvas+'" class="chart"></canvas></div>' +
        '<div class="chart-cell"><div class="val-row">'+esc(rLabel)+' <span class="val-val" id="'+rValId+'">--</span></div>' +
         '<canvas id="'+rCanvas+'" class="chart"></canvas></div>';
      sec.appendChild(row);
    }
    function addFullRow(fLabel, fValId, fCanvas) {
      var row = document.createElement('div');
      row.className = 'full-row';
      row.innerHTML = '<div class="val-row">'+esc(fLabel)+' <span class="val-val" id="'+fValId+'">--</span></div>' +
                      '<canvas id="'+fCanvas+'" class="chart"></canvas>';
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
    if (g.power_watts > 0 || g.power_limit_watts > 0) {
      var limit = g.power_limit_watts;
      if (limit <= 0) limit = 400;
      addFullRow('Power','gpu'+i+'PowVal','gpu'+i+'PowChart');
      var pKey = 'gpu'+i+'PowChart';
      if (!gpuCharts[pKey]) gpuCharts[pKey] = makeChart(pKey, col, Math.max(250, limit*1.1));
      document.getElementById('gpu'+i+'PowVal').textContent = g.power_watts.toFixed(1)+' W';
      gpuCharts[pKey].push(g.power_watts);
    } else {
      addFullRow('Power','gpu'+i+'PowVal','gpu'+i+'PowChart');
      document.getElementById('gpu'+i+'PowVal').textContent = 'N/A';
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
