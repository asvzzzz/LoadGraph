#include <wx/wx.h>
#include "platform.h"
#include "logger.h"
#include "main_frame.h"
#include "metrics_api.h"
#include "http_server.h"
#include <cstdio>
#include <cstdlib>

#ifdef PLATFORM_WINDOWS
#include "msr_sensor.h"
#include <cstring>
#endif

#ifdef PLATFORM_LINUX
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

static void CrashHandler(int sig)
{
    fprintf(stderr, "Caught signal %d\n", sig);
    fflush(stderr);
    _exit(128 + sig);
}
#endif

#ifdef PLATFORM_WINDOWS
#include <windows.h>

static void WriteCrashLog(const char* msg)
{
    wchar_t exePath[MAX_PATH] = {};
    DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return;
    std::wstring path(exePath);
    auto slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos) path.resize(slash + 1);
    path += L"crash.log";

    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE)
    {
        DWORD dw;
        WriteFile(h, msg, (DWORD)strlen(msg), &dw, nullptr);
        CloseHandle(h);
    }
}

LONG WINAPI TopLevelFilter(EXCEPTION_POINTERS* ep)
{
    char buf[1024];
    snprintf(buf, sizeof(buf),
             "=== CRASH ===\n"
             "Code: 0x%08X\n"
             "Addr: 0x%p\n",
             ep->ExceptionRecord->ExceptionCode,
             ep->ExceptionRecord->ExceptionAddress);
    WriteCrashLog(buf);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

// ----------------------------------------------------------------------------
// Create an NVIDIA-green icon programmatically (no external file needed).
// Draws a 16x16 rounded rectangle with the NVIDIA green (#76B900) fill and
// a white "LG" label in the center.
// ----------------------------------------------------------------------------
#ifdef PLATFORM_WINDOWS
// Match the ID used in src/icon.rc: IDI_ICON ICON "loadgraph.ico"
#define IDI_ICON 101

static wxIcon MakeEmbeddedGreenIcon()
{
    // Load the embedded Windows icon resource (IDI_ICON) compiled into the
    // binary via src/icon.rc. Recolor non-transparent pixels to NVIDIA green.
    const int R = 118, G = 185, B = 0;

    // Use MAKEINTRESOURCE so Windows looks up the icon by ID in the module's
    // resource table — this is the correct way to load a compiled .rc icon.
    HICON hIcon = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_ICON));
    if (!hIcon) return wxIcon();

    ICONINFO info{};
    if (!GetIconInfo(hIcon, &info)) { DestroyIcon(hIcon); return wxIcon(); }

    // Copy icon bitmap into a 32-bit DIB section for pixel manipulation.
    HDC hdcScreen = GetDC(nullptr);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    HBITMAP hbmSrc = info.hbmColor ? info.hbmColor : info.hbmMask;
    HBITMAP hbmOld = (HBITMAP)SelectObject(hdcMem, hbmSrc);

    BITMAP bm{};
    GetObject(hbmSrc, sizeof(bm), &bm);

    BITMAPINFOHEADER bih{};
    bih.biSize          = sizeof(BITMAPINFOHEADER);
    bih.biWidth         = bm.bmWidth;
    bih.biHeight        = -bm.bmHeight;   // top-down DIB
    bih.biPlanes        = 1;
    bih.biBitCount      = 32;
    bih.biCompression   = BI_RGB;

    void* pBits = nullptr;
    HBITMAP hbmDIB = CreateDIBSection(hdcMem, (BITMAPINFO*)&bih, DIB_RGB_COLORS, &pBits, nullptr, 0);
    HBITMAP hbmOld2 = (HBITMAP)SelectObject(hdcMem, hbmDIB);
    BitBlt(hdcMem, 0, 0, bm.bmWidth, bm.bmHeight, hdcMem, 0, 0, SRCCOPY);

    // Copy pixels BEFORE cleaning up GDI objects.
    unsigned char* pixels = static_cast<unsigned char*>(pBits);
    const int w = bm.bmWidth, h = bm.bmHeight;
    wxImage img(w, h);
    memcpy(img.GetData(), pBits, w * h * 4);

    // Recolor non-transparent pixels to NVIDIA green (#76B900).
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            int idx = (y * w + x) * 4;
            if (pixels[idx + 3] > 0)
            {
                pixels[idx]     = B;
                pixels[idx + 1] = G;
                pixels[idx + 2] = R;
            }
        }

    // Build alpha channel from the copied pixels.
    unsigned char* alpha = new unsigned char[w * h];
    for (int i = 0; i < w * h; ++i)
        alpha[i] = pixels[i * 4 + 3];
    img.SetAlpha(alpha);

    SelectObject(hdcMem, hbmOld2);
    DeleteObject(hbmDIB);
    SelectObject(hdcMem, hbmOld);
    DeleteDC(hdcMem);
    ReleaseDC(nullptr, hdcScreen);
    DestroyIcon(hIcon);
    if (info.hbmColor)  DeleteObject(info.hbmColor);
    if (info.hbmMask)   DeleteObject(info.hbmMask);

    wxBitmap bmp(img);
    wxIcon icon;
    icon.CopyFromBitmap(bmp);
    return icon;
}
#endif

