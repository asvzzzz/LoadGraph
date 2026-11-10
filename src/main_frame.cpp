#include "main_frame.h"
#include "config.h"
#include "theme.h"
#include "power_table.h"
#include "logger.h"
#include "metrics_api.h"
#include "storage_monitor.h"
#include "app_config.h"
#include <wx/display.h>
#include <wx/dcbuffer.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

wxBEGIN_EVENT_TABLE(MainFrame, wxFrame)
    EVT_TIMER(wxID_ANY, MainFrame::OnTimer)
    EVT_CLOSE(MainFrame::OnClose)
    EVT_SIZE(MainFrame::OnSize)
wxEND_EVENT_TABLE()

// Parse a 6-char hex color string (e.g. "76B900") into wxColour.
static wxColour HexColor(const std::string& hex)
{
    unsigned int val = 0;
    if (hex.size() == 6 && std::sscanf(hex.c_str(), "%06x", &val) == 1)
        return wxColour(val >> 16 & 0xFF, val >> 8 & 0xFF, val & 0xFF);
    return wxColour(128, 128, 128); // fallback gray
}

static const double kAlarmTempC  = 80.0;
static const int    kGraphHeightPx = 40;

// Format uptime in ms as "YY:MM:DD HH:MI:SS", omitting leading zero groups.
// Short uptimes (< 1 min) render as "<N> s" to avoid bare numbers like "05".
static wxString FormatUptime(uint64_t totalMs)
{
	uint64_t s = totalMs / 1000ULL;
	const uint64_t secPerYear  = 365ULL * 24ULL * 3600ULL;
	const uint64_t secPerMonth = 30ULL * 24ULL * 3600ULL;
	const uint64_t secPerDay   = 24ULL * 3600ULL;

	uint64_t yy = s / secPerYear;  s %= secPerYear;
	uint64_t mm = s / secPerMonth; s %= secPerMonth;
	uint64_t dd = s / secPerDay;   s %= secPerDay;
	uint64_t hh = s / 3600ULL;     s %= 3600ULL;
	uint64_t mi = s / 60ULL;       s %= 60ULL;
	uint64_t ss = s;

	// Fast path: sub-minute uptimes render with an explicit unit.
	if (yy == 0 && mm == 0 && dd == 0 && hh == 0 && mi == 0)
		return wxString::Format("%llu s", static_cast<unsigned long long>(ss));

	// Format as "YY:MM:DD HH:MI:SS", skipping only leading zero groups.
	// Once a non-zero group is emitted, all remaining groups are shown too
	// (including trailing zeros such as SS=00). If everything is zero the
	// full six-field string is returned.
	bool started = false;
	wxString result;

	auto appendField = [&](uint64_t val) {
		if (!started && val == 0) return;
		started = true;
		if (!result.IsEmpty()) result += ":";
		result += wxString::Format("%02llu", static_cast<unsigned long long>(val));
	};

	appendField(yy);
	appendField(mm);
	appendField(dd);

	bool hasDate = started; // dd was the last date field; if we reached here, date is present.
	bool hasTime = (hh > 0 || mi > 0 || ss > 0) || started;
	if (hasDate && hasTime && result.Last() == ':')
		result[(size_t)(result.Length() - 1)] = ' ';

	appendField(hh);
	appendField(mi);
	appendField(ss);

	if (result.IsEmpty()) return "00:00:00 00:00:00";
	return result;
}

// Round a watt value up to the next multiple of 50 (minimum 100 W).
static int RoundUpWattsTo50(double watts)
{
	if (watts < 50.0) return 100;
	return static_cast<int>(std::ceil(watts / 50.0)) * 50;
}

// Convert bytes to gigabytes.
static double BytesToGbs(uint64_t bytes)
{
	return static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
}

