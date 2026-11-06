#include "graph_panel.h"
#include "logger.h"
#include "theme.h"
#include <wx/dcbuffer.h>
#include <algorithm>
#include <cmath>

wxBEGIN_EVENT_TABLE(GraphPanel, wxPanel)
    EVT_PAINT(GraphPanel::OnPaint)
    EVT_SIZE(GraphPanel::OnSize)
wxEND_EVENT_TABLE()

GraphPanel::GraphPanel(wxWindow* parent, const wxString& title, const wxString& yLabel)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL)
    , m_title(title)
    , m_yLabel(yLabel)
{
    // Custom background style prevents flicker: OnPaint handles all painting,
    // Windows won't clear the background between Refresh() calls.
    SetBackgroundStyle(wxBG_STYLE_CUSTOM);
    SetBackgroundColour(Theme::Card());
    // Minimum size so the chart area stays readable but the row can shrink
    // when the window is resized vertically (no scrollbar needed).
    SetMinSize(wxSize(160, 40));
    m_clientSize = GetClientSize();
}

void GraphPanel::AddPoint(double value)
{
    m_data.push_back(value);
    if (static_cast<int>(m_data.size()) > kGraphHistorySize)
        m_data.erase(m_data.begin());
    Refresh();
}

void GraphPanel::Clear()
{
    m_data.clear();
    Refresh();
}

void GraphPanel::SetYRange(double minVal, double maxVal)
{
    m_fixedRange = true;
    m_fixedMin   = minVal;
    m_fixedMax   = maxVal;
}

void GraphPanel::ResetYRange()
{
    m_fixedRange = false;
}

void GraphPanel::SetTickCount(int count)
{
    // Zero or negative disables the override (falls back to auto).
    m_tickCount = (count > 0) ? count : -1;
}

void GraphPanel::SetAlarmThreshold(double threshold)
{
    m_alarmThreshold = threshold;
}

// Purple for GPU graphs (Task Manager style), blue for CPU / RAM.
void GraphPanel::SetPurple(bool on)
{
    m_purple = on;
}

// Select Y-axis tick spacing mode.
void GraphPanel::SetYMode(GraphYMode mode)
{
    m_yMode = mode;
}

void GraphPanel::OnSize(wxSizeEvent& evt)
{
    m_clientSize = GetClientSize();
    Refresh();
    Update();
    evt.Skip();
}

// Manual alpha blend of fg over bg (GDI wxDC has no alpha brush).
wxColour GraphPanel::Blend(const wxColour& fg, const wxColour& bg, double alpha)
{
    const double a = std::clamp(alpha, 0.0, 1.0);
    const auto mix = [a](unsigned char f, unsigned char b) {
        return static_cast<unsigned char>(f * a + b * (1.0 - a) + 0.5);
    };
    return wxColour(mix(fg.Red(),   bg.Red()),
                    mix(fg.Green(), bg.Green()),
                    mix(fg.Blue(),  bg.Blue()));
}

