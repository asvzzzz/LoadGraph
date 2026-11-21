#ifndef JSONL_LOG_H
#define JSONL_LOG_H

// JSONL metrics snapshot log shared by LoadGraphGUI and LoadGraphTTY.
// Enabled through "log_file": true in loadgraph.cfg: the app builds the
// path and calls JsonlLogSetPath() once at startup; the UI refresh code
// then calls JsonlLogSnapshot() at its own cadence (1 s timer in the GUI,
// 100 ms loop in the TTY) and the writer throttles itself to one line
// every 5 seconds. Format: one JSON object per line
// {"timestamp":"...","cpu":{...},"ram":{...},"gpus":[...]}.
// The first snapshot of a run rewrites the file, so it holds only the
// current session - same fresh-per-run behavior as loadgraph.log.
// Timestamps are local time WITHOUT any UTC designator.
// A single thread per process performs the writes (the GUI timer thread
// or the TTY loop thread), so no locking is required; the file is reopened
// on every snapshot to avoid lock issues when the user views it.

#include <string>
#include <vector>

// Metric structs, defined in lib_sensors/metrics_api.h.
struct CpuData;
struct RamData;
struct GpuData;

// Enables the log (empty path disables it). Call once at startup before
// the periodic JsonlLogSnapshot() calls; resets the per-run state.
void JsonlLogSetPath(const std::string& path);

// Records one metrics snapshot: writes only when the log is enabled and
// at least 5 seconds have passed since the previous write attempt.
void JsonlLogSnapshot(const CpuData& cpu, const RamData& ram,
                      const std::vector<GpuData>& gpus);

#endif // JSONL_LOG_H