MainFrame::MainFrame(const wxString& title, const AppConfig& cfg)
	: wxFrame(nullptr, wxID_ANY, title, wxDefaultPosition, wxDefaultSize, wxDEFAULT_FRAME_STYLE),
	  m_cfg(cfg)
{
	// Refresh the system palette once before first use.
	Theme::Update();

	int x = 100, y = 100, w = 640, h = 720;
#ifdef PLATFORM_WINDOWS
	x = m_cfg.window_x;
	y = m_cfg.window_y;
#endif
	bool hasPos = false;
#ifdef PLATFORM_WINDOWS
	hasPos = (m_cfg.window_x != 0 || m_cfg.window_y != 0);
#else
	hasPos = Config::LoadWindowPosition(x, y);
	Config::LoadWindowSize(w, h);
#endif

	wxPoint pos = hasPos ? wxPoint(x, y) : wxDefaultPosition;
	wxSize  size = hasPos ? wxSize(w, h) : wxSize(640, 720);

	SetPosition(pos);
	SetClientSize(size);
	SetMinSize(wxSize(480, 540));

	// Application icon (loaded from the working directory).
	if (wxFileExists("loadgraph.png"))
	{
		wxIcon icon("loadgraph.png", wxBITMAP_TYPE_PNG);
		if (icon.IsOk()) SetIcon(icon);
	}

	// Initialize the shared metrics system (CPU / RAM / GPU monitors).
	MetricsInit();

	// Log every detected GPU - helps to spot devices missing from NVML.
	const auto gpus0 = MetricsGetGpus();
	LOGI("GPU list: %d device(s)", static_cast<int>(gpus0.size()));
	for (size_t i = 0; i < gpus0.size(); ++i)
		LOGI("  GPU %d: %hs", static_cast<int>(i), gpus0[i].name.c_str());

	// Build the layout first - the View menu needs the created graph panels.
	RealizeUI();

	std::map<std::string, bool> initialState;
	Config::LoadViewMenuState(initialState);

	wxMenuBar* menuBar = new wxMenuBar();
	BuildViewMenu(menuBar, initialState);
	SetMenuBar(menuBar);

	// Apply the persisted visibility (hide everything unchecked).
	for (const auto& kv : initialState)
	{
		if (!kv.second) ApplyHeightAdjustment(kv.first, false);
	}

	// Fit the window height exactly to the content - no empty space at the
	// bottom. The width stays at least the saved / minimal one.
	wxSizer* sizer = GetSizer();
	if (sizer)
	{
		const wxPoint winPos = GetPosition();
		const wxSize  mn = sizer->CalcMin();
		const wxSize  cs = GetClientSize();
		// Use saved height if it is larger than content minimum — this preserves
		// the user's preferred window size across restarts.
		wxSize finalSz(std::max(cs.x, mn.x), std::max(cs.y, mn.y));

		// Do not exceed the working area of the display hosting the window
		// (with many GPUs the content can be taller than the screen).
		const int dispIdx = wxDisplay::GetFromPoint(winPos);
		int maxH = 0;
		if (dispIdx != wxNOT_FOUND)
		{
			maxH = wxDisplay(dispIdx).GetClientArea().height - 80;
			if (maxH > 0 && finalSz.y > maxH) finalSz.y = maxH;
		}
		LOGI("fit: contentMin=%dx%d before=%dx%d final=%dx%d dispIdx=%d maxH=%d",
		     mn.x, mn.y, cs.x, cs.y, finalSz.x, finalSz.y, dispIdx, maxH);

		SetClientSize(finalSz);
		Layout();
		SetPosition(winPos);

		// Prevent the window from being shrunk below the content minimum.
		// The minimum must be the OUTER window size, so the frame decorations
		// (title bar, borders, menu bar) are added to the client minimum:
		// otherwise the client area is clipped by that amount at the minimum
		// size and the bottom section disappears when the user drags the
		// resize border down to the limit.
		const wxSize deco = GetSize() - GetClientSize();
		SetMinSize(wxSize(std::max(480, mn.x + deco.x),
		                  std::max(540, mn.y + deco.y)));
	}

	RefreshSectionBoxes();

	// Log the final layout for diagnostics.
	int cw = 0, ch = 0;
	GetClientSize(&cw, &ch);
	LOGI("UI ready: gpuPanels=%d gpuKeys=%d viewKeys=%d client=%dx%d",
	     static_cast<int>(m_gpuPanels.size()),
	     static_cast<int>(m_gpuGroupToggles.size()),
	     static_cast<int>(m_viewToggles.size()), cw, ch);

	m_timer.SetOwner(this);
	m_timer.Start(1000);

}

MainFrame::~MainFrame()
{
	if (m_timer.IsRunning()) m_timer.Stop();

	// Detach the content paint handler before the frame is destroyed so no
	// event can reach a partially destroyed sink.
	if (m_content)
		m_content->Disconnect(wxEVT_PAINT,
		                      wxPaintEventHandler(MainFrame::OnContentPaint),
		                      nullptr, this);
}

