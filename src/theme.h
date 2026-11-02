#ifndef THEME_H
#define THEME_H

#include <wx/colour.h>

// System color scheme detection (Windows 11 light/dark app mode).
// Call Update() once before first use and then periodically (e.g. every
// timer tick) to react to live theme changes.
namespace Theme
{
    // Reads HKCU AppsUseLightTheme and refreshes the palette cache.
    // Returns true if the scheme has changed since the previous call.
    bool Update();

    // True if the system currently uses the dark app mode.
    bool IsDark();

    // Main window / scrolled area background.
    wxColour Bg();

    // Graph card background.
    wxColour Card();

    // Primary text (titles, section headers).
    wxColour Text();

    // Secondary text (GPU names, hints).
    wxColour SubText();

    // Chart grid line color.
    wxColour Grid();

    // Chart plot area border color.
    wxColour Border();

    // Chart data line color (system accent in light mode).
    wxColour Line();
}

#endif // THEME_H
