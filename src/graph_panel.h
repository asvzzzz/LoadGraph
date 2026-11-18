#ifndef GRAPH_PANEL_H
#define GRAPH_PANEL_H

#include <wx/wx.h>
#include <wx/dcbuffer.h>
#include <vector>
#include <string>

// Number of data points stored in the rolling buffer.
static const int kGraphHistorySize = 60;

// Value formatting mode — controls how the header readout is formatted so it
// matches the web dashboard (load "26.5%", temp "52 C", RAM "13.8 GB").
enum class GraphYMode { Auto, Percent, Gigabytes, Celsius, Watts };

// Custom panel that draws a single time-series chart in the web-dashboard
// style: rounded chart canvas, 4-level horizontal grid only, gradient fill
// under a 2 px anti-aliased line. A red palette is used once the last value
// exceeds the configured alarm threshold (e.g. temperature > 80 C).
class GraphPanel : public wxPanel
{
public:
    // title — label shown above the chart (e.g. "Load").
    // yLabel — unit suffix for the header readout (e.g. "%", "C", "W").
    GraphPanel(wxWindow* parent, const wxString& title, const wxString& yLabel);

    // Append a new data point. Older points are discarded to keep history at kGraphHistorySize.
    void AddPoint(double value);

    // Clear the buffer (called on start or reset).
    void Clear();

    // Fix the Y axis to the range [minVal, maxVal] (linear scale).
    // Call before the first points are added.
    void SetYRange(double minVal, double maxVal);

    // Switch the Y axis back to automatic data-driven scaling (0..max).
    void ResetYRange();

    // Draw the line red once the last value is strictly greater than threshold.
    // Pass a negative value to disable the alarm coloring.
    void SetAlarmThreshold(double threshold);

    // When true, draw the line in purple (Task Manager GPU style). CPU / RAM
    // graphs keep the default blue; GPU graphs switch to purple here.
    void SetPurple(bool on);

    // Use a custom color for the line and gradient fill. Overrides purple/blue logic.
    void SetColor(const wxColour& color);

    // Set the chart canvas background color (replaces theme-based card bg).
    void SetBgColor(const wxColour& color);

    // Set the color painted behind the title row and around the canvas —
    // should match the section card color so the panel blends into its card.
    void SetSectionColor(const wxColour& color);

    // Set the value formatting mode.
    void SetYMode(GraphYMode mode);
    wxString FormatValue(double val) const;

protected:
    void OnPaint(wxPaintEvent& evt);
    void OnSize(wxSizeEvent& evt);

    wxDECLARE_EVENT_TABLE();

private:
    // Blend a foreground color over a background color (manual alpha).
    static wxColour Blend(const wxColour& fg, const wxColour& bg, double alpha);

    wxString  m_title;
    wxString  m_yLabel;
    std::vector<double> m_data;
    wxSize  m_clientSize;

    // Fixed Y axis range (used when m_fixedRange is true).
    bool    m_fixedRange = false;
    double  m_fixedMin   = 0.0;
    double  m_fixedMax   = 100.0;

    // Alarm threshold: last value above it switches the line to red.
    double  m_alarmThreshold = -1.0;

    // When true, use purple (GPU style) instead of blue (CPU/RAM style).
    bool    m_purple = false;

    // Custom color override (empty = use purple/blue default).
    wxColour m_color;
    bool     m_hasColor = false;

    // Chart canvas background color override (false = use theme card bg).
    wxColour m_bgColor;
    bool     m_hasBg = false;

    // Section card background behind the title row (false = theme card bg).
    wxColour m_sectionColor;
    bool     m_hasSectionBg = false;

    // Value formatting mode.
    GraphYMode m_yMode = GraphYMode::Auto;
};

#endif // GRAPH_PANEL_H
