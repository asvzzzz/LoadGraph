#include "theme.h"

#include <windows.h>
#include <wx/settings.h>

namespace
{
    // Palette cache. false = light scheme (safe default).
    bool g_dark = false;

    // Read the Windows app mode flag: 0 means dark apps are requested.
    bool ReadDarkFlag()
    {
        DWORD value = 1;
        DWORD size  = static_cast<DWORD>(sizeof(value));
        const LSTATUS rc = RegGetValueW(
            HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            L"AppsUseLightTheme",
            RRF_RT_REG_DWORD,
            nullptr,
            &value,
            &size);

        // Missing value (old OS / server) -> keep the light scheme.
        if (rc != ERROR_SUCCESS || size != sizeof(value))
            return false;

        return value == 0;
    }
}

bool Theme::Update()
{
    const bool wasDark = g_dark;
    g_dark = ReadDarkFlag();
    return g_dark != wasDark;
}

bool Theme::IsDark()
{
    return g_dark;
}

wxColour Theme::Bg()
{
    return g_dark ? wxColour(32, 32, 32) : wxColour(240, 240, 240);
}

wxColour Theme::Card()
{
    return g_dark ? wxColour(43, 43, 43) : wxColour(255, 255, 255);
}

wxColour Theme::Text()
{
    return g_dark ? wxColour(255, 255, 255) : wxColour(32, 32, 32);
}

wxColour Theme::SubText()
{
    return g_dark ? wxColour(180, 180, 180) : wxColour(96, 96, 96);
}

wxColour Theme::Grid()
{
    // Visible but subtle grid lines on both light and dark backgrounds.
    return g_dark ? wxColour(85, 85, 85) : wxColour(220, 220, 220);
}

wxColour Theme::Border()
{
    return g_dark ? wxColour(90, 90, 90) : wxColour(220, 220, 220);
}

wxColour Theme::Line()
{
    // System accent color follows the user's personalization choice.
    const wxColour accent = wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHT);
    if (g_dark)
    {
        // Ensure the accent stays visible on a dark background.
        const int lum = (accent.Red() * 299 + accent.Green() * 587 +
                         accent.Blue() * 114) / 1000;
        if (lum < 100)
            return wxColour(76, 194, 255);
    }
    return accent;
}