void GraphPanel::OnPaint(wxPaintEvent& /*evt*/)
{
    wxAutoBufferedPaintDC dc(this);
    const wxColour cardBg = Theme::Card();

    // Explicitly fill the entire panel background to eliminate resize artifacts.
    dc.SetBackground(wxBrush(cardBg));
    dc.Clear();

    // Update cached size if it was not set yet (e.g. before first OnSize).
    m_clientSize = GetClientSize();
    if (m_clientSize.GetWidth() <= 0 || m_clientSize.GetHeight() <= 0)
        return;

    // Compact vertical margins so the panel can be short after a resize.
    // Title needs ~17 px at the top, the bottom margin shrinks with the panel.
    const int panelH   = m_clientSize.GetHeight();
    const int marginTop    = std::min(24, std::max(17, panelH / 3));
    const int marginBottom = std::min(16, std::max(8, panelH / 6));
    const int marginLeft   = 52;
    const int marginRight  = 44;

    const double plotLeft   = static_cast<double>(marginLeft);
    const double plotRight  = static_cast<double>(m_clientSize.GetWidth()  - marginRight);
    const double plotTop    = static_cast<double>(marginTop);
    const double plotBottom = static_cast<double>(m_clientSize.GetHeight() - marginBottom);
    const double plotWidth  = plotRight - plotLeft;
    const double plotHeight = plotBottom - plotTop;

    // Last valid value drives both the alarm state and the header readout.
    double lastValue = -1.0;
    for (auto it = m_data.rbegin(); it != m_data.rend(); ++it)
    {
        if (*it >= 0.0) { lastValue = *it; break; }
    }

    // Active alarm: last value above the threshold -> red palette.
    const bool alarm = (m_alarmThreshold >= 0.0) && (lastValue > m_alarmThreshold);
    // Purple for GPU, blue for CPU / RAM; red when alarmed.
    wxColour lineColor;
    if (alarm)
        lineColor = wxColour(220, 60, 50);
    else if (m_purple)
        lineColor = wxColour(168, 85, 247);   // purple
    else
        lineColor = wxColour(59, 130, 246);  // blue
    const wxColour fillColor   = Blend(lineColor, cardBg, 0.22);

    // ---- Draw title + current value (e.g. "CPU Load  10%") ----
    {
        wxFont titleFont(11, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_BOLD);
        dc.SetFont(titleFont);
        dc.SetTextForeground(Theme::Text());
        dc.DrawText(m_title, plotLeft, 2.0);

        if (lastValue >= 0.0)
        {
            const wxString valText = FormatValue(lastValue) + m_yLabel;
            wxSize tsTitle = dc.GetTextExtent(m_title);
            wxFont valFont(11, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_BOLD);
            dc.SetFont(valFont);
            dc.SetTextForeground(lineColor);
            dc.DrawText(valText, plotLeft + tsTitle.GetWidth() + 10.0, 2.0);
        }
    }

    if (plotWidth <= 0 || plotHeight <= 0)
        return;

    // ---- Determine Y axis range ----
    double yMin = 0.0, yMax = 0.0;
    bool   hasValidData = false;
    if (m_fixedRange)
    {
        yMin = m_fixedMin;
        yMax = m_fixedMax;
        hasValidData = std::any_of(m_data.begin(), m_data.end(),
                                   [](double v) { return v >= 0.0; });
    }
    else
    {
        // Auto scale: skip negative values - they mean "N/A".
        auto validBegin = std::find_if(m_data.begin(), m_data.end(),
                                       [](double v) { return v >= 0.0; });
        if (validBegin != m_data.end())
        {
            hasValidData = true;
            yMin = *std::min_element(validBegin, m_data.end());
            yMax = *std::max_element(validBegin, m_data.end());

            double range = yMax - yMin;
            if (range < 0.5) range = 10.0;
            const double pad = range * 0.1;
            yMin -= pad;
            yMax += pad;
        }
    }

    // ---- No valid data yet: show "N/A" placeholder and stop ----
    if (!hasValidData)
    {
        wxFont font(10, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL);
        dc.SetFont(font);
        dc.SetTextForeground(Theme::SubText());
        const wxString na("N/A");
        wxSize ts = dc.GetTextExtent(na);
        dc.DrawText(na,
                    plotLeft + (plotWidth - ts.GetWidth()) / 2.0,
                    plotTop + (plotHeight - ts.GetHeight()) / 2.0);
        return;
    }

    const double ySpan = (yMax - yMin) != 0.0 ? (yMax - yMin) : 1.0;
    const auto yToPx = [&](double v) {
        return plotBottom - plotHeight * (v - yMin) / ySpan;
    };

    // Adaptive Y-axis tick count based on available plot height — unless the
    // caller explicitly requested a fixed number via SetTickCount().
    int tickCount = -1;
    if (m_tickCount > 0)
        tickCount = m_tickCount;
    else
        tickCount = std::max(4, (plotHeight < 40.0) ? 1 :
                                  (plotHeight < 80.0) ? 2 :
                                  (plotHeight < 130.0) ? 3 :
                                  (plotHeight < 200.0) ? 4 : 5);

    // Compute nice Y-axis tick values based on mode and available height.
    const auto yTicks = ComputeYTicks(yMin, yMax, tickCount, m_yMode);

    // ---- Draw grid lines ----
    {
        wxPen penGrid(Theme::Grid(), 1, wxPENSTYLE_SOLID);
        dc.SetPen(penGrid);
        dc.SetBrush(*wxTRANSPARENT_BRUSH);

        // Horizontal grid lines: always draw for each tick value including boundaries.
        for (size_t i = 0; i < yTicks.size(); ++i)
        {
            double y = plotBottom - plotHeight * (yTicks[i] - yMin) / ySpan;
            // Clamp to plot area.
            y = std::max(plotTop, std::min(plotBottom, y));
            dc.DrawLine(plotLeft, y, plotRight, y);
        }

        // Vertical grid lines: draw at regular intervals based on panel width.
        // Aim for ~5-8 vertical lines depending on width.
        const int vTickCount = std::max(2, std::min(8, static_cast<int>(plotWidth / 80.0)));
        for (int i = 0; i <= vTickCount; ++i)
        {
            double x = plotLeft + plotWidth * static_cast<double>(i) /
                                   static_cast<double>(vTickCount);
            dc.DrawLine(x, plotTop, x, plotBottom);
        }
    }

    // ---- Draw Y-axis labels ----
    {
        wxFont font(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL);
        dc.SetFont(font);
        dc.SetTextForeground(Theme::SubText());

        for (size_t i = 0; i < yTicks.size(); ++i)
        {
            double val = yTicks[i];
            double y   = plotBottom - plotHeight * (val - yMin) / ySpan;
            // Right-align Y labels: end 6 px left of the plot area.
            wxString lbl = FormatValue(val);
            wxSize   ts  = dc.GetTextExtent(lbl);
            dc.DrawText(lbl, plotLeft - 6.0 - ts.GetWidth(), y - ts.GetHeight() / 2.0);
        }
        dc.DrawText(m_yLabel, plotRight + 8.0, plotTop);
    }

    // ---- Draw filled area + line (clipped to the plot area) ----
    if (m_data.size() >= 2)
    {
        dc.SetClippingRegion(static_cast<int>(plotLeft), static_cast<int>(plotTop),
                             static_cast<int>(plotWidth), static_cast<int>(plotHeight));

        const double xStep = plotWidth /
                             static_cast<double>(std::max(1, kGraphHistorySize - 1));

        // Build the polyline in pixel coordinates.
        std::vector<wxPoint> pts;
        pts.reserve(m_data.size());
        for (size_t i = 0; i < m_data.size(); ++i)
        {
            const double x = plotLeft + xStep * static_cast<double>(i);
            pts.emplace_back(static_cast<int>(x), static_cast<int>(yToPx(m_data[i])));
        }

        // Filled area under the line down to the plot baseline.
        {
            std::vector<wxPoint> poly;
            poly.reserve(pts.size() + 2);
            poly.emplace_back(pts.front().x, static_cast<int>(plotBottom));
            poly.insert(poly.end(), pts.begin(), pts.end());
            poly.emplace_back(pts.back().x, static_cast<int>(plotBottom));

            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(fillColor));
            dc.DrawPolygon(static_cast<int>(poly.size()), poly.data());
        }

        // The line itself on top of the fill.
        {
            wxPen penLine(lineColor, 2, wxPENSTYLE_SOLID);
            dc.SetPen(penLine);
            dc.SetBrush(*wxTRANSPARENT_BRUSH);
            for (size_t i = 1; i < pts.size(); ++i)
                dc.DrawLine(pts[i - 1], pts[i]);
        }

        dc.DestroyClippingRegion();
    }

    // ---- Draw border rectangle around plot area ----
    {
        wxPen penBorder(Theme::Border(), 0, wxPENSTYLE_SOLID);
        dc.SetPen(penBorder);
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        dc.DrawRectangle(static_cast<int>(plotLeft), static_cast<int>(plotTop),
                         static_cast<int>(plotWidth), static_cast<int>(plotHeight));
    }
}

