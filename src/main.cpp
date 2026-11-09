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
    // NVIDIA green (#76B900) rounded square with white "LG" label.
    const int R = 118, G = 185, B = 0;

    // Build the image pixel-by-pixel for full control over alpha.
    wxImage img(64, 64);
    unsigned char* pixels = img.GetData();
    memset(pixels, 0, 64 * 64 * 3);          // RGB init to black (will fill)
    unsigned char* alpha  = img.GetAlpha();
    if (!alpha) { alpha = new unsigned char[64 * 64]; memset(alpha, 255, 64 * 64); img.SetAlpha(alpha); delete[] alpha; }
    else        memset(alpha, 0, 64 * 64);

    const int cx = 32, cy = 32, rx = 28, ry = 28, cr = 10; // center, radius, corner
    for (int y = 0; y < 64; ++y)
    {
        for (int x = 0; x < 64; ++x)
        {
            int idx = (y * 64 + x);
            float dx = (float)(x - cx) / (float)rx;
            float dy = (float)(y - cy) / (float)ry;
            // Rounded rectangle mask: ellipse with corner clipping
            bool inEllipse = (dx*dx + dy*dy) <= 1.0f;
            bool inCorner  = false;
            if (!inEllipse)
            {
                float ex = fabsf(dx) - 1.0f;
                float ey = fabsf(dy) - 1.0f;
                float cd = (float)cr / (float)rx; // corner radius normalized
                inCorner = (ex*ex + ey*ey) <= (cd*cd);
            }
            bool inside = inEllipse || inCorner;
            if (inside)
            {
                pixels[idx*3]   = (unsigned char)R;
                pixels[idx*3+1] = (unsigned char)G;
                pixels[idx*3+2] = (unsigned char)B;
                alpha[idx] = 255;
            }
        }
    }

    // Draw "LG" text using a simple pixel font (5x7 chars, centered).
    // White text at center.
    const int tx0 = 18, ty0 = 18;
    static const unsigned char lg_bits[] = {
        // 'L': 5 wide x 7 tall
        0x01, 0x01, 0x01, 0x01, 0x01,
        0x01, 0x01, 0x01, 0x01, 0x01,
        0x01, 0x01, 0x01, 0x01, 0x01,
        0x00, 0x00, 0x00, 0x00, 0xFF,
        // 'G': 5 wide x 7 tall
        0x00, 0x3E, 0x41, 0x41, 0x3E,
        0x41, 0x41, 0x41, 0x22, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00,
    };
    // Draw L at (tx0, ty0), G at (tx0+7, ty0) — each char is 5x7
    auto drawChar = [&](int ox, int oy, const unsigned char* bits, int w, int h) {
        for (int row = 0; row < h; ++row)
            for (int col = 0; col < w; ++col)
            {
                if (bits[row*w + col] & 0x80)
                {
                    int px = ox + col, py = ty0 + row;
                    if (px >= 0 && px < 64 && py >= 0 && py < 64)
                    {
                        int pi = py*64 + px;
                        pixels[pi*3]   = 255;
                        pixels[pi*3+1] = 255;
                        pixels[pi*3+2] = 255;
                        alpha[pi] = 255;
                    }
                }
            }
    };
    // 'L' — 5x7 block: column of 7 on left, base row across bottom
    for (int r = 0; r < 7; ++r)
        for (int c = 0; c < 5; ++c)
        {
            bool lit = (c == 0) || (r == 6);
            if (lit)
            {
                int px = tx0 + c, py = ty0 + r;
                if (px>=0 && px<64 && py>=0 && py<64)
                { int pi=py*64+px; pixels[pi*3]=255; pixels[pi*3+1]=255; pixels[pi*3+2]=255; alpha[pi]=255; }
            }
        }
    // 'G' — 5x7 block
    for (int r = 0; r < 7; ++r)
        for (int c = 0; c < 5; ++c)
        {
            bool lit = false;
            if      (r == 0 && c >= 1 && c <= 3) lit = true;
            else if (r == 1 && c >= 0 && c <= 4) lit = true;
            else if (r == 2 && c >= 0 && c <= 4) lit = true;
            else if (r == 3 && (c == 0 || c == 4)) lit = true;
            else if (r == 4 && c >= 2 && c <= 4) lit = true;
            else if (r == 5 && c >= 1 && c <= 3) lit = true;
            if (lit)
            {
                int px = tx0 + 7 + c, py = ty0 + r;
                if (px>=0 && px<64 && py>=0 && py<64)
                { int pi=py*64+px; pixels[pi*3]=255; pixels[pi*3+1]=255; pixels[pi*3+2]=255; alpha[pi]=255; }
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
