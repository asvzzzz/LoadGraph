#ifndef MAIN_FRAME_H
#define MAIN_FRAME_H

#include <wx/wx.h>
#include <wx/sizer.h>
#include <wx/timer.h>
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

    // Re-apply the system color scheme to all controls (light/dark switch).
    void ApplyTheme();

    // Register a label for theme recoloring (primary = title color).
    void AddThemedText(wxStaticText* st, bool primary);

    // Build the CPU/RAM block: one row with CPU Load + RAM Usage, and a second
    // row with CPU Temp + CPU Fan Speed that is omitted entirely when both
    // sensors report N/A.
    void BuildCpuSection(wxBoxSizer* container, wxWindow* parent);

    // Build GPU section (dynamic per-GPU: load/temp row, vram/fan row).
    void BuildGpuSection(wxBoxSizer* container, wxWindow* parent);

    // Monitoring objects.
    CpuMonitor  m_cpu;
    RamMonitor  m_ram;
    GpuMonitor  m_gpu;

    // Timer for periodic GUI updates (1000 ms).
    wxTimer     m_timer;

    // Non-scrolling content panel (CPU / RAM / GPU sections).
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

    // ---- Uptime label ----
    wxStaticText* m_uptimeText = nullptr;

    // ---- GPU panels ----
    struct GpuPanelData
    {
        wxString    name;
        GraphPanel* loadGraph = nullptr;
        GraphPanel* tempGraph = nullptr;
        GraphPanel* vramGraph = nullptr;
        GraphPanel* fanGraph  = nullptr;
    };
    std::vector<GpuPanelData> m_gpuPanels;

    // UI created flag (Linux: deferred to first timer tick).
    bool    m_uiCreated = false;

    wxDECLARE_EVENT_TABLE();
};

#endif // MAIN_FRAME_H