void MainFrame::RealizeUI()
{
	if (m_uiCreated) return;

	wxBoxSizer* mainS = new wxBoxSizer(wxVERTICAL);

	// Header bar: title on the left, uptime on the right.
	m_header = new wxPanel(this, wxID_ANY);
	wxBoxSizer* hdrS = new wxBoxSizer(wxHORIZONTAL);

	// CPU model label (the redundant "LoadGraph" title was removed; the label
	// stretches and ellipsizes so the uptime never gets pushed out).
	wxString cpuLabel = "CPU";
	const CpuData cpuInit = MetricsGetCpu();
	const std::wstring cpuName = cpuInit.cpuName;
	if (!cpuName.empty()) cpuLabel += "  " + wxString(cpuName.c_str());
	else                  cpuLabel += "  (N/A)";
	wxStaticText* cpuT = new wxStaticText(m_header, wxID_ANY, cpuLabel);
	wxFont cpuFont(11, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_BOLD);
	cpuT->SetFont(cpuFont);
	// Reserve space for the uptime label so it is never clipped at minimum
	// width. The CPU label ellipsizes instead of pushing uptime off-screen.
	const int kUptimeMinWidth = 200;
	cpuT->SetMaxSize(wxSize(-1, -1));
	AddThemedText(cpuT, true);
	hdrS->Add(cpuT, 1, wxALL | wxALIGN_CENTER_VERTICAL, 6);

	m_uptimeText = new wxStaticText(m_header, wxID_ANY, " Uptime: ...");
	AddThemedText(m_uptimeText, false);
	// Give uptime a minimum width so it is never compressed to nothing.
	m_uptimeText->SetMinSize(wxSize(kUptimeMinWidth, -1));
	hdrS->Add(m_uptimeText, 0, wxALL | wxALIGN_CENTER_VERTICAL, 6);

	m_header->SetSizer(hdrS);
	mainS->Add(m_header, 0, wxEXPAND);

	// Content: CPU section, GPU section, total power row.
	m_content = new wxPanel(this, wxID_ANY);

	// The section borders are painted on m_content: its coordinate space
	// matches the child windows exactly (no title bar offset), and the border
	// lines fall into the background gaps where no child window covers them.
	m_content->SetBackgroundStyle(wxBG_STYLE_PAINT);
	m_content->Connect(wxEVT_PAINT,
	                   wxPaintEventHandler(MainFrame::OnContentPaint),
	                   nullptr, this);

	wxBoxSizer* contentS = new wxBoxSizer(wxVERTICAL);

	// Guaranteed background gaps: the outer border lines (6 px) and the shared
	// line between two sections (5 px) are painted on m_content, so they must
	// land in areas that are not covered by child windows.
	contentS->AddSpacer(6);

	BuildCpuSection(contentS, m_content);
	contentS->AddSpacer(5);
	if (MetricsGpuAvailable()) BuildGpuSection(contentS, m_content);
	BuildTotalPowerRow(contentS, m_content);
	contentS->AddSpacer(6);

	m_content->SetSizer(contentS);
	mainS->Add(m_content, 1, wxEXPAND);

	SetSizer(mainS);
	ApplyTheme();

	// Force an initial layout pass so the header is correctly sized even when
	// the frame starts at minimum dimensions (prevents uptime text from being
	// clipped before the first timer tick triggers a resize).
	Layout();

	m_lastFullSize = GetClientSize();
	m_uiCreated = true;
}

void MainFrame::BuildCpuSection(wxBoxSizer* container, wxWindow* parent)
{
	// Row 1: CPU Load + RAM Usage side by side.
	wxBoxSizer* row1 = new wxBoxSizer(wxHORIZONTAL);

	m_cpuLoadGraph = new GraphPanel(parent, "Load", "%");
	m_cpuLoadGraph->SetYRange(0.0, 100.0);
	m_cpuLoadGraph->SetYMode(GraphYMode::Percent);
	m_cpuLoadGraph->SetColor(HexColor(m_cfg.cpu_load_color));
	m_cpuLoadGraph->SetBgColor(HexColor(m_cfg.chart_bg_color));

	m_ramUsageGraph = new GraphPanel(parent, "RAM", "GB");
	m_ramUsageGraph->SetYMode(GraphYMode::Gigabytes);
	m_ramUsageGraph->SetColor(HexColor(m_cfg.ram_color));
	m_ramUsageGraph->SetBgColor(HexColor(m_cfg.chart_bg_color));
	// Fixed 0..total RAM range: avoids a drifting auto-scale.
	const RamData ramInit = MetricsGetRam();
	if (ramInit.totalBytes > 0)
		m_ramUsageGraph->SetYRange(0.0, std::ceil(BytesToGbs(ramInit.totalBytes)));

	// No vertical padding: rows share vertical space proportionally so all
	// graphs scale together when the window is resized.
	row1->Add(m_cpuLoadGraph,  1, wxEXPAND | wxLEFT | wxRIGHT, 5);
	row1->Add(m_ramUsageGraph, 1, wxEXPAND | wxLEFT | wxRIGHT, 5);
	container->Add(row1, 1, wxEXPAND);
	container->AddSpacer(5);

	m_viewToggles["cpu_load"]  = m_cpuLoadGraph;
	m_viewToggles["ram"]       = m_ramUsageGraph;

	const CpuData cpuInit = MetricsGetCpu();

	// Row 2: CPU Temp (+ CPU Fan when a fan sensor exists).
	if (cpuInit.temperature >= 0.0 || cpuInit.fanRpm > 0)
	{
		wxBoxSizer* row2 = new wxBoxSizer(wxHORIZONTAL);

		m_cpuTempGraph = new GraphPanel(parent, "Temp", "C");
		m_cpuTempGraph->SetYRange(0.0, 100.0);
		m_cpuTempGraph->SetYMode(GraphYMode::Celsius);
		m_cpuTempGraph->SetAlarmThreshold(kAlarmTempC);
		m_cpuTempGraph->SetColor(HexColor(m_cfg.cpu_temp_color));
		m_cpuTempGraph->SetBgColor(HexColor(m_cfg.chart_bg_color));
		row2->Add(m_cpuTempGraph, (cpuInit.fanRpm > 0) ? 1 : 2,
		          wxEXPAND | wxLEFT | wxRIGHT, 5);

		if (cpuInit.fanRpm > 0)
		{
			m_cpuFanGraph = new GraphPanel(parent, "Fan", "RPM");
			row2->Add(m_cpuFanGraph, 1, wxEXPAND | wxLEFT | wxRIGHT, 5);
		}

		container->Add(row2, 1, wxEXPAND);

		m_viewToggles["cpu_temp"] = m_cpuTempGraph;

		if (m_cpuFanGraph)
		{
			m_viewToggles["fan"] = m_cpuFanGraph;
		}
	}

	// Row 3: CPU Power (only when a TDP or a real power sensor is known).
	if (cpuInit.tdpWatts > 0.0)
	{
		m_cpuPowerGraph = new GraphPanel(parent, "Power", "W");
		const double maxP = std::max(250.0, cpuInit.tdpWatts * 1.1);
		m_cpuPowerGraph->SetYRange(0.0, maxP);
		m_cpuPowerGraph->SetYMode(GraphYMode::Watts);
		m_cpuPowerGraph->SetColor(HexColor(m_cfg.cpu_power_color));
		m_cpuPowerGraph->SetBgColor(HexColor(m_cfg.chart_bg_color));

		// No trailing spacer here: RealizeUI adds the 5 px gap between the
		// CPU section and the next one, so their border lines coincide.
		container->Add(m_cpuPowerGraph, 1, wxEXPAND | wxLEFT | wxRIGHT, 5);

		m_viewToggles["cpu_power"] = m_cpuPowerGraph;
	}
}

