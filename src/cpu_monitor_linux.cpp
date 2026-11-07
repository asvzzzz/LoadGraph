#include "cpu_monitor.h"
#include "power_table.h"
#include "platform.h"
#include "logger.h"

#ifdef PLATFORM_LINUX

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <ctime>
#include <algorithm>
#include <set>
#include <utility>

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

    // Count unique physical cores from /proc/cpuinfo ("physical id" + "core id"
    // pairs, one pair per physical core regardless of hyperthreading).
    // Used by the idle-floor power estimate (P_idle scales with core count).
    // Falls back to the logical processor count when the fields are absent
    // (ARM, some VMs/WSL); returns 0 only if /proc/cpuinfo is unreadable.
    uint32_t CountPhysicalCores()
    {
        std::string content;
        if (!ReadFileToString("/proc/cpuinfo", content))
            return 0;

        std::set<std::pair<int, int>> cores;
        int processors = 0;

        std::istringstream iss(content);
        std::string line;
        int physId = -1, coreId = -1;
        while (std::getline(iss, line))
        {
            if (line.empty())
            {
                if (physId >= 0 && coreId >= 0)
                    cores.insert(std::make_pair(physId, coreId));
                physId = coreId = -1;
                continue;
            }

            if (line.compare(0, 9, "processor") == 0)
                ++processors;

            const size_t colon = line.find(':');
            if (colon == std::string::npos)
                continue;

            // Key is everything left of ':', trailing spaces/tabs trimmed.
            std::string key = line.substr(0, colon);
            const size_t keyEnd = key.find_last_not_of(" \t");
            if (keyEnd == std::string::npos)
                continue;
            key.erase(keyEnd + 1);

            const int value = std::atoi(line.c_str() + colon + 1);
            if (key == "physical id")
                physId = value;
            else if (key == "core id")
                coreId = value;
        }
        if (physId >= 0 && coreId >= 0)
            cores.insert(std::make_pair(physId, coreId));

        if (!cores.empty())
            return static_cast<uint32_t>(cores.size());
        return (processors > 0) ? static_cast<uint32_t>(processors) : 0;
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
    LOGI("CPU name: %s", m_metrics.cpuName.c_str());

    // Resolve the TDP limit once from the static table via family+model key.
    const std::string famModel = ReadCpuFamilyModel();
    if (!famModel.empty())
    {
        m_metrics.tdpWatts = static_cast<double>(
            PowerTable::GetCpuTdp(famModel.c_str()));
        LOGI("CPU family/model: %hs, TDP: %.0f W",
             famModel.c_str(), m_metrics.tdpWatts);
    }
    else
    {
        m_metrics.tdpWatts = 125.0;
        LOGI("CPU family/model unavailable, TDP default: 125 W");
    }

    // Physical core count feeds the idle-floor power estimate (used when the
    // RAPL counter is unavailable, e.g. under WSL2).
    const uint32_t cores = CountPhysicalCores();
    m_coreCount.store(cores, std::memory_order_relaxed);
    LOGI("Physical cores detected: %u", cores);

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

// Read the RAPL energy counter and return package power in watts.
// Returns -1.0 when RAPL is unavailable (WSL2, AMD, root-only perms).
double CpuMonitor::ReadRaplPowerWatts()
{
    if (!m_rapl.opened)
    {
        m_rapl.opened = true;
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
                m_rapl.path  = paths[i];
                m_rapl.valid = true;
                break;
            }
        }
        if (m_rapl.valid)
        {
            // max_energy_range_uj lives next to energy_uj.
            std::string dir = m_rapl.path.substr(0, m_rapl.path.rfind('/'));
            std::string maxContent;
            if (ReadFileToString((dir + "/max_energy_range_uj").c_str(), maxContent))
            {
                try { m_rapl.maxEnergyUj = std::stoll(maxContent); }
                catch (...) { m_rapl.maxEnergyUj = 0; }
            }
            LOGI("RAPL energy counter found: %hs", m_rapl.path.c_str());
        }
        else
        {
            LOGI("RAPL unavailable - CPU power uses idle-floor estimate.");
        }
    }
    if (!m_rapl.valid)
        return -1.0;

    std::string content;
    if (!ReadFileToString(m_rapl.path.c_str(), content))
    {
        m_rapl.valid = false; // counter disappeared (module unloaded)
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
    if (m_rapl.prevTimeSec > 0.0)
    {
        long long dE = energyUj - m_rapl.prevEnergyUj;
        // The 64-bit counter wraps at max_energy_range_uj.
        if (dE < 0 && m_rapl.maxEnergyUj > 0)
            dE += m_rapl.maxEnergyUj;
        const double dt = nowSec - m_rapl.prevTimeSec;
        if (dE >= 0 && dt > 0.1)
            watts = static_cast<double>(dE) / 1000000.0 / dt; // uJ -> J
    }
    m_rapl.prevEnergyUj = energyUj;
    m_rapl.prevTimeSec  = nowSec;
    return watts;
}