std::vector<double> GraphPanel::ComputeYTicks(double minVal, double maxVal,
                                               int count, GraphYMode mode) const
{
    std::vector<double> ticks;
    if (count <= 0) return ticks;

    double range = maxVal - minVal;
    if (range <= 0.0) range = 1.0;

    // Target round step sizes per mode — ordered from largest to smallest.
    // Covers common memory sizes: 1GB, 2GB, 3GB, 4GB, 8GB, 12GB, 16GB, 24GB, 32GB, 48GB, 64GB, 128GB.
    static const double kPctSteps[]     = {25.0, 20.0, 10.0, 5.0, 2.0};
    static const double kGbSteps[]      = {64.0, 32.0, 24.0, 16.0, 8.0, 4.0, 3.0, 2.0, 1.0};
    static const double kCelsiusSteps[] = {25.0, 20.0, 10.0, 5.0, 2.0};
    static const double kWattsSteps[]   = {100.0, 50.0, 40.0, 25.0, 20.0, 10.0, 5.0, 2.0, 1.0};
    const double* steps = nullptr;
    int stepsCount = 0;

    switch (mode)
    {
        case GraphYMode::Percent:
            steps = kPctSteps; stepsCount = 5; break;
        case GraphYMode::Gigabytes:
            steps = kGbSteps;  stepsCount = 9; break;
        case GraphYMode::Celsius:
            steps = kCelsiusSteps; stepsCount = 5; break;
        case GraphYMode::Watts:
            steps = kWattsSteps; stepsCount = 9; break;
        default: // Auto / RPM — use a generic step.
            steps = kPctSteps; stepsCount = 5; break;
    }

    // Find the step that gives approximately count intervals.
    // Choose the step whose interval count is closest to target count.
    double step = range;
    double bestDiff = std::numeric_limits<double>::max();
    for (int i = 0; i < stepsCount; ++i)
    {
        double intervals = range / steps[i];
        double diff = std::abs(intervals - static_cast<double>(count));
        // Prefer steps that give between count-1 and count+1 intervals.
        if (intervals >= static_cast<double>(count - 1) &&
            intervals <= static_cast<double>(count + 1) &&
            diff < bestDiff)
        {
            bestDiff = diff;
            step = steps[i];
        }
    }

    // Snap min/max to multiples of step.
    double snapMin = std::floor(minVal / step) * step;
    double snapMax = std::ceil(maxVal / step) * step;
    if (snapMax <= snapMin) snapMax = snapMin + step;

    for (double v = snapMin; v <= snapMax + step * 0.5; v += step)
        ticks.push_back(v);

    // Ensure we always have at least count ticks (bottom and top plus intermediates).
    if (static_cast<int>(ticks.size()) < count)
    {
        // Force uniform spacing to meet the minimum tick requirement.
        ticks.clear();
        double forcedStep = (snapMax - snapMin) / static_cast<double>(count - 1);
        for (int i = 0; i < count; ++i)
            ticks.push_back(snapMin + forcedStep * i);
    }

    return ticks;
}

// Format a value for axis labels and the header readout.
wxString GraphPanel::FormatValue(double val)
{
    // Round to nearest integer to avoid floating point artifacts.
    long rounded = static_cast<long>(std::round(val));
    return wxString::Format("%ld", rounded);
}