void MainFrame::BuildGpuSection(wxBoxSizer* container, wxWindow* parent)
{
	const std::vector<GpuData> gpus = MetricsGetGpus();
	if (gpus.empty()) return;

	// Five group toggles: one checkbox controls this metric type on ALL GPUs.
	static const char* kGroupKeys[5] = {
		"gpu_load", "gpu_temp", "gpu_vram", "gpu_fan", "gpu_power"
	};
	for (int k = 0; k < 5; ++k)
	{
		const bool exists = std::any_of(
			m_gpuGroupToggles.begin(), m_gpuGroupToggles.end(),
			[k](const GpuGroupToggle& g) { return g.key == kGroupKeys[k]; });
		if (!exists)
		{
			GpuGroupToggle gt;
			gt.key = kGroupKeys[k];
			m_gpuGroupToggles.push_back(gt);
		}
	}

	// Register a panel in its GPU metric-type group.
	auto addPanel = [this](const char* key, GraphPanel* panel)
	{
		for (GpuGroupToggle& g : m_gpuGroupToggles)
		{
			if (g.key == key) { g.panels.push_back(panel); break; }
		}
	};

	for (size_t i = 0; i < gpus.size(); ++i)
	{
		const GpuData& gpu = gpus[i];

		const wxString gpuTitle = wxString::Format("GPU %d: ", static_cast<int>(i))
			+ wxString::FromUTF8(gpu.name);

		// Title label as a direct child of the container (proportion=0, no stretch).
		wxStaticText* gpuTitleLabel = new wxStaticText(parent, wxID_ANY, gpuTitle);
		gpuTitleLabel->SetFont(
			wxFont(11, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_BOLD));
		m_gpuBoxes.push_back(gpuTitleLabel);
		container->Add(gpuTitleLabel, 0, wxLEFT | wxRIGHT, 5);

		GpuPanelData data;
		data.name = wxString::FromUTF8(gpu.name);

		// Row 1: Load + Temp.
		wxBoxSizer* row1 = new wxBoxSizer(wxHORIZONTAL);
		if (gpu.loadPercent >= 0.0)
		{
			data.loadGraph = new GraphPanel(parent, "Load", "%");
			data.loadGraph->SetYMode(GraphYMode::Percent);
			// Fixed 0..100% range: auto scaling showed -1..2 at idle.
			data.loadGraph->SetYRange(0.0, 100.0);
			data.loadGraph->SetColor(HexColor(m_cfg.gpu_load_color));
			data.loadGraph->SetBgColor(HexColor(m_cfg.chart_bg_color));
			row1->Add(data.loadGraph, 1, wxEXPAND | wxLEFT | wxRIGHT, 5);
			addPanel("gpu_load", data.loadGraph);
		}
		if (gpu.temperature >= 0.0)
		{
			data.tempGraph = new GraphPanel(parent, "Temp", "C");
			data.tempGraph->SetYMode(GraphYMode::Celsius);
			// Fixed 0..100 C range: predictable scale instead of a drifting one.
			data.tempGraph->SetYRange(0.0, 100.0);
			data.tempGraph->SetAlarmThreshold(kAlarmTempC);
			data.tempGraph->SetColor(HexColor(m_cfg.gpu_temp_color));
			data.tempGraph->SetBgColor(HexColor(m_cfg.chart_bg_color));
			row1->Add(data.tempGraph, 1, wxEXPAND | wxLEFT | wxRIGHT, 5);
			addPanel("gpu_temp", data.tempGraph);
		}
		if (row1->GetItemCount() > 0)
		{
			container->Add(row1, 1, wxEXPAND);
			container->AddSpacer(5);
		}

		// Row 2: VRAM + Fan.
		wxBoxSizer* row2 = new wxBoxSizer(wxHORIZONTAL);
		if (gpu.vramTotalBytes > 0)
		{
			data.vramGraph = new GraphPanel(parent, "VRAM", "GB");
			data.vramGraph->SetYMode(GraphYMode::Gigabytes);
			// Fixed 0..total VRAM range (auto scaling showed -1..2 at idle).
			data.vramGraph->SetYRange(0.0, std::ceil(BytesToGbs(gpu.vramTotalBytes)));
			data.vramGraph->SetColor(HexColor(m_cfg.gpu_vram_color));
			data.vramGraph->SetBgColor(HexColor(m_cfg.chart_bg_color));
			row2->Add(data.vramGraph, (gpu.fanPercent >= 0.0 || gpu.fanRpm >= 0) ? 1 : 2,
			          wxEXPAND | wxLEFT | wxRIGHT, 5);
			addPanel("gpu_vram", data.vramGraph);
		}
		if (gpu.fanPercent >= 0.0 || gpu.fanRpm >= 0)
		{
			const bool inPercent = (gpu.fanPercent >= 0.0);
			data.fanGraph = new GraphPanel(parent, "Fan", inPercent ? "%" : "RPM");
			// Fixed range: 0..100% (or 0..3000 RPM for the RPM fallback).
			data.fanGraph->SetYRange(0.0, inPercent ? 100.0 : 3000.0);
			data.fanGraph->SetColor(HexColor(m_cfg.gpu_fan_color));
			data.fanGraph->SetBgColor(HexColor(m_cfg.chart_bg_color));
			row2->Add(data.fanGraph, (gpu.vramTotalBytes > 0) ? 1 : 2,
			          wxEXPAND | wxLEFT | wxRIGHT, 5);
			addPanel("gpu_fan", data.fanGraph);
		}
		if (row2->GetItemCount() > 0)
		{
			container->Add(row2, 1, wxEXPAND);
			container->AddSpacer(5);
		}

		// Row 3: Power (rounded up to the next multiple of 50 W).
		if (gpu.powerWatts >= 0.0 || gpu.powerLimitWatts > 0.0)
		{
			data.powerGraph = new GraphPanel(parent, "Power", "W");
			data.powerGraph->SetYMode(GraphYMode::Watts);
			data.powerGraph->SetColor(HexColor(m_cfg.gpu_power_color));
			data.powerGraph->SetBgColor(HexColor(m_cfg.chart_bg_color));

			double limit = gpu.powerLimitWatts;
			if (limit <= 0.0) limit = static_cast<double>(PowerTable::GetGpuTdp(gpu.name));
			if (limit <= 0.0) limit = 400.0;
			data.powerGraph->SetYRange(0.0, std::max(250.0, limit * 1.1));

			container->Add(data.powerGraph, 1, wxEXPAND | wxLEFT | wxRIGHT, 5);
			addPanel("gpu_power", data.powerGraph);
		}

		m_gpuPanels.push_back(data);

		// Background gap after each GPU section so its bottom border line is
		// not covered by the next section's first child window. When the last
		// row already added a spacer, the gap is exactly 5 px already.
		if (container->GetItemCount() > 0)
		{
			wxSizerItem* last = container->GetItem(container->GetItemCount() - 1);
			if (!(last && last->IsSpacer()))
				container->AddSpacer(5);
		}
	}
}

