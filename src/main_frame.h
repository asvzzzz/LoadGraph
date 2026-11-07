#ifndef MAIN_FRAME_H
#define MAIN_FRAME_H

#include <wx/wx.h>
#include <wx/sizer.h>
#include <unordered_map>
#include <wx/timer.h>
#include <map>
#include "cpu_monitor.h"
#include "ram_monitor.h"
#include "gpu_monitor.h"
#include "graph_panel.h"

// Main application window. Resizable, contains CPU / RAM / GPU metric panels.
class MainFrame : public wxFrame
{
public:
    explicit MainFrame(const wxString& title);
    ~MainFrame() override;

    // Create the window layout (called on Windows after Show).
    void RealizeUI();

private:
    // Timer callback - reads metrics and refreshes graph panels.
    void OnTimer(wxTimerEvent& evt);

    // Close handler - saves window position/size to registry before closing.
    void OnClose(wxCloseEvent& evt);

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

    // Monitoring objects.
    CpuMonitor  m_cpu;
    RamMonitor  m_ram;
    GpuMonitor  m_gpu;

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

    // ---- CPU sensor row (created only when sensors exist) ----
    GraphPanel* m_cpuTempGraph  = nullptr;
    GraphPanel* m_cpuFanGraph   = nullptr;

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
    };
    std::vector<GpuPanelData> m_gpuPanels;

    // Static boxes of the GPU sections. Recolored in ApplyTheme() so the GPU
    // names match the rest of the labels (wxStaticBox uses the system color).
    std::vector<wxWindow*> m_gpuBoxes;

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

    // Sizer items belonging to each toggleable key: hiding such an item must
    // shrink the window by exactly this item's height, without empty space and
    // without moving the window position (proportions are preserved).
    struct ToggleGroup
    {
        std::vector<wxSizerItem*> items;   // sizer entries to show/hide together
        // Note: deltaH was historically used to track row height, but the actual
        // resize is computed dynamically via sizer->CalcMin() in ApplyHeightAdjustment.
    };
    // Keys: "CPU Load", "RAM Usage", ..., plus GPU group keys ("gpu_load", ...).
    std::unordered_map<std::string, ToggleGroup> m_toggleGroups;

    void BuildViewMenu(wxMenuBar* menuBar,
                       const std::map<std::string, bool>& initialState);
    void OnViewToggle(wxCommandEvent& evt);
    void ApplyHeightAdjustment(const std::string& key, bool showingItem);

    // UI created flag (Linux: deferred to first timer tick).
    bool    m_uiCreated = false;

    // Section bounding boxes for frame drawing (m_content coordinates).
    struct SectionBox { wxString title; wxRect rect; };
    std::vector<SectionBox> m_sectionBoxes;

    void RefreshSectionBoxes();
    void OnContentPaint(wxPaintEvent& evt);

    wxDECLARE_EVENT_TABLE();
};

#endif // MAIN_FRAME_H
