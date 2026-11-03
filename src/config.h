#ifndef CONFIG_H
#define CONFIG_H

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
}

#endif // CONFIG_H
