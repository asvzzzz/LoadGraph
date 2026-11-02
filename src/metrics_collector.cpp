#include "metrics_collector.h"
#include <chrono>
#include <windows.h>

void MetricsCollector::PollLoop()
{
    while (!m_stopFlag.load(std::memory_order_relaxed))
    {
        // Collect metrics (implemented by derived classes).
        Collect();

        // Sleep up to 1000 ms; Stop() notifies the condition variable so the
        // thread wakes immediately instead of waiting out the full interval.
        std::unique_lock<std::mutex> lock(m_wakeMutex);
        m_wakeCv.wait_for(lock, std::chrono::milliseconds(1000), [this] {
            return m_stopFlag.load(std::memory_order_relaxed);
        });
    }
}

void MetricsCollector::Start()
{
    if (m_thread.joinable())
        return; // Already running.

    m_stopFlag.store(false, std::memory_order_relaxed);
    m_thread = std::thread(&MetricsCollector::PollLoop, this);
}

void MetricsCollector::Stop()
{
    {
        std::lock_guard<std::mutex> lock(m_wakeMutex);
        m_stopFlag.store(true, std::memory_order_release);
    }
    m_wakeCv.notify_all();
    if (m_thread.joinable())
        m_thread.join();
}

bool MetricsCollector::IsRunning() const
{
    return m_thread.joinable();
}

// Derived classes must implement this method.
void MetricsCollector::Collect()
{
    // Abstract - implemented by CPU/RAM/GPU subclasses.
}
