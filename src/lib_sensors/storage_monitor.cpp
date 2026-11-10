#include "storage_monitor.h"
#include "logger.h"

#ifdef PLATFORM_WINDOWS
#include <windows.h>
#include <wbemidl.h>
#include <comdef.h>
#pragma comment(lib, "wbemuuid.lib")

#include <thread>
#include <mutex>
#include <atomic>

// ----------------------------------------------------------------------------
// Windows implementation — reads disk info via WMI.
// ----------------------------------------------------------------------------

namespace {

// Estimate power consumption based on device type and interface.
static double EstimatePowerWatts(const std::string& name, const std::string& iface)
{
    // HDD (spinning disk): ~5-10W depending on size/speed.
    if (iface.find("SATA") != std::string::npos ||
        iface.find("IDE")  != std::string::npos)
    {
        bool isSSD = (name.find("SSD") != std::string::npos ||
                      name.find("Solid") != std::string::npos);
        if (!isSSD) return 8.0;  // 3.5" HDD ~8W, 2.5" ~5W
        return 3.0;              // SATA SSD ~3W
    }
    // NVMe SSD: ~3-7W depending on generation/load.
    if (iface.find("NVMe") != std::string::npos ||
        iface.find("PCIe") != std::string::npos)
    {
        return 5.0;
    }
    // USB/external: ~2-5W.
    if (iface.find("USB") != std::string::npos)
    {
        return 3.0;
    }
    // Unknown — conservative estimate.
    return 5.0;
}

class WindowsStorageMonitor : public StorageMonitor
{
public:
    void Start() override
    {
        m_stop = false;
        m_thread = std::thread([this] {
            while (true)
            {
                Collect();
                // Wait up to 5s for Stop() to signal the condition variable.
                std::unique_lock<std::mutex> lock(m_mutex);
                m_cv.wait_for(lock, std::chrono::milliseconds(5000), [this] {
                    return m_stop;
                });
                if (m_stop) break;
            }
        });
    }

    void Stop() override
    {
        StorageMonitor::Stop();
    }

protected:
    void Collect() override
    {
        std::vector<StorageData> result;
        IWbemLocator* pLoc = nullptr;
        IWbemServices* pSvc = nullptr;

        HRESULT hrInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        bool comInit = SUCCEEDED(hrInit);

        // CoInitializeSecurity: best-effort. It can be called only once per process;
        // if cpu_monitor already initialized it (RPC_E_TOO_LATE), proceed anyway —
        // local WMI queries work without explicit security setup on the same machine.
        HRESULT hrSec = RPC_E_TOO_LATE;
        if (comInit)
        {
            hrSec = CoInitializeSecurity(NULL, -1, NULL, NULL,
                RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE,
                NULL, EOAC_NONE, NULL);
            if (FAILED(hrSec) && hrSec != RPC_E_TOO_LATE)
            {
                // Real failure — not "already initialized". Clean up and return empty.
                CoUninitialize();
                std::lock_guard<std::mutex> lock(m_mutex);
                m_metrics = std::move(result);
                return;
            }
        }

        if (SUCCEEDED(CoCreateInstance(CLSID_WbemLocator, nullptr,
                CLSCTX_INPROC_SERVER, IID_IWbemLocator, (LPVOID*)&pLoc)))
        {
            if (SUCCEEDED(pLoc->ConnectServer(
                    BSTR(L"ROOT\\cimv2"), NULL, NULL, NULL,
                    0, NULL, NULL, &pSvc)))
            {
                CoSetProxyBlanket(pSvc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE,
                    NULL, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE,
                    NULL, EOAC_NONE);

                BSTR query = SysAllocString(
                    L"SELECT Name, InterfaceType, Size FROM Win32_DiskDrive");
                if (query)
                {
                    IEnumWbemClassObject* pEnumerator = nullptr;
                    if (SUCCEEDED(pSvc->ExecQuery(
                            BSTR(L"WQL"), query,
                            WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                            NULL, &pEnumerator)))
                    {
                        IWbemClassObject* pObj = nullptr;
                        ULONG uCount = 0;
                        while (pEnumerator)
                        {
                            HRESULT hr = pEnumerator->Next(
                                WBEM_INFINITE, 1, &pObj, &uCount);
                            if (!SUCCEEDED(hr) || uCount == 0) break;

                            StorageData d{};
                            VARIANT vt;
                            VariantInit(&vt);

                            if (SUCCEEDED(pObj->Get(L"Name", 0, &vt, NULL, NULL))
                                && vt.bstrVal != NULL)
                                d.name = _bstr_t(vt.bstrVal);
                            VariantClear(&vt);

                            if (SUCCEEDED(pObj->Get(L"InterfaceType", 0, &vt, NULL, NULL))
                                && vt.bstrVal != NULL)
                                d.iface = _bstr_t(vt.bstrVal);
                            VariantClear(&vt);

                            if (SUCCEEDED(pObj->Get(L"Size", 0, &vt, NULL, NULL))
                                && vt.bstrVal != NULL)
                            {
                                double sizeDb = _wtof(vt.bstrVal);
                                d.totalBytes = static_cast<uint64_t>(sizeDb);
                            }
                            VariantClear(&vt);

                            d.powerWatts = EstimatePowerWatts(d.name, d.iface);
                            result.push_back(d);
                            pObj->Release();
                        }
                        if (pEnumerator) pEnumerator->Release();
                    }
                    SysFreeString(query);
                }
            }
            pLoc->Release();
        }

        if (comInit) CoUninitialize();

        std::lock_guard<std::mutex> lock(m_mutex);
        m_metrics = std::move(result);
    }
};

} // namespace

// Create platform-specific instance.
StorageMonitor* CreateStorageMonitor()
{
    return new WindowsStorageMonitor();
}

#else
// Linux/other platforms — placeholder.
StorageMonitor* CreateStorageMonitor()
{
    return nullptr;
}
#endif


