#include "main_frame.h"
#include "platform.h"
#include "config.h"
#include "logger.h"
#include "theme.h"
#include "graph_panel.h"
#include "power_table.h"
#include <wx/statline.h>
#include <cstdint>

#ifdef PLATFORM_WINDOWS
#include <windows.h>
#endif

wxBEGIN_EVENT_TABLE(MainFrame, wxFrame)
    EVT_TIMER(wxID_ANY, MainFrame::OnTimer)
    EVT_CLOSE(MainFrame::OnClose)
wxEND_EVENT_TABLE()

// Temperature above which the temp graphs turn red (Celsius).
static const double kAlarmTempC = 80.0;

// Format system uptime as YY:MM:DD:HH:MM:SS.
static wxString FormatUptime(uint64_t totalSec)
{
    const uint64_t secPerYear  = 365ULL * 24ULL * 3600ULL;
    const uint64_t secPerMonth = 30ULL * 24ULL * 3600ULL;
    const uint64_t secPerDay   = 24ULL * 3600ULL;

    uint64_t yy = totalSec / secPerYear;  totalSec %= secPerYear;
    uint64_t mm = totalSec / secPerMonth; totalSec %= secPerMonth;
    uint64_t dd = totalSec / secPerDay;   totalSec %= secPerDay;
    uint64_t hh = totalSec / 3600ULL;     totalSec %= 3600ULL;
    uint64_t mi = totalSec / 60ULL;
    uint64_t ss = totalSec % 60ULL;

    return wxString::Format("%02lu:%02lu:%02lu:%02lu:%02lu:%02lu",
                            static_cast<unsigned long>(yy),
                            static_cast<unsigned long>(mm),
                            static_cast<unsigned long>(dd),
                            static_cast<unsigned long>(hh),
                            static_cast<unsigned long>(mi),
                            static_cast<unsigned long>(ss));
}

// ----------------------------------------------------------------------------
// Construction / destruction
// ----------------------------------------------------------------------------
MainFrame::MainFrame(const wxString& title)
{
    int x = 100, y = 100, w = 640, h = 720;
    bool hasPos = Config::LoadWindowPosition(x, y);
    bool hasSize = Config::LoadWindowSize(w, h);

    wxPoint pos = hasPos ? wxPoint(x, y) : wxDefaultPosition;
    wxSize  size = hasSize ? wxSize(w, h) : wxSize(640, 720);

    Create(nullptr, wxID_ANY, title, pos, size,
           wxDEFAULT_FRAME_STYLE | wxRESIZE_BORDER);

    // Application icon — loaded from a PNG in the current working directory.
    // Fails silently when the file is missing (no crash).
    {
        wxString iconPath = wxGetCwd() + "/" + "loadgraph.png";
        wxIcon icon(iconPath);
        if (icon.IsOk())
            SetIcon(icon);
    }

    LOGI("Starting metric collectors...");

    m_cpu.QueryCpuName();
    m_cpu.CollectNow();
    m_cpu.Start();
    LOGI("CPU monitor started (temp=%s fan=%s).",
         m_cpu.GetMetrics().temperature >= 0.0 ? "OK" : "N/A",
         m_cpu.GetMetrics().fanRpm >= 0 ? "OK" : "N/A");

    m_ram.CollectNow();
    m_ram.Start();
    LOGI("RAM monitor started (total=%llu bytes).",
         static_cast<unsigned long long>(m_ram.GetMetrics().totalBytes));

    m_gpu.Start();
    LOGI("GPU monitor started (NVML/NVAPI available: %s)",
         m_gpu.IsAvailable() ? "yes" : "no");

    if (m_gpu.IsAvailable())
    {
        m_gpu.CollectNow();
        auto gm = m_gpu.GetMetrics();
        LOGI("Initial GPU collect done: %zu GPUs found.", gm.size());
    }

    // Update GUI every 1000 ms - matches polling rate.
    m_timer.SetOwner(this, wxID_ANY);
    m_timer.Start(1000);
    LOGI("Timer started.");

    // Create UI here — before Show(true) so the sizer is applied to a
    // fully-built window and the first paint shows content immediately.
    RealizeUI();
    SetMinSize(wxSize(480, 540));

    // Apply saved View menu state (hidden graphs restored from previous run).
    std::map<std::string, bool> viewState;
    Config::LoadViewMenuState(viewState);
    for (const auto& kv : m_viewToggles)
    {
        auto it = viewState.find(kv.first);
        bool show = (it == viewState.end()) ? true : it->second;
        kv.second->Show(show);
    }

    // Build the View menu with checkboxes for every graph panel.
    wxMenuBar* menuBar = new wxMenuBar;
    BuildViewMenu(menuBar, viewState);
    SetMenuBar(menuBar);
}