void MainFrame::BuildTotalPowerRow(wxBoxSizer* container, wxWindow* parent)
{
	// Fixed 0..1000 W range.
	m_totalPowerGraph = new GraphPanel(parent, "Total Power", "W");
	m_totalPowerGraph->SetYRange(0.0, 1000.0);
	m_totalPowerGraph->SetYMode(GraphYMode::Watts);
	m_totalPowerGraph->SetTickCount(5);
	m_totalPowerGraph->SetColor(HexColor(m_cfg.total_power_color));
	m_totalPowerGraph->SetBgColor(HexColor(m_cfg.chart_bg_color));
	container->Add(m_totalPowerGraph, 1, wxEXPAND | wxLEFT | wxRIGHT, 5);

	m_viewToggles["Total Power"] = m_totalPowerGraph;
}

void MainFrame::BuildViewMenu(wxMenuBar* menuBar, const std::map<std::string, bool>& initialState)
{
	wxMenu* vm = new wxMenu();

	auto addToggle = [&](const char* key, const wxString& label)
	{
		const int id = wxID_HIGHEST + 100 + static_cast<int>(m_menuIdToKey.size());
		m_menuIdToKey[id] = key;

		bool checked = true;
		const auto itS = initialState.find(key);
		if (itS != initialState.end()) checked = itS->second;

		wxMenuItem* mi = vm->AppendCheckItem(id, label);
		mi->Check(checked);

		Bind(wxEVT_MENU, &MainFrame::OnViewToggle, this, id);
	};

	if (m_viewToggles.count("cpu_load"))  addToggle("cpu_load",  "Load");
	if (m_viewToggles.count("ram"))       addToggle("ram",       "RAM");
	if (m_viewToggles.count("cpu_power")) addToggle("cpu_power", "Power");
	if (m_viewToggles.count("cpu_temp"))  addToggle("cpu_temp",  "Temp");
	if (m_viewToggles.count("fan"))       addToggle("fan",       "Fan");

	const bool anyGpu = !m_gpuGroupToggles.empty();
	if (anyGpu)
	{
		vm->AppendSeparator();
		addToggle("gpu_load",  "GPU Load");
		addToggle("gpu_temp",  "GPU Temp");
		addToggle("gpu_vram",  "GPU VRAM");
		addToggle("gpu_fan",   "GPU Fan");
		addToggle("gpu_power", "GPU Power");
	}

	if (m_viewToggles.count("Total Power"))
	{
		vm->AppendSeparator();
		addToggle("Total Power", "Total Power");
	}

	menuBar->Append(vm, "&View");
}