class LoadGraphApp : public wxApp
{
public:
    bool OnInit() override
    {
#ifdef PLATFORM_WINDOWS
        SetUnhandledExceptionFilter(TopLevelFilter);
#endif

#ifdef PLATFORM_LINUX
        signal(SIGSEGV, CrashHandler);
#endif

        // Parse command-line arguments before logging.
        int httpPort = 0;
        for (int i = 1; i < argc; ++i)
        {
            wxString arg = argv[i];
            if (arg == L"-port" && i + 1 < argc)
            {
                long p = 0;
                if (argv[i + 1].ToLong(&p) && p > 0 && p <= 65535)
                    httpPort = (int)p;
                else
                    fprintf(stderr, "Invalid port: %s\n", argv[i + 1]);
                ++i;
            }
            else if (arg == L"-h" || arg == L"--help")
            {
                printf("LoadGraph - system monitoring dashboard\n"
                       "Usage: LoadGraph [-port N] [-h]\n"
                       "  -port N   Start HTTP server on port N, serve web dashboard\n"
                       "  -h        Show this help\n");
                return false;
            }
        }

        wxString logPath = wxGetCwd() + "/loadgraph.log";
        Logger::Init(logPath);
        LOGI("Starting...");
        LOGI("wxWidgets %s", wxVERSION_STRING);

#ifdef PLATFORM_WINDOWS
        // Elevated helper mode: invoked via UAC "runas" with --install-driver
        for (int i = 1; i < argc; ++i)
        {
            if (wxStrcmp(argv[i], L"--install-driver") == 0)
            {
                LOGI("Elevated helper mode (--install-driver).");
                const int rc = MsrSensor::RunInstallHelper(nullptr);
                Logger::Close();
                exit(rc);
            }
        }
#endif

        // Start HTTP server in a background thread if -port was specified.
        if (httpPort > 0)
            StartHttpServer(httpPort);

        try
        {
            LOGI("Init images...");
            wxInitAllImageHandlers();
            LOGI("Images OK.");

            LOGI("Creating frame...");
            MainFrame* frame = new MainFrame(L"LoadGraph");
            if (!frame) return false;
            LOGI("Frame created 0x%p", static_cast<void*>(frame));

#ifdef PLATFORM_WINDOWS
            frame->SetIcon(MakeEmbeddedGreenIcon());
#endif

            LOGI("Showing frame...");
            frame->Show(true);
            frame->Raise();
            LOGI("Done.");
            return true;
        }
        catch (const std::exception& e)
        {
            LOGE("Exception: %hs", e.what());
#ifdef PLATFORM_WINDOWS
            WriteCrashLog(e.what());
            WriteCrashLog("\n");
#endif
            return false;
        }
        catch (...)
        {
#ifdef PLATFORM_WINDOWS
            WriteCrashLog("Unknown exception!\n");
#endif
            return false;
        }
    }

    int OnExit() override
    {
        Logger::Close();
        return wxApp::OnExit();
    }
};

wxIMPLEMENT_APP(LoadGraphApp);