MainFrame::~MainFrame()
{
    LOGI("MainFrame destructor...");
    m_timer.Stop();
    m_cpu.Stop();
    m_ram.Stop();
    m_gpu.Stop();
    LOGI("MainFrame destructor done.");
}

// ----------------------------------------------------------------------------
// Event handlers
// ----------------------------------------------------------------------------
void MainFrame::OnClose(wxCloseEvent& evt)
{
    Config::SaveWindowPosition(GetPosition().x, GetPosition().y,
                               GetSize().GetWidth(), GetSize().GetHeight());
    LOGI("Saved window pos=%dx%d size=%dx%d.",
         GetPosition().x, GetPosition().y,
         GetSize().GetWidth(), GetSize().GetHeight());
    evt.Skip();
}

void MainFrame::OnTimer(wxTimerEvent& /*evt*/)
{
#ifdef PLATFORM_LINUX
    // On Linux (wxWidgets 3.0.x / GTK) creating child windows before the GTK
    // widget is realized causes SIGSEGV. We skip UI creation entirely on this
    // platform — the app runs and collects metrics but shows no graphs.
    if (!m_uiCreated)
    {
        m_uiCreated = true;
        LOGI("UI skipped (wxWidgets 3.0.x GTK limitation).");
    }
#endif

    // ---- CPU ----
    const double cpuLoad = m_cpu.SampleLoad();
    const auto cm = m_cpu.GetMetrics();

    if (m_uptimeText)
        m_uptimeText->SetLabel(wxString::Format("Uptime %s",
                                                FormatUptime(cm.uptimeMs / 1000ULL)));

    if (m_cpuLoadGraph)
        m_cpuLoadGraph->AddPoint(cpuLoad >= 0.0 ? cpuLoad : cm.loadPercent);

    if (m_cpuTempGraph && cm.temperature >= 0.0)
        m_cpuTempGraph->AddPoint(cm.temperature);
    if (m_cpuFanGraph && cm.fanRpm >= 0)
        m_cpuFanGraph->AddPoint(static_cast<double>(cm.fanRpm));

    // ---- CPU power (watts) ----
    if (m_cpuPowerGraph && cm.powerWatts >= 0.0)
        m_cpuPowerGraph->AddPoint(cm.powerWatts);

    // ---- RAM usage in GB ----
    const auto rm = m_ram.GetMetrics();
    if (m_ramUsageGraph && rm.totalBytes > 0)
    {
        const double totalGb = std::round(static_cast<double>(rm.totalBytes) /
                                          1073741824.0);
        const double usedGb = static_cast<double>(rm.totalBytes - rm.freeBytes) /
                              1073741824.0;
        m_ramUsageGraph->SetYRange(0.0, totalGb);
        m_ramUsageGraph->AddPoint(usedGb);
    }

    // ---- GPU ----
    const auto gm = m_gpu.GetMetrics();
    for (size_t g = 0; g < m_gpuPanels.size() && g < gm.size(); ++g)
    {
        auto& pd   = m_gpuPanels[g];
        auto& meta = gm[g];

        if (pd.loadGraph)
            pd.loadGraph->AddPoint(meta.loadPercent);

        if (pd.tempGraph && meta.temperature >= 0.0)
            pd.tempGraph->AddPoint(meta.temperature);

        if (pd.vramGraph)
        {
            double vramGb = 16.0;
            if (meta.vramTotalBytes > 0)
            {
                vramGb = std::round(static_cast<double>(meta.vramTotalBytes) /
                                    1073741824.0);
                pd.vramGraph->SetYRange(0.0, vramGb);
            }
            const double vramUsedGb = static_cast<double>(meta.vramUsedBytes) /
                                      1073741824.0;
            pd.vramGraph->AddPoint(vramUsedGb);
        }

        if (pd.fanGraph && meta.fanPercent >= 0.0)
            pd.fanGraph->AddPoint(meta.fanPercent);

        if (pd.powerGraph && meta.powerWatts >= 0.0)
            pd.powerGraph->AddPoint(meta.powerWatts);
    }

    // ---- Total power: CPU + every GPU (watts) ----
    if (m_totalPowerGraph)
    {
        double total = 0.0;
        bool   any   = false;
        if (cm.powerWatts >= 0.0)
        {
            total += cm.powerWatts;
            any = true;
        }
        for (const auto& meta : gm)
        {
            if (meta.powerWatts >= 0.0)
            {
                total += meta.powerWatts;
                any = true;
            }
        }
        if (any)
            m_totalPowerGraph->AddPoint(total);
    }

    // ---- React to a live system light/dark scheme switch ----
    if (Theme::Update())
    {
        LOGI("System color scheme changed to %s, reapplying theme.",
             Theme::IsDark() ? "dark" : "light");
        ApplyTheme();
    }
}

