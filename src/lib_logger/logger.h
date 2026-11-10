#ifndef LOGGER_H
#define LOGGER_H

#include <cstdio>
#include <ctime>
#include <cstdarg>

// Simple file logger using stdio. Works with both wxWidgets and standalone builds.
class Logger
{
public:
    static void Init(const char* filename)
    {
        if (s_file) fclose(s_file);
        s_file = fopen(filename, "a");
        if (!s_file)
            fprintf(stderr, "LoadGraph: cannot open %s for logging\n", filename);
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

    static void Info(const char* fmt, ...)
    {
        if (!s_file) return;
        time_t now = time(nullptr);
        char timeStr[64];
        strftime(timeStr, sizeof(timeStr), "%H:%M:%S", localtime(&now));
        va_list args;
        va_start(args, fmt);
        vfprintf(s_file, fmt, args);
        va_end(args);
        fprintf(s_file, "\n");
        fflush(s_file);
    }

    static void Error(const char* fmt, ...)
    {
        if (!s_file) return;
        time_t now = time(nullptr);
        char timeStr[64];
        strftime(timeStr, sizeof(timeStr), "%H:%M:%S", localtime(&now));
        va_list args;
        va_start(args, fmt);
        vfprintf(s_file, fmt, args);
        va_end(args);
        fprintf(s_file, "\n");
        fflush(s_file);
    }

private:
    static FILE* s_file;
};

#define LOGI(...) Logger::Info(__VA_ARGS__)
#define LOGE(...) Logger::Error(__VA_ARGS__)

#endif // LOGGER_H
