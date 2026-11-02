#include "main_frame.h"
#include "logger.h"
#include "theme.h"
#include "graph_panel.h"
#include <wx/statline.h>
#include <windows.h>

// Registry key for saving/restoring window position and size.
static const wchar_t* kRegPath = L"Software\\LoadGraph";
static const wchar_t* kRegPos  = L"WindowPos";
static const wchar_t* kRegSize = L"WindowSize";

wxBEGIN_EVENT_TABLE(MainFrame, wxFrame)
    EVT_TIMER(wxID_ANY, MainFrame::OnTimer)
    EVT_CLOSE(MainFrame::OnClose)
wxEND_EVENT_TABLE()

// Temperature above which the temp graphs turn red (Celsius).
static const double kAlarmTempC = 80.0;

// Format system uptime as YY:MM:DD:HH:MM:SS.
// Approximation: 1 month = 30 days, 1 year = 365 days (uptime display only).
static wxString FormatUptime(ULONGLONG totalSec)
{
    const ULONGLONG secPerYear  = 365ULL * 24ULL * 3600ULL;
    const ULONGLONG secPerMonth = 30ULL * 24ULL * 3600ULL;
    const ULONGLONG secPerDay   = 24ULL * 3600ULL;

    ULONGLONG yy = totalSec / secPerYear;  totalSec %= secPerYear;
    ULONGLONG mm = totalSec / secPerMonth; totalSec %= secPerMonth;
    ULONGLONG dd = totalSec / secPerDay;   totalSec %= secPerDay;
    ULONGLONG hh = totalSec / 3600ULL;     totalSec %= 3600ULL;
    ULONGLONG mi = totalSec / 60ULL;
    ULONGLONG ss = totalSec % 60ULL;

    return wxString::Format("%02lu:%02lu:%02lu:%02lu:%02lu:%02lu",
                            static_cast<unsigned long>(yy),
                            static_cast<unsigned long>(mm),
                            static_cast<unsigned long>(dd),
                            static_cast<unsigned long>(hh),
                            static_cast<unsigned long>(mi),
                            static_cast<unsigned long>(ss));
}

// ----------------------------------------------------------------------------
// Window position/size persistence via registry.
// ----------------------------------------------------------------------------
static wxPoint LoadWindowPos()
{
    LONG val = 0;
    DWORD size = sizeof(val);
    if (RegGetValueW(HKEY_CURRENT_USER, kRegPath, kRegPos, RRF_RT_REG_DWORD,
                     nullptr, &val, &size) != ERROR_SUCCESS)
        return wxDefaultPosition;
    // Encode x and y in a single DWORD: high word = x, low word = y.
    int x = static_cast<short>(HIWORD(val));
    int y = static_cast<short>(LOWORD(val));
    return wxPoint(x, y);
}

static wxSize LoadWindowSize()
{
    LONG val = 0;
    DWORD size = sizeof(val);
    if (RegGetValueW(HKEY_CURRENT_USER, kRegPath, kRegSize, RRF_RT_REG_DWORD,
                     nullptr, &val, &size) != ERROR_SUCCESS)
        return wxSize(640, 720);
    int w = static_cast<short>(HIWORD(val));
    int h = static_cast<short>(LOWORD(val));
    // Clamp to sane range.
    if (w < 480)  w = 640;
    if (h < 540)  h = 720;
    return wxSize(w, h);
}

static void SaveWindowPos(const wxPoint& pos)
{
    HKEY hKey;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegPath, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &hKey, nullptr) == ERROR_SUCCESS)
    {
        int val = (static_cast<int>(pos.x) << 16) | (static_cast<unsigned short>(pos.y));
        RegSetValueExW(hKey, kRegPos, 0, REG_DWORD,
                       reinterpret_cast<const BYTE*>(&val), sizeof(val));
        RegCloseKey(hKey);
    }
}

static void SaveWindowSize(const wxSize& size)
{
    HKEY hKey;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegPath, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &hKey, nullptr) == ERROR_SUCCESS)
    {
        int val = (static_cast<int>(size.GetWidth()) << 16) |
                  (static_cast<unsigned short>(size.GetHeight()));
        RegSetValueExW(hKey, kRegSize, 0, REG_DWORD,
                       reinterpret_cast<const BYTE*>(&val), sizeof(val));
        RegCloseKey(hKey);
    }
}

