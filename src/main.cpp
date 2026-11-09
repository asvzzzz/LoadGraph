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
    // NVIDIA green (#76B900)
    const int R = 118, G = 185, B = 0;

    // Create a bitmap and draw into it via a memory DC.
    wxBitmap bmp(64, 64);
    wxMemoryDC dc(bmp);
    dc.SetBackground(*wxTRANSPARENT_BRUSH);
    dc.Clear();

    wxPen   pen(wxColour(R, G, B), 0);
    wxBrush brush(wxColour(R, G, B));
    dc.SetPen(pen);
    dc.SetBrush(brush);
    dc.DrawRoundedRectangle(0, 0, 64, 64, 12);

    // White "LG" label
    dc.SetTextForeground(*wxWHITE);
    wxFont font(28, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_BOLD);
    dc.SetFont(font);
    dc.DrawText("LG", 14, 16);

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
