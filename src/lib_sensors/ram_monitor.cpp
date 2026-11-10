#include "ram_monitor.h"
#include <windows.h>
#include <wbemidl.h>
#include <comdef.h>

#pragma comment(lib, "wbemuuid.lib")

RamMonitor::RamMonitor()
{
}

RamMetrics RamMonitor::GetMetrics()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_metrics;
}

void RamMonitor::Collect()
{
    RamMetrics fresh{};
    QueryMemoryStatus(fresh);
    // RAM speed query via WMI is disabled on this system
    // due to a crash in fastprox.dll. Value remains at default (-1).
    fresh.speedMhz = -1;

    // Estimate RAM power: ~4W per DIMM slot (DDR4) or ~5W (DDR5).
    // Approximate number of modules from total capacity (common sizes: 4, 8, 16 GB).
    if (fresh.totalBytes > 0)
    {
        int dimms = static_cast<int>(fresh.totalBytes / (8ULL * 1024 * 1024 * 1024));
        if (dimms < 1) dimms = 1;
        else if (dimms > 8) dimms = 8;
        fresh.powerWatts = static_cast<double>(dimms) * 4.0;
    }

    // Publish atomically: GetMetrics() reads m_metrics from the GUI thread.
    std::lock_guard<std::mutex> lock(m_mutex);
    m_metrics = fresh;
}

// QUERY MEMORY STATUS USING GLOBALMEMORYSTATUSEX
bool RamMonitor::QueryMemoryStatus(RamMetrics& metrics)
{
    MEMORYSTATUSEX statex{};
    statex.dwLength = sizeof(statex);

    if (!GlobalMemoryStatusEx(&statex))
        return false;

    metrics.totalBytes = static_cast<ULONGLONG>(statex.ullTotalPhys);
    metrics.freeBytes  = static_cast<ULONGLONG>(statex.ullAvailPhys);
    metrics.loadPercent = static_cast<double>(statex.dwMemoryLoad);
    return true;
}

// QUERY RAM SPEED VIA WMI (Win32_PhysicalMemory.Speed)
bool RamMonitor::QueryRamSpeed(int& speedMhz)
{
    speedMhz = -1;

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool fNeedUninit = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;

    IWbemLocator* pLoc = nullptr;
    hr = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
                          IID_IWbemLocator, reinterpret_cast<void**>(&pLoc));
    if (FAILED(hr)) { if (fNeedUninit) CoUninitialize(); return false; }

    IWbemServices* pSvc = nullptr;
    hr = pLoc->ConnectServer(_bstr_t(L"ROOT\\CIMV2"), nullptr, nullptr, 0,
                             NULL, NULL, NULL, &pSvc);
    pLoc->Release();
    if (FAILED(hr)) { if (fNeedUninit) CoUninitialize(); return false; }

    IEnumWbemClassObject* pEnumerator = nullptr;
    hr = pSvc->ExecQuery(_bstr_t(L"WQL"),
                         _bstr_t(L"SELECT Speed FROM Win32_PhysicalMemory"),
                         WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                         nullptr, &pEnumerator);
    pSvc->Release();

    if (fNeedUninit) CoUninitialize();
    if (FAILED(hr))
        return false;

    ULONG   retLen = 0;
    IWbemClassObject* pObj = nullptr;

    while (hr = pEnumerator->Next(WBEM_INFINITE, 1, &pObj, &retLen),
           hr == S_OK && pObj)
    {
        VARIANT vt;
        VariantInit(&vt);
        if (SUCCEEDED(pObj->Get(L"Speed", 0, &vt, nullptr, nullptr)))
        {
            if (vt.vt == VT_I4 && speedMhz == -1)
                speedMhz = vt.lVal;
            VariantClear(&vt);
        }
        pObj->Release();

        if (speedMhz != -1)
            break;
    }

    pEnumerator->Release();
    return true;
}
