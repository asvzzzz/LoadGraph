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
static const char* kHtmlPageHead = R"(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>LoadGraph</title>
<style>
  * { margin: 0; padding: 0; box-sizing: border-box; }
  html, body { height: 100%; background: #1a1a2e; color: #e0e0e0;
               font-family: Consolas, monospace; overflow: hidden; }
  body { display: flex; flex-direction: column; padding: 12px; gap: 8px; }

  h1 { color: #76B900; font-size: 18px; flex: 0 0 auto; }
  .cpu-name { color: #00bcd4; }

  /* Контент занимает всё свободное место, ничего не выталкивает за окно. */
  .content { flex: 1 1 0; min-height: 0; display: flex;
             flex-direction: column; gap: 8px; overflow: hidden; }

  /* Секции делят высоту контента поровну. min-height:0 разрешает им сжиматься. */
  .section, .gpu-section {
    background: #16213e; border-radius: 8px; padding: 8px 10px;
    flex: 1 1 0; min-height: 0;
    display: flex; flex-direction: column; gap: 4px;
  }

  /* Контейнер GPU-карточек — тоже flex-элемент контента. */
  #gpuSections { flex: 1 1 0; min-height: 0;
                 display: flex; flex-direction: column; gap: 8px;
                 overflow: hidden; }
  #gpuSections:empty { display: none; }

  /* Ряды внутри секции тоже тянутся по высоте. */
  .chart-row { display: flex; gap: 8px; flex: 1 1 0; min-height: 0; }
  .full-row  { flex: 1 1 0; min-height: 0;
               display: flex; flex-direction: column; }
  .chart-cell { flex: 1 1 0; min-width: 0; min-height: 0;
                display: flex; flex-direction: column; }

  /* Подпись — фиксированной высоты, чарт забирает остаток. */
  .val-row { flex: 0 0 auto; height: 18px; line-height: 18px;
             font-size: 12px; color: #aaa; white-space: nowrap;
             overflow: hidden; text-overflow: ellipsis; }
  .val-val { color: #fff; font-weight: bold; font-size: 13px; }

  /* Ключевое: вместо height:60px / flex-shrink:0 — растягиваемый чарт. */
  canvas.chart { background: #0f3460; border-radius: 4px; display: block;
                 width: 100%; flex: 1 1 0; min-height: 0; }

  .gpu-title { color: #76B900; font-size: 13px; font-weight: bold;
               flex: 0 0 auto; white-space: nowrap; overflow: hidden;
               text-overflow: ellipsis; }
  .green  { color: #76B900; }
  .cyan   { color: #00bcd4; }
  .yellow { color: #ffc107; }
  .red    { color: #f44336; }
</style>
</head>)";
static const char* kHtmlPageBody = R"(<body>
<h1>LoadGraph <span id="cpuName" class="cpu-name"></span></h1>
<div class="content">

<!-- CPU section — rows mirror GUI layout -->
<div class="section" id="cpuSection">
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
  <!-- Row 3: CPU Temp | Fan -->
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

<!-- Total Power — section hides as a whole when no reading is available -->
<div class="section" id="totalPowerSection" style="display:none">
  <div class="full-row" id="totalPowerRow">
    <div class="val-row">Total Power <span class="val-val" id="totalPowerVal">N/A</span></div>
    <canvas id="totalPowerChart" class="chart"></canvas>
  </div>
</div>

</div>

<script>
const GPU_COLOR     = '#9c27b0'; // default for GPU charts without a dedicated color
const MAX_P = 60;

// Rolling chart. draw() is now exposed AND a ResizeObserver is attached per
// canvas, so the drawing buffer tracks the CSS box on any layout change
// (window resize, CPU-Power row appearing, GPU count changing, etc).
function makeChart(id, color, yMax) {
    const defYmax = yMax || 100;
    const data = [];
    var curYmax = defYmax;
    var observer = null;
    var observedEl = null;

    function draw() {
        const c = document.getElementById(id);
        if (!c || !c.parentElement) return;
        // clientWidth/clientHeight reflect the flex-allocated CSS size.
        // Guard against zero-size during the first tick before layout.
        const cw = c.clientWidth  * 2 || 400;
        const ch = c.clientHeight * 2 || 80;
        if (c.width  !== cw) c.width  = cw;
        if (c.height !== ch) c.height = ch;
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

    // The GPU-section DOM is rebuilt every second; re-observe only when
    // the underlying <canvas> element is actually replaced.
    function attach() {
        const c = document.getElementById(id);
        if (!c || c === observedEl) return;
        if (observer) observer.disconnect();
        observer = new ResizeObserver(function() { draw(); });
        observer.observe(c);
        observedEl = c;
    }

    return {
        get ymax() { return curYmax; },
        set ymax(v) { curYmax = Math.max(curYmax, v); },
        push(v) {
            data.push(Math.max(0, v));
            if (data.length > MAX_P) data.shift();
            attach();
            draw();
        },
        draw: draw
    };
}

// ── Charts ────────────────────────────────────────────────────────────────
let cpuChart      = makeChart('cpuChart',       '#76B900', 100); // green — CPU load
let ramChart      = makeChart('ramChart',       '#AEB4E1', 128); // periwinkle — RAM usage
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
  ramChart.push(ramGb);
  ramChart.ymax = Math.max(totGb, ramChart.ymax || totGb);

  // CPU Power — row shown whenever a positive reading is available.
  var ppRow = document.getElementById('cpuPowerRow');
  if (d.cpu_power > 0) {
    ppRow.style.display = '';
    if (!powerChart) {
      var ymax = (d.tdp_watts > 0) ? Math.max(250, d.tdp_watts * 1.1) : 300;
      powerChart = makeChart('powerChart', '#ffc107', ymax);
    } else if (d.tdp_watts > 0) {
      powerChart.ymax = Math.max(250, d.tdp_watts * 1.1);
    }
    document.getElementById('cpuPowerVal').textContent = d.cpu_power.toFixed(1) + ' W';
    powerChart.push(d.cpu_power);
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
  var tpSec = document.getElementById('totalPowerSection');
  if (totalW > 0) {
    tpSec.style.display = '';
    if (!totalPowerChart) totalPowerChart = makeChart('totalPowerChart', '#ff5722', 500);
    document.getElementById('totalPowerVal').textContent = totalW.toFixed(1) + ' W';
    totalPowerChart.push(totalW);
  } else {
    tpSec.style.display = 'none';
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
  div.innerHTML = '<div class="gpu-title">GPU '+i+': '+esc(name)+'</div>';
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
    var sec  = existing[i] || getOrCreateGpuSec(i, g.name);

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
    if (!gpuCharts[lcKey]) gpuCharts[lcKey] = makeChart(lcKey, '#e91e63', 100); // pink — GPU load
    document.getElementById('gpu'+i+'LoadVal').textContent = g.load_percent.toFixed(1)+'%';
    gpuCharts[lcKey].push(g.load_percent);

    var tvEl = document.getElementById('gpu'+i+'TempVal');
    if (g.temperature >= 0) {
      tvEl.innerHTML = '<span class="'+tempColor(g.temperature)+'">'+g.temperature.toFixed(0)+' C</span>';
      var tcKey = 'gpu'+i+'TempChart';
      if (!gpuCharts[tcKey]) gpuCharts[tcKey] = makeChart(tcKey, '#00bcd4', 100); // cyan — matches CPU Temp
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
      if (!gpuCharts[vKey]) gpuCharts[vKey] = makeChart(vKey, '#AEB4E1', 64); // periwinkle — GPU VRAM
      gpuCharts[vKey].push(vGb);
      gpuCharts[vKey].ymax = Math.max(vtGb, gpuCharts[vKey].ymax || vtGb);
    } else { document.getElementById('gpu'+i+'VramVal').textContent = 'N/A'; }

    var fvEl = document.getElementById('gpu'+i+'FanVal');
    var fKey = 'gpu'+i+'FanChart';
    if (g.fan_rpm > 0) {
      fvEl.textContent = g.fan_rpm + ' RPM';
      if (!gpuCharts[fKey]) gpuCharts[fKey] = makeChart(fKey, '#3154E1', 3000); // deep blue — GPU fan
      gpuCharts[fKey].push(g.fan_rpm);
    } else if (g.fan_percent >= 0) {
      fvEl.textContent = g.fan_percent.toFixed(0) + '%';
      if (!gpuCharts[fKey]) gpuCharts[fKey] = makeChart(fKey, '#3154E1', 100); // deep blue — GPU fan
      gpuCharts[fKey].push(g.fan_percent);
    } else { fvEl.textContent = 'N/A'; }

    // Row 3: Power (full width).
    if (g.power_watts > 0 || g.power_limit_watts > 0) {
      var limit = g.power_limit_watts;
      if (limit <= 0) limit = 400;
      addFullRow('Power','gpu'+i+'PowVal','gpu'+i+'PowChart');
      var pKey = 'gpu'+i+'PowChart';
      if (!gpuCharts[pKey]) gpuCharts[pKey] = makeChart(pKey, '#ffc107', Math.max(250, limit*1.1)); // yellow — matches CPU Power
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

// ── Flex distribution: equal chart heights across the whole page ──────────
// Each .section / .gpu-section lives in a flex column. We set its flex-grow
// to the number of visible chart rows it contains, so every row (CPU, GPU,
// Total) gets the same slice of the window height, no matter how many GPUs
// are present. Sections with more rows therefore get proportionally more
// height, which is exactly what we want.

function countVisibleRows(el) {
    void el.offsetHeight;   // force layout so offsetParent is accurate
    var n = 0;
    el.querySelectorAll(':scope > .chart-row, :scope > .full-row').forEach(function(r) {
        if (r.offsetParent !== null) n++;
    });
    return n;
}

function setFlex(el, grow) {
    if (grow <= 0) return;
    el.style.flexGrow   = String(grow);
    el.style.flexShrink = '1';
    el.style.flexBasis  = '0';
}

function updateFlex() {
    var cpuSec = document.getElementById('cpuSection');
    if (cpuSec) setFlex(cpuSec, countVisibleRows(cpuSec));

    var gpuDiv = document.getElementById('gpuSections');
    var gpuTotal = 0;
    gpuDiv.querySelectorAll(':scope > .gpu-section').forEach(function(gs) {
        var n = countVisibleRows(gs);
        setFlex(gs, n);
        gpuTotal += n;
    });
    if (gpuTotal > 0) setFlex(gpuDiv, gpuTotal);

    var totalSec = document.getElementById('totalPowerSection');
    if (totalSec && totalSec.style.display !== 'none') {
        setFlex(totalSec, countVisibleRows(totalSec));
    }
}

// ── Resize handler — draw() reads the new clientWidth/clientHeight ────────
function redrawAll() {
    Object.keys(gpuCharts).forEach(function(k) {
        if (gpuCharts[k] && typeof gpuCharts[k].draw === 'function') gpuCharts[k].draw();
    });
    [cpuChart, ramChart, tempChart, powerChart, fanChart, totalPowerChart]
        .filter(function(c) { return c && typeof c.draw === 'function'; })
        .forEach(function(c) { c.draw(); });
}
window.addEventListener('resize', redrawAll);

// ── Main loop ─────────────────────────────────────────────────────────────
async function fetchMetrics() {
  try { var r = await fetch('/api/metrics'); var d = await r.json(); updateCpu(d); updateGpu(d); updateFlex(); } catch(e) {}
}
updateFlex();
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
        sockaddr_in clientAddr{};
        int clientLen = sizeof(clientAddr);
        SOCKET client = accept(srv, (sockaddr*)&clientAddr, &clientLen);
        if (client == INVALID_SOCKET) continue;
        char ipStr[64] = {};
        inet_ntop(AF_INET, &clientAddr.sin_addr, ipStr, sizeof(ipStr));
        char req[2048] = {};
        int n = recv(client, req, sizeof(req)-1, 0);
        if (n <= 0) { closesocket(client); continue; }
        req[n] = '\0';
        // Log: client IP + first line of request.
        char firstLine[256] = {};
        strncpy(firstLine, req, sizeof(firstLine)-1);
        char* nl = strchr(firstLine, '\n'); if (nl) *nl = '\0';
        printf("HTTP: %s -> %s\n", ipStr, firstLine);
        fflush(stdout);
        std::string body;
        if (strstr(req, "GET /api/metrics") != nullptr)
        {
            body = BuildJson();
        }
        else if (strstr(req, "GET /") != nullptr)
        {
            body = RenderHtml(kHtmlPageHead, kHtmlPageBody, port, cfg);
        }
        else
        {
            body = "Not Found";
            send(client, "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nNot Found", 58, 0);
            closesocket(client);
            continue;
        }
        // Build headers with Content-Length for HTTP/1.1 compliance.
        char hdr[512] = {};
        int hlen = snprintf(hdr, sizeof(hdr),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: %s\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Connection: close\r\n"
            "Content-Length: %u\r\n"
            "\r\n",
            strstr(req, "GET /api/metrics") != nullptr ? "application/json" : "text/html; charset=utf-8",
            (unsigned int)body.size());
        send(client, hdr, hlen, 0);
        send(client, body.c_str(), (int)body.size(), 0);
        closesocket(client);
    }
    closesocket(srv); WSACleanup();
}

void StartHttpServer(const AppConfig& cfg)
{
    std::thread([cfg] { HttpLoop(cfg.http_port, cfg); }).detach();
}
#else
static void StartHttpServer(int) {}
#endif // PLATFORM_WINDOWS
