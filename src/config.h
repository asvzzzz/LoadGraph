#ifndef CONFIG_H
#define CONFIG_H

#include <map>
#include <string>

// Cross-platform configuration storage (position, size, etc.).
namespace Config
{
    // Load window position from persistent store.
    // Returns true if values were found and valid.
    bool LoadWindowPosition(int& x, int& y);

    // Load window size from persistent store.
    // Returns true if values were found and valid.
    bool LoadWindowSize(int& w, int& h);

    // Save window position and size to persistent store.
    void SaveWindowPosition(int x, int y, int w, int h);

    // Load the per-graph visibility state written by the View menu.
    // Keys are stable strings (e.g. "cpu_load"); value true = visible.
    bool LoadViewMenuState(std::map<std::string, bool>& out);

    // Save the per-graph visibility state to persistent store.
    void SaveViewMenuState(const std::map<std::string, bool>& state);
}

#endif // CONFIG_H