MainFrame::MainFrame(const wxString& title)
    : wxFrame(nullptr, wxID_ANY, title,
              LoadWindowPos(), LoadWindowSize(),
              wxDEFAULT_FRAME_STYLE | wxRESIZE_BORDER)
{
    LOGI("MainFrame constructor started.");
    // 1.5x narrower than the previous 720 px minimum.
    SetMinSize(wxSize(480, 540));

    try
    {
        // Synchronous first collect BEFORE starting the polling threads:
        // BuildCpuSection() must know whether the WMI temperature/fan sensors
        // exist so it can skip the whole row when both report N/A. Doing it
        // here also means the one-shot WMI probe runs once, on this thread,
        // and the background thread will see m_wmiTried already set.
        LOGI("Starting metric collectors...");

        // Read CPU model name from registry before UI creation.
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

        // Synchronous initial collect so BuildGpuSection sees GPU list.
        if (m_gpu.IsAvailable())
        {
            LOGI("Initial GPU collect...");
            m_gpu.CollectNow();
            auto gm = m_gpu.GetMetrics();
            LOGI("Initial GPU collect done: %zu GPUs found.", gm.size());
            for (size_t i = 0; i < gm.size(); ++i)
                LOGI("  GPU %zu: %s", i, gm[i].name.c_str());
        }

        // Create UI.
        LOGI("Creating UI...");
        CreateUI();
        LOGI("UI created successfully.");
    }
    catch (const std::exception& e)
    {
        LOGE("Exception in MainFrame constructor: %s", e.what());
    }
    catch (...)
    {
        LOGE("Unknown exception in MainFrame constructor!");
    }

    // Update GUI every 1000 ms - matches polling rate.
    // Timer events go to this frame (owner must be set explicitly).
    m_timer.SetOwner(this, wxID_ANY);
    m_timer.Start(1000);
    LOGI("Timer started, owner=0x%p.", static_cast<void*>(this));
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

void MainFrame::OnClose(wxCloseEvent& evt)
{
    // Remember the current position and size.
    SaveWindowPos(GetPosition());
    SaveWindowSize(GetSize());
    LOGI("Saved window pos=%dx%d size=%dx%d.",
         GetPosition().x, GetPosition().y, GetSize().GetWidth(), GetSize().GetHeight());
    evt.Skip(); // allow the frame to actually close
}

void MainFrame::CreateUI()
{
    LOGI("CreateUI started.");

    // Read the system light/dark scheme before any control is created.
    Theme::Update();
    LOGI("System color scheme: %s", Theme::IsDark() ? "dark" : "light");

    SetBackgroundColour(Theme::Bg());

    wxBoxSizer* mainSizer = new wxBoxSizer(wxVERTICAL);

    // ---- Title bar area (static) ----
    {
        wxPanel* header = new wxPanel(this, wxID_ANY);
        header->SetBackgroundColour(Theme::Bg());
        m_header = header;
        wxBoxSizer* hSizer = new wxBoxSizer(wxHORIZONTAL);

        wxStaticText* stTitle = new wxStaticText(header, wxID_ANY,
                                                 L"Performance Monitor",
                                                 wxDefaultPosition, wxDefaultSize,
                                                 wxALIGN_CENTER_VERTICAL);
        stTitle->SetFont(wxFont(14, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
                                wxFONTWEIGHT_BOLD));
        stTitle->SetForegroundColour(Theme::Text());
        AddThemedText(stTitle, true);

        hSizer->Add(stTitle, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, 16);
        header->SetSizer(hSizer);
        header->SetMinSize(wxSize(-1, 28));
        mainSizer->Add(header, 0, wxEXPAND);
    }

    // ---- Non-scrolling content area ----
    // A plain panel (no wxScrolledWindow): graph rows carry sizer proportion,
    // so their heights follow the frame height and no scrollbar is needed.
    wxPanel* content = new wxPanel(this, wxID_ANY);
    content->SetBackgroundColour(Theme::Bg());
    m_content = content;

    wxBoxSizer* container = new wxBoxSizer(wxVERTICAL);

    // Separator after header.
    container->Add(new wxStaticLine(content, wxID_ANY), 0, wxEXPAND | wxTOP | wxBOTTOM, 2);

    // Build sections (pass content as parent for graph panels).
    BuildCpuSection(container, content);
    BuildGpuSection(container, content);

    content->SetSizer(container);
    mainSizer->Add(content, 1, wxEXPAND);
    SetSizer(mainSizer);
    LOGI("CreateUI completed.");
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

    // Graph cards repaint with the new palette on the next paint event.
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
    for (const auto& pd : m_gpuPanels)
    {
        refreshGraph(pd.loadGraph);
        refreshGraph(pd.tempGraph);
        refreshGraph(pd.vramGraph);
        refreshGraph(pd.fanGraph);
    }

    Refresh();
}

// ----------------------------------------------------------------------------
// CPU / RAM block:
//   row 1: CPU Load + RAM Usage        (always present, side by side)
//   row 2: CPU Temp                   (only when the sensor exists)
//   row 3: CPU Fan Speed              (only when the sensor exists)
//
// Every row uses proportion 1, and each GPU group uses proportion 2 because it
// contains two rows. With total proportion P each row therefore receives the
// same 1/P share of the extra height, so all graphs end up equally tall.
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
        row->Add(m_ramUsageGraph, 1, wxEXPAND | wxBOTTOM | wxLEFT, 8);

        row->AddSpacer(4);
        container->Add(row, 1, wxEXPAND);
        container->AddSpacer(2);
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
//
// The group carries proportion 2 (two rows) so that each individual GPU row
// gets the same share of the extra height as a single CPU/RAM row.
// ----------------------------------------------------------------------------
void MainFrame::BuildGpuSection(wxBoxSizer* container, wxWindow* parent)
{
    // Separator.
    container->Add(new wxStaticLine(parent, wxID_ANY),
                   0, wxEXPAND | wxTOP | wxBOTTOM, 2);

    // Section header.
    {
        wxStaticText* st = new wxStaticText(parent, wxID_ANY, L"GPU");
        st->SetFont(wxFont(11, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
                           wxFONTWEIGHT_BOLD));
        st->SetForegroundColour(Theme::Text());
        AddThemedText(st, true);
        container->Add(st, 0, wxTOP | wxBOTTOM, 2);
    }

    if (!m_gpu.IsAvailable())
    {
        wxStaticText* info = new wxStaticText(parent, wxID_ANY,
                                              L"NVML / NVAPI not available - no Nvidia GPU detected.");
        info->SetForegroundColour(Theme::SubText());
        AddThemedText(info, false);
        container->Add(info, 0, wxTOP | wxLEFT, 16);
        return;
    }

    // Initial enumeration.
    auto gpus = m_gpu.GetMetrics();
    for (size_t g = 0; g < gpus.size(); ++g)
    {
        GpuPanelData pd;
        pd.name = gpus[g].name.empty() ? wxString::Format(L"GPU %lu", g) :
                                         wxString::FromUTF8(gpus[g].name);

        wxBoxSizer* group = new wxBoxSizer(wxVERTICAL);
        group->AddSpacer(2);

        // GPU name label. The graph titles below repeat only the metric name,
        // because the full device name would not fit next to the value on a
        // narrow window.
        wxStaticText* nameLabel = new wxStaticText(parent, wxID_ANY, pd.name);
        nameLabel->SetFont(wxFont(10, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
                                  wxFONTWEIGHT_BOLD));
        nameLabel->SetForegroundColour(Theme::SubText());
        AddThemedText(nameLabel, false);
        group->Add(nameLabel, 0, wxLEFT | wxBOTTOM, 2);

        // Row 1: Load % (0..100) + Temperature (0..100, red above 80).
        wxBoxSizer* row1 = new wxBoxSizer(wxHORIZONTAL);
        row1->AddSpacer(4);

        pd.loadGraph = new GraphPanel(parent, L"Load", L"%");
        pd.loadGraph->SetYRange(0.0, 100.0);
        pd.loadGraph->SetPurple(true);
        pd.loadGraph->SetYMode(GraphYMode::Percent);
        row1->Add(pd.loadGraph, 1, wxEXPAND | wxBOTTOM, 2);

        pd.tempGraph = new GraphPanel(parent, L"Temp", L"C");
        pd.tempGraph->SetYRange(0.0, 100.0);
        pd.tempGraph->SetAlarmThreshold(kAlarmTempC);
        pd.tempGraph->SetPurple(true);
        pd.tempGraph->SetYMode(GraphYMode::Celsius);
        row1->Add(pd.tempGraph, 1, wxEXPAND | wxBOTTOM | wxLEFT, 8);

        row1->AddSpacer(4);
        group->Add(row1, 1, wxEXPAND);

        // Row 2: VRAM usage in GB (0..total) + Fan speed (percent).
        wxBoxSizer* row2 = new wxBoxSizer(wxHORIZONTAL);
        row2->AddSpacer(4);

        pd.vramGraph = new GraphPanel(parent, L"VRAM Usage", L"GB");
        // Clamp the axis to the real VRAM size when it is already known.
        // Round to nearest integer to avoid fractional tick labels.
        if (gpus[g].vramTotalBytes > 0)
        {
            const double vramGb = std::round(static_cast<double>(gpus[g].vramTotalBytes) /
                                             1073741824.0);
            pd.vramGraph->SetYRange(0.0, vramGb);
        }
        else
        {
            pd.vramGraph->SetYRange(0.0, 16.0);
        }
        pd.vramGraph->SetPurple(true);
        pd.vramGraph->SetYMode(GraphYMode::Gigabytes);
        row2->Add(pd.vramGraph, 1, wxEXPAND | wxBOTTOM, 2);

        pd.fanGraph = new GraphPanel(parent, L"Fan", L"%");
        pd.fanGraph->SetYRange(0.0, 100.0);
        pd.fanGraph->SetPurple(true);
        pd.fanGraph->SetYMode(GraphYMode::Percent);
        row2->Add(pd.fanGraph, 1, wxEXPAND | wxBOTTOM | wxLEFT, 8);

        row2->AddSpacer(4);
        group->Add(row2, 1, wxEXPAND);

        group->AddSpacer(2);
        // Proportion 2 = two rows: each GPU row then matches a CPU/RAM row.
        container->Add(group, 2, wxEXPAND);

        m_gpuPanels.push_back(pd);
    }

    container->AddSpacer(2);
}

// ----------------------------------------------------------------------------
// Timer - reads metrics and pushes into graph buffers
// ----------------------------------------------------------------------------
void MainFrame::OnTimer(wxTimerEvent& /*evt*/)
{
    // One-shot diagnostics to confirm the timer fires.
    static bool s_loggedFirstTick = false;
    if (!s_loggedFirstTick)
    {
        s_loggedFirstTick = true;
        const auto cm0 = m_cpu.GetMetrics();
        LOGI("OnTimer first tick: cpuLoad=%.1f uptimeMs=%llu sensorRow=%s gpuPanels=%zu",
             cm0.loadPercent, static_cast<unsigned long long>(cm0.uptimeMs),
             (m_cpuTempGraph || m_cpuFanGraph) ? "yes" : "hidden",
             m_gpuPanels.size());
    }

    // ---- CPU ----
    // Sample the load right now via PDH so the value matches Task Manager
    // (no ~1s phase lag from the background thread).
    const double cpuLoad = m_cpu.SampleLoad();
    const auto cm = m_cpu.GetMetrics();

    // Digital uptime readout: YY:MM:DD:HH:MM:SS.
    if (m_uptimeText)
        m_uptimeText->SetLabel(wxString::Format("Uptime %s",
                                                FormatUptime(cm.uptimeMs / 1000ULL)));

    if (m_cpuLoadGraph)
        m_cpuLoadGraph->AddPoint(cpuLoad >= 0.0 ? cpuLoad : cm.loadPercent);

    // Only exists when the sensor was found at startup.
    if (m_cpuTempGraph && cm.temperature >= 0.0)
        m_cpuTempGraph->AddPoint(cm.temperature);
    if (m_cpuFanGraph && cm.fanRpm >= 0)
        m_cpuFanGraph->AddPoint(static_cast<double>(cm.fanRpm));

    // ---- RAM usage in GB ----
    const auto rm = m_ram.GetMetrics();
    if (m_ramUsageGraph && rm.totalBytes > 0)
    {
        // Axis always spans the full physical RAM: 0..total GB.
        // Round to nearest integer to avoid fractional tick labels.
        const double totalGb = std::round(static_cast<double>(rm.totalBytes) /
                                          1073741824.0);
        m_ramUsageGraph->SetYRange(0.0, totalGb);
        const double usedGb = static_cast<double>(rm.totalBytes - rm.freeBytes) /
                              1073741824.0;
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
            // Axis always spans the full VRAM: 0..total GB.
            // Round to nearest integer to avoid fractional tick labels.
            double vramGb = 16.0; // default fallback
            if (meta.vramTotalBytes > 0)
            {
                vramGb = std::round(static_cast<double>(meta.vramTotalBytes) /
                                    1073741824.0);
                pd.vramGraph->SetYRange(0.0, vramGb);
            }
            pd.vramGraph->AddPoint(static_cast<double>(meta.vramUsedBytes) /
                                   1073741824.0);
        }

        if (pd.fanGraph && meta.fanPercent >= 0.0)
            pd.fanGraph->AddPoint(meta.fanPercent);
    }

    // ---- React to a live system light/dark scheme switch ----
    if (Theme::Update())
    {
        LOGI("System color scheme changed to %s, reapplying theme.",
             Theme::IsDark() ? "dark" : "light");
        ApplyTheme();
    }
}
