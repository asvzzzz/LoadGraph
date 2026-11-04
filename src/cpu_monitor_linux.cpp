#include "cpu_monitor.h"
#include "power_table.h"
#include "platform.h"
#include "logger.h"

#ifdef PLATFORM_LINUX

#include <chrono>
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

    // Build the "family_model" key from /proc/cpuinfo (e.g. "6_79").
    // Format in the file: "cpu family\t: 6" and "model\t\t: 79".
    std::string ReadCpuFamilyModel()
    {
        std::string content;
        if (!ReadFileToString("/proc/cpuinfo", content))
            return "";

        std::istringstream iss(content);
        std::string line;
        std::string family, model;
        while (std::getline(iss, line))
        {
            if (line.compare(0, 10, "cpu family") == 0)
            {
                auto pos = line.find(':');
                if (pos != std::string::npos)
                    family = line.substr(pos + 1);
            }
            else if (line.compare(0, 5, "model") == 0 &&
                     line.compare(0, 10, "model name") != 0)
            {
                auto pos = line.find(':');
                if (pos != std::string::npos)
                    model = line.substr(pos + 1);
            }
            if (!family.empty() && !model.empty())
                break;
        }

        // Trim whitespace/CR around the captured numbers.
        auto trim = [](std::string& s) {
            size_t b = s.find_first_not_of(" \t\r");
            size_t e = s.find_last_not_of(" \t\r");
            s = (b == std::string::npos) ? std::string() : s.substr(b, e - b + 1);
        };
        trim(family);
        trim(model);
        if (family.empty() || model.empty())
            return "";
        return family + "_" + model;
    }

    // RAPL package energy counter state. The counter is monotonic in microjoules
    // and wraps at max_energy_range_uj; power = deltaE / deltaTime.
    // Only SampleLoad() (GUI thread, 1000 ms tick) touches this state, so no
    // extra locking is required.
    struct RaplState
    {
        bool        opened      = false;  // probe already attempted
        bool        valid       = false;  // counter path found and readable
        std::string path;                 // energy_uj file path
        long long   prevEnergyUj = 0;
        double      prevTimeSec  = 0.0;
        long long   maxEnergyUj  = 0;     // wrap-around range (0 = unknown)
    };
    static RaplState s_rapl;

    // Read the RAPL energy counter and return package power in watts.
    // Returns -1.0 when RAPL is unavailable (WSL2, AMD, root-only perms).
    double ReadRaplPowerWatts()
    {
        if (!s_rapl.opened)
        {
            s_rapl.opened = true;
            const char* paths[] = {
                "/sys/class/powercap/intel-rapl:0/energy_uj",
                "/sys/class/powercap/intel-rapl/intel-rapl:0/energy_uj",
                nullptr
            };
            for (int i = 0; paths[i]; ++i)
            {
                std::string probe;
                if (ReadFileToString(paths[i], probe))
                {
                    s_rapl.path  = paths[i];
                    s_rapl.valid = true;
                    break;
                }
            }
            if (s_rapl.valid)
            {
                // max_energy_range_uj lives next to energy_uj.
                std::string dir = s_rapl.path.substr(0, s_rapl.path.rfind('/'));
                std::string maxContent;
                if (ReadFileToString((dir + "/max_energy_range_uj").c_str(), maxContent))
                {
                    try { s_rapl.maxEnergyUj = std::stoll(maxContent); }
                    catch (...) { s_rapl.maxEnergyUj = 0; }
                }
                LOGI("RAPL energy counter found: %s", s_rapl.path.c_str());
            }
            else
            {
                LOGI("RAPL unavailable - CPU power uses load x TDP estimate.");
            }
        }
        if (!s_rapl.valid)
            return -1.0;

        std::string content;
        if (!ReadFileToString(s_rapl.path.c_str(), content))
        {
            s_rapl.valid = false; // counter disappeared (module unloaded)
            LOGI("RAPL energy counter became unreadable - disabling.");
            return -1.0;
        }

        long long energyUj = 0;
        try { energyUj = std::stoll(content); }
        catch (...) { return -1.0; }

        const auto now = std::chrono::steady_clock::now();
        const double nowSec =
            std::chrono::duration<double>(now.time_since_epoch()).count();

        double watts = -1.0;
        if (s_rapl.prevTimeSec > 0.0)
        {
            long long dE = energyUj - s_rapl.prevEnergyUj;
            // The 64-bit counter wraps at max_energy_range_uj.
            if (dE < 0 && s_rapl.maxEnergyUj > 0)
                dE += s_rapl.maxEnergyUj;
            const double dt = nowSec - s_rapl.prevTimeSec;
            if (dE >= 0 && dt > 0.1)
                watts = static_cast<double>(dE) / 1000000.0 / dt; // uJ -> J
        }
        s_rapl.prevEnergyUj = energyUj;
        s_rapl.prevTimeSec  = nowSec;
        return watts;
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

    // Resolve the TDP limit once from the static table via family+model key.
    const std::string famModel = ReadCpuFamilyModel();
    if (!famModel.empty())
    {
        m_metrics.tdpWatts = static_cast<double>(
            PowerTable::GetCpuTdp(famModel.c_str()));
        LOGI("CPU family/model: %s, TDP: %.0f W",
             famModel.c_str(), m_metrics.tdpWatts);
    }
    else
    {
        m_metrics.tdpWatts = 125.0;
        LOGI("CPU family/model unavailable, TDP default: 125 W");
    }

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
        return -1.0;
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
    if (total <= 0.0) return -1.0;

    // CPU usage = (total - idle) / total * 100
    double load = (total - dIdle) / total * 100.0;

    // CPU power: RAPL energy delta when readable, otherwise load% x TDP.
    // The RAPL file read happens outside the lock (I/O), values are stored
    // together with loadPercent under the mutex for GetMetrics().
    {
        double watts = ReadRaplPowerWatts();
        if (watts < 0.0)
        {
            const double tdp = (m_metrics.tdpWatts > 0.0) ? m_metrics.tdpWatts : 125.0;
            watts = load * tdp / 100.0;
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        m_metrics.loadPercent = load;
        m_metrics.powerWatts  = watts;
    }

    return load;
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
