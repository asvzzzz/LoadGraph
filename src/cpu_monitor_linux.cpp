#include "cpu_monitor.h"
#include "platform.h"
#include "logger.h"

#ifdef PLATFORM_LINUX

#include <cstdint>
#include <fstream>
#include <sstream>
#include <ctime>
#include <algorithm>

// ----------------------------------------------------------------------------
// Linux-specific CPU monitor implementation.
// Uses /proc/stat for load and /sys/class/thermal for temperature.
// ----------------------------------------------------------------------------

namespace
{
    // Parse /proc/stat to get CPU times.
    // Format: cpu  user nice system idle iowait irq softirq steal guest guest_nice
    bool ReadProcStat(double& user, double& nice, double& system, double& idle)
    {
        std::string line;
        if (!ReadFileToString("/proc/stat", line))
            return false;

        std::istringstream iss(line);
        std::string label;
        iss >> label; // "cpu"
        if (label != "cpu") return false;

        double u, n, s, id, iw, irq, sq, st, g, gn;
        iss >> u >> n >> s >> id >> iw >> irq >> sq >> st >> g >> gn;

        user = u;
        nice = n;
        system = s;
        idle = id;
        return true;
    }

    // Read CPU temperature from thermal zones.
    // Returns temperature in Celsius, or -1.0 if unavailable.
    double ReadCpuTemperature()
    {
        // Try common thermal zone paths.
        const char* paths[] = {
            "/sys/class/thermal/thermal_zone0/temp",
            "/sys/class/thermal/zones/zone0/temp",
            nullptr
        };

        for (int i = 0; paths[i]; ++i)
        {
            std::string content;
            if (!ReadFileToString(paths[i], content))
                continue;

            // Temperature is in millidegrees Celsius.
            int raw = 0;
            try { raw = std::stoi(content); }
            catch (...) { continue; }

            if (raw < 0) continue;
            return static_cast<double>(raw) / 1000.0;
        }

        return -1.0;
    }

    // Read CPU name from /proc/cpuinfo.
    std::string ReadCpuName()
    {
        std::string content;
        if (!ReadFileToString("/proc/cpuinfo", content))
            return "";

        std::istringstream iss(content);
        std::string line;
        while (std::getline(iss, line))
        {
            if (line.find("model name") == 0)
            {
                auto pos = line.find(':');
                if (pos != std::string::npos)
                    return line.substr(pos + 2); // Skip ": "
            }
        }
        return "";
    }

    // Compute uptime in milliseconds from /proc/uptime.
    uint64_t ReadUptimeMs()
    {
        std::string content;
        if (!ReadFileToString("/proc/uptime", content))
            return 0;

        double seconds = 0.0;
        try { seconds = std::stod(content); }
        catch (...) { return 0; }

        return static_cast<uint64_t>(seconds * 1000.0);
    }
} // anonymous namespace

// ----------------------------------------------------------------------------
// CpuMonitor implementation for Linux
// ----------------------------------------------------------------------------

CpuMonitor::CpuMonitor()
{
    LOGI("CPU monitor initializing (Linux /proc path)...");

    // Read CPU name once at startup.
    std::string name = ReadCpuName();
    if (!name.empty())
        m_metrics.cpuName = std::wstring(name.begin(), name.end());
    LOGI("CPU name: %S", m_metrics.cpuName.c_str());

    // Initial uptime read.
    m_metrics.uptimeMs = ReadUptimeMs();
}

CpuMonitor::~CpuMonitor()
{
}

CpuMetrics CpuMonitor::GetMetrics()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_metrics;
}

double CpuMonitor::SampleLoad()
{
    double user = 0, nice = 0, system = 0, idle = 0;
    if (!ReadProcStat(user, nice, system, idle))
        return -1.0;

    // Calculate CPU usage since last sample.
    static double s_prevUser = 0, s_prevNice = 0, s_prevSystem = 0, s_prevIdle = 0;
    static bool s_first = true;

    if (s_first)
    {
        s_prevUser = user;
        s_prevNice = nice;
        s_prevSystem = system;
        s_prevIdle = idle;
        s_first = false;
        return 0.0;
    }

    double dUser = user - s_prevUser;
    double dNice = nice - s_prevNice;
    double dSystem = system - s_prevSystem;
    double dIdle = idle - s_prevIdle;

    s_prevUser = user;
    s_prevNice = nice;
    s_prevSystem = system;
    s_prevIdle = idle;

    double total = dUser + dNice + dSystem + dIdle;
    if (total <= 0.0) return 0.0;

    // CPU usage = (total - idle) / total * 100
    return (total - dIdle) / total * 100.0;
}

void CpuMonitor::QueryCpuName()
{
    // Already read in constructor on Linux.
}

void CpuMonitor::Collect()
{
    // Update uptime.
    m_metrics.uptimeMs = ReadUptimeMs();

    // Temperature (one-shot probe, same as Windows WMI approach).
    if (!m_wmiTried)
    {
        m_wmiTried = true;
        double temp = ReadCpuTemperature();
        if (temp >= 0.0)
        {
            m_metrics.temperature = temp;
            LOGI("CPU temp sensor found: %.1f C", temp);
        }
        else
        {
            LOGI("CPU temp sensor unavailable");
        }
        m_wmiOk = (temp >= 0.0);
    }

    // Fan speed - not commonly available on Linux desktops without special drivers.
    // Leaving as N/A by default.
}

#ifdef PLATFORM_WINDOWS
bool CpuMonitor::QueryOverallLoad(double& loadPercent)
{
    loadPercent = SampleLoad();
    return loadPercent >= 0.0;
}

unsigned int CpuMonitor::ProbeWmiSensor(double& tempC, int& fanRpm)
{
    tempC = ReadCpuTemperature();
    fanRpm = -1;
    return (tempC >= 0.0) ? 1 : 0;
}
#endif

#endif // PLATFORM_LINUX
