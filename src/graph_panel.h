#ifndef GRAPH_PANEL_H
#define GRAPH_PANEL_H

#include <wx/wx.h>
#include <wx/dcbuffer.h>
#include <vector>
#include <string>

// Number of data points stored in the rolling buffer.
static const int kGraphHistorySize = 60;

// Y-axis tick spacing mode — controls how axis labels are computed so they
// always land on round numbers regardless of panel height.
enum class GraphYMode { Auto, Percent, Gigabytes, Celsius };

// Custom panel that draws a single time-series chart in Windows 11 Task Manager style.
// The area under the line is filled. A red color is used once the last value
// exceeds the configured alarm threshold (e.g. temperature > 80 C).
class GraphPanel : public wxPanel
{
public:
    // title — label shown above the chart (e.g. "CPU 0 Load").
    // yLabel — text for the Y-axis (e.g. "%", "C", "MHz").
    GraphPanel(wxWindow* parent, const wxString& title, const wxString& yLabel);

    // Append a new data point. Older points are discarded to keep history at kGraphHistorySize.
    void AddPoint(double value);

    // Clear the buffer (called on start or reset).
    void Clear();

    // Fix the Y axis to the range [minVal, maxVal] (linear scale).
    // Call before the first points are added.
    void SetYRange(double minVal, double maxVal);

    // Switch the Y axis back to automatic data-driven scaling.
    void ResetYRange();

    // Draw the line red once the last value is strictly greater than threshold.
    // Pass a negative value to disable the alarm coloring.
    void SetAlarmThreshold(double threshold);

    // When true, draw the line in purple (Task Manager GPU style). CPU / RAM
    // graphs keep the default blue; GPU graphs switch to purple here.
    void SetPurple(bool on);

    // Set Y-axis tick spacing mode. Controls how axis labels are computed so
    // that they always land on round numbers regardless of panel height.
    void SetYMode(GraphYMode mode);
    static wxString FormatValue(double val);

protected:
    void OnPaint(wxPaintEvent& evt);
    void OnSize(wxSizeEvent& evt);

    wxDECLARE_EVENT_TABLE();

private:
    // Compute adaptive Y-axis tick values for the given mode and range.
    std::vector<double> ComputeYTicks(double minVal, double maxVal,
                                      int count, GraphYMode mode) const;

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

    // Y-axis tick spacing mode. Controls how the axis labels are computed.
    GraphYMode m_yMode = GraphYMode::Auto;
};

#endif // GRAPH_PANEL_H