// ----------------------------------------------------------------------------
// UI creation
// ----------------------------------------------------------------------------
void MainFrame::RealizeUI()
{
    LOGI("RealizeUI started.");
    Theme::Update();
    LOGI("System color scheme: %s", Theme::IsDark() ? "dark" : "light");

    SetBackgroundColour(Theme::Bg());
    LOGI("Background colour set.");

    wxBoxSizer* mainSizer = new wxBoxSizer(wxVERTICAL);
    LOGI("Main sizer created.");

    LOGI("Creating header panel...");
    {
        wxPanel* header = new wxPanel(this, wxID_ANY);
        LOGI("Header ptr=%p", static_cast<void*>(header));
        header->SetBackgroundColour(Theme::Bg());
        m_header = header;
        header->SetMinSize(wxSize(-1, 2));
        mainSizer->Add(header, 0, wxEXPAND);
        LOGI("Header added.");
    }

    LOGI("Creating content panel...");
    wxPanel* content = new wxPanel(this, wxID_ANY);
    LOGI("Content ptr=%p", static_cast<void*>(content));
    content->SetBackgroundColour(Theme::Bg());
    // NB: content panel must keep the DEFAULT background style. It has no
    // OnPaint handler, so wxBG_STYLE_CUSTOM would leave its background
    // unpainted - causing white contours around graph cards and stale
    // pixels after a resize. Only GraphPanel (which paints itself) uses
    // wxBG_STYLE_CUSTOM.
    m_content = content;
    LOGI("Content created.");

    wxBoxSizer* container = new wxBoxSizer(wxVERTICAL);
    LOGI("Container sizer created.");

    LOGI("Building CPU section...");
    BuildCpuSection(container, content);
    LOGI("CPU section built.");
    LOGI("Building GPU section...");
    BuildGpuSection(container, content);
    LOGI("GPU section built.");
    LOGI("Building total power row...");
    BuildTotalPowerRow(container, content);
    LOGI("Total power row built.");

    content->SetSizer(container);
    mainSizer->Add(content, 1, wxEXPAND);
    SetSizer(mainSizer);
    Layout(); // force immediate layout calculation on already-visible window
    LOGI("RealizeUI completed.");
}

// ----------------------------------------------------------------------------
// Theme support
// ----------------------------------------------------------------------------
void MainFrame::AddThemedText(wxStaticText* st, bool primary)
{
    if (!st)
        return;
    (primary ? m_primaryTexts : m_secondaryTexts).push_back(st);
}

