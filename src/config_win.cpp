#include "platform.h"
#include "config.h"
#include "logger.h"

#ifdef PLATFORM_WINDOWS

#include <windows.h>

namespace
{
    // Encode x and y into a single DWORD: high word = x, low word = y.
    static DWORD EncodePos(int x, int y)
    {
        return (static_cast<DWORD>(static_cast<USHORT>(x)) << 16) |
               static_cast<DWORD>(static_cast<USHORT>(y));
    }

    // Decode a DWORD into x and y.
    static void DecodePos(DWORD val, int& x, int& y)
    {
        x = static_cast<short>(HIWORD(val));
        y = static_cast<short>(LOWORD(val));
    }

    static const wchar_t* kRegPath = L"Software\\LoadGraph";
    static const wchar_t* kRegPos  = L"WindowPos";
    static const wchar_t* kRegSize = L"WindowSize";
}

bool Config::LoadWindowPosition(int& x, int& y)
{
    DWORD val = 0;
    DWORD size = sizeof(val);
    if (RegGetValueW(HKEY_CURRENT_USER, kRegPath, kRegPos, RRF_RT_REG_DWORD,
                     nullptr, &val, &size) != ERROR_SUCCESS)
        return false;
    DecodePos(val, x, y);

    // Clamp to sane range.
    if (x < -1000 || x > 5000) x = 100;
    if (y < -1000 || y > 5000) y = 100;

    LOGI("Loaded position: (%d,%d)", x, y);
    return true;
}

bool Config::LoadWindowSize(int& w, int& h)
{
    DWORD val = 0;
    DWORD size = sizeof(val);
    if (RegGetValueW(HKEY_CURRENT_USER, kRegPath, kRegSize, RRF_RT_REG_DWORD,
                     nullptr, &val, &size) != ERROR_SUCCESS)
        return false;

    w = static_cast<short>(HIWORD(val));
    h = static_cast<short>(LOWORD(val));

    // Clamp to sane range.
    if (w < 480 || w > 3840) w = 640;
    if (h < 540 || h > 2160) h = 720;

    LOGI("Loaded size: %dx%d", w, h);
    return true;
}

void Config::SaveWindowPosition(int x, int y, int w, int h)
{
    HKEY hKey;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegPath, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &hKey, nullptr) == ERROR_SUCCESS)
    {
        DWORD val = EncodePos(x, y);
        RegSetValueExW(hKey, kRegPos, 0, REG_DWORD,
                       reinterpret_cast<const BYTE*>(&val), sizeof(val));
        RegCloseKey(hKey);
    }

    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegPath, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &hKey, nullptr) == ERROR_SUCCESS)
    {
        DWORD val = EncodePos(w, h);
        RegSetValueExW(hKey, kRegSize, 0, REG_DWORD,
                       reinterpret_cast<const BYTE*>(&val), sizeof(val));
        RegCloseKey(hKey);
    }

    LOGI("Saved position: (%d,%d) size=%dx%d", x, y, w, h);
}

#endif // PLATFORM_WINDOWS
