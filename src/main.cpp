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
static wxIcon MakeNvidiaGreenIcon()
{
    // NVIDIA green (#76B900) rounded square with white "LG".
    // Use wxImage (32-bit RGBA) directly — wxBitmap without alpha produces
    // a black icon on this wxWidgets build because CopyFromBitmap can't
    // create a proper mask from a device-dependent bitmap.
    const int R = 118, G = 185, B = 0;

    wxImage img(64, 64);
    unsigned char* pixels = img.GetData();
    memset(pixels, 0, 64 * 64 * 3);

    // Fill with green.
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x)
        {
            int idx = (y * 64 + x) * 3;
            pixels[idx]   = R;
            pixels[idx+1] = G;
            pixels[idx+2] = B;
        }

    // Rounded-rect mask — transparent corners.
    unsigned char* alpha = new unsigned char[64 * 64];
    const int cx = 32, cy = 32, rx = 28, ry = 28, cr = 10;
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x)
        {
            float dx = (float)(x - cx) / (float)rx;
            float dy = (float)(y - cy) / (float)ry;
            bool inEllipse = (dx*dx + dy*dy) <= 1.0f;
            bool inCorner = false;
            if (!inEllipse)
            {
                float ex = fabsf(dx) - 1.0f;
                float ey = fabsf(dy) - 1.0f;
                float cd = (float)cr / (float)rx;
                inCorner = (ex*ex + ey*ey) <= (cd*cd);
            }
            alpha[y * 64 + x] = (inEllipse || inCorner) ? 255 : 0;
        }
    img.SetAlpha(alpha);

    // Draw "LG" text — white, opaque.
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x)
        {
            int pi = y * 64 + x;
            // Simple pixel-font "LG" at center.
            bool lit = false;
            // 'L' at (16, 18), 5 wide x 7 tall
            if (x >= 16 && x < 21 && y >= 18 && y < 25)
            {
                int lx = x - 16, ly = y - 18;
                lit = (lx == 0) || (ly == 6);
            }
            // 'G' at (23, 18), 5 wide x 7 tall
            if (x >= 23 && x < 28 && y >= 18 && y < 25)
            {
                int gx = x - 23, gy = y - 18;
                lit = (gy == 0 && gx >= 1 && gx <= 3) ||
                      (gy >= 1 && gy <= 2 && gx >= 0 && gx <= 4) ||
                      (gy == 3 && (gx == 0 || gx == 4)) ||
                      (gy == 4 && gx >= 2 && gx <= 4) ||
                      (gy == 5 && gx >= 1 && gx <= 3);
            }
            if (lit)
            {
                int pi2 = y * 64 + x;
                pixels[pi2*3]   = 255;
                pixels[pi2*3+1] = 255;
                pixels[pi2*3+2] = 255;
                alpha[pi2] = 255;
            }
        }

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
            frame->SetIcon(MakeNvidiaGreenIcon());
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