void MainFrame::ApplyTheme()
{
    SetBackgroundColour(Theme::Bg());
    if (m_header)
        m_header->SetBackgroundColour(Theme::Bg());
    if (m_content)
        m_content->SetBackgroundColour(Theme::Bg());

    for (wxStaticText* st : m_primaryTexts)
        if (st) st->SetForegroundColour(Theme::Text());
    for (wxStaticText* st : m_secondaryTexts)
        if (st) st->SetForegroundColour(Theme::SubText());

    const auto refreshGraph = [](GraphPanel* gp) {
        if (gp)
        {
            gp->SetBackgroundColour(Theme::Card());
            gp->Refresh();
        }
    };
    refreshGraph(m_cpuLoadGraph);
    refreshGraph(m_ramUsageGraph);
    refreshGraph(m_cpuTempGraph);
    refreshGraph(m_cpuFanGraph);
    refreshGraph(m_cpuPowerGraph);
    refreshGraph(m_totalPowerGraph);
    for (const auto& pd : m_gpuPanels)
    {
        refreshGraph(pd.loadGraph);
        refreshGraph(pd.tempGraph);
        refreshGraph(pd.vramGraph);
        refreshGraph(pd.fanGraph);
        refreshGraph(pd.powerGraph);
    }

    Refresh();
}

void MainFrame::BuildViewMenu(wxMenuBar* menuBar,
                               const std::map<std::string, bool>& initialState)
{
    wxMenu* viewMenu = new wxMenu;
    // Individual graph toggles — rendered as checkboxes.
    for (const auto& kv : m_viewToggles)
    {
        wxMenuItem* item = viewMenu->AppendCheckItem(wxID_ANY, kv.first);
        auto it = initialState.find(kv.first);
        bool show = (it == initialState.end()) ? true : it->second;
        item->Check(show);
        m_menuIdToKey[item->GetId()] = kv.first;
        Connect(item->GetId(), wxEVT_MENU,
                wxCommandEventHandler(MainFrame::OnViewToggle));
    }

    // One checkbox per GPU metric type (affects ALL GPUs at once).
    static const struct { const char* key; const wchar_t* label; } gpuGroups[] = {
        { "gpu_load",  L"GPU Load"  },
        { "gpu_temp",  L"GPU Temp"  },
        { "gpu_vram",  L"GPU VRAM"  },
        { "gpu_fan",   L"GPU Fan"   },
        { "gpu_power", L"GPU Power" },
    };
    for (const auto& g : gpuGroups)
    {
        // Collect panels belonging to this group.
        GpuGroupToggle grp;
        grp.key = g.key;
        for (const auto& pd : m_gpuPanels)
        {
            GraphPanel* p = nullptr;
            if (g.key == "gpu_load")  p = pd.loadGraph;
            if (g.key == "gpu_temp")  p = pd.tempGraph;
            if (g.key == "gpu_vram")  p = pd.vramGraph;
            if (g.key == "gpu_fan")   p = pd.fanGraph;
            if (g.key == "gpu_power") p = pd.powerGraph;
            if (p) grp.panels.push_back(p);
        }
        m_gpuGroupToggles.push_back(std::move(grp));

        wxMenuItem* item = viewMenu->AppendCheckItem(wxID_ANY, g.label);
        auto it = initialState.find(g.key);
        bool show = (it == initialState.end()) ? true : it->second;
        // A group is checked only if ALL its panels are visible.
        for (GraphPanel* pp : m_gpuGroupToggles.back().panels)
            show = show && pp->IsShown();
        item->Check(show);
        m_menuIdToKey[item->GetId()] = g.key;
        Connect(item->GetId(), wxEVT_MENU,
                wxCommandEventHandler(MainFrame::OnViewToggle));
    }

    viewMenu->AppendSeparator();
    {
        wxMenuItem* all = viewMenu->AppendCheckItem(wxID_ANY, L"Show all");
        Connect(all->GetId(), wxEVT_MENU,
                wxCommandEventHandler(MainFrame::OnViewToggle));
        m_menuIdToKey[all->GetId()] = "__show_all__";
    }
    {
        wxMenuItem* none = viewMenu->AppendCheckItem(wxID_ANY, L"Hide all");
        Connect(none->GetId(), wxEVT_MENU,
                wxCommandEventHandler(MainFrame::OnViewToggle));
        m_menuIdToKey[none->GetId()] = "__hide_all__";
    }
    menuBar->Append(viewMenu, L"&View");
}

