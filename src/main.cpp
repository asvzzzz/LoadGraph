#include <wx/wx.h>
#include "logger.h"
#include "main_frame.h"
#include <cstdio>

static void WriteCrashLog(const char* msg)
{
    const wchar_t* path = L"C:\\Projects\\OpenCode\\loadgraph\\build\\bin\\Release\\crash.log";
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, nullptr,
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

class LoadGraphApp : public wxApp
{
public:
    bool OnInit() override
    {
        SetUnhandledExceptionFilter(TopLevelFilter);

        wxString logPath = wxGetCwd() + L"\\loadgraph.log";
        Logger::Init(logPath);
        LOGI("Starting...");
        LOGI("wxWidgets %s", wxVERSION_STRING);

        try
        {
            LOGI("Init images...");
            wxInitAllImageHandlers();
            LOGI("Images OK.");

            LOGI("Creating frame...");
            MainFrame* frame = new MainFrame(L"LoadGraph");
            if (!frame) return false;
            LOGI("Frame created 0x%p", static_cast<void*>(frame));

            LOGI("Showing...");
            frame->Show(true);
            frame->Raise();
            LOGI("Done.");
            return true;
        }
        catch (const std::exception& e)
        {
            LOGE("Exception: %s", e.what());
            WriteCrashLog(e.what());
            WriteCrashLog("\n");
            return false;
        }
        catch (...)
        {
            WriteCrashLog("Unknown exception!\n");
            return false;
        }
    }

    int OnExit() override
    {
        LOGI("Exiting.");
        Logger::Close();
        return 0;
    }
};

wxIMPLEMENT_APP(LoadGraphApp);