void MainFrame::OnViewToggle(wxCommandEvent& evt)
{
	const auto itK = m_menuIdToKey.find(evt.GetId());
	if (itK == m_menuIdToKey.end()) return;

	// Show/hide the panels and resize the window by exactly one row.
	ApplyHeightAdjustment(itK->second, evt.IsChecked());

	// Persist the checked state of every toggle in the View menu.
	wxMenuBar* mb = GetMenuBar();
	if (!mb) return;
	for (size_t m = 0; m < mb->GetMenuCount(); ++m)
	{
		wxMenu* menu = mb->GetMenu(m);
		if (!menu) continue;

		std::map<std::string, bool> state;
		bool foundAny = false;
		for (size_t i = 0; i < menu->GetMenuItemCount(); ++i)
		{
			wxMenuItem* mi = menu->FindItemByPosition(i);
			if (!mi || !mi->IsCheckable()) continue;
			const auto itId = m_menuIdToKey.find(mi->GetId());
			if (itId == m_menuIdToKey.end()) continue;
			state[itId->second] = mi->IsChecked();
			foundAny = true;
		}
		if (foundAny) { Config::SaveViewMenuState(state); break; }
	}
}

void MainFrame::ApplyHeightAdjustment(const std::string& key, bool showingItem)
{
	wxSizer* sizer = GetSizer();
	if (!sizer) return;

	// 1. Find the panels controlled by this menu key.
	std::vector<GraphPanel*> panels;
	const auto itG = std::find_if(
		m_gpuGroupToggles.begin(), m_gpuGroupToggles.end(),
		[&key](const GpuGroupToggle& g) { return g.key == key; });
	if (itG != m_gpuGroupToggles.end())
	{
		panels = itG->panels;
	}
	else
	{
		const auto itV = m_viewToggles.find(key);
		if (itV != m_viewToggles.end() && itV->second) panels.push_back(itV->second);
	}
	if (panels.empty()) return;

	// 2. Minimal content height before the change.
	const wxPoint pos = GetPosition();
	const int oldMinH = sizer->CalcMin().y;

	// 3. Toggle visibility: a hidden window is ignored by CalcMin(), so the
	//    content height changes by roughly the row minimum (kGraphHeightPx).
	for (GraphPanel* p : panels)
	{
		if (p) p->Show(showingItem);
	}

	const wxSize mn2 = sizer->CalcMin();
	const int newMinH = mn2.y;
	const wxSize cs = GetClientSize();
	int newH = cs.y - (oldMinH - newMinH);
	if (newH < newMinH) newH = newMinH;

	// 4. Keep the outer minimum in sync with the new content minimum so the
	//    window can be shrunk exactly to the visible content: no clipping of
	//    the bottom graph and no empty space left at the bottom.
	const wxSize deco = GetSize() - GetClientSize();
	SetMinSize(wxSize(std::max(480, mn2.x + deco.x),
	                  std::max(540, mn2.y + deco.y)));

	// 5. Resize without moving the window position.
	SetClientSize(cs.x, newH);
	Layout();
	SetPosition(pos);
	RefreshSectionBoxes();
	Refresh();
	if (m_content) m_content->Refresh();
}