double CpuMonitor::SampleLoad()
{
    double user = 0, nice = 0, system = 0, idle = 0;
    if (!ReadProcStat(user, nice, system, idle))
        return -1.0;

    // Calculate CPU usage since last sample.
    // State lives in class members (m_prevUser etc.) so multiple CpuMonitor
    // instances do not share counters — the old statics were a latent bug.
    if (m_loadFirst)
    {
        m_prevUser  = user;
        m_prevNice  = nice;
        m_prevSystem = system;
        m_prevIdle  = idle;
        m_loadFirst = false;
        return -1.0;
    }

    double dUser = user - m_prevUser;
    double dNice = nice - m_prevNice;
    double dSystem = system - m_prevSystem;
    double dIdle = idle - m_prevIdle;

    m_prevUser  = user;
    m_prevNice  = nice;
    m_prevSystem = system;
    m_prevIdle  = idle;

    double total = dUser + dNice + dSystem + dIdle;
    if (total <= 0.0) return -1.0;

    // CPU usage = (total - idle) / total * 100
    double load = (total - dIdle) / total * 100.0;

    // CPU power: RAPL energy delta when readable, otherwise the idle-floor
    // estimate (plain load x TDP showed ~1 W at idle under WSL2 where RAPL is
    // not exposed). The RAPL file read happens outside the lock (I/O); all
    // m_metrics writes happen under the mutex for GetMetrics().
    {
        double watts = ReadRaplPowerWatts();
        // Exactly 0.0 W over a 1 s window means a stuck/bogus reading -
        // fall back to the estimate instead of drawing a flat zero line.
        const bool real = (watts > 0.0);

        std::lock_guard<std::mutex> lock(m_mutex);
        m_metrics.loadPercent = load;
        if (!real)
        {
            const double tdp = (m_metrics.tdpWatts > 0.0) ? m_metrics.tdpWatts : 125.0;
            watts = PowerTable::EstimateCpuPowerWatts(
                load,
                static_cast<uint32_t>(tdp + 0.5),
                m_coreCount.load(std::memory_order_relaxed));
        }
        m_metrics.powerWatts = watts;
        m_powerReal.store(real, std::memory_order_relaxed);
    }

    return load;
}

void CpuMonitor::QueryCpuName()
{
    // Already read in constructor on Linux.
}

void CpuMonitor::Collect()
{
    const uint64_t uptimeMs = ReadUptimeMs();

    // Temperature: availability is probed once (guarded); after a successful
    // probe the sysfs value is re-read every tick - a file read is cheap and
    // keeps the graph live (the previous one-shot read froze it at the value
    // from startup). A failed re-read keeps the last known value.
    double temp = -1.0;
    bool   probe = false;
    if (!m_wmiTried)
    {
        m_wmiTried = true;
        temp = ReadCpuTemperature();
        m_wmiOk = (temp >= 0.0);
        probe = true;
    }
    else if (m_wmiOk)
    {
        temp = ReadCpuTemperature();
    }

    // All writes to m_metrics must hold the mutex: GetMetrics() reads it from
    // the GUI thread (data race otherwise).
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_metrics.uptimeMs = uptimeMs;
        if (temp >= 0.0)
            m_metrics.temperature = temp;
    }

    if (probe)
    {
        if (m_wmiOk)
            LOGI("CPU temp sensor found: %.1f C", temp);
        else
            LOGI("CPU temp sensor unavailable");
    }

    // Fan speed - not commonly available on Linux desktops without special drivers.
    // Leaving as N/A by default.
}

#endif // PLATFORM_LINUX
