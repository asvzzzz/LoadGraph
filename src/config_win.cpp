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
    // Clamp before encoding: EncodePos truncates to USHORT (0..65535), and the
    // signed decode interprets high bits as negative. Values outside [-32768,
    // 32767] would silently corrupt the stored coordinate.
    if (x < -32768) x = -32768; else if (x > 32767) x = 32767;
    if (y < -32768) y = -32768; else if (y > 32767) y = 32767;
    if (w < 0)      w = 0;      else if (w > 32767) w = 32767;
    if (h < 0)      h = 0;      else if (h > 32767) h = 32767;

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

bool Config::LoadViewMenuState(std::map<std::string, bool>& out)
{
    // Read all view_* keys from the registry; missing key -> true (default).
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegPath, 0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return false;

    DWORD idx = 0;
    wchar_t name[128] = {};
    DWORD nSize = sizeof(name);
    bool any = false;
    while (RegEnumValueW(hKey, idx, name, &nSize, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS)
    {
        if (wcsncmp(name, L"view_", 5) == 0)
        {
            std::string key;
            for (size_t i = 5; name[i]; ++i)
                key += static_cast<char>(name[i]); // ASCII-safe: keys are ASCII.
            DWORD val = 0, vsz = sizeof(val);
            if (RegGetValueW(hKey, nullptr, name, RRF_RT_REG_DWORD, nullptr, &val, &vsz) == ERROR_SUCCESS)
                out[key] = (val != 0);
            else
                out[key] = true;
            any = true;
        }
        nSize = sizeof(name);
        ++idx;
    }
    RegCloseKey(hKey);
    return any;
}

void Config::SaveViewMenuState(const std::map<std::string, bool>& state)
{
    HKEY hKey;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegPath, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &hKey, nullptr) != ERROR_SUCCESS)
        return;

    for (const auto& kv : state)
    {
        std::wstring wkey = L"view_";
        for (char c : kv.first)
            wkey += static_cast<wchar_t>(c);
        DWORD val = kv.second ? 1u : 0u;
        RegSetValueExW(hKey, wkey.c_str(), 0, REG_DWORD,
                       reinterpret_cast<const BYTE*>(&val), sizeof(val));
    }
    RegCloseKey(hKey);
}

#endif // PLATFORM_WINDOWS
