#ifndef RAM_MONITOR_H
#define RAM_MONITOR_H

#include "metrics_collector.h"
#include <windows.h>
#include <string>

struct RamMetrics
{
    ULONGLONG totalBytes   = 0;
    ULONGLONG freeBytes    = 0;
    double    loadPercent  = 0.0;
    int       speedMhz     = -1;
};

class RamMonitor : public MetricsCollector
{
public:
    RamMonitor();
    ~RamMonitor() override = default;
    RamMetrics GetMetrics();
protected:
    void Collect() override;
private:
    bool QueryMemoryStatus(RamMetrics& metrics);
    bool QueryRamSpeed(int& speedMhz);
    RamMetrics m_metrics;
};

#endif // RAM_MONITOR_H