void MainFrame::OnTimer(wxTimerEvent&)
{
	if (!m_uiCreated) return;

	// Sample CPU load NOW so powerWatts estimate is fresh (also reads PDH).
	MetricsSampleCpuLoad();

	const CpuData  cpu  = MetricsGetCpu();
	const RamData  ram  = MetricsGetRam();
	const std::vector<GpuData> gpus = MetricsGetGpus();

	// React to live light/dark theme switches.
	if (Theme::Update()) ApplyTheme();

	// Uptime label.
	if (m_uptimeText && cpu.uptimeMs > 0)
		m_uptimeText->SetLabel(" Uptime: " + FormatUptime(cpu.uptimeMs));

	// CPU / RAM graphs.
	if (m_cpuLoadGraph && cpu.loadPercent >= 0.0)
		m_cpuLoadGraph->AddPoint(cpu.loadPercent);
	if (m_ramUsageGraph && ram.totalBytes > 0)
	{
		// Guard against an unsigned underflow if the counters are inconsistent.
		const uint64_t usedBytes = (ram.freeBytes < ram.totalBytes)
			? (ram.totalBytes - ram.freeBytes) : 0;
		m_ramUsageGraph->AddPoint(BytesToGbs(usedBytes));
	}

	// CPU sensors.
	if (m_cpuTempGraph && cpu.temperature >= 0.0)
		m_cpuTempGraph->AddPoint(cpu.temperature);
	if (m_cpuFanGraph && cpu.fanRpm > 0)
		m_cpuFanGraph->AddPoint(static_cast<double>(cpu.fanRpm));

	// GPU graphs + total power accumulator.
	double totalWatts = (cpu.powerWatts > 0.0) ? cpu.powerWatts : 0.0;
	if (ram.powerWatts > 0.0) totalWatts += ram.powerWatts;

	const size_t n = std::min(gpus.size(), m_gpuPanels.size());
	for (size_t i = 0; i < n; ++i)
	{
		const GpuData& gpu = gpus[i];
		GpuPanelData& d = m_gpuPanels[i];

		if (d.loadGraph && gpu.loadPercent >= 0.0) d.loadGraph->AddPoint(gpu.loadPercent);
		if (d.tempGraph && gpu.temperature >= 0.0) d.tempGraph->AddPoint(gpu.temperature);
		if (d.vramGraph && gpu.vramTotalBytes > 0)
			d.vramGraph->AddPoint(BytesToGbs(gpu.vramUsedBytes));
		if (d.fanGraph)
		{
			if (gpu.fanPercent >= 0.0)      d.fanGraph->AddPoint(gpu.fanPercent);
			else if (gpu.fanRpm >= 0)       d.fanGraph->AddPoint(static_cast<double>(gpu.fanRpm));
		}
		if (d.powerGraph && gpu.powerWatts >= 0.0) d.powerGraph->AddPoint(gpu.powerWatts);

		if (gpu.powerWatts > 0.0) totalWatts += gpu.powerWatts;
	}

	// Add storage power.
	auto storage = MetricsGetStorage();
	for (const auto& s : storage)
	{
		if (s.powerWatts > 0.0) totalWatts += s.powerWatts;
	}

	if (m_cpuPowerGraph && cpu.powerWatts > 0.0)
		m_cpuPowerGraph->AddPoint(cpu.powerWatts);
	if (m_totalPowerGraph)
		m_totalPowerGraph->AddPoint(totalWatts);

}

void MainFrame::OnClose(wxCloseEvent& evt)
{
	int x = 0, y = 0, w = 0, h = 0;
	GetPosition(&x, &y);
	GetClientSize(&w, &h);
#ifdef PLATFORM_WINDOWS
	m_cfg.window_x = x;
	m_cfg.window_y = y;
	SaveAppConfig(m_cfg);
#else
	Config::SaveWindowPosition(x, y, w, h);
#endif

	if (m_timer.IsRunning()) m_timer.Stop();
	MetricsShutdown();

	evt.Skip();
}

// Recursively unite the rectangles (m_content coordinates) of all shown
// windows inside the given sizer. Nested sizers (graph rows) are traversed so
// the section bounds include every visible graph. Window positions are
// relative to m_content because every graph panel is its direct child.
static void UnionShownBounds(wxSizer* sizer, wxRect& out, bool& has)
{
	if (!sizer) return;
	const size_t n = sizer->GetItemCount();
	for (size_t i = 0; i < n; ++i)
	{
		wxSizerItem* si = sizer->GetItem(i);
		if (!si || !si->IsShown()) continue;

		if (wxWindow* w = si->GetWindow())
		{
			if (!w->IsShown()) continue;
			const wxPoint p = w->GetPosition();
			const wxSize  s = w->GetSize();
			if (s.x <= 0 || s.y <= 0) continue;
			const wxRect r(p.x, p.y, s.x, s.y);
			if (!has) { out = r; has = true; }
			else out.Union(r);
		}
		else if (wxSizer* sub = si->GetSizer())
		{
			UnionShownBounds(sub, out, has);
		}
	}
}

