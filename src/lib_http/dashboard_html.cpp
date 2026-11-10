#include "dashboard_html.h"

// HTML dashboard strings - copied from http_server.cpp so both GUI and console
// can use them without depending on metrics_api (which pulls in wxWidgets).
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

  h1 { color: var(--c-cpu-title, #76B900); font-size: 18px; flex: 0 0 auto; }
  .cpu-name { color: var(--c-cpu-name, #00bcd4); }
  .gpu-title { color: var(--c-cpu-title, #76B900); font-size: 13px; font-weight: bold;
               flex: 0 0 auto; white-space: nowrap; overflow: hidden;
               text-overflow: ellipsis; }

  /* Content fills all free space without pushing anything outside the window. */
  .content { flex: 1 1 0; min-height: 0; display: flex;
             flex-direction: column; gap: 8px; overflow: hidden; }

  /* Sections share content height equally. min-height:0 allows them to shrink. */
  .section, .gpu-section {
    background: #16213e; border-radius: 8px; padding: 8px 10px;
    flex: 1 1 0; min-height: 0;
    display: flex; flex-direction: column; gap: 4px;
  }

  /* GPU cards container - also a content flex item. */
  #gpuSections { flex: 1 1 0; min-height: 0;
                 display: flex; flex-direction: column; gap: 8px;
                 overflow: hidden; }
  #gpuSections:empty { display: none; }

  /* Rows inside section also stretch by height. */
  .chart-row { display: flex; gap: 8px; flex: 1 1 0; min-height: 0; }
  .full-row  { flex: 1 1 0; min-height: 0;
               display: flex; flex-direction: column; }
  .chart-cell { flex: 1 1 0; min-width: 0; min-height: 0;
                display: flex; flex-direction: column; }

  /* Label - fixed height, chart takes the rest. */
  .val-row { flex: 0 0 auto; height: 18px; line-height: 18px;
             font-size: 12px; color: #aaa; white-space: nowrap;
             overflow: hidden; text-overflow: ellipsis; }
  .val-val { color: #fff; font-weight: bold; font-size: 13px; }

  /* Key: instead of height:60px / flex-shrink:0 - stretchable chart. */
  canvas.chart { background: #0f3460; border-radius: 4px; display: block;
                 width: 100%; flex: 1 1 0; min-height: 0; }

   /* Blinking connection lost warning. */
   #connWarn { display:none; text-align:center; padding:6px 0;
               font-size:13px; color:#ff5722; font-weight:bold;
               animation: blinkWarn 1s step-start infinite; }
   @keyframes blinkWarn { 0%,100% { opacity:1; } 50% { opacity:0; } }

</style>
<script>window.__CFG={color:{cpuLoad:'%CPU_LOAD%',cpuTemp:'%CPU_TEMP%',cpuPower:'%CPU_POWER%',ram:'%RAM%',gpuLoad:'%GPU_LOAD%',gpuTemp:'%GPU_TEMP%',gpuVram:'%GPU_VRAM%',gpuFan:'%GPU_FAN%',gpuPower:'%GPU_POWER%',totalPower:'%TOTAL_POWER%'}};</script>
</head>)";
// Split into two literals to avoid MSVC C2026 on large raw strings.
static const char* kHtmlPageBody1 = R"(<body>
<h1>LoadGraph <span id="cpuName" class="cpu-name"></span></h1>
<div id="connWarn">Warning -- retrying...</div>
<div class="content">

<!-- CPU section - rows mirror GUI layout -->
<div class="section" id="cpuSection">
  <!-- Row 1: Load | RAM -->
  <div class="chart-row">
    <div class="chart-cell">
      <div class="val-row">Load <span class="val-val" id="cpuLoadVal">--%</span></div>
      <canvas id="cpuChart" class="chart"></canvas>
    </div>
    <div class="chart-cell">
      <div class="val-row">RAM <span class="val-val" id="ramUsedVal">-- GB</span></div>
      <canvas id="ramChart" class="chart"></canvas>
    </div>
  </div>
  <!-- Row 2: Power (full width, shown only when TDP > 0) -->
  <div class="full-row" id="cpuPowerRow" style="display:none">
    <div class="val-row">Power <span class="val-val" id="cpuPowerVal">N/A</span></div>
      <canvas id="powerChart" class="chart"></canvas>
  </div>
  <!-- Row 3: Temp | Fan -->
  <div class="chart-row">
    <div class="chart-cell">
      <div class="val-row">Temp <span class="val-val" id="cpuTempVal">N/A</span></div>
      <canvas id="tempChart" class="chart"></canvas>
    </div>
    <div class="chart-cell" id="fanCell" style="display:none">
      <div class="val-row">Fan <span class="val-val" id="fanVal">N/A</span></div>
      <canvas id="fanChart" class="chart"></canvas>
    </div>
  </div>
</div>

<div id="gpuSections"></div>

<!-- Total Power - section hides as a whole when no reading is available -->
<div class="section" id="totalPowerSection" style="display:none">
  <div class="full-row" id="totalPowerRow">
    <div class="val-row">Total Power <span class="val-val" id="totalPowerVal">N/A</span></div>
      <canvas id="totalPowerChart" class="chart"></canvas>
  </div>
</div>

</div>)";
static const char* kHtmlPageBody2 = R"(
<script>
const C = window.__CFG.color;
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

// ── Charts ---------------------------------------------------------------─
let cpuChart      = makeChart('cpuChart',       C.cpuLoad,   100);
let ramChart      = makeChart('ramChart',       C.ram,       128);
let tempChart     = makeChart('tempChart',      C.cpuTemp,   100);
let powerChart    = null;
let fanChart      = null;
let totalPowerChart = null;

const gpuCharts  = {};

// ── Helpers ───────────────────────────────────────────────────────────────
function esc(s)  { return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;'); }
function tempColor(t) { return t > 80 ? 'red' : t > 65 ? 'yellow' : 'green'; }

// ── CPU section update ────────────────────────────────────────────────────
function updateCpu(d) {
  document.getElementById('cpuName').textContent = d.cpu_name || '';
  document.getElementById('cpuLoadVal').textContent = d.cpu_load.toFixed(1) + '%';
  cpuChart.push(d.cpu_load);

  // RAM - yMax grows with total RAM so the chart is always proportional.
  var ramGb = d.ram_used_bytes / (1024*1024*1024);
  var totGb = d.ram_total_bytes   / (1024*1024*1024);
  document.getElementById('ramUsedVal').textContent = ramGb.toFixed(1) + ' GB';
  ramChart.push(ramGb);
  ramChart.ymax = Math.max(totGb, ramChart.ymax || totGb);

  // CPU Power - row shown whenever TDP is known OR a real power reading exists.
  var ppRow = document.getElementById('cpuPowerRow');
  if (d.tdp_watts > 0 || d.cpu_power > 0) {
    ppRow.style.display = '';
    var pYmax = Math.max(250, (d.tdp_watts > 0 ? d.tdp_watts : 300) * 1.1);
    if (!powerChart) {
      powerChart = makeChart('powerChart', C.cpuPower, pYmax);
    } else {
      powerChart.ymax = pYmax;
    }
    if (d.cpu_power > 0) {
      document.getElementById('cpuPowerVal').textContent = d.cpu_power.toFixed(1) + ' W';
      powerChart.push(d.cpu_power);
    } else {
      document.getElementById('cpuPowerVal').textContent = 'N/A';
    }
  } else {
    ppRow.style.display = 'none';
  }

  // Total Power - CPU + RAM + all GPUs with valid power readings.
  var totalW = 0;
  if (d.cpu_power > 0) totalW += d.cpu_power;
  if (d.gpus && d.gpus.length > 0) {
    for (var gi = 0; gi < d.gpus.length; gi++) {
      if (d.gpus[gi].power_watts > 0) totalW += d.gpus[gi].power_watts;
    }
  }
  var tpSec = document.getElementById('totalPowerSection');
  if (totalW > 0) {
    tpSec.style.display = '';
    if (!totalPowerChart) totalPowerChart = makeChart('totalPowerChart', C.totalPower, 1000);
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

// ── GPU sections - preserve DOM, update in place ──────────────────────────
function getOrCreateGpuSec(i, name) {
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
    if (!gpuCharts[lcKey]) gpuCharts[lcKey] = makeChart(lcKey, C.gpuLoad, 100);
    document.getElementById('gpu'+i+'LoadVal').textContent = g.load_percent.toFixed(1)+'%';
    gpuCharts[lcKey].push(g.load_percent);

    var tvEl = document.getElementById('gpu'+i+'TempVal');
    if (g.temperature >= 0) {
      tvEl.innerHTML = '<span class="'+tempColor(g.temperature)+'">'+g.temperature.toFixed(0)+' C</span>';
      var tcKey = 'gpu'+i+'TempChart';
      if (!gpuCharts[tcKey]) gpuCharts[tcKey] = makeChart(tcKey, C.gpuTemp, 100);
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
      if (!gpuCharts[vKey]) gpuCharts[vKey] = makeChart(vKey, C.gpuVram, vtGb);
      gpuCharts[vKey].push(vGb);
      gpuCharts[vKey].ymax = Math.max(vtGb, gpuCharts[vKey].ymax || vtGb);
    } else { document.getElementById('gpu'+i+'VramVal').textContent = 'N/A'; }

    var fvEl = document.getElementById('gpu'+i+'FanVal');
    var fKey = 'gpu'+i+'FanChart';
    if (g.fan_percent >= 0) {
      fvEl.textContent = g.fan_percent.toFixed(0) + '%';
      if (!gpuCharts[fKey]) gpuCharts[fKey] = makeChart(fKey, C.gpuFan, 100);
      gpuCharts[fKey].push(g.fan_percent);
    } else { fvEl.textContent = 'N/A'; }

    // Row 3: Power (full width). Show row when power reading or limit exists.
    if (g.power_watts > 0) {
      addFullRow('Power','gpu'+i+'PowVal','gpu'+i+'PowChart');
      var pKey = 'gpu'+i+'PowChart';
      if (!gpuCharts[pKey]) gpuCharts[pKey] = makeChart(pKey, C.gpuPower, Math.max(250, 400*1.1));
      document.getElementById('gpu'+i+'PowVal').textContent = g.power_watts.toFixed(1)+' W';
      gpuCharts[pKey].push(g.power_watts);
    }
  }
}

// ── Flex layout - distributes free space among sections proportionally ---
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

function redrawAll() {
    Object.keys(gpuCharts).forEach(function(k) {
        if (gpuCharts[k] && typeof gpuCharts[k].draw === 'function') gpuCharts[k].draw();
    });
    [cpuChart, ramChart, tempChart, powerChart, fanChart, totalPowerChart]
        .filter(function(c) { return c && typeof c.draw === 'function'; })
        .forEach(function(c) { c.draw(); });
}
window.addEventListener('resize', redrawAll);

// ── Fetch & refresh ───────────────────────────────────────────────────────
var connLost = false;
function setConnLost(v) {
  var el = document.getElementById('connWarn');
  if (!el) return;
  // Explicit 'block'/'none': empty string '' does not override the CSS rule
  // display:none from the stylesheet, so the warning stayed hidden.
  el.style.display = v ? 'block' : 'none';
  connLost = v;
}
async function fetchMetrics() {
  try {
    var r = await fetch('/api/metrics');
    if (!r.ok) {
      console.error('fetchMetrics: HTTP status ' + r.status);
      setConnLost(true);
      return;
    }
    var text = await r.text();
    var d;
    try {
      d = JSON.parse(text);
    } catch (pe) {
      // Diagnostic: print body head so invalid JSON is immediately visible.
      console.error('fetchMetrics: JSON parse failed:', pe,
                    'body head:', text.substring(0, 200));
      setConnLost(true);
      return;
    }
    setConnLost(false);
    updateCpu(d); updateGpu(d); updateFlex();
  } catch (e) {
    console.error('fetchMetrics: network error:', e);
    setConnLost(true);
  }
}
updateFlex();
setInterval(fetchMetrics, 1000);
fetchMetrics();
</script>
</body>
</html>)";

std::string GetHtmlBody()
{
    static const std::string s = std::string(kHtmlPageBody1) + kHtmlPageBody2;
    return s;
}

std::string RenderDashboard(int port, const AppConfig& cfg)
{
    return RenderHtml(kHtmlPageHead, GetHtmlBody().c_str(), port, cfg);
}

std::string RenderHtml(const char* head, const char* body, int port, const AppConfig& cfg)
{
    struct Rep { const char* from; std::string to; };
    Rep reps[] = {
        { "%CPU_LOAD%",   "#" + cfg.cpu_load_color },
        { "%CPU_TEMP%",   "#" + cfg.cpu_temp_color },
        { "%CPU_POWER%",  "#" + cfg.cpu_power_color },
        { "%RAM%",        "#" + cfg.ram_color },
        { "%GPU_LOAD%",   "#" + cfg.gpu_load_color },
        { "%GPU_TEMP%",   "#" + cfg.gpu_temp_color },
        { "%GPU_VRAM%",   "#" + cfg.gpu_vram_color },
        { "%GPU_FAN%",    "#" + cfg.gpu_fan_color },
        { "%GPU_POWER%",  "#" + cfg.gpu_power_color },
        { "%TOTAL_POWER%", "#" + cfg.total_power_color },
    };

    std::string result;
    result.reserve(strlen(head) + strlen(body));
    const char* p = head;
    while (*p)
    {
        bool matched = false;
        for (int i = 0; i < 10; i++)
        {
            size_t len = strlen(reps[i].from);
            if (strncmp(p, reps[i].from, len) == 0)
            { result += reps[i].to; p += (int)len; matched = true; break; }
        }
        if (!matched) { result += *p; ++p; }
    }
    std::string portScript = "<script>window.__CFG.port=" + std::to_string(port) + ";</script>";
    const char* headEnd = strstr(result.c_str(), "</head>");
    if (headEnd)
        result.insert(static_cast<size_t>(headEnd - result.c_str()), portScript);
    else
        result += portScript;
    result += body;
    return result;
}
