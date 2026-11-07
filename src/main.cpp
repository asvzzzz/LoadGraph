#include <wx/wx.h>
#include "platform.h"
#include "logger.h"
#include "main_frame.h"
#include <cstdio>
#include <cstdlib>   // exit() for the elevated helper mode

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
    // Place crash.log next to the executable so it survives on any machine.
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
        // Do NOT ignore SIGABRT: GLib/GCC assert() calls abort(), and silencing it
        // suppresses the crash log and core dump. Let the default handler run.
#endif

        wxString logPath = wxGetCwd() + "/loadgraph.log";
        Logger::Init(logPath);
        LOGI("Starting...");
        LOGI("wxWidgets %s", wxVERSION_STRING);

#ifdef PLATFORM_WINDOWS
        // Elevated helper mode: invoked via UAC "runas" with --install-driver
        // to register/start the MSR driver service, then exit immediately.
        // No GUI is created in this mode.
        for (int i = 1; i < argc; ++i)
        {
            if (wxStrcmp(argv[i], L"--install-driver") == 0)
            {
                LOGI("Elevated helper mode (--install-driver).");
                const int rc = MsrSensor::RunInstallHelper(nullptr);
                Logger::Close();
                exit(rc); // 0 = driver installed and running
            }
        }
#endif

        try
        {
            LOGI("Init images...");
            wxInitAllImageHandlers();
            LOGI("Images OK.");

            LOGI("Creating frame...");
            MainFrame* frame = new MainFrame(L"LoadGraph");
            if (!frame) return false;
            LOGI("Frame created 0x%p", static_cast<void*>(frame));

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