void MainFrame::RefreshSectionBoxes()
{
	m_sectionBoxes.clear();
	if (!m_content) return;
	wxSizer* sizer = m_content->GetSizer();
	if (!sizer) return;

	// Split the direct children of the content sizer into sections: items
	// before the first "GPU ..." label form the CPU section, every GPU label
	// starts its own section and the Total Power graph forms the last one.
	struct Section { wxString title; std::vector<wxSizerItem*> items; };
	std::vector<Section> sections;
	Section* cur = nullptr;

	const size_t n = sizer->GetItemCount();
	for (size_t i = 0; i < n; ++i)
	{
		wxSizerItem* si = sizer->GetItem(i);
		if (!si || !si->IsShown()) continue;
		wxWindow* w = si->GetWindow();
		wxStaticText* st = w ? dynamic_cast<wxStaticText*>(w) : nullptr;
		const bool isGpuTitle = (st != nullptr) && st->GetLabel().StartsWith("GPU ");
		const bool isTotalPower = (w != nullptr) && (w == m_totalPowerGraph);

		if (isGpuTitle || isTotalPower)
		{
			Section s;
			s.title = isTotalPower ? wxString("Total Power") : st->GetLabel();
			sections.push_back(s);
			cur = &sections.back();
		}
		else if (!cur)
		{
			Section s;
			s.title = "CPU";
			sections.push_back(s);
			cur = &sections.back();
		}
		cur->items.push_back(si);
	}

	// Build one border rectangle per section, expanded 3 px into the
	// background gaps around it so the lines are not covered by children.
	const wxSize cc = m_content->GetClientSize();
	for (const Section& sec : sections)
	{
		wxRect r;
		bool has = false;
		for (wxSizerItem* si : sec.items)
		{
			if (!si) continue;
			if (wxWindow* w = si->GetWindow())
			{
				if (!w->IsShown()) continue;
				const wxPoint p = w->GetPosition();
				const wxSize  s = w->GetSize();
				if (s.x <= 0 || s.y <= 0) continue;
				const wxRect wr(p.x, p.y, s.x, s.y);
				if (!has) { r = wr; has = true; }
				else r.Union(wr);
			}
			else if (wxSizer* sub = si->GetSizer())
			{
				UnionShownBounds(sub, r, has);
			}
		}
		if (!has) continue;

		int x1 = r.x - 3;
		int y1 = r.y - 3;
		int x2 = r.x + r.width + 2;   // right line:  child right edge + 3
		int y2 = r.y + r.height + 2;  // bottom line: child bottom edge + 3

		// Clamp into the content client area (a long GPU title may be wider
		// than the graphs and must not push the border outside the panel).
		if (x1 < 2) x1 = 2;
		if (y1 < 0) y1 = 0;
		if (x2 > cc.x - 3) x2 = cc.x - 3;
		if (y2 > cc.y - 1) y2 = cc.y - 1;
		if (x2 < x1 || y2 < y1) continue;

		m_sectionBoxes.push_back(
			{ sec.title, wxRect(x1, y1, x2 - x1 + 1, y2 - y1 + 1) });
	}
}

void MainFrame::OnSize(wxSizeEvent& evt)
{
	evt.Skip();

	// Repaint everything that changed size: the frame clears the strips
	// vacated by shrinking children, the header and the content panel redraw
	// themselves with the new layout. The section borders are recomputed by
	// OnContentPaint itself, so no explicit call is needed here.
	Refresh();
	if (m_header) m_header->Refresh();
	if (m_content) m_content->Refresh();
}

void MainFrame::OnContentPaint(wxPaintEvent&)
{
	// The event comes from m_content, so the DC must be bound to that panel:
	// a DC created for the frame would leave the panel surface unpainted and
	// the background gaps would show the system default color instead of the
	// theme background. The panel uses wxBG_STYLE_PAINT, so we own the fill.
	wxAutoBufferedPaintDC dc(m_content);
	dc.SetBackground(wxBrush(m_content->GetBackgroundColour()));
	dc.Clear();

	// Recompute the boxes: a sizer layout may have moved the children after
	// the last RefreshSectionBoxes() call (paint runs after the size event).
	RefreshSectionBoxes();

	// Diagnostic: space between the Total Power graph bottom and the content
	// bottom - must stay at the 6 px bottom gap at any window size.
#ifdef LOADGRAPH_DEBUG
	if (m_totalPowerGraph)
	{
		int x = 0, y = 0, w = 0, h = 0;
		m_totalPowerGraph->GetPosition(&x, &y);
		m_totalPowerGraph->GetSize(&w, &h);
		const wxSize cc = m_content->GetClientSize();
		LOGI("Paint: content=%dx%d totalBottom=%d bottomGap=%d boxes=%d",
		     cc.x, cc.y, y + h, cc.y - (y + h),
		     static_cast<int>(m_sectionBoxes.size()));
	}
#endif

	const wxColour frameCol(200, 200, 200);
	dc.SetPen(wxPen(frameCol, 1));
	dc.SetBrush(*wxTRANSPARENT_BRUSH);
	for (const SectionBox& sb : m_sectionBoxes)
		dc.DrawRectangle(sb.rect);

	// No evt.Skip(): with wxBG_STYLE_PAINT the default handler must not run,
	// otherwise the buffered content may be overwritten by the erase logic.
}

void MainFrame::ApplyTheme()
{
	const wxColour bg      = HexColor(m_cfg.page_bg_color);
	const wxColour section = HexColor(m_cfg.section_bg_color);
	const wxColour text    = Theme::Text();
	const wxColour sub     = Theme::SubText();

	SetBackgroundColour(bg);
	if (m_header)  m_header->SetBackgroundColour(section);
	if (m_content) m_content->SetBackgroundColour(bg);

	for (wxStaticText* t : m_primaryTexts)
		if (t) t->SetForegroundColour(text);
	for (wxStaticText* t : m_secondaryTexts)
		if (t) t->SetForegroundColour(sub);

	// GPU section boxes: wxStaticBox defaults to the system color, recolor it
	// to match the graph titles.
	for (wxWindow* b : m_gpuBoxes)
	{
		if (!b) continue;
		b->SetForegroundColour(text);
		b->Refresh();
	}

	Refresh();
	// The frame Refresh() does not repaint child windows: redraw the content
	// panel so the section borders follow the new background colour.
	if (m_content) m_content->Refresh();
}

void MainFrame::AddThemedText(wxStaticText* st, bool primary)
{
	if (!st) return;
	if (primary) m_primaryTexts.push_back(st);
	else         m_secondaryTexts.push_back(st);
}