void MainFrame::OnViewToggle(wxCommandEvent& evt)
{
    auto it = m_menuIdToKey.find(evt.GetId());
    if (it == m_menuIdToKey.end()) return;
    const std::string key = it->second;

    std::map<std::string, bool> state;
    Config::LoadViewMenuState(state);

    if (key == "__show_all__")
    {
        for (const auto& kv : m_viewToggles)
            kv.second->Show(true);
        for (auto& grp : m_gpuGroupToggles)
            for (GraphPanel* p : grp.panels)
                p->Show(true);
        // Record every key as checked.
        for (const auto& kv : m_viewToggles) state[kv.first] = true;
        for (const auto& g : { "gpu_load", "gpu_temp", "gpu_vram", "gpu_fan", "gpu_power" })
            state[g] = true;
    }
    else if (key == "__hide_all__")
    {
        for (const auto& kv : m_viewToggles)
            kv.second->Show(false);
        for (auto& grp : m_gpuGroupToggles)
            for (GraphPanel* p : grp.panels)
                p->Show(false);
        for (const auto& kv : m_viewToggles) state[kv.first] = false;
        for (const auto& g : { "gpu_load", "gpu_temp", "gpu_vram", "gpu_fan", "gpu_power" })
            state[g] = false;
    }
    else if (key == "gpu_load" || key == "gpu_temp" || key == "gpu_vram" ||
             key == "gpu_fan"  || key == "gpu_power")
    {
        // Unified GPU group toggle: show/hide this metric across ALL GPUs.
        bool show = evt.IsChecked();
        for (auto& grp : m_gpuGroupToggles)
        {
            if (grp.key != key) continue;
            for (GraphPanel* p : grp.panels)
                p->Show(show);
            state[grp.key] = show;
        }
    }
    else
    {
        // Single graph toggle.
        auto pit = m_viewToggles.find(key);
        if (pit != m_viewToggles.end())
        {
            bool show = evt.IsChecked();
            pit->second->Show(show);
            state[key] = show;
        }
    }

    this->Layout();
    Config::SaveViewMenuState(state);
}

