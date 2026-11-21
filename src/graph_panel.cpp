#include "graph_panel.h"
#include "logger.h"
#include "theme.h"
#include <wx/dcbuffer.h>
#include <wx/graphics.h>
#include <wx/dcgraph.h>
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

void GraphPanel::SetAlarmThreshold(double threshold)
{
    m_alarmThreshold = threshold;
}

// Purple for GPU graphs (Task Manager style), blue for CPU / RAM.
void GraphPanel::SetPurple(bool on)
{
    m_purple = on;
}

// Apply a custom color, overriding purple/blue defaults.
void GraphPanel::SetColor(const wxColour& color)
{
    m_color = color;
    m_hasColor = true;
}

// Set the chart canvas background color.
void GraphPanel::SetBgColor(const wxColour& color)
{
    m_bgColor = color;
    m_hasBg = true;
}

// Set the section card color painted behind the title row.
void GraphPanel::SetSectionColor(const wxColour& color)
{
    m_sectionColor = color;
    m_hasSectionBg = true;
}

// Select the header readout formatting mode.
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
    // Buffered paint DC + graphics context: anti-aliased lines, gradients and
    // rounded corners, matching the web dashboard rendering.
    wxAutoBufferedPaintDC pdc(this);
    wxGCDC gdc(pdc);
    wxGraphicsContext* gcp = gdc.GetGraphicsContext();
    if (!gcp)
    {
        // Graphics context unavailable (should not happen with
        // wxUSE_GRAPHICS_CONTEXT=1): fill with the section color and stop so
        // the panel never shows uninitialized pixels.
        gdc.SetBackground(wxBrush(m_hasSectionBg ? m_sectionColor : Theme::Card()));
        gdc.Clear();
        return;
    }
    wxGraphicsContext& gc = *gcp;

    const wxColour sectionCol = m_hasSectionBg ? m_sectionColor : Theme::Card();
    const wxColour chartBg    = m_hasBg ? m_bgColor : Theme::Card();

    // Explicitly fill the entire panel background to eliminate resize artifacts.
    gdc.SetBackground(wxBrush(sectionCol));
    gdc.Clear();

    // Update cached size if it was not set yet (e.g. before first OnSize).
    m_clientSize = GetClientSize();
    const int panelW = m_clientSize.GetWidth();
    const int panelH = m_clientSize.GetHeight();
    if (panelW <= 0 || panelH <= 0)
        return;

    // ---- Title row: gray label + bold white value (web .val-row style) ----
    // Last valid value drives both the alarm state and the header readout.
    double lastValue = -1.0;
    for (auto it = m_data.rbegin(); it != m_data.rend(); ++it)
    {
        if (*it >= 0.0) { lastValue = *it; break; }
    }

    // Active alarm: last value above the threshold -> red palette (like the
    // dashboard tempColor() helper).
    const bool alarm = (m_alarmThreshold >= 0.0) && (lastValue > m_alarmThreshold);

    {
        const wxFont labelFont(wxFontInfo(wxSize(0, 13)).FaceName("Consolas"));
        const wxFont valFont(wxFontInfo(wxSize(0, 13)).FaceName("Consolas").Bold());

        gdc.SetFont(labelFont);
        gdc.SetTextForeground(wxColour(170, 170, 170));  // #aaaaaa like the dashboard
        gdc.DrawText(m_title, 8, 3);
        const wxSize titleExtent = gdc.GetTextExtent(m_title);

        gdc.SetFont(valFont);
        if (lastValue >= 0.0)
        {
            // Unit suffix: "%" is glued to the number, other units use a space
            // (dashboard renders "26.5%" but "100.6 GB").
            const wxString sep   = (m_yLabel == "%") ? wxString() : wxString(" ");
            const wxString value = FormatValue(lastValue) + sep + m_yLabel;
            gdc.SetTextForeground(alarm ? wxColour(255, 0, 0) : wxColour(255, 255, 255));
            gdc.DrawText(value, 8 + titleExtent.GetWidth() + 6, 3);
        }
        else
        {
            gdc.SetTextForeground(wxColour(170, 170, 170));
            gdc.DrawText("--", 8 + titleExtent.GetWidth() + 6, 3);
        }
    }

    // ---- Chart canvas: rounded rect filling the full panel width ----
    // Matches "canvas.chart { background: #0f3460; border-radius: 4px }".
    const double canvasLeft   = 0.0;
    const double canvasTop    = 21.0;
    const double canvasRight  = static_cast<double>(panelW);
    const double canvasBottom = static_cast<double>(panelH);
    const double canvasW = canvasRight  - canvasLeft;
    const double canvasH = canvasBottom - canvasTop;
    if (canvasW <= 0.0 || canvasH <= 0.0)
        return;

    gc.SetBrush(wxBrush(chartBg));
    gc.SetPen(*wxTRANSPARENT_PEN);
    gc.DrawRoundedRectangle(canvasLeft, canvasTop, canvasW, canvasH, 4.0);

    // ---- Grid: horizontal lines only, white at 7% over the canvas bg ----
    // Dashboard draws 5 lines (4 divisions); the boundary lines coincide with
    // the canvas edges, so only the 3 interior levels are painted here.
    {
        const wxColour gridCol = Blend(wxColour(255, 255, 255), chartBg, 0.07);
        gc.SetPen(wxPen(gridCol, 1));
        for (int i = 1; i <= 3; ++i)
        {
            const double y = canvasBottom - canvasH * static_cast<double>(i) / 4.0;
            gc.StrokeLine(canvasLeft, y, canvasRight, y);
        }
    }

    if (m_data.size() < 2)
        return; // Dashboard: grid stays visible until at least 2 points exist.

    // ---- Determine Y axis range: fixed range or 0..max (dashboard scale) ----
    double yMin = 0.0, yMax = 0.0;
    if (m_fixedRange)
    {
        yMin = m_fixedMin;
        yMax = m_fixedMax;
    }
    else
    {
        const double maxVal = *std::max_element(m_data.begin(), m_data.end());
        yMin = 0.0;
        yMax = (maxVal > 1.0) ? maxVal * 1.1 : 1.0;
    }
    const double ySpan = (yMax - yMin) > 0.0 ? (yMax - yMin) : 1.0;

    // Pixel mapping, clamped so an out-of-range spike cannot escape the canvas.
    auto yToPx = [&](double v) {
        double y = canvasBottom - canvasH * (v - yMin) / ySpan;
        return std::clamp(y, canvasTop, canvasBottom);
    };

    const double xStep = canvasW / static_cast<double>(kGraphHistorySize - 1);

    // Line color: red on alarm, custom color otherwise, purple/blue fallback.
    wxColour lineColor;
    if (alarm)
        lineColor = wxColour(255, 0, 0);
    else if (m_hasColor)
        lineColor = m_color;
    else if (m_purple)
        lineColor = wxColour(168, 85, 247);   // purple
    else
        lineColor = wxColour(59, 130, 246);   // blue

    // ---- Filled area with vertical gradient (color CC -> color 22) ----
    // Mirrors the dashboard createLinearGradientBrush over the full canvas
    // height: 80% opacity at the top, ~13% at the bottom.
    {
        wxGraphicsPath area = gc.CreatePath();
        area.MoveToPoint(canvasLeft, canvasBottom);
        for (size_t i = 0; i < m_data.size(); ++i)
            area.AddLineToPoint(canvasLeft + xStep * static_cast<double>(i),
                                yToPx(m_data[i]));
        area.AddLineToPoint(canvasLeft + xStep * static_cast<double>(m_data.size() - 1),
                            canvasBottom);
        area.CloseSubpath();

        wxColour top(lineColor.Red(),   lineColor.Green(), lineColor.Blue(), 0xCC);
        wxColour bottom(lineColor.Red(), lineColor.Green(), lineColor.Blue(), 0x22);
        gc.SetBrush(gc.CreateLinearGradientBrush(canvasLeft, canvasTop,
                                                 canvasLeft, canvasBottom,
                                                 top, bottom));
        gc.SetPen(*wxTRANSPARENT_PEN);
        gc.FillPath(area);
    }

    // ---- The line itself, 2 px anti-aliased, on top of the fill ----
    {
        wxGraphicsPath line = gc.CreatePath();
        for (size_t i = 0; i < m_data.size(); ++i)
        {
            const double x = canvasLeft + xStep * static_cast<double>(i);
            const double y = yToPx(m_data[i]);
            if (i == 0) line.MoveToPoint(x, y);
            else        line.AddLineToPoint(x, y);
        }
        gc.SetPen(gc.CreatePen(wxGraphicsPenInfo(lineColor).Width(2.0)));
        gc.StrokePath(line);
    }
}

// Format a value for the header readout, matching the web dashboard precision:
// load 26.5%, temp 52 C, RAM 13.8 GB, power 31.9 W, fan/RPM integers.
wxString GraphPanel::FormatValue(double val) const
{
    // Guard against NaN / Inf - static_cast of NaN to integral is UB.
    if (!std::isfinite(val)) return "N/A";
    switch (m_yMode)
    {
        case GraphYMode::Gigabytes: return wxString::Format("%.1f", val);
        case GraphYMode::Celsius:   return wxString::Format("%.0f", val);
        case GraphYMode::Watts:     return wxString::Format("%.1f", val);
        case GraphYMode::Percent:   return wxString::Format("%.1f", val);
        default:                    return wxString::Format("%.0f", val); // Auto / RPM / fan %
    }
}
