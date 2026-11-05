#include "main_frame.h"
#include "config.h"
#include "theme.h"
#include "power_table.h"
#include "logger.h"
#include <wx/display.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

wxBEGIN_EVENT_TABLE(MainFrame, wxFrame)
    EVT_TIMER(wxID_ANY, MainFrame::OnTimer)
    EVT_CLOSE(MainFrame::OnClose)
    EVT_SIZE(MainFrame::OnSize)
    EVT_PAINT(MainFrame::OnPaint)
wxEND_EVENT_TABLE()

static const double kAlarmTempC  = 80.0;
static const int    kGraphHeightPx = 40;

// Format uptime in ms as "YY:MM:DD HH:MI:SS".
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

	return wxString::Format("%02llu:%02llu:%02llu %02llu:%02llu:%02llu",
	                        static_cast<unsigned long long>(yy),
	                        static_cast<unsigned long long>(mm),
	                        static_cast<unsigned long long>(dd),
	                        static_cast<unsigned long long>(hh),
	                        static_cast<unsigned long long>(mi),
	                        static_cast<unsigned long long>(ss));
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

MainFrame::MainFrame(const wxString& title)
	: wxFrame(nullptr, wxID_ANY, title, wxDefaultPosition, wxDefaultSize, wxDEFAULT_FRAME_STYLE)
{
	// Refresh the system palette once before first use.
	Theme::Update();

	int x = 0, y = 0, w = 640, h = 540;
	const bool hasPos  = Config::LoadWindowPosition(x, y);
	const bool hasSize = Config::LoadWindowSize(w, h);
	if (hasPos) SetPosition(wxPoint(x, y));
	// Width comes from the config; the height is fitted to the content below
	// (a saved height would only produce empty space at the bottom).
	SetClientSize(wxSize(hasSize ? w : 640, 540));
	SetMinSize(wxSize(480, 540));

	// Application icon (loaded from the working directory).
	if (wxFileExists("loadgraph.png"))
	{
		wxIcon icon("loadgraph.png", wxBITMAP_TYPE_PNG);
		if (icon.IsOk()) SetIcon(icon);
	}

	// Resolve the CPU name / TDP once: the layout needs tdpWatts before
	// RealizeUI(), and QueryCpuName() must run before the polling starts.
	m_cpu.QueryCpuName();

	// Synchronous first collection: the layout (GPU list, CPU TDP / sensors)
	// is built from these metrics, so they must be filled before RealizeUI().
	m_cpu.CollectNow();
	m_ram.CollectNow();
	if (m_gpu.IsAvailable()) m_gpu.CollectNow();

	// Log every detected GPU - helps to spot devices missing from NVML.
	if (m_gpu.IsAvailable())
	{
		const std::vector<GpuMetrics> gpus0 = m_gpu.GetMetrics();
		LOGI("GPU list: %d device(s)", static_cast<int>(gpus0.size()));
		for (size_t i = 0; i < gpus0.size(); ++i)
			LOGI("  GPU %d: %s", static_cast<int>(i), gpus0[i].name.c_str());
	}

	// Start background collectors (1000 ms polling, handled by each monitor).
	m_cpu.Start();
	m_ram.Start();
	if (m_gpu.IsAvailable()) m_gpu.Start();

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
		wxSize finalSz(std::max(cs.x, mn.x), mn.y);

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

		// Prevent the window from being shrunk below content minimum — this
		// eliminates clipping of bottom graphs when the user drags the resize border.
		SetMinSize(wxSize(mn.x, mn.y));
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
	const std::wstring cpuName = m_cpu.GetMetrics().cpuName;
	if (!cpuName.empty()) cpuLabel += "  " + wxString(cpuName.c_str());
	else                  cpuLabel += "  (N/A)";
	wxStaticText* cpuT = new wxStaticText(m_header, wxID_ANY, cpuLabel);
	wxFont cpuFont(11, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_BOLD);
	cpuT->SetFont(cpuFont);
	// Cap the CPU label width so it cannot force the window wider than needed;
	// ellipsize truncates long names while keeping uptime visible.
	cpuT->SetMaxSize(wxSize(500, -1));
	AddThemedText(cpuT, true);
	hdrS->Add(cpuT, 1, wxALL | wxALIGN_CENTER_VERTICAL, 6);

	m_uptimeText = new wxStaticText(m_header, wxID_ANY, " Uptime: ...");
	AddThemedText(m_uptimeText, false);
	// No stretch spacer: the CPU label already has proportion=1 and will fill
	// remaining width, keeping uptime pinned to the right edge.
	hdrS->Add(m_uptimeText, 0, wxALL | wxALIGN_CENTER_VERTICAL, 6);

	m_header->SetSizer(hdrS);
	mainS->Add(m_header, 0, wxEXPAND);

	// Content: CPU section, GPU section, total power row.
	m_content = new wxPanel(this, wxID_ANY);
	wxBoxSizer* contentS = new wxBoxSizer(wxVERTICAL);

	BuildCpuSection(contentS, m_content);
	if (m_gpu.IsAvailable()) BuildGpuSection(contentS, m_content);
	BuildTotalPowerRow(contentS, m_content);

	m_content->SetSizer(contentS);
	mainS->Add(m_content, 1, wxEXPAND);

	SetSizer(mainS);
	ApplyTheme();

	m_lastFullSize = GetClientSize();
	m_uiCreated = true;
}

