/* a3d.dll: the A3D 1.x object (CLSID_A3d). It is a DirectSound object that
 * also answers to IA3d / IA3d2. Positional audio goes through the normal
 * DirectSound 3D interfaces of the dsound.dll underneath (DSOAL), which is
 * how A3D 1.x applications used it. */
#include "a3d_decl.h"
#include "common.h"
#include "comreg.h"

#include <atomic>

namespace {

HMODULE gModule{nullptr};
std::atomic<LONG> gObjects{0};

class A3dDS final : public IDirectSound {
    std::atomic<LONG> mRef{1};
    IDirectSound *mInner{nullptr};

    struct Face final : IA3d2X {
        A3dDS *self{nullptr};
        DWORD front{OUTPUT_HEADPHONES_}, rear{OUTPUT_HEADPHONES_}, quad{1};
        DWORD rm{0}, version{0};
        float hf{1.0f};
        static constexpr DWORD OUTPUT_HEADPHONES_{1};

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID r, void **o) override { return self->QueryInterface(r, o); }
        ULONG STDMETHODCALLTYPE AddRef() override { return self->AddRef(); }
        ULONG STDMETHODCALLTYPE Release() override { return self->Release(); }
        HRESULT STDMETHODCALLTYPE SetOutputMode(DWORD f, DWORD r, DWORD q) override
        { front = f; rear = r; quad = q; a3d::logf("A3D1 SetOutputMode(%lu, %lu, %lu)", f, r, q); return S_OK; }
        HRESULT STDMETHODCALLTYPE GetOutputMode(DWORD *f, DWORD *r, DWORD *q) override
        { if(f) *f = front; if(r) *r = rear; if(q) *q = quad; return S_OK; }
        HRESULT STDMETHODCALLTYPE SetResourceManagerMode(DWORD m) override { rm = m; return S_OK; }
        HRESULT STDMETHODCALLTYPE GetResourceManagerMode(DWORD *m) override
        { if(!m) return E_INVALIDARG; *m = rm; return S_OK; }
        HRESULT STDMETHODCALLTYPE SetHFAbsorbFactor(float f) override { hf = f; return S_OK; }
        HRESULT STDMETHODCALLTYPE GetHFAbsorbFactor(float *f) override
        { if(!f) return E_INVALIDARG; *f = hf; return S_OK; }
        HRESULT STDMETHODCALLTYPE RegisterVersion(DWORD v) override { version = v; return S_OK; }
        HRESULT STDMETHODCALLTYPE GetSoftwareCaps(void *c) override
        {
            if(!c) return E_INVALIDARG;
            const DWORD size{*static_cast<DWORD*>(c)};
            A3DCaps2Software caps{};
            caps.dwSize = size;
            caps.dwVersion = 12;
            caps.dwOutputChannels = 2;
            caps.dwMinSampleRate = 4000;
            caps.dwMaxSampleRate = 48000;
            caps.dwMax2DBuffers = 64;
            caps.dwMax3DBuffers = 64;
            std::memcpy(c, &caps, std::min<size_t>(size ? size : sizeof(A3DCaps1Software), sizeof(caps)));
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE GetHardwareCaps(void *c) override
        {
            if(!c) return E_INVALIDARG;
            const DWORD size{*static_cast<DWORD*>(c)};
            A3DCaps2Hardware caps{};
            caps.dwSize = size;
            caps.dwFlags = A3DF_DIRECT_PATH_A3D;
            caps.dwOutputChannels = 2;
            caps.dwMinSampleRate = 4000;
            caps.dwMaxSampleRate = 48000;
            caps.dwMax2DBuffers = 64;
            caps.dwMax3DBuffers = 64;
            std::memcpy(c, &caps, std::min<size_t>(size ? size : sizeof(A3DCaps1Hardware), sizeof(caps)));
            return S_OK;
        }
    } mFace;

public:
    A3dDS() { mFace.self = this; ++gObjects; }
    ~A3dDS() { if(mInner) mInner->Release(); --gObjects; a3d::restoreSessionVolume(); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override
    {
        if(!out) return E_POINTER;
        if(riid == IID_IUnknown || riid == IID_IDirectSound)
        {
            AddRef();
            *out = static_cast<IDirectSound*>(this);
            return S_OK;
        }
        if(riid == IID_IA3d_ || riid == IID_IA3d2_)
        {
            AddRef();
            *out = static_cast<IA3d2X*>(&mFace);
            return S_OK;
        }
        if(mInner)
            return mInner->QueryInterface(riid, out);
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ULONG(++mRef); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const LONG r{--mRef};
        if(r == 0) delete this;
        return ULONG(r);
    }

    HRESULT STDMETHODCALLTYPE CreateSoundBuffer(LPCDSBUFFERDESC d, LPDIRECTSOUNDBUFFER *b, LPUNKNOWN o) override
    { return mInner ? mInner->CreateSoundBuffer(d, b, o) : DSERR_UNINITIALIZED; }
    HRESULT STDMETHODCALLTYPE GetCaps(LPDSCAPS c) override
    { return mInner ? mInner->GetCaps(c) : DSERR_UNINITIALIZED; }
    HRESULT STDMETHODCALLTYPE DuplicateSoundBuffer(LPDIRECTSOUNDBUFFER s, LPDIRECTSOUNDBUFFER *d) override
    { return mInner ? mInner->DuplicateSoundBuffer(s, d) : DSERR_UNINITIALIZED; }
    HRESULT STDMETHODCALLTYPE SetCooperativeLevel(HWND h, DWORD l) override
    { return mInner ? mInner->SetCooperativeLevel(h, l) : DSERR_UNINITIALIZED; }
    HRESULT STDMETHODCALLTYPE Compact() override
    { return mInner ? mInner->Compact() : DSERR_UNINITIALIZED; }
    HRESULT STDMETHODCALLTYPE GetSpeakerConfig(LPDWORD c) override
    { return mInner ? mInner->GetSpeakerConfig(c) : DSERR_UNINITIALIZED; }
    HRESULT STDMETHODCALLTYPE SetSpeakerConfig(DWORD c) override
    { return mInner ? mInner->SetSpeakerConfig(c) : DSERR_UNINITIALIZED; }
    HRESULT STDMETHODCALLTYPE Initialize(LPCGUID g) override
    {
        if(mInner) return DSERR_ALREADYINITIALIZED;
        const HRESULT hr{a3d::createDirectSound(g, &mInner)};
        a3d::logf("A3D1 Initialize: %08lx", hr);
        return hr;
    }
};

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
        auto *o = new A3dDS{};
        const HRESULT hr{o->QueryInterface(riid, out)};
        o->Release();
        return hr;
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override
    {
        if(lock) ++gObjects; else --gObjects;
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
    if(clsid != CLSID_A3d_)
        return CLASS_E_CLASSNOTAVAILABLE;
    return gFactory.QueryInterface(riid, out);
}

HRESULT WINAPI DllCanUnloadNow()
{
    return gObjects == 0 ? S_OK : S_FALSE;
}

HRESULT WINAPI DllRegisterServer()
{
    return a3d::registerClass(gModule, "{D8F1EEE0-F634-11CF-8700-00A0245D918B}", "A3d",
        "A3d Object");
}

HRESULT WINAPI DllUnregisterServer()
{
    return a3d::unregisterClass("{D8F1EEE0-F634-11CF-8700-00A0245D918B}", "A3d");
}

/* Exported under its C++ name in the original DLL (see a3d.def). */
HRESULT WINAPI A3dCreateImpl(GUID *guid, IDirectSound **out, IUnknown *outer)
{
    if(!out) return E_POINTER;
    *out = nullptr;
    if(outer) return CLASS_E_NOAGGREGATION;
    auto *o = new A3dDS{};
    const HRESULT hr{o->Initialize(guid)};
    if(FAILED(hr))
    {
        o->Release();
        return hr;
    }
    *out = o;
    return S_OK;
}

} // extern "C"
