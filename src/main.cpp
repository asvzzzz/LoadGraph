#include <wx/wx.h>
#include "platform.h"
#include "logger.h"
#include "main_frame.h"
#include "metrics_api.h"
#include "http_server.h"
#include "app_config.h"
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
static wxIcon MakeEmbeddedGreenIcon()
{
    // Load the embedded Windows icon resource directly.
    // The .rc file compiles loadgraph.ico as ordinal 1 into the executable,
    // so Windows Explorer shows it immediately — no runtime temp files needed.
    return wxIcon(wxT("#1"), wxBITMAP_TYPE_ICO_RESOURCE);
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
        for (int i = 1; i < argc; ++i)
        {
            if (wxStrcmp(argv[i], L"-h") == 0 || wxStrcmp(argv[i], L"--help") == 0)
            {
                printf("LoadGraph - system monitoring dashboard\n"
                       "Usage: LoadGraph [-h]\n"
                       "  -h        Show this help\n");
                return false;
            }
        }

        wxString logPath = wxGetCwd() + "/loadgraph.log";
        Logger::Init(logPath);

        AppConfig appCfg = LoadAppConfig();

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

        // Start HTTP server if enabled in config.
        if (appCfg.http_enabled)
            StartHttpServer(appCfg);

        try
        {
            LOGI("Init images...");
            wxInitAllImageHandlers();
            LOGI("Images OK.");

            LOGI("Creating frame...");
            MainFrame* frame = nullptr;
            try { frame = new MainFrame(L"LoadGraph", appCfg); }
            catch (const std::exception& e) { LOGE("MainFrame ctor exception: %hs", e.what()); throw; }
            catch (...) { LOGE("MainFrame ctor: unknown exception"); throw; }
            if (!frame) return false;
            LOGI("Frame created 0x%p", static_cast<void*>(frame));

#ifdef PLATFORM_WINDOWS
            {
                wxIcon icon = MakeEmbeddedGreenIcon();
                LOGI("MakeEmbeddedGreenIcon: isOk=%d", icon.IsOk());
                if (icon.IsOk()) frame->SetIcon(icon);
            }
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