// ----------------------------------------------------------------------------
// CPU / RAM block:
//   row 1: CPU Load + RAM Usage        (always present, side by side)
//   row 2: CPU Temp                   (only when the sensor exists)
//   row 3: CPU Fan Speed              (only when the sensor exists)
// ----------------------------------------------------------------------------
void MainFrame::BuildCpuSection(wxBoxSizer* container, wxWindow* parent)
{
    const auto cm = m_cpu.GetMetrics();
    const auto rm = m_ram.GetMetrics();

    // Section header with CPU model name and digital uptime on the right.
    {
        wxBoxSizer* hdr = new wxBoxSizer(wxHORIZONTAL);

        wxString cpuLabel = L"CPU";
        if (!cm.cpuName.empty())
            cpuLabel += L"  " + wxString(cm.cpuName.c_str(), wxConvUTF8);
        else
            cpuLabel += L"  (N/A)";

        wxStaticText* st = new wxStaticText(parent, wxID_ANY, cpuLabel);
        st->SetFont(wxFont(11, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
                           wxFONTWEIGHT_BOLD));
        st->SetForegroundColour(Theme::Text());
        AddThemedText(st, true);
        hdr->Add(st, 0, wxTOP | wxBOTTOM, 2);

        hdr->AddStretchSpacer();

        m_uptimeText = new wxStaticText(parent, wxID_ANY, L"Uptime 00:00:00:00:00:00");
        m_uptimeText->SetFont(wxFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
                                     wxFONTWEIGHT_NORMAL));
        m_uptimeText->SetForegroundColour(Theme::SubText());
        AddThemedText(m_uptimeText, false);
        hdr->Add(m_uptimeText, 0, wxALIGN_CENTER_VERTICAL | wxBOTTOM, 2);

        container->Add(hdr, 0, wxEXPAND);
    }

    // ---- Row 1: CPU Load (0..100%) + RAM Usage (0..total GB) side by side ----
    {
        wxBoxSizer* row = new wxBoxSizer(wxHORIZONTAL);
        row->AddSpacer(4);

        m_cpuLoadGraph = new GraphPanel(parent, L"CPU Load", L"%");
        m_cpuLoadGraph->SetYRange(0.0, 100.0);
        m_cpuLoadGraph->SetYMode(GraphYMode::Percent);
        row->Add(m_cpuLoadGraph, 1, wxEXPAND | wxBOTTOM, 2);
        m_viewToggles["cpu_load"] = m_cpuLoadGraph;

        m_ramUsageGraph = new GraphPanel(parent, L"RAM Usage", L"GB");
        if (rm.totalBytes > 0)
        {
            const double totalGb = std::round(static_cast<double>(rm.totalBytes) /
                                              1073741824.0);
            m_ramUsageGraph->SetYRange(0.0, totalGb);
        }
        else
        {
            m_ramUsageGraph->SetYRange(0.0, 16.0);
        }
        m_ramUsageGraph->SetYMode(GraphYMode::Gigabytes);
        // Horizontal gap via a separate spacer. wxSizer applies ONE border value
        // to every side flag, so "wxBOTTOM | wxLEFT, 8" would give this graph an
        // 8px bottom border too (the left graph has 2px) and render the right
        // card 6px shorter. A spacer occupies only the main axis, so heights match.
        row->AddSpacer(8);
        row->Add(m_ramUsageGraph, 1, wxEXPAND | wxBOTTOM, 2);
        m_viewToggles["ram_usage"] = m_ramUsageGraph;

        row->AddSpacer(4);
        container->Add(row, 1, wxEXPAND);
        container->AddSpacer(2);
    }

    // ---- CPU Power (watts, full width row) ----
    {
        wxBoxSizer* row = new wxBoxSizer(wxHORIZONTAL);
        row->AddSpacer(4);
        m_cpuPowerGraph = new GraphPanel(parent, L"CPU Power", L"W");
        const double cpuTdp = (cm.tdpWatts > 0.0) ? cm.tdpWatts : 125.0;
        m_cpuPowerGraph->SetYRange(0.0, cpuTdp);
        m_cpuPowerGraph->SetYMode(GraphYMode::Watts);
        row->Add(m_cpuPowerGraph, 1, wxEXPAND | wxBOTTOM, 2);
        row->AddSpacer(4);
        container->Add(row, 1, wxEXPAND);
        container->AddSpacer(2);
        m_viewToggles["cpu_power"] = m_cpuPowerGraph;
    }

    // ---- Row 2: CPU Temp (only when available) ----
    const bool hasTemp = (cm.temperature >= 0.0);
    const bool hasFan  = (cm.fanRpm >= 0);

    if (hasTemp)
    {
        wxBoxSizer* row = new wxBoxSizer(wxHORIZONTAL);
        row->AddSpacer(4);
        m_cpuTempGraph = new GraphPanel(parent, L"CPU Temp", L"C");
        m_cpuTempGraph->SetYRange(0.0, 100.0);
        m_cpuTempGraph->SetAlarmThreshold(kAlarmTempC);
        m_cpuTempGraph->SetYMode(GraphYMode::Celsius);
        row->Add(m_cpuTempGraph, 1, wxEXPAND | wxBOTTOM, 2);
        row->AddSpacer(4);
        container->Add(row, 1, wxEXPAND);
        container->AddSpacer(2);
        m_viewToggles["cpu_temp"] = m_cpuTempGraph;
    }

    // ---- Row 3: CPU Fan Speed (only when available) ----
    if (hasFan)
    {
        wxBoxSizer* row = new wxBoxSizer(wxHORIZONTAL);
        row->AddSpacer(4);
        m_cpuFanGraph = new GraphPanel(parent, L"CPU Fan Speed", L"RPM");
        row->Add(m_cpuFanGraph, 1, wxEXPAND | wxBOTTOM, 2);
        row->AddSpacer(4);
        container->Add(row, 1, wxEXPAND);
        container->AddSpacer(2);
        m_viewToggles["cpu_fan"] = m_cpuFanGraph;
    }
    else if (!hasTemp)
    {
        LOGI("CPU temp/fan sensors unavailable - sensor rows hidden.");
    }
}

