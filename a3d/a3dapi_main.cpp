/* a3dapi.dll: the A3D 2.0 COM server (CLSID_A3dApi). */
#include "engine.h"
#include "comreg.h"

namespace {

HMODULE gModule{nullptr};

class Factory final : public IClassFactory {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override
    {
        if(!out) return E_POINTER;
        if(riid == IID_IUnknown || riid == IID_IClassFactory)
        {
            *out = static_cast<IClassFactory*>(this);
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown *outer, REFIID riid, void **out) override
    {
        if(!out) return E_POINTER;
        *out = nullptr;
        if(outer) return CLASS_E_NOAGGREGATION;
        auto *e = new a3d::Engine{};
        const HRESULT hr{e->QueryInterface(riid, out)};
        e->Release();
        a3d::logf("A3dApi object created (hr %08lx)", hr);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override
    {
        if(lock) ++a3d::gObjects; else --a3d::gObjects;
        return S_OK;
    }
};

Factory gFactory;

} // namespace

extern "C" {

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void*)
{
    if(reason == DLL_PROCESS_ATTACH)
    {
        gModule = inst;
        DisableThreadLibraryCalls(inst);
    }
    return TRUE;
}

HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void **out)
{
    if(clsid != CLSID_A3dApi_)
        return CLASS_E_CLASSNOTAVAILABLE;
    return gFactory.QueryInterface(riid, out);
}

HRESULT WINAPI DllCanUnloadNow()
{
    return a3d::gObjects == 0 ? S_OK : S_FALSE;
}

HRESULT WINAPI DllRegisterServer()
{
    return a3d::registerClass(gModule, "{92FA2C24-253C-11D2-90FB-006008A1F441}",
        "A3dApi", "A3dApi Object");
}

HRESULT WINAPI DllUnregisterServer()
{
    return a3d::unregisterClass("{92FA2C24-253C-11D2-90FB-006008A1F441}", "A3dApi");
}

} // extern "C"
