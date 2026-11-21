#include "jsonl_log.h"

#include "metrics_api.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

// ============================================================================
// JSONL snapshot writer - see jsonl_log.h for the contract.
// Moved out of console_app.cpp so the GUI can log the same snapshots.
// ============================================================================

// GPU load classification - determine workload type from utilization patterns.
// Returns descriptive string for the "type" field.
static std::string ClassifyGpuLoad(double load, double temp, double power, double fanPct)
{
    // High load + high temperature + high power = compute-intensive (LLM inference, rendering)
    if (load > 80.0 && temp > 65.0 && power > 50.0)
        return "compute";
    // Moderate load with variable patterns = general purpose
    if (load > 20.0)
        return "mixed";
    // Very low load = idle or light task
    if (load < 5.0)
        return "idle";
    // Medium load with cooling activity = sustained workload
    if (fanPct > 50.0)
        return "sustained";
    return "light";
}

// Path of the active log; empty = disabled. Written once at startup from
// the main thread, read by the single writer thread afterwards.
static std::string s_path;

// Cleared after the first successful write of this run. Only touched by
// the single writer thread, atomic keeps it race-free by construction.
static std::atomic<bool> s_freshWrite{true};

// 5 s throttle anchor. Updated on every write ATTEMPT (not only success)
// so a broken path cannot turn into a fopen retry storm.
static std::chrono::steady_clock::time_point s_lastWrite{};

void JsonlLogSetPath(const std::string& path)
{
    s_path = path;
    s_freshWrite.store(true);
    // First snapshot becomes due 5 s after startup, matching the previous
    // console behavior (no partial-metrics line at launch).
    s_lastWrite = std::chrono::steady_clock::now();
}

void JsonlLogSnapshot(const CpuData& cpu, const RamData& ram,
                      const std::vector<GpuData>& gpus)
{
    if (s_path.empty()) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - s_lastWrite < std::chrono::seconds(5)) return;
    s_lastWrite = now;

    // Get current timestamp. Local time, no UTC designator: the value is
    // what the wall clock shows, "Z" would falsely claim UTC. system_clock
    // (not the steady_clock used for throttling) is the wall-clock source.
    const auto sysNow = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(sysNow);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        sysNow.time_since_epoch()) % 1000;
    char timeBuf[64];
    struct tm tm_info;
#ifdef PLATFORM_WINDOWS
    localtime_s(&tm_info, &time_t_now);
#else
    localtime_r(&time_t_now, &tm_info);
#endif
    strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%dT%H:%M:%S", &tm_info);
    std::string timestamp = std::string(timeBuf) + "." + std::to_string(ms.count());

    // Build JSON manually to avoid dependency on JSON library.
    std::string json;
    json += "{\"timestamp\":\"" + timestamp + "\",";
    json += "\"cpu\":{\"load\":" + std::to_string(cpu.loadPercent) + ",";
    json += "\"temp\":" + std::to_string(cpu.temperature) + ",";
    json += "\"fan\":" + std::to_string(cpu.fanRpm) + ",";
    json += "\"power\":" + std::to_string(cpu.powerWatts) + ",";
    json += "\"tdp\":" + std::to_string(cpu.tdpWatts) + "},";
    json += "\"ram\":{\"load\":" + std::to_string(ram.loadPercent) + ",";
    json += "\"total\":" + std::to_string(static_cast<double>(ram.totalBytes) / (1024.0*1024*1024)) + ",";
    json += "\"used\":" + std::to_string(static_cast<double>(ram.totalBytes - ram.freeBytes) / (1024.0*1024*1024)) + "},";
    json += "\"gpus\":[";

    for (size_t i = 0; i < gpus.size(); ++i)
    {
        const auto& g = gpus[i];
        std::string loadType = ClassifyGpuLoad(g.loadPercent, g.temperature, g.powerWatts, g.fanPercent);
        if (i > 0) json += ",";
        json += "{\"idx\":" + std::to_string(i) + ",";
        json += "\"load\":" + std::to_string(g.loadPercent) + ",";
        json += "\"temp\":" + std::to_string(g.temperature) + ",";
        json += "\"fan\":" + std::to_string(g.fanRpm) + ",";
        json += "\"power\":" + std::to_string(g.powerWatts) + ",";
        json += "\"vram_total\":" + std::to_string(static_cast<double>(g.vramTotalBytes) / (1024.0*1024*1024)) + ",";
        json += "\"vram_used\":" + std::to_string(static_cast<double>(g.vramUsedBytes) / (1024.0*1024*1024)) + ",";
        json += "\"type\":\"" + loadType + "\"}";
    }

    json += "]}";

    // Write to file: the first snapshot of this run truncates it, the rest
    // append. The fresh flag is cleared only after a successful write, so a
    // failed open does not turn the next attempt into an append over the old
    // file of the previous run.
    const bool fresh = s_freshWrite.load();
    FILE* fp = nullptr;
#ifdef PLATFORM_WINDOWS
    fopen_s(&fp, s_path.c_str(), fresh ? "w" : "a");
#else
    fp = fopen(s_path.c_str(), fresh ? "w" : "a");
#endif
    if (fp)
    {
        fprintf(fp, "%s\n", json.c_str());
        fclose(fp);
        s_freshWrite.store(false);
    }
}
