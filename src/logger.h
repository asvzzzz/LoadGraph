#ifndef LOGGER_H
#define LOGGER_H

#include <wx/wx.h>
#include <cstdio>
#include <ctime>

// Simple file logger using stdio.
class Logger
{
public:
    static void Init(const wxString& filename)
    {
        if (s_file) fclose(s_file);
        wxScopedCharBuffer buf = filename.mb_str();
        s_file = fopen(static_cast<const char*>(buf), "a");
    }

    static void Close()
    {
        if (s_file)
        {
            fflush(s_file);
            fclose(s_file);
            s_file = nullptr;
        }
    }

    static bool IsOpen() { return s_file != nullptr; }

    static void Info(const wxString& msg)
    {
        if (!s_file) return;
        time_t now = time(nullptr);
        char timeStr[64];
        strftime(timeStr, sizeof(timeStr), "%H:%M:%S", localtime(&now));
        wxScopedCharBuffer buf = msg.mb_str();
        fprintf(s_file, "%s INFO: %s\n", timeStr, static_cast<const char*>(buf));
        fflush(s_file);
    }

    static void Error(const wxString& msg)
    {
        if (!s_file) return;
        time_t now = time(nullptr);
        char timeStr[64];
        strftime(timeStr, sizeof(timeStr), "%H:%M:%S", localtime(&now));
        wxScopedCharBuffer buf = msg.mb_str();
        fprintf(s_file, "%s ERROR: %s\n", timeStr, static_cast<const char*>(buf));
        fflush(s_file);
    }

private:
    static FILE* s_file;
};

#define LOGI(...) Logger::Info(wxString::Format(__VA_ARGS__))
#define LOGE(...) Logger::Error(wxString::Format(__VA_ARGS__))

#endif // LOGGER_H
