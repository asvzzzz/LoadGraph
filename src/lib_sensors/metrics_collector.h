#ifndef METRICS_COLLECTOR_H
#define METRICS_COLLECTOR_H

#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <vector>

// Base abstract class for all metric collectors.
// Handles polling thread lifecycle and synchronization.
class MetricsCollector
{
public:
    virtual ~MetricsCollector() = default;

    // Start background polling at 1000ms intervals.
    void Start();

    // Stop polling thread gracefully.
    void Stop();

    // Check if collector is currently running.
    bool IsRunning() const;

    // Force a synchronous collection (blocks until done).
    // Must be called only before Start() or after Stop() — calling it while
    // the polling thread is running races on collector-specific state (PDH handles,
    // /proc/stat deltas, NVML device handles).
    void CollectNow() { Collect(); }

protected:
    MetricsCollector() = default;

    std::atomic<bool> m_stopFlag{false};
    std::thread       m_thread;
    std::mutex        m_mutex;

    // Wakes the polling thread immediately on Stop() instead of waiting for
    // the current 1000 ms sleep to finish (keeps shutdown fast).
    std::mutex        m_wakeMutex;
    std::condition_variable m_wakeCv;

    // Derived classes must implement this to collect metrics.
    virtual void Collect() = 0;

private:
    void PollLoop();
};

#endif // METRICS_COLLECTOR_H