void MainFrame::BuildCpuSection(wxBoxSizer* container, wxWindow* parent)
{
	// Row 1: CPU Load + RAM Usage side by side.
	wxBoxSizer* row1 = new wxBoxSizer(wxHORIZONTAL);

	m_cpuLoadGraph = new GraphPanel(parent, "CPU Load", "%");
	m_cpuLoadGraph->SetYRange(0.0, 100.0);
	m_cpuLoadGraph->SetYMode(GraphYMode::Percent);

	m_ramUsageGraph = new GraphPanel(parent, "RAM Usage", "GB");
	m_ramUsageGraph->SetYMode(GraphYMode::Gigabytes);
	// Fixed 0..total RAM range: avoids a drifting auto-scale.
	const RamMetrics ramInit = m_ram.GetMetrics();
	if (ramInit.totalBytes > 0)
		m_ramUsageGraph->SetYRange(0.0, std::ceil(BytesToGbs(ramInit.totalBytes)));

	// No vertical padding: rows share vertical space proportionally so all
	// graphs scale together when the window is resized.
	row1->Add(m_cpuLoadGraph,  1, wxEXPAND | wxLEFT | wxRIGHT, 5);
	row1->Add(m_ramUsageGraph, 1, wxEXPAND | wxLEFT | wxRIGHT, 5);
	container->Add(row1, 1, wxEXPAND);
	container->AddSpacer(5);

	m_viewToggles["CPU Load"]  = m_cpuLoadGraph;
	m_viewToggles["RAM Usage"] = m_ramUsageGraph;
	m_toggleGroups["CPU Load"].items.push_back(row1->GetItem(static_cast<size_t>(0)));
	m_toggleGroups["RAM Usage"].items.push_back(row1->GetItem(static_cast<size_t>(1)));
	m_toggleGroups["CPU Load"].deltaH  = kGraphHeightPx;
	m_toggleGroups["RAM Usage"].deltaH = kGraphHeightPx;

	const CpuMetrics cpuInit = m_cpu.GetMetrics();

	// Row 2: CPU Power (only when a TDP or a real power sensor is known).
	if (cpuInit.tdpWatts > 0.0)
	{
		m_cpuPowerGraph = new GraphPanel(parent, "CPU Power", "W");
		const double maxP = std::max(250.0, cpuInit.tdpWatts * 1.1);
		m_cpuPowerGraph->SetYRange(0.0, RoundUpWattsTo50(maxP));
		m_cpuPowerGraph->SetYMode(GraphYMode::Watts);

		container->Add(m_cpuPowerGraph, 1, wxEXPAND | wxLEFT | wxRIGHT, 5);
		container->AddSpacer(5);

		m_viewToggles["CPU Power"] = m_cpuPowerGraph;
		m_toggleGroups["CPU Power"].items.push_back(
			container->GetItem(static_cast<size_t>(container->GetItemCount() - 1)));
		m_toggleGroups["CPU Power"].deltaH = kGraphHeightPx;
	}

	// Row 3: CPU Temp (+ CPU Fan when a fan sensor exists).
	if (cpuInit.temperature >= 0.0 || cpuInit.fanRpm > 0)
	{
		wxBoxSizer* row2 = new wxBoxSizer(wxHORIZONTAL);

		m_cpuTempGraph = new GraphPanel(parent, "CPU Temp", "C");
		m_cpuTempGraph->SetYMode(GraphYMode::Celsius);
		m_cpuTempGraph->SetAlarmThreshold(kAlarmTempC);
		row2->Add(m_cpuTempGraph, (cpuInit.fanRpm > 0) ? 1 : 2,
		          wxEXPAND | wxLEFT | wxRIGHT, 5);

		if (cpuInit.fanRpm > 0)
		{
			m_cpuFanGraph = new GraphPanel(parent, "CPU Fan", "RPM");
			row2->Add(m_cpuFanGraph, 1, wxEXPAND | wxLEFT | wxRIGHT, 5);
		}

		container->Add(row2, 1, wxEXPAND);

		m_viewToggles["CPU Temp"] = m_cpuTempGraph;
		m_toggleGroups["CPU Temp"].items.push_back(row2->GetItem(static_cast<size_t>(0)));
		m_toggleGroups["CPU Temp"].deltaH = kGraphHeightPx;

		if (m_cpuFanGraph)
		{
			m_viewToggles["CPU Fan"] = m_cpuFanGraph;
			m_toggleGroups["CPU Fan"].items.push_back(row2->GetItem(static_cast<size_t>(1)));
			m_toggleGroups["CPU Fan"].deltaH = kGraphHeightPx;
		}
	}
}

