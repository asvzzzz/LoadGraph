#ifndef MAIN_FRAME_H
#define MAIN_FRAME_H

#include <wx/wx.h>
#include <wx/sizer.h>
#include <unordered_map>
#include <wx/timer.h>
#include <map>
#include "metrics_api.h"
#include "graph_panel.h"
#include "app_config.h"

// Main application window. Resizable, contains CPU / RAM / GPU metric panels.

// Command IDs for custom menu items (wxID_HIGHEST+100 block is used by View).
enum
{
    ID_HTTP_TOGGLE = wxID_HIGHEST + 900,
    // View -> Super I/O fan probe. Kept outside the wxID_HIGHEST+100 block the
    // View toggles allocate sequentially, and outside m_menuIdToKey, so its
    // check state is stored in superio_fan and not in the view_menu map.
    ID_SUPERIO_FAN = wxID_HIGHEST + 910,
};

class MainFrame : public wxFrame
{
public:
    explicit MainFrame(const wxString& title, const AppConfig& cfg);
    ~MainFrame() override;

    // Create the window layout (called on Windows after Show).
    void RealizeUI();

private:
    // Timer callback - reads metrics and refreshes graph panels.
    void OnTimer(wxTimerEvent& evt);

    // Close handler - saves window position/size to registry before closing.
    void OnClose(wxCloseEvent& evt);

    // File -> Exit handler. Needed because window-manager close requests are
    // not always delivered (WSLg/msrdc caption buttons can stop responding).
    void OnExitMenu(wxCommandEvent& evt);

    // File -> HTTP server toggle (checkable). Starts/stops the web dashboard
    // on the configured port and persists the choice in loadgraph.cfg.
    void OnHttpToggle(wxCommandEvent& evt);

    // Size handler.
    void OnSize(wxSizeEvent& evt);

    // Re-apply the system color scheme to all controls (light/dark switch).
    void ApplyTheme();

    // Register a label for theme recoloring (primary = title color).
    void AddThemedText(wxStaticText* st, bool primary);

    // Build the CPU/RAM block: row with CPU Load + RAM Usage, CPU Power row,
    // and CPU Temp / CPU Fan rows omitted entirely when both sensors report N/A.
    void BuildCpuSection(wxBoxSizer* container, wxWindow* parent);

    // Build GPU section (dynamic per-GPU: load/temp row, vram/fan row, power row).
    void BuildGpuSection(wxBoxSizer* container, wxWindow* parent);

    // Build the total power row (CPU + all GPUs) at the bottom of the window.
    void BuildTotalPowerRow(wxBoxSizer* container, wxWindow* parent);

    // Monitoring handled via metrics_api (shared with HTTP server).

    // Timer for periodic GUI updates (1000 ms).
    wxTimer     m_timer;

    // Content panel (CPU / RAM / GPU sections).
    wxPanel*    m_content = nullptr;

    // Static header area (title bar inside the frame).
    wxWindow*   m_header = nullptr;

    // Labels that must be recolored when the system theme changes.
    std::vector<wxStaticText*> m_primaryTexts;
    std::vector<wxStaticText*> m_secondaryTexts;

    // ---- CPU / RAM row (always present) ----
    GraphPanel* m_cpuLoadGraph  = nullptr;
    GraphPanel* m_ramUsageGraph = nullptr;

    // ---- CPU sensor panels ----
    GraphPanel* m_cpuTempGraph  = nullptr;
    GraphPanel* m_cpuFanGraph   = nullptr;

    // Sizer of the CPU RAM row (the slot a GPU VRAM chart occupies). Kept
    // because the fan chart can appear later: a sensor may start reporting
    // only after the UI was built (Super I/O probe, LibreHardwareMonitor
    // started afterwards), and the row is then extended in place, like the
    // web dashboard shows its Fan cell.
    wxBoxSizer* m_cpuFanRow = nullptr;

    // ---- CPU power row (watts) ----
    GraphPanel* m_cpuPowerGraph = nullptr;

    // ---- Total CPU + GPU power row (watts, bottom of the window) ----
    GraphPanel* m_totalPowerGraph = nullptr;

    // ---- Uptime label ----
    wxStaticText* m_uptimeText = nullptr;

    // ---- GPU panels ----
    struct GpuPanelData
    {
        wxString    name;
        GraphPanel* loadGraph  = nullptr;
        GraphPanel* tempGraph  = nullptr;
        GraphPanel* vramGraph  = nullptr;
        GraphPanel* fanGraph   = nullptr;
        GraphPanel* powerGraph = nullptr;
        // Unit the fan panel was created with. Drivers may start or stop
        // reporting RPM later, but a panel must not change its scale and unit
        // under the user, so the choice is fixed when the panel is built.
        bool          fanInPercent = false;
    };
    std::vector<GpuPanelData> m_gpuPanels;

    // Static boxes of the GPU sections. Recolored in ApplyTheme() so the GPU
    // names match the rest of the labels (wxStaticBox uses the system color).
    std::vector<wxWindow*> m_gpuBoxes;

    // Labels that must keep a custom config color even after theme switches.
    struct ColoredLabel { wxWindow* win = nullptr; wxColour color; };
    std::vector<ColoredLabel> m_customColoredLabels;

    // View menu state persistence. Keys map to graph panel pointers so the
    // toggle handler can show/hide a specific chart on demand.
    std::map<std::string, GraphPanel*> m_viewToggles;
    std::map<int, std::string>         m_menuIdToKey;

    // Unified GPU category toggles: one checkbox hides/shows this metric type
    // across ALL GPUs simultaneously (e.g. "gpu_load" controls every GPU's
    // load graph). Populated during BuildGpuSection().
    struct GpuGroupToggle
    {
        std::string key;
        std::vector<GraphPanel*> panels;
    };
    std::vector<GpuGroupToggle> m_gpuGroupToggles;

    // Last known "full" size (all rows visible). Used to restore the window
    // when a previously-hidden row is made visible again.
    wxSize m_lastFullSize;

    void BuildViewMenu(wxMenuBar* menuBar,
                       const std::map<std::string, bool>& initialState);
    void OnViewToggle(wxCommandEvent& evt);

    // View -> CPU fan via Super I/O (Windows only). The probe reads the
    // motherboard monitor chip through direct port access, so the first time it
    // is switched on the user confirms the notice; the confirmation is remembered
    // in loadgraph.cfg (superio_warn_ok) and never asked again.
    void OnSuperIoFanToggle(wxCommandEvent& evt);
    void ApplyHeightAdjustment(const std::string& key, bool showingItem);

    // Adds the CPU fan chart to the CPU Temp row when the fan sensor starts
    // reporting data: it can appear only after the UI was built, and the web
    // dashboard already shows its Fan cell as soon as fan_rpm > 0.
    void EnsureCpuFanPanel(int fanRpm);

    // Inserts one checkbox into the View menu right after the entry named by
    // afterKey. Needed for panels that are created at run time, because the
    // menu is built once from the panels that exist at startup.
    void AddViewToggleItem(const std::string& key, const wxString& label,
                           const std::string& afterKey);

    // UI created flag (Linux: deferred to first timer tick).
    bool    m_uiCreated = false;

    // App config with chart colors.
    AppConfig m_cfg;

    // Section bounding boxes for frame drawing (m_content coordinates).
    struct SectionBox { wxString title; wxRect rect; };
    std::vector<SectionBox> m_sectionBoxes;

    void RefreshSectionBoxes();
    void OnContentPaint(wxPaintEvent& evt);

    wxDECLARE_EVENT_TABLE();
};

#endif // MAIN_FRAME_H
