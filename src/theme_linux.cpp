#include "theme.h"
#include "platform.h"
#include <wx/settings.h>

#ifdef PLATFORM_LINUX

#include <cstdlib>
#include <cstring>
#include <string>
#include <fstream>

namespace
{
    // Palette cache. false = light scheme (safe default).
    bool g_dark = false;

    // Minimal INI reader — only parses a single key under [Settings].
    // Returns true if the file exists and key was found with value "1".
    static bool ReadIniBool(const char* path, const char* key)
    {
        std::ifstream in(path);
        if (!in.is_open()) return false;

        std::string line;
        bool inSettings = false;
        while (std::getline(in, line))
        {
            // Strip leading/trailing whitespace.
            size_t start = line.find_first_not_of(" \t\r\n");
            if (start == std::string::npos) continue;
            line.erase(0, start);

            // Skip comments and empty lines.
            if (line[0] == ';' || line[0] == '#' || line.empty())
                continue;

            // Section header — check for [Settings].
            if (line[0] == '[')
            {
                size_t end = line.find(']');
                if (end != std::string::npos)
                    inSettings = (line.substr(1, end - 1) == "Settings");
                continue;
            }

            // Key=Value inside [Settings].
            if (!inSettings) continue;
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;

            std::string k = line.substr(0, eq);
            std::string v = line.substr(eq + 1);
            // Trim whitespace.
            size_t ks = k.find_first_not_of(" \t");
            size_t ke = k.find_last_not_of(" \t");
            if (ks == std::string::npos) continue;
            k = k.substr(ks, ke - ks + 1);

            size_t vs = v.find_first_not_of(" \t\r\n");
            if (vs == std::string::npos) continue;
            v = v.substr(vs);

            if (k == key)
                return (v == "1");
        }
        return false;
    }

    // Read GTK theme setting to determine dark/light mode.
    bool ReadDarkFlag()
    {
        // 1) Explicit env var — fastest path.
        const char* gtk_theme = getenv("GTK_THEME");
        if (gtk_theme && gtk_theme[0])
        {
            std::string theme = gtk_theme;
            for (auto& c : theme) c = tolower(c);
            return theme.find("dark") != std::string::npos ||
                   theme.find("black") != std::string::npos ||
                   theme.find("night") != std::string::npos;
        }

        // 2) XDG_CURRENT_DESKTOP — GNOME defaults to dark on modern systems.
        const char* desktop = getenv("XDG_CURRENT_DESKTOP");
        if (desktop && strstr(desktop, "GNOME"))
            return true;

        // 3) GTK settings.ini — works for Cinnamon, MATE, XFCE, Deepin etc.
        //    Path is stable across all GTK3 desktops.
        const char* home = getenv("HOME");
        if (home && home[0])
        {
            std::string path;
            path.reserve(std::strlen(home) + 48);
            path += home;
            path += "/.config/gtk-3.0/settings.ini";
            if (ReadIniBool(path.c_str(), "gtk-application-prefer-dark-theme"))
                return true;
        }

        // Default to light for compatibility with bare terminal environments.
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