void MainFrame::BuildGpuSection(wxBoxSizer* container, wxWindow* parent)
{
	const std::vector<GpuMetrics> gpus = m_gpu.GetMetrics();
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

	// Register a panel in its metric-type group (and in the toggle groups).
	auto addPanel = [this](const char* key, GraphPanel* panel)
	{
		for (GpuGroupToggle& g : m_gpuGroupToggles)
		{
			if (g.key == key) { g.panels.push_back(panel); break; }
		}
		ToggleGroup& tg = m_toggleGroups[key];
		tg.deltaH += kGraphHeightPx;
	};

	for (size_t i = 0; i < gpus.size(); ++i)
	{
		const GpuMetrics& gpu = gpus[i];

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
			data.loadGraph->SetPurple(true);
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
			data.tempGraph->SetPurple(true);
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
			data.vramGraph->SetPurple(true);
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
			data.fanGraph->SetPurple(true);
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
			data.powerGraph->SetPurple(true);

			double limit = gpu.powerLimitWatts;
			if (limit <= 0.0) limit = static_cast<double>(PowerTable::GetGpuTdp(gpu.name));
			if (limit <= 0.0) limit = 400.0;
			data.powerGraph->SetYRange(0.0, RoundUpWattsTo50(limit * 1.1));

			container->Add(data.powerGraph, 1, wxEXPAND | wxLEFT | wxRIGHT, 5);
			addPanel("gpu_power", data.powerGraph);
		}

		m_gpuPanels.push_back(data);
	}
}

void MainFrame::BuildTotalPowerRow(wxBoxSizer* container, wxWindow* parent)
{
	// Fixed 0..1000 W range with 5 ticks: 0, 250, 500, 750, 1000.
	m_totalPowerGraph = new GraphPanel(parent, "Total Power", "W");
	m_totalPowerGraph->SetYRange(0.0, 1000.0);
	m_totalPowerGraph->SetYMode(GraphYMode::Watts);
	m_totalPowerGraph->SetTickCount(5);
	container->Add(m_totalPowerGraph, 1, wxEXPAND | wxLEFT | wxRIGHT, 5);

	m_viewToggles["Total Power"] = m_totalPowerGraph;
	m_toggleGroups["Total Power"].items.push_back(
		container->GetItem(static_cast<size_t>(container->GetItemCount() - 1)));
	m_toggleGroups["Total Power"].deltaH = kGraphHeightPx;
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

	if (m_viewToggles.count("CPU Load"))  addToggle("CPU Load",  "CPU Load");
	if (m_viewToggles.count("RAM Usage")) addToggle("RAM Usage", "RAM Usage");
	if (m_viewToggles.count("CPU Power")) addToggle("CPU Power", "CPU Power");
	if (m_viewToggles.count("CPU Temp"))  addToggle("CPU Temp",  "CPU Temperature");
	if (m_viewToggles.count("CPU Fan"))   addToggle("CPU Fan",   "CPU Fan Speed");

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

	const int newMinH = sizer->CalcMin().y;
	const wxSize cs = GetClientSize();
	int newH = cs.y - (oldMinH - newMinH);
	if (newH < newMinH) newH = newMinH;
	const int frameMinH = GetMinSize().y;
	if (newH < frameMinH) newH = frameMinH;

	// 4. Resize without moving the window position.
	SetClientSize(cs.x, newH);
	Layout();
	SetPosition(pos);
	RefreshSectionBoxes();
	Refresh();
}

