#ifndef STORAGE_MONITOR_H
#define STORAGE_MONITOR_H

#include "metrics_api.h"
#include <vector>
#include <string>
#include <mutex>
#include <thread>
#include <atomic>

// Collects storage device metrics (name, size, estimated power).
class StorageMonitor
{
public:
    StorageMonitor() = default;
    virtual ~StorageMonitor() { Stop(); }

    // Get current storage metrics (thread-safe copy).
    std::vector<StorageData> GetMetrics()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_metrics;
    }

    // Start background polling at 5000ms intervals.
    virtual void Start() = 0;

    // Stop polling thread gracefully.
    virtual void Stop()
    {
        m_stop = true;
        if (m_thread.joinable()) m_thread.join();
    }

protected:
    virtual void Collect() = 0;

private:
    std::vector<StorageData> m_metrics;
    std::atomic<bool> m_stop{false};
    std::thread m_thread;
    std::mutex m_mutex;
};

// Platform-specific factory.
StorageMonitor* CreateStorageMonitor();

#endif // STORAGE_MONITOR_H