// ----------------------------------------------------------------------------
// GPU Section - dynamic per GPU:
//   row 1: Load + Temp
//   row 2: VRAM Usage + Fan
// ----------------------------------------------------------------------------
void MainFrame::BuildGpuSection(wxBoxSizer* container, wxWindow* parent)
{
    if (!m_gpu.IsAvailable())
    {
        wxStaticText* info = new wxStaticText(parent, wxID_ANY,
                                              L"NVML / NVAPI not available - no Nvidia GPU detected.");
        info->SetForegroundColour(Theme::SubText());
        AddThemedText(info, false);
        container->Add(info, 0, wxTOP | wxLEFT, 16);
        return;
    }

    // Initial enumeration. Each GPU gets exactly ONE GpuPanelData with all 4 graphs.
    auto gpus = m_gpu.GetMetrics();
    for (size_t g = 0; g < gpus.size(); ++g)
    {
        const auto& meta = gpus[g];

        // GPU name label.
        {
            wxString name = L"GPU " + wxString::Format("%zu", g);
            if (!meta.name.empty())
                name += L"  " + wxString(meta.name.c_str(), wxConvUTF8);
            if (meta.vramTotalBytes > 0)
            {
                const double vramGb = std::round(
                    static_cast<double>(meta.vramTotalBytes) / 1073741824.0);
                name += L"  " + wxString::Format(L"%.0fGB", vramGb);
            }
            wxStaticText* st = new wxStaticText(parent, wxID_ANY, name);
            st->SetFont(wxFont(10, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
                               wxFONTWEIGHT_BOLD));
            st->SetForegroundColour(Theme::SubText());
            AddThemedText(st, false);
            container->Add(st, 0, wxTOP | wxBOTTOM, 2);
        }

        // Group sizer: two rows per GPU. ONE GpuPanelData holds all graphs for this GPU.
        wxBoxSizer* group = new wxBoxSizer(wxVERTICAL);

        auto& pd = m_gpuPanels.emplace_back(GpuPanelData{});
        pd.name = meta.name;

        // ---- Row 1: Load + Temp ----
        {
            wxBoxSizer* row = new wxBoxSizer(wxHORIZONTAL);
            row->AddSpacer(4);

            pd.loadGraph = new GraphPanel(parent, L"Load", L"%");
            pd.loadGraph->SetYRange(0.0, 100.0);
            pd.loadGraph->SetYMode(GraphYMode::Percent);
            pd.loadGraph->SetPurple(true);
            row->Add(pd.loadGraph, 1, wxEXPAND | wxBOTTOM, 2);

            // Horizontal gap as a separate spacer (see CPU row): a combined
            // wxBOTTOM | wxLEFT border would give the right graph an 8px bottom
            // border vs 2px on the left, making the right card 6px shorter.
            row->AddSpacer(8);

            if (meta.temperature >= 0.0)
            {
                pd.tempGraph = new GraphPanel(parent, L"Temp", L"C");
                pd.tempGraph->SetYRange(0.0, 100.0);
                pd.tempGraph->SetAlarmThreshold(kAlarmTempC);
                pd.tempGraph->SetYMode(GraphYMode::Celsius);
                pd.tempGraph->SetPurple(true);
                row->Add(pd.tempGraph, 1, wxEXPAND | wxBOTTOM, 2);
            }
            else
            {
                // Placeholder to keep layout consistent.
                auto* ph = new GraphPanel(parent, L"Temp", L"C");
                ph->SetYRange(0.0, 100.0);
                ph->SetYMode(GraphYMode::Celsius);
                ph->SetPurple(true);
                row->Add(ph, 1, wxEXPAND | wxBOTTOM, 2);
            }

            row->AddSpacer(4);
            group->Add(row, 1, wxEXPAND);
        }

        // ---- Row 2: VRAM + Fan ----
        {
            wxBoxSizer* row = new wxBoxSizer(wxHORIZONTAL);
            row->AddSpacer(4);

            pd.vramGraph = new GraphPanel(parent, L"VRAM", L"GB");
            if (meta.vramTotalBytes > 0)
            {
                const double totalGb = std::round(
                    static_cast<double>(meta.vramTotalBytes) / 1073741824.0);
                pd.vramGraph->SetYRange(0.0, totalGb);
            }
            else
            {
                pd.vramGraph->SetYRange(0.0, 16.0);
            }
            pd.vramGraph->SetYMode(GraphYMode::Gigabytes);
            pd.vramGraph->SetPurple(true);
            row->Add(pd.vramGraph, 1, wxEXPAND | wxBOTTOM, 2);

            // Horizontal gap as a separate spacer (see Row 1).
            row->AddSpacer(8);

            if (meta.fanPercent >= 0.0)
            {
                pd.fanGraph = new GraphPanel(parent, L"Fan", L"%");
                pd.fanGraph->SetYRange(0.0, 100.0);
                pd.fanGraph->SetYMode(GraphYMode::Percent);
                pd.fanGraph->SetPurple(true);
                row->Add(pd.fanGraph, 1, wxEXPAND | wxBOTTOM, 2);
            }
            else
            {
                auto* ph = new GraphPanel(parent, L"Fan", L"%");
                ph->SetYRange(0.0, 100.0);
                ph->SetYMode(GraphYMode::Percent);
                ph->SetPurple(true);
                row->Add(ph, 1, wxEXPAND | wxBOTTOM, 2);
            }

            row->AddSpacer(4);
            group->Add(row, 1, wxEXPAND);
        }

        // ---- Row 3: Power draw (watts, full width of the GPU group) ----
        {
            wxBoxSizer* row = new wxBoxSizer(wxHORIZONTAL);
            row->AddSpacer(4);

            pd.powerGraph = new GraphPanel(parent, L"Power", L"W");
            // Y maximum: NVML power limit, falling back to the static TDP table.
            double gpuMax = meta.powerLimitWatts;
            if (gpuMax <= 0.0)
                gpuMax = static_cast<double>(PowerTable::GetGpuTdp(meta.name));
            if (gpuMax <= 0.0)
                gpuMax = 100.0;
            pd.powerGraph->SetYRange(0.0, gpuMax);
            pd.powerGraph->SetYMode(GraphYMode::Watts);
            pd.powerGraph->SetPurple(true);
            row->Add(pd.powerGraph, 1, wxEXPAND | wxBOTTOM, 2);

            row->AddSpacer(4);
            group->Add(row, 1, wxEXPAND);
        }

        group->AddSpacer(2);
        container->Add(group, 2, wxEXPAND);
    }

    container->AddSpacer(2);
}

