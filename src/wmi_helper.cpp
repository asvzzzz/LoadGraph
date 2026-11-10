// WMI helper for CPU temperature reading in the console app.
// Separate TU so windows.h + wbemidl.h are processed first without conflicts.
#include "platform.h"
#ifdef PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#undef WIN32_LEAN_AND_MEAN
#include <objbase.h>
#include <oleauto.h>
#include <wbemidl.h>
#pragma comment(lib, "wbemuuid.lib")

// Runs a single WQL query and returns the first numeric property value.
// Returns true + fills outValue on success; false on any failure.
// Wrapped in __try/__except because fastprox.dll may crash on some systems.
bool QueryWmiValueRaw(const wchar_t* ns, const wchar_t* query,
                      const wchar_t* prop, double& outValue)
{
    outValue = -1.0;

    struct Context {
        IWbemLocator*            pLoc  = nullptr;
        IWbemServices*           pSvc  = nullptr;
        IEnumWbemClassObject*    pEnum = nullptr;
        IWbemClassObject*        pObj  = nullptr;
        bool                     ok    = false;
        bool                     comInit = false;
    };

    Context* ctx = new (std::nothrow) Context();
    if (!ctx) return false;

    __try
    {
        HRESULT hres = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ctx->comInit = SUCCEEDED(hres);
        CoInitializeSecurity(nullptr, -1, nullptr, nullptr,
                             RPC_C_AUTHN_LEVEL_DEFAULT,
                             RPC_C_IMP_LEVEL_IMPERSONATE,
                             nullptr, EOAC_NONE, nullptr);

        hres = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IWbemLocator,
                                reinterpret_cast<LPVOID*>(&ctx->pLoc));
        if (SUCCEEDED(hres) && ctx->pLoc)
        {
            BSTR bNs = SysAllocString(ns);
            hres = ctx->pLoc->ConnectServer(bNs, nullptr, nullptr, nullptr, 0,
                                           nullptr, nullptr, &ctx->pSvc);
            SysFreeString(bNs);
            if (SUCCEEDED(hres) && ctx->pSvc)
            {
                CoSetProxyBlanket(ctx->pSvc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE,
                                  nullptr, RPC_C_AUTHN_LEVEL_CALL,
                                  RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
                BSTR bQ = SysAllocString(query);
                hres = ctx->pSvc->ExecQuery(BSTR(L"WQL"), bQ,
                                            WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                                            nullptr, &ctx->pEnum);
                SysFreeString(bQ);
                if (SUCCEEDED(hres) && ctx->pEnum)
                {
                    ULONG returned = 0;
                    if (ctx->pEnum->Next(WBEM_INFINITE, 1, &ctx->pObj, &returned) == S_OK
                        && returned > 0 && ctx->pObj)
                    {
                        VARIANT vt; VariantInit(&vt);
                        BSTR bProp = SysAllocString(prop);
                        hres = ctx->pObj->Get(bProp, 0, &vt, nullptr, nullptr);
                        SysFreeString(bProp);
                        if (SUCCEEDED(hres))
                        {
                            if (vt.vt == VT_I4)         { outValue = static_cast<double>(vt.lVal);    ctx->ok = true; }
                            else if (vt.vt == VT_R8)     { outValue = vt.dblVal;                       ctx->ok = true; }
                            else if (vt.vt == VT_UI4)    { outValue = static_cast<double>(vt.ulVal);   ctx->ok = true; }
                        }
                        VariantClear(&vt);
                    }
                }
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        ctx->ok = false;
        outValue = -1.0;
    }
    delete ctx;
    return ctx && ctx->ok;
}
#endif // PLATFORM_WINDOWS
