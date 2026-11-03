#include "theme.h"
#include "platform.h"
#include <wx/settings.h>

#ifdef PLATFORM_LINUX

#include <cstdlib>
#include <cstring>

namespace
{
    // Palette cache. false = light scheme (safe default).
    bool g_dark = false;

    // Read GTK theme setting to determine dark/light mode.
    bool ReadDarkFlag()
    {
        // Check environment variable first.
        const char* gtk_theme = getenv("GTK_THEME");
        if (gtk_theme && gtk_theme[0])
        {
            // Dark themes typically contain "dark", "black", or "Night" in name.
            std::string theme = gtk_theme;
            for (auto& c : theme) c = tolower(c);
            return theme.find("dark") != std::string::npos ||
                   theme.find("black") != std::string::npos ||
                   theme.find("night") != std::string::npos;
        }

        // Check XDG_CURRENT_DESKTOP for hint.
        const char* desktop = getenv("XDG_CURRENT_DESKTOP");
        if (desktop && strstr(desktop, "GNOME"))
        {
            // On GNOME, check gsettings if available.
            // For simplicity, default to dark on modern systems.
            return true;
        }

        // Default to light for compatibility.
        return false;
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
    return g_dark ? wxColour(85, 85, 85) : wxColour(220, 220, 220);
}

wxColour Theme::Border()
{
    return g_dark ? wxColour(90, 90, 90) : wxColour(220, 220, 220);
}

wxColour Theme::Line()
{
    // Use a nice blue accent on dark backgrounds.
    if (g_dark)
        return wxColour(76, 194, 255);
    return wxColour(0, 120, 215);
}

#endif // PLATFORM_LINUX