void MainFrame::OnTimer(wxTimerEvent&)
{
	if (!m_uiCreated) return;

	// SampleLoad() must run on the GUI thread each tick: it reads the PDH
	// counter and refreshes the CPU power estimate (powerWatts).
	m_cpu.SampleLoad();

	const CpuMetrics cpu = m_cpu.GetMetrics();
	const RamMetrics ram = m_ram.GetMetrics();
	const std::vector<GpuMetrics> gpus =
		m_gpu.IsAvailable() ? m_gpu.GetMetrics() : std::vector<GpuMetrics>();

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
	const size_t n = std::min(gpus.size(), m_gpuPanels.size());
	for (size_t i = 0; i < n; ++i)
	{
		const GpuMetrics& gpu = gpus[i];
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

	if (m_cpuPowerGraph && cpu.powerWatts > 0.0)
		m_cpuPowerGraph->AddPoint(cpu.powerWatts);
	if (m_totalPowerGraph)
		m_totalPowerGraph->AddPoint(totalWatts);

	// Redraw section borders every tick so they stay on top of children.
	RefreshSectionBoxes();
	Refresh();
}

void MainFrame::OnClose(wxCloseEvent& evt)
{
	int x = 0, y = 0, w = 0, h = 0;
	GetPosition(&x, &y);
	GetClientSize(&w, &h);
	Config::SaveWindowPosition(x, y, w, h);

	if (m_timer.IsRunning()) m_timer.Stop();
	m_cpu.Stop();
	m_ram.Stop();
	if (m_gpu.IsAvailable()) m_gpu.Stop();

	evt.Skip();
}

void MainFrame::RefreshSectionBoxes()
{
	m_sectionBoxes.clear();
	if (!m_content) return;
	wxSizer* sizer = m_content->GetSizer();
	if (!sizer) return;

	// Find CPU section bounds (from first item to first GPU title).
	int cpuStart = 0, cpuEnd = -1;
	std::vector<int> gpuStarts;

	for (int i = 0; i < sizer->GetItemCount(); ++i)
	{
		wxSizerItem* item = sizer->GetItem(static_cast<size_t>(i));
		if (!item) continue;
		wxWindow* win = item->GetWindow();
		if (!win) continue;
		wxStaticText* st = dynamic_cast<wxStaticText*>(win);
		if (st && st->GetLabel().Find(wxT("GPU ")) == 0)
		{
			if (cpuEnd < 0) cpuEnd = i;
			gpuStarts.push_back(i);
		}
		if (win == m_totalPowerGraph)
		{
			if (cpuEnd < 0) cpuEnd = i;
			for (size_t k = 0; k < gpuStarts.size(); ++k)
				if (gpuStarts[k] > i) gpuStarts[k] = -1;
		}
	}
	if (cpuEnd < 0) cpuEnd = gpuStarts.empty() ? sizer->GetItemCount() : gpuStarts[0];

	// CPU box.
	{
		int lx = 9999, ly = 9999, rx = 0, by = 0;
		for (int i = cpuStart; i < cpuEnd; ++i)
		{
			wxSizerItem* si = sizer->GetItem(static_cast<size_t>(i));
			if (!si || !si->IsShown()) continue;
			wxWindow* w = si->GetWindow();
			if (!w) continue;
			int x = 0, y = 0, ww = 0, hh = 0;
			w->GetPosition(&x, &y); w->GetSize(&ww, &hh);
			if (x < lx) lx = x; if (y < ly) ly = y;
			if (x + ww > rx) rx = x + ww; if (y + hh > by) by = y + hh;
		}
		if (ly < 9999)
			m_sectionBoxes.push_back({ "CPU", wxRect(lx - 3, ly - 18, rx - lx + 6, by - ly + 3) });
	}

	// GPU boxes.
	for (size_t gi = 0; gi < gpuStarts.size(); ++gi)
	{
		int gs = gpuStarts[gi];
		if (gs < 0) continue;
		int ge = -1;
		for (size_t j = static_cast<size_t>(gs + 1); j < sizer->GetItemCount(); ++j)
		{
			wxWindow* w = sizer->GetItem(j)->GetWindow();
			if (!w) continue;
			wxStaticText* st = dynamic_cast<wxStaticText*>(w);
			if (st && st->GetLabel().Find(wxT("GPU ")) == 0) { ge = static_cast<int>(j); break; }
			if (w == m_totalPowerGraph) { ge = static_cast<int>(j); break; }
		}
		if (ge < 0) ge = sizer->GetItemCount();

		int lx = 9999, ly = 9999, rx = 0, by = 0;
		for (int i = gs; i < ge; ++i)
		{
			wxSizerItem* si = sizer->GetItem(static_cast<size_t>(i));
			if (!si || !si->IsShown()) continue;
			wxWindow* w = si->GetWindow();
			if (!w) continue;
			int x = 0, y = 0, ww = 0, hh = 0;
			w->GetPosition(&x, &y); w->GetSize(&ww, &hh);
			if (x < lx) lx = x; if (y < ly) ly = y;
			if (x + ww > rx) rx = x + ww; if (y + hh > by) by = y + hh;
		}
		if (ly < 9999)
		{
			wxString title = sizer->GetItem(static_cast<size_t>(gs))->GetWindow()->GetLabel();
			m_sectionBoxes.push_back({ title, wxRect(lx - 3, ly - 18, rx - lx + 6, by - ly + 3) });
		}
	}

	// Total Power box.
	for (int i = 0; i < sizer->GetItemCount(); ++i)
	{
		wxSizerItem* si = sizer->GetItem(static_cast<size_t>(i));
		if (!si || !si->IsShown()) continue;
		if (si->GetWindow() == m_totalPowerGraph)
		{
			int x = 0, y = 0, ww = 0, hh = 0;
			m_totalPowerGraph->GetPosition(&x, &y);
			m_totalPowerGraph->GetSize(&ww, &hh);
			m_sectionBoxes.push_back({ "Total Power", wxRect(x - 3, y - 2, ww + 6, hh + 2) });
			break;
		}
	}
}

void MainFrame::OnSize(wxSizeEvent& evt)
{
	RefreshSectionBoxes();
	m_needFrameRefresh = true;
	Refresh();
	evt.Skip();
}

void MainFrame::OnPaint(wxPaintEvent& evt)
{
	wxPaintDC dc(this);
	const wxColour frameCol(200, 200, 200);
	dc.SetPen(wxPen(frameCol, 1));
	dc.SetBrush(*wxTRANSPARENT_BRUSH);
	for (const SectionBox& sb : m_sectionBoxes)
	{
		const wxRect& r = sb.rect;
		dc.DrawRectangle(r.x + 1, r.y + 1, r.width - 2, r.height - 2);
	}
	LOGI("OnPaint: %d frames", static_cast<int>(m_sectionBoxes.size()));
	evt.Skip();
}

void MainFrame::ApplyTheme()
{
	const wxColour bg   = Theme::Bg();
	const wxColour card = Theme::Card();
	const wxColour text = Theme::Text();
	const wxColour sub  = Theme::SubText();

	SetBackgroundColour(bg);
	if (m_header)  m_header->SetBackgroundColour(card);
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
}

void MainFrame::AddThemedText(wxStaticText* st, bool primary)
{
	if (!st) return;
	if (primary) m_primaryTexts.push_back(st);
	else         m_secondaryTexts.push_back(st);
}