// ----------------------------------------------------------------------------
// Total Power row - sum of CPU and every GPU power draw, full window width.
// ----------------------------------------------------------------------------
void MainFrame::BuildTotalPowerRow(wxBoxSizer* container, wxWindow* parent)
{
    // Y axis maximum: CPU TDP + every GPU power limit/TDP fallback, then
    // snapped up to the next clean multiple of 50 (avoids ugly tick labels).
    double maxY = 0.0;
    const auto cm = m_cpu.GetMetrics();
    maxY += (cm.tdpWatts > 0.0) ? cm.tdpWatts : 125.0;

    const auto gpus = m_gpu.GetMetrics();
    for (const auto& meta : gpus)
    {
        double gMax = meta.powerLimitWatts;
        if (gMax <= 0.0)
            gMax = static_cast<double>(PowerTable::GetGpuTdp(meta.name));
        if (gMax <= 0.0)
            gMax = 100.0;
        maxY += gMax;
    }
    maxY = std::ceil(maxY / 50.0) * 50.0;

    wxBoxSizer* row = new wxBoxSizer(wxHORIZONTAL);
    row->AddSpacer(4);
    m_totalPowerGraph = new GraphPanel(parent, L"Total Power (CPU + GPU)", L"W");
    m_totalPowerGraph->SetYRange(0.0, maxY);
    m_totalPowerGraph->SetYMode(GraphYMode::Watts);
    // Four evenly-spaced labels: 0, max/3, 2*max/3, max — gives a clean layout
    // regardless of panel height.
    m_totalPowerGraph->SetTickCount(4);
    row->Add(m_totalPowerGraph, 1, wxEXPAND | wxBOTTOM, 2);
    row->AddSpacer(4);
    container->Add(row, 1, wxEXPAND);
    container->AddSpacer(2);
    m_viewToggles["total_power"] = m_totalPowerGraph;
}
