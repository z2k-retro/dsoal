/* IA3d4 / IA3dGeom / IA3dListener and the per-frame renderer. */
#include "engine.h"

#include <ks.h>

namespace a3d {

std::atomic<LONG> gObjects{0};

namespace {

using Guard = std::lock_guard<std::recursive_mutex>;
constexpr float SpeedOfSound{343.0f};

template<typename T> void safeRelease(T *&p) { if(p) { p->Release(); p = nullptr; } }

/* Is X (on the polygon's plane) inside the convex polygon? */
bool insidePoly(const V3 &X, const Poly &p)
{
    float scale{0.0f};
    for(int i{0};i < p.count;++i)
        scale = std::max(scale, length(p.v[i] - p.v[0]));
    const float eps{1e-5f * scale * scale};
    bool pos{false}, neg{false};
    for(int i{0};i < p.count;++i)
    {
        const V3 &a{p.v[i]}, &b{p.v[(i+1) % p.count]};
        const float c{dot(cross(b - a, X - a), p.normal)};
        if(c > eps) pos = true;
        else if(c < -eps) neg = true;
        if(pos && neg) return false;
    }
    return true;
}

/* Does the segment A-B cross the polygon? Fills in the crossing point. */
bool segmentHits(const V3 &A, const V3 &B, const Poly &p, V3 &X)
{
    const float da{dot(A - p.v[0], p.normal)};
    const float db{dot(B - p.v[0], p.normal)};
    if(!(da*db < 0.0f)) return false;
    const float t{da / (da - db)};
    X = A + (B - A)*t;
    return insidePoly(X, p);
}

float distanceGain(const Source *s, float d, float globalScale)
{
    if(d > s->maxDist)
    {
        if(s->maxMode == A3DMAX_MUTE) return 0.0f;
        d = s->maxDist;
    }
    if(d <= s->minDist) return 1.0f;
    const float r{(d - s->minDist) * s->dms * globalScale + s->minDist};
    return (r > 0.0f) ? std::min(1.0f, s->minDist / r) : 1.0f;
}

/* The Vortex 2 driver turns every treble control (SetEq, distance, a
 * wall's high-frequency transmittance) into one "brightness" value h, the
 * product of the three, and programs the source's two-pole low-pass
 * (b0 = (1-p)^2, a1 = 2p, a2 = -p^2) from it. Table: the pole p the
 * driver wrote for SetEq(h) in a traced test run. Distance contributes
 * h = exp(-0.037 per metre beyond the minimum distance), which matches
 * the poles written for 2..64 m. */
float poleFromBrightness(float h)
{
    static constexpr float H[]{0.0f, 0.1f, 0.3f, 0.5f, 0.75f, 0.9f, 1.0f};
    static constexpr float P[]{0.995f, 0.8755f, 0.8005f, 0.728f, 0.559f, 0.3455f, 0.0f};
    h = std::clamp(h, 0.0f, 1.0f);
    for(int i{1};i < 7;++i)
        if(h <= H[i])
            return P[i-1] + (P[i] - P[i-1]) * (h - H[i-1]) / (H[i] - H[i-1]);
    return 0.0f;
}

/* That filter's gain at 5 kHz (48 kHz rate), which is where EAX's DirectHF
 * (and OpenAL Soft's low-pass) is specified. */
float brightnessToHF(float h)
{
    const float p{poleFromBrightness(h)};
    const float c{std::cos(2.0f * 3.14159265f * 5000.0f / 48000.0f)};
    return (1.0f - p)*(1.0f - p) / (1.0f - 2.0f*p*c + p*p);
}

float distanceBrightness(float d, float minDist, float factor)
{
    return std::exp(-settings().hfAbsorbPerMeter * factor * std::max(0.0f, d - minDist));
}

} // namespace

Engine::Engine()
{
    ++gObjects;
}

Engine::~Engine()
{
    shutdown();
    --gObjects;
    restoreSessionVolume();
}

void Engine::shutdown()
{
    quit = true;
    if(worker.joinable() && worker.get_id() != std::this_thread::get_id())
        worker.join();
    Guard _{lock};
    for(Source *s : sources)
    {
        s->stopReflections();
        s->releaseBuffer();
    }
    safeRelease(reverb);
    appliedReverb = nullptr;
    safeRelease(fxPs);
    safeRelease(fxBuf);
    safeRelease(dsListener);
    safeRelease(primary);
    safeRelease(ds);
    inited = false;
}

HRESULT STDMETHODCALLTYPE Engine::QueryInterface(REFIID riid, void **out)
{
    if(!out) return E_POINTER;
    if(riid == IID_IUnknown || riid == IID_IA3d5_ || riid == IID_IA3d4_ || riid == IID_IA3d3_
        || riid == IID_IA3d2_ || riid == IID_IA3d_)
    {
        AddRef();
        *out = static_cast<IA3d5X*>(this);
        return S_OK;
    }
    if(riid == IID_IA3dGeom_ || riid == IID_IA3dGeom2_)
    {
        AddRef();
        *out = static_cast<IA3dGeom2X*>(this);
        return S_OK;
    }
    if(riid == IID_IA3dListener_)
    {
        AddRef();
        *out = static_cast<IA3dListenerX*>(this);
        return S_OK;
    }
    /* Undocumented: Aureal's a3dapi.dll answers this with the DirectSound
       object underneath it. Half-Life and Quake 3 ask for it and make their
       own buffers on the result, without checking for failure. */
    static const GUID IID_A3dDirectSound{0x62507921, 0x3ea6, 0x11d2,
        {0x90, 0xfb, 0x00, 0x60, 0x08, 0xa1, 0xf4, 0x41}};
    if((riid == IID_IDirectSound || riid == IID_A3dDirectSound) && ds)
    {
        ds->AddRef();
        *out = ds;
        return S_OK;
    }
    char g[64]{};
    snprintf(g, sizeof(g), "%08lx-%04x-%04x", riid.Data1, riid.Data2, riid.Data3);
    logf("IA3d5::QueryInterface: unsupported %s", g);
    *out = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE Engine::Release()
{
    const LONG r{--mRef};
    if(r == 0) delete this;
    return ULONG(r);
}

/* ------------------------------------------------------------------ IA3d */

HRESULT STDMETHODCALLTYPE Engine::SetOutputMode(DWORD f, DWORD r, DWORD q)
{
    outFront = f; outRear = r; outQuad = q;
    logf("SetOutputMode(%lu, %lu, %lu)", f, r, q);
    return S_OK;
}
HRESULT STDMETHODCALLTYPE Engine::GetOutputMode(DWORD *f, DWORD *r, DWORD *q)
{
    if(f) *f = outFront;
    if(r) *r = outRear;
    if(q) *q = outQuad;
    return S_OK;
}
HRESULT STDMETHODCALLTYPE Engine::SetResourceManagerMode(DWORD m) { rmMode = m; return S_OK; }
HRESULT STDMETHODCALLTYPE Engine::GetResourceManagerMode(DWORD *m)
{ if(!m) return E_INVALIDARG; *m = rmMode; return S_OK; }
HRESULT STDMETHODCALLTYPE Engine::SetHFAbsorbFactor(float f)
{ hfAbsorb = std::max(0.0f, f); return S_OK; }
HRESULT STDMETHODCALLTYPE Engine::GetHFAbsorbFactor(float *f)
{ if(!f) return E_INVALIDARG; *f = hfAbsorb; return S_OK; }
HRESULT STDMETHODCALLTYPE Engine::RegisterVersion(DWORD v) { version = v; return S_OK; }

HRESULT STDMETHODCALLTYPE Engine::GetSoftwareCaps(void *c)
{
    if(!c) return E_INVALIDARG;
    const DWORD size{*static_cast<DWORD*>(c)};
    A3DCaps2Software caps{};
    caps.dwSize = size;
    caps.dwVersion = 20;
    caps.dwFlags = A3DF_DIRECT_PATH_A3D | A3DF_OCCLUSIONS | A3DF_1ST_REFLECTIONS;
    caps.dwOutputChannels = 2;
    caps.dwMinSampleRate = 4000;
    caps.dwMaxSampleRate = 48000;
    caps.dwMax2DBuffers = 64;
    caps.dwMax3DBuffers = 64;
    std::memcpy(c, &caps, std::min<size_t>(size ? size : sizeof(A3DCaps1Software), sizeof(caps)));
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Engine::GetHardwareCaps(void *c)
{
    if(!c) return E_INVALIDARG;
    const DWORD size{*static_cast<DWORD*>(c)};
    A3DCaps2Hardware caps{};
    caps.dwSize = size;
    caps.dwFlags = A3DF_DIRECT_PATH_A3D | A3DF_OCCLUSIONS | A3DF_1ST_REFLECTIONS;
    caps.dwOutputChannels = 2;
    caps.dwMinSampleRate = 4000;
    caps.dwMaxSampleRate = 48000;
    caps.dwMax2DBuffers = 64;
    caps.dwMax3DBuffers = 64;
    std::memcpy(c, &caps, std::min<size_t>(size ? size : sizeof(A3DCaps1Hardware), sizeof(caps)));
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Engine::Clear()
{
    Guard _{lock};
    back.clear();
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Engine::Flush()
{
    Guard _{lock};
    flushFrame(true);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Engine::Init(GUID *dev, DWORD features, DWORD prefs)
{
    Guard _{lock};
    if(inited) return S_OK;
    logf("Init(features %#lx, prefs %#lx)", features, prefs);

    if(FAILED(createDirectSound(dev, &ds)))
    {
        logf("DirectSoundCreate failed");
        return A3DERR_FAILED_INIT;
    }
    HWND hwnd{GetForegroundWindow()};
    if(!hwnd) hwnd = GetDesktopWindow();
    ds->SetCooperativeLevel(hwnd, DSSCL_PRIORITY);

    DSBUFFERDESC desc{};
    desc.dwSize = sizeof(desc);
    desc.dwFlags = DSBCAPS_PRIMARYBUFFER | DSBCAPS_CTRL3D;
    if(SUCCEEDED(ds->CreateSoundBuffer(&desc, &primary, nullptr)))
    {
        WAVEFORMATEX wf{};
        wf.wFormatTag = WAVE_FORMAT_PCM;
        wf.nChannels = 2;
        wf.nSamplesPerSec = 48000;
        wf.wBitsPerSample = 16;
        wf.nBlockAlign = 4;
        wf.nAvgBytesPerSec = 48000 * 4;
        primary->SetFormat(&wf);
        if(SUCCEEDED(primary->QueryInterface(IID_IDirectSound3DListener,
            reinterpret_cast<void**>(&dsListener))))
        {
            /* Distance and Doppler are worked out here, so the DirectSound
             * side only places sources by direction. */
            dsListener->SetRolloffFactor(0.0f, DS3D_IMMEDIATE);
            dsListener->SetDopplerFactor(0.0f, DS3D_IMMEDIATE);
            dsListener->SetPosition(0, 0, 0, DS3D_IMMEDIATE);
            dsListener->SetOrientation(0, 0, 1, 0, 1, 0, DS3D_IMMEDIATE);
        }
    }
    if(!dsListener)
        logf("no 3D listener; positional sound will not work");

    available = features & (A3DF_OCCLUSIONS | A3DF_1ST_REFLECTIONS);
    requested = features;

    /* A3D 3.0 reverb runs on OpenAL Soft's EAX reverb. Listener EAX
     * properties go through a buffer's property set, so keep a silent one. */
    {
        WAVEFORMATEX wf{};
        wf.wFormatTag = WAVE_FORMAT_PCM;
        wf.nChannels = 1;
        wf.nSamplesPerSec = 22050;
        wf.wBitsPerSample = 16;
        wf.nBlockAlign = 2;
        wf.nAvgBytesPerSec = 44100;
        DSBUFFERDESC bd{};
        bd.dwSize = sizeof(bd);
        bd.dwFlags = DSBCAPS_CTRL3D | DSBCAPS_CTRLVOLUME;
        bd.dwBufferBytes = 4410;
        bd.lpwfxFormat = &wf;
        if(SUCCEEDED(ds->CreateSoundBuffer(&bd, &fxBuf, nullptr))
            && SUCCEEDED(fxBuf->QueryInterface(IID_IKsPropertySet, reinterpret_cast<void**>(&fxPs))))
        {
            constexpr GUID eax10listener{0x4a4e6fc1, 0xc341, 0x11d1,
                {0xb7, 0x3a, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
            ULONG support{0};
            if(FAILED(fxPs->QuerySupport(eax10listener, 0, &support)) || !(support & KSPROPERTY_SUPPORT_SET))
                safeRelease(fxPs);
        }
        if(fxPs)
            available |= features & A3DF_REVERB;
        else if(features & A3DF_REVERB)
            logf("reverb requested, but EAX is not available");
    }

    inited = true;
    lastFlushTick = GetTickCount();
    quit = false;
    worker = std::thread{[this]{ workerLoop(); }};
    return S_OK;
}

/* ----------------------------------------------------------------- IA3d5 */

HRESULT STDMETHODCALLTYPE Engine::InitEx(GUID *dev, DWORD features, DWORD prefs, HWND hwnd, DWORD coop)
{
    logf("InitEx(features %#lx, prefs %#lx, coop %lu)", features, prefs, coop);
    if(HRESULT hr{Init(dev, features, prefs)}; FAILED(hr))
        return hr;
    if(hwnd)
        SetCooperativeLevel(hwnd, coop);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Engine::NewReverb(IA3dReverbX **out)
{
    if(!out) return E_POINTER;
    if(!(requested & A3DF_REVERB)) return A3DERR_FEATURE_NOT_REQUESTED;
    *out = new Reverb{};
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Engine::BindReverb(IA3dReverbX *r)
{
    Guard _{lock};
    if(!(requested & A3DF_REVERB)) return A3DERR_FEATURE_NOT_REQUESTED;
    if(r && !(available & A3DF_REVERB)) return E_FAIL;
    auto *rv = static_cast<Reverb*>(r);
    if(rv) rv->AddRef();
    safeRelease(reverb);
    reverb = rv;
    logf("BindReverb(%s)", rv ? "reverb" : "none");
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Engine::GetStreamingProperties(DWORD *ms, DWORD *prio)
{
    if(!ms || !prio) return E_POINTER;
    *ms = streamMs;
    *prio = streamPrio;
    return S_OK;
}

/* Kept for the application to read back; sources are decoded whole when
 * loaded, so there is no streaming buffer to size. */
HRESULT STDMETHODCALLTYPE Engine::SetStreamingProperties(DWORD ms, DWORD prio)
{
    if(ms > 10000) return A3DERR_STREAMING_BUFFER_LENGTH;
    if(prio > A3DSTREAM_PRIORITY_HIGHEST) return A3DERR_STREAMING_PRIORITY;
    streamMs = ms;
    streamPrio = prio;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Engine::A3dEnumerate(A3DEnumCallback cb, void *ctx)
{
    if(!cb) return E_POINTER;
    cb(nullptr, L"A3D on DSOAL", L"a3dapi.dll", ctx);
    return S_OK;
}

/* AC-3 is not supported, so there is no decoder to unlock. */
HRESULT STDMETHODCALLTYPE Engine::UnlockFallbackAC3Decoder(char *, DWORD) { return S_OK; }

HRESULT STDMETHODCALLTYPE Engine::SetMaxHardwareSources(DWORD n) { maxHwSources = n; return S_OK; }
HRESULT STDMETHODCALLTYPE Engine::GetMaxHardwareSources(DWORD *n)
{ if(!n) return E_POINTER; *n = maxHwSources; return S_OK; }

HRESULT STDMETHODCALLTYPE Engine::IsFeatureAvailable(DWORD f)
{
    return (available & f) ? TRUE : FALSE;
}

HRESULT STDMETHODCALLTYPE Engine::NewSource(DWORD type, IA3dSourceX **out)
{
    if(!out) return E_INVALIDARG;
    *out = new Source{this, type};
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Engine::DuplicateSource(IA3dSourceX *orig, IA3dSourceX **out)
{
    if(!orig || !out) return E_INVALIDARG;
    Guard _{lock};
    auto *o = static_cast<Source*>(orig);
    if(!o->buf) return A3DERR_NO_WAVE_DATA;
    IDirectSoundBuffer *b{nullptr};
    if(!ds || FAILED(ds->DuplicateSoundBuffer(o->buf, &b)))
        return A3DERR_DUPLICATION;
    auto *s = new Source{this, o->type};
    s->fmt = o->fmt;
    s->haveFmt = true;
    s->pos = o->pos; s->vel = o->vel; s->front = o->front; s->up = o->up;
    std::copy(std::begin(o->angles), std::end(o->angles), s->angles);
    s->bound = o->bound; s->isBound = o->isBound;
    s->coneInner = o->coneInner; s->coneOuter = o->coneOuter; s->coneGain = o->coneGain;
    s->minDist = o->minDist; s->maxDist = o->maxDist; s->maxMode = o->maxMode;
    s->gain = o->gain; s->pitch = o->pitch; s->doppler = o->doppler; s->dms = o->dms;
    s->eq = o->eq; s->priority = o->priority; s->renderMode = o->renderMode;
    s->xform = o->xform; s->pan[0] = o->pan[0]; s->pan[1] = o->pan[1];
    s->reflDelayScale = o->reflDelayScale; s->reflGainScale = o->reflGainScale;
    std::copy(std::begin(o->volBounds), std::end(o->volBounds), s->volBounds);
    s->volDamp = o->volDamp;
    s->revLevel = o->revLevel; s->revHiFreq = o->revHiFreq; s->revBright = o->revBright;
    s->fileType = o->fileType; s->mp3Info = o->mp3Info;
    s->attachBuffer(b);
    s->appBytes = o->appBytes;
    s->mixdown = o->mixdown;
    s->dsFmt = o->dsFmt;
    s->shadow = o->shadow;
    *out = s;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Engine::SetCooperativeLevel(HWND hwnd, DWORD level)
{
    Guard _{lock};
    coop = level;
    if(!ds) return A3DERR_FAILED_INIT;
    ds->SetCooperativeLevel(hwnd, level == DSSCL_EXCLUSIVE ? DSSCL_EXCLUSIVE : DSSCL_PRIORITY);
    return S_OK;
}
HRESULT STDMETHODCALLTYPE Engine::GetCooperativeLevel(DWORD *level)
{ if(!level) return E_INVALIDARG; *level = coop; return S_OK; }

HRESULT STDMETHODCALLTYPE Engine::SetMaxReflectionDelayTime(A3DVAL s)
{ if(s < 0.0f) return E_INVALIDARG; maxReflDelay = s; return S_OK; }
HRESULT STDMETHODCALLTYPE Engine::GetMaxReflectionDelayTime(A3DVAL *s)
{ if(!s) return E_INVALIDARG; *s = maxReflDelay; return S_OK; }
HRESULT STDMETHODCALLTYPE Engine::SetCoordinateSystem(DWORD cs)
{
    Guard _{lock};
    coordSys = cs;
    lFront = {0, 0, leftHanded() ? 1.0f : -1.0f};
    return S_OK;
}
HRESULT STDMETHODCALLTYPE Engine::GetCoordinateSystem(DWORD *cs)
{ if(!cs) return E_INVALIDARG; *cs = coordSys; return S_OK; }
HRESULT STDMETHODCALLTYPE Engine::SetOutputGain(A3DVAL g)
{ outputGain = std::clamp(g, 0.0f, 1.0f); return S_OK; }
HRESULT STDMETHODCALLTYPE Engine::GetOutputGain(A3DVAL *g)
{ if(!g) return E_INVALIDARG; *g = outputGain; return S_OK; }
HRESULT STDMETHODCALLTYPE Engine::SetNumFallbackSources(DWORD n) { fallback = n; return S_OK; }
HRESULT STDMETHODCALLTYPE Engine::GetNumFallbackSources(DWORD *n)
{ if(!n) return E_INVALIDARG; *n = fallback; return S_OK; }
HRESULT STDMETHODCALLTYPE Engine::SetRMPriorityBias(A3DVAL b) { rmBias = b; return S_OK; }
HRESULT STDMETHODCALLTYPE Engine::GetRMPriorityBias(A3DVAL *b)
{ if(!b) return E_INVALIDARG; *b = rmBias; return S_OK; }
HRESULT STDMETHODCALLTYPE Engine::SetUnitsPerMeter(A3DVAL u)
{ if(!(u > 0.0f)) return E_INVALIDARG; unitsPerMeter = u; return S_OK; }
HRESULT STDMETHODCALLTYPE Engine::GetUnitsPerMeter(A3DVAL *u)
{ if(!u) return E_INVALIDARG; *u = unitsPerMeter; return S_OK; }
HRESULT STDMETHODCALLTYPE Engine::SetDopplerScale(A3DVAL s)
{ dopplerScale = std::max(0.0f, s); return S_OK; }
HRESULT STDMETHODCALLTYPE Engine::GetDopplerScale(A3DVAL *s)
{ if(!s) return E_INVALIDARG; *s = dopplerScale; return S_OK; }
HRESULT STDMETHODCALLTYPE Engine::SetDistanceModelScale(A3DVAL s)
{ if(s < 0.0f) return E_INVALIDARG; dmsGlobal = s; return S_OK; }
HRESULT STDMETHODCALLTYPE Engine::GetDistanceModelScale(A3DVAL *s)
{ if(!s) return E_INVALIDARG; *s = dmsGlobal; return S_OK; }
HRESULT STDMETHODCALLTYPE Engine::SetEq(A3DVAL e)
{ if(e < 0.0f || e > 1.0f) return E_INVALIDARG; eqGlobal = e; return S_OK; }
HRESULT STDMETHODCALLTYPE Engine::GetEq(A3DVAL *e)
{ if(!e) return E_INVALIDARG; *e = eqGlobal; return S_OK; }

HRESULT STDMETHODCALLTYPE Engine::Shutdown()
{
    logf("Shutdown");
    shutdown();
    return S_OK;
}

/* -------------------------------------------------------------- IA3dGeom */

HRESULT STDMETHODCALLTYPE Engine::Enable(DWORD f)
{
    Guard _{lock};
    if(!inited) return A3DERR_FAILED_INIT;
    if(!(available & f)) return E_FAIL;
    enabled |= f;
    return S_OK;
}
HRESULT STDMETHODCALLTYPE Engine::Disable(DWORD f)
{
    Guard _{lock};
    enabled &= ~f;
    return S_OK;
}
BOOL STDMETHODCALLTYPE Engine::IsEnabled(DWORD f) { return (enabled & f) ? TRUE : FALSE; }
HRESULT STDMETHODCALLTYPE Engine::SetOcclusionMode(DWORD m) { occlusionMode = m; return S_OK; }
HRESULT STDMETHODCALLTYPE Engine::GetOcclusionMode(DWORD *m)
{ if(!m) return E_INVALIDARG; *m = occlusionMode; return S_OK; }
HRESULT STDMETHODCALLTYPE Engine::SetReflectionMode(DWORD m) { reflectionMode = m; return S_OK; }
HRESULT STDMETHODCALLTYPE Engine::GetReflectionMode(DWORD *m)
{ if(!m) return E_INVALIDARG; *m = reflectionMode; return S_OK; }
HRESULT STDMETHODCALLTYPE Engine::SetReflectionGainScale(A3DVAL s)
{ reflGainScale = std::max(0.0f, s); return S_OK; }
HRESULT STDMETHODCALLTYPE Engine::GetReflectionGainScale(A3DVAL *s)
{ if(!s) return E_INVALIDARG; *s = reflGainScale; return S_OK; }
HRESULT STDMETHODCALLTYPE Engine::SetReflectionDelayScale(A3DVAL s)
{ reflDelayScale = std::max(0.0f, s); return S_OK; }
HRESULT STDMETHODCALLTYPE Engine::GetReflectionDelayScale(A3DVAL *s)
{ if(!s) return E_INVALIDARG; *s = reflDelayScale; return S_OK; }

ULONG STDMETHODCALLTYPE Engine::PushMatrix()
{
    Guard _{lock};
    stack.push_back(stack.back());
    return ULONG(stack.size());
}
ULONG STDMETHODCALLTYPE Engine::PopMatrix()
{
    Guard _{lock};
    if(stack.size() > 1) stack.pop_back();
    return ULONG(stack.size());
}
HRESULT STDMETHODCALLTYPE Engine::LoadIdentity()
{ Guard _{lock}; stack.back() = M4::identity(); return S_OK; }
HRESULT STDMETHODCALLTYPE Engine::LoadMatrix(A3DVAL *m)
{
    if(!m) return E_INVALIDARG;
    Guard _{lock};
    std::memcpy(stack.back().m, m, sizeof(float)*16);
    return S_OK;
}
HRESULT STDMETHODCALLTYPE Engine::GetMatrix(A3DVAL *m)
{
    if(!m) return E_INVALIDARG;
    Guard _{lock};
    std::memcpy(m, stack.back().m, sizeof(float)*16);
    return S_OK;
}
HRESULT STDMETHODCALLTYPE Engine::MultMatrix(A3DVAL *m)
{
    if(!m) return E_INVALIDARG;
    Guard _{lock};
    M4 r;
    std::memcpy(r.m, m, sizeof(float)*16);
    stack.back() = stack.back() * r;
    return S_OK;
}
HRESULT STDMETHODCALLTYPE Engine::Translate3f(A3DVAL x, A3DVAL y, A3DVAL z)
{ Guard _{lock}; stack.back() = stack.back() * M4::translate(x, y, z); return S_OK; }
HRESULT STDMETHODCALLTYPE Engine::Translate3fv(A3DVAL *v)
{ if(!v) return E_INVALIDARG; return Translate3f(v[0], v[1], v[2]); }
HRESULT STDMETHODCALLTYPE Engine::Rotate3f(A3DVAL d, A3DVAL x, A3DVAL y, A3DVAL z)
{ Guard _{lock}; stack.back() = stack.back() * M4::rotate(d, x, y, z); return S_OK; }
HRESULT STDMETHODCALLTYPE Engine::Rotate3fv(A3DVAL d, A3DVAL *v)
{ if(!v) return E_INVALIDARG; return Rotate3f(d, v[0], v[1], v[2]); }
HRESULT STDMETHODCALLTYPE Engine::Scale3f(A3DVAL x, A3DVAL y, A3DVAL z)
{ Guard _{lock}; stack.back() = stack.back() * M4::scale(x, y, z); return S_OK; }
HRESULT STDMETHODCALLTYPE Engine::Scale3fv(A3DVAL *v)
{ if(!v) return E_INVALIDARG; return Scale3f(v[0], v[1], v[2]); }

HRESULT STDMETHODCALLTYPE Engine::Begin(DWORD type)
{
    Guard _{lock};
    if(inBegin) return A3DERR_BEGIN_MODE;
    const DWORD n{type & A3DP_VERTEX_MASK};
    if(n != A3DP_LINES && n != A3DP_TRIANGLES && n != A3DP_QUADS && n != A3DP_MATERIAL)
        return A3DERR_BEGIN_MODE;
    inBegin = true;
    beginType = type;
    verts.clear();
    haveNormal = false;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Engine::End()
{
    Guard _{lock};
    if(!inBegin) return A3DERR_END_BEFORE_BEGIN;
    inBegin = false;
    verts.clear();
    return S_OK;
}

void Engine::emitPoly(int count)
{
    if(count < 3) return; /* 2D line walls are not modelled */
    Poly p;
    p.count = count;
    for(int i{0};i < count;++i) p.v[i] = verts[size_t(i)];
    V3 n{normalize(cross(p.v[1] - p.v[0], p.v[2] - p.v[0]))};
    if(length(n) < 0.5f && haveNormal) n = normalize(curNormal);
    if(length(n) < 0.5f) return;
    p.normal = n;
    p.mat = curMat;
    p.mode = polyMode;
    p.tag = curTag;
    p.sub = (beginType & A3DP_SUBFACE) != 0;
    p.opening = curOpening;
    p.openingPtr = curOpeningPtr;
    auto &out = target();
    if(p.sub)
    {
        for(int i{int(out.size())-1};i >= 0;--i)
            if(!out[size_t(i)].sub) { p.parent = i; break; }
        if(p.parent < 0) return;
    }
    out.push_back(p);
}

HRESULT STDMETHODCALLTYPE Engine::Vertex3f(A3DVAL x, A3DVAL y, A3DVAL z)
{
    Guard _{lock};
    if(!inBegin) return A3DERR_OUTSIDE_BEGIN_END;
    const int n{int(beginType & A3DP_VERTEX_MASK)};
    if(n == A3DP_MATERIAL) return S_OK;
    verts.push_back(stack.back().point({x, y, z}));
    if(int(verts.size()) == n)
    {
        emitPoly(n);
        verts.clear();
        haveNormal = false;
    }
    return S_OK;
}
HRESULT STDMETHODCALLTYPE Engine::Vertex3fv(A3DVAL *v)
{ if(!v) return E_INVALIDARG; return Vertex3f(v[0], v[1], v[2]); }
HRESULT STDMETHODCALLTYPE Engine::Normal3f(A3DVAL x, A3DVAL y, A3DVAL z)
{
    Guard _{lock};
    curNormal = stack.back().dir({x, y, z});
    haveNormal = true;
    return S_OK;
}
HRESULT STDMETHODCALLTYPE Engine::Normal3fv(A3DVAL *v)
{ if(!v) return E_INVALIDARG; return Normal3f(v[0], v[1], v[2]); }
HRESULT STDMETHODCALLTYPE Engine::Tag(DWORD t) { Guard _{lock}; curTag = t; return S_OK; }
HRESULT STDMETHODCALLTYPE Engine::SetOpeningFactorf(A3DVAL f)
{ Guard _{lock}; curOpening = f; curOpeningPtr = nullptr; return S_OK; }
HRESULT STDMETHODCALLTYPE Engine::SetOpeningFactorfv(A3DVAL *f)
{
    if(!f) return E_INVALIDARG;
    Guard _{lock};
    curOpening = *f;
    curOpeningPtr = f;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Engine::NewMaterial(IA3dMaterialX **out)
{
    if(!out) return E_INVALIDARG;
    ++gObjects;
    *out = new Material{};
    return S_OK;
}
HRESULT STDMETHODCALLTYPE Engine::BindMaterial(IA3dMaterialX *m)
{
    Guard _{lock};
    curMat = m ? static_cast<Material*>(m)->props : MatProps{};
    return S_OK;
}
HRESULT STDMETHODCALLTYPE Engine::NewList(IA3dListX **out)
{
    if(!out) return E_INVALIDARG;
    *out = new List{this};
    return S_OK;
}
HRESULT STDMETHODCALLTYPE Engine::BindListener()
{ Guard _{lock}; lBound = stack.back(); return S_OK; }
HRESULT STDMETHODCALLTYPE Engine::BindSource(IA3dSourceX *s)
{
    if(!s) return E_INVALIDARG;
    Guard _{lock};
    auto *src = static_cast<Source*>(s);
    src->bound = stack.back();
    src->isBound = true;
    return S_OK;
}
HRESULT STDMETHODCALLTYPE Engine::NewEnvironment(void **out)
{
    if(out) *out = nullptr;
    logf("NewEnvironment is not supported");
    return E_NOTIMPL;
}
HRESULT STDMETHODCALLTYPE Engine::SetRenderMode(DWORD m) { Guard _{lock}; polyMode = m; return S_OK; }
HRESULT STDMETHODCALLTYPE Engine::GetRenderMode(DWORD *m)
{ if(!m) return E_INVALIDARG; *m = polyMode; return S_OK; }
HRESULT STDMETHODCALLTYPE Engine::SetPolygonBloatFactor(A3DVAL f) { bloat = f; return S_OK; }
HRESULT STDMETHODCALLTYPE Engine::GetPolygonBloatFactor(A3DVAL *f)
{ if(!f) return E_INVALIDARG; *f = bloat; return S_OK; }
HRESULT STDMETHODCALLTYPE Engine::SetReflectionUpdateInterval(DWORD n) { reflInterval = n; return S_OK; }
HRESULT STDMETHODCALLTYPE Engine::GetReflectionUpdateInterval(DWORD *n)
{ if(!n) return E_INVALIDARG; *n = reflInterval; return S_OK; }
HRESULT STDMETHODCALLTYPE Engine::SetOcclusionUpdateInterval(DWORD n) { occlInterval = n; return S_OK; }
HRESULT STDMETHODCALLTYPE Engine::GetOcclusionUpdateInterval(DWORD *n)
{ if(!n) return E_INVALIDARG; *n = occlInterval; return S_OK; }

/* ---------------------------------------------------------- IA3dListener */

HRESULT STDMETHODCALLTYPE Engine::SetPosition3f(A3DVAL x, A3DVAL y, A3DVAL z)
{ Guard _{lock}; lPos = {x, y, z}; return S_OK; }
HRESULT STDMETHODCALLTYPE Engine::GetPosition3f(A3DVAL *x, A3DVAL *y, A3DVAL *z)
{ if(!x || !y || !z) return E_INVALIDARG; *x = lPos.x; *y = lPos.y; *z = lPos.z; return S_OK; }
HRESULT STDMETHODCALLTYPE Engine::SetPosition3fv(A3DVAL *v)
{ if(!v) return E_INVALIDARG; return SetPosition3f(v[0], v[1], v[2]); }
HRESULT STDMETHODCALLTYPE Engine::GetPosition3fv(A3DVAL *v)
{ if(!v) return E_INVALIDARG; return GetPosition3f(&v[0], &v[1], &v[2]); }
HRESULT STDMETHODCALLTYPE Engine::SetOrientationAngles3f(A3DVAL h, A3DVAL p, A3DVAL r)
{
    Guard _{lock};
    lAngles[0] = h; lAngles[1] = p; lAngles[2] = r;
    anglesToVectors(h, p, r, leftHanded(), lFront, lUp);
    return S_OK;
}

/* Geometric reverb (reverb worked out from the A3D 2.0 geometry) is not
 * implemented; the parameters are kept for the application to read. */
HRESULT STDMETHODCALLTYPE Engine::SetGeomReverbParam(A3DGeomReverbParam *p)
{
    if(!p) return E_POINTER;
    if(p->dwSize < sizeof(A3DGeomReverbParam) || p->nVerticalAxis > 2) return E_INVALIDARG;
    geomReverb = *p;
    return S_OK;
}
HRESULT STDMETHODCALLTYPE Engine::GetGeomReverbParam(A3DGeomReverbParam *p)
{
    if(!p) return E_POINTER;
    if(p->dwSize < sizeof(A3DGeomReverbParam)) return E_INVALIDARG;
    *p = geomReverb;
    return S_OK;
}
HRESULT STDMETHODCALLTYPE Engine::GetOrientationAngles3f(A3DVAL *h, A3DVAL *p, A3DVAL *r)
{
    if(!h || !p || !r) return E_INVALIDARG;
    *h = lAngles[0]; *p = lAngles[1]; *r = lAngles[2];
    return S_OK;
}
HRESULT STDMETHODCALLTYPE Engine::SetOrientationAngles3fv(A3DVAL *v)
{ if(!v) return E_INVALIDARG; return SetOrientationAngles3f(v[0], v[1], v[2]); }
HRESULT STDMETHODCALLTYPE Engine::GetOrientationAngles3fv(A3DVAL *v)
{ if(!v) return E_INVALIDARG; return GetOrientationAngles3f(&v[0], &v[1], &v[2]); }
HRESULT STDMETHODCALLTYPE Engine::SetOrientation6f(A3DVAL fx, A3DVAL fy, A3DVAL fz, A3DVAL ux, A3DVAL uy, A3DVAL uz)
{
    Guard _{lock};
    lFront = {fx, fy, fz};
    lUp = {ux, uy, uz};
    vectorsToAngles(lFront, lUp, leftHanded(), lAngles[0], lAngles[1], lAngles[2]);
    return S_OK;
}
HRESULT STDMETHODCALLTYPE Engine::GetOrientation6f(A3DVAL *fx, A3DVAL *fy, A3DVAL *fz, A3DVAL *ux, A3DVAL *uy, A3DVAL *uz)
{
    if(!fx || !fy || !fz || !ux || !uy || !uz) return E_INVALIDARG;
    *fx = lFront.x; *fy = lFront.y; *fz = lFront.z;
    *ux = lUp.x; *uy = lUp.y; *uz = lUp.z;
    return S_OK;
}
HRESULT STDMETHODCALLTYPE Engine::SetOrientation6fv(A3DVAL *v)
{ if(!v) return E_INVALIDARG; return SetOrientation6f(v[0], v[1], v[2], v[3], v[4], v[5]); }
HRESULT STDMETHODCALLTYPE Engine::GetOrientation6fv(A3DVAL *v)
{ if(!v) return E_INVALIDARG; return GetOrientation6f(&v[0], &v[1], &v[2], &v[3], &v[4], &v[5]); }
HRESULT STDMETHODCALLTYPE Engine::SetVelocity3f(A3DVAL x, A3DVAL y, A3DVAL z)
{ Guard _{lock}; lVel = {x, y, z}; return S_OK; }
HRESULT STDMETHODCALLTYPE Engine::GetVelocity3f(A3DVAL *x, A3DVAL *y, A3DVAL *z)
{ if(!x || !y || !z) return E_INVALIDARG; *x = lVel.x; *y = lVel.y; *z = lVel.z; return S_OK; }
HRESULT STDMETHODCALLTYPE Engine::SetVelocity3fv(A3DVAL *v)
{ if(!v) return E_INVALIDARG; return SetVelocity3f(v[0], v[1], v[2]); }
HRESULT STDMETHODCALLTYPE Engine::GetVelocity3fv(A3DVAL *v)
{ if(!v) return E_INVALIDARG; return GetVelocity3f(&v[0], &v[1], &v[2]); }

/* ------------------------------------------------------------- rendering */

void Engine::flushFrame(bool fromApp)
{
    if(!inited) return;
    if(fromApp)
    {
        front = back;
        children.assign(front.size(), {});
        for(size_t i{0};i < front.size();++i)
            if(front[i].sub && front[i].parent >= 0 && size_t(front[i].parent) < front.size())
                children[size_t(front[i].parent)].push_back(int(i));
        ++flushCount;
    }
    lastFlushTick = GetTickCount();

    /* The listener's frame in world space. */
    const bool lh{leftHanded()};
    wP = lBound.point(lPos);
    wF = normalize(lBound.dir(lFront));
    const V3 u0{lBound.dir(lUp)};
    if(lh) { wR = normalize(cross(u0, wF)); wU = cross(wF, wR); }
    else   { wR = normalize(cross(wF, u0)); wU = cross(wR, wF); }
    wV = lBound.dir(lVel);

    applyReverb();
    for(Source *s : sources)
        processSource(s);
    if(dsListener)
        dsListener->CommitDeferredSettings();
}

void Engine::processSource(Source *s)
{
    if(!s->buf)
    {
        s->pendingPlay = s->pendingStop = false;
        s->audibility = s->occlusion = 0.0f;
        return;
    }
    if(s->pendingStop)
    {
        s->buf->Stop();
        s->stopReflections();
        s->pendingStop = false;
    }
    s->rebuildBuffer();

    const float upm{unitsPerMeter};
    const bool native{(s->renderMode & A3DRM_NATIVE) != 0};
    const bool mono{(s->renderMode & A3DRM_MONO) != 0};
    const float baseGain{outputGain * s->gain};
    float g{baseGain};
    float hf{eqGlobal * s->eq};
    float freqMul{s->pitch};
    float occl{0.0f};
    float dist{0.0f};
    V3 S{}, local{};

    if(native)
    {
        const float l{s->pan[0]}, r{s->pan[1]};
        g *= std::max(l, r);
        LONG pan{0};
        if(l <= 0.00001f) pan = DSBPAN_RIGHT;
        else if(r <= 0.00001f) pan = DSBPAN_LEFT;
        else pan = LONG(std::clamp(2000.0f*std::log10(r/l), -10000.0f, 10000.0f));
        if(!s->is3d) s->buf->SetPan(pan);
    }
    else
    {
        /* World position, front and velocity of the source. */
        const float sgn{leftHanded() ? -1.0f : 1.0f};
        V3 lp{s->isBound ? s->bound.point(s->pos) : s->pos};
        V3 lf{s->isBound ? s->bound.dir(s->front) : s->front};
        V3 lu{s->isBound ? s->bound.dir(s->up) : s->up};
        V3 lv{s->isBound ? s->bound.dir(s->vel) : s->vel};
        V3 Sf, Su, Sv;
        if(s->xform == A3DXFORM_HEADRELATIVE)
        {
            auto toWorld = [&](const V3 &v) { return wR*v.x + wU*v.y + wF*(-sgn*v.z); };
            S = wP + toWorld(lp);
            Sf = toWorld(lf);
            Su = toWorld(lu);
            Sv = wV + toWorld(lv);
        }
        else
        {
            S = lp;
            Sf = lf;
            Su = lu;
            Sv = lv;
        }

        /* A3D 3.0 volumetric source: a box centred on the source position,
         * oriented by the source's orientation. It is heard from its point
         * nearest the listener, and from all round (no direction) when the
         * listener is inside it. */
        if(s->volumetric())
        {
            const V3 f{normalize(Sf)};
            const V3 u{normalize(Su - f*dot(Su, f))};
            const V3 r{cross(f, u)};
            const V3 d{wP - S};
            const float hx{0.5f*s->volBounds[0]}, hy{0.5f*s->volBounds[1]}, hz{0.5f*s->volBounds[2]};
            S = S + r*std::clamp(dot(d, r), -hx, hx) + u*std::clamp(dot(d, u), -hy, hy)
                + f*std::clamp(dot(d, f), -hz, hz);
        }

        const V3 rel{S - wP};
        local = V3{dot(rel, wR), dot(rel, wU), dot(rel, wF)} * (1.0f/upm);
        dist = length(local);

        if(!mono)
        {
            g *= distanceGain(s, dist, dmsGlobal);

            if(s->coneOuter > 0.0f && s->coneGain < 1.0f && s->coneInner < 360.0f && dist > 0.0f)
            {
                const V3 toL{normalize(wP - S)};
                const float a{std::acos(std::clamp(dot(normalize(Sf), toL), -1.0f, 1.0f))
                    * 180.0f / 3.14159265f};
                float cg{1.0f};
                if(a >= s->coneOuter) cg = s->coneGain;
                else if(a > s->coneInner && s->coneOuter > s->coneInner)
                    cg = 1.0f + (s->coneGain - 1.0f) * (a - s->coneInner) / (s->coneOuter - s->coneInner);
                g *= cg;
            }

            hf *= distanceBrightness(dist, s->minDist, hfAbsorb);

            if((enabled & A3DF_OCCLUSIONS) && (s->renderMode & A3DRM_OCCLUSIONS) && !front.empty())
            {
                float tl{1.0f}, th{1.0f}, open{1.0f};
                for(size_t i{0};i < front.size();++i)
                {
                    const Poly &p{front[i]};
                    if(p.sub || !(p.mode & A3DPOLY_OCCLUSIONS)) continue;
                    V3 X;
                    if(!segmentHits(wP, S, p, X)) continue;
                    float o{0.0f};
                    for(int c : children[i])
                        if(insidePoly(X, front[size_t(c)]))
                            o = std::max(o, front[size_t(c)].open());
                    tl *= p.mat.transLow + o*(1.0f - p.mat.transLow);
                    th *= p.mat.transHigh + o*(1.0f - p.mat.transHigh);
                    open *= o;
                }
                occl = 1.0f - open;
                g *= tl;
                hf *= th;
            }

            const float k{dopplerScale * s->doppler};
            if(k > 0.0f && dist > 0.0f)
            {
                const V3 dir{normalize(rel)};
                const float c{SpeedOfSound};
                const float vl{std::clamp(dot(wV, dir)/upm*k, -0.9f*c, 0.9f*c)};
                const float vs{std::clamp(-dot(Sv, dir)/upm*k, -0.9f*c, 0.9f*c)};
                freqMul *= std::clamp((c + vl)/(c - vs), 0.25f, 4.0f);
            }
        }

        if(s->is3d && s->b3d)
        {
            if(mono)
                s->b3d->SetMode(DS3DMODE_DISABLE, DS3D_DEFERRED);
            else
            {
                s->b3d->SetMode(DS3DMODE_HEADRELATIVE, DS3D_DEFERRED);
                s->b3d->SetPosition(local.x, local.y, local.z, DS3D_DEFERRED);
            }
        }
        else if(!s->is3d)
            s->buf->SetPan(0);
    }

    s->buf->SetVolume(gainTo_mB(g));
    const DWORD freq{DWORD(std::clamp(float(s->fmt.nSamplesPerSec) * freqMul,
        float(DSBFREQUENCY_MIN), float(DSBFREQUENCY_MAX)))};
    s->buf->SetFrequency(freq);
    /* A3D 3.0 reverb: every 3D source feeds the bound reverb at its own
     * reverb mix (1.0 unless the application set one), through EAX's
     * per-source room level; HiFreq darkens the source's direct sound. */
    const bool wet{reverb && fxPs && !native && !mono && s->is3d};
    if(wet) hf *= s->revBright;
    s->setDirectHF(s->ps, (native || mono) ? 1.0f : brightnessToHF(hf));
    s->setRoom(wet ? gainTo_mB(s->revLevel) : -10000);

    DWORD status{0};
    s->buf->GetStatus(&status);
    if(s->eventsDirty && !(status & DSBSTATUS_PLAYING))
    {
        IDirectSoundNotify *n{nullptr};
        if(SUCCEEDED(s->buf->QueryInterface(IID_IDirectSoundNotify, reinterpret_cast<void**>(&n))))
        {
            std::vector<DSBPOSITIONNOTIFY> ev{s->events};
            for(auto &e : ev)
                if(e.dwOffset != DSBPN_OFFSETSTOP)
                    e.dwOffset = s->toDS(e.dwOffset);
            n->SetNotificationPositions(DWORD(ev.size()), ev.empty() ? nullptr : ev.data());
            n->Release();
        }
        s->eventsDirty = false;
    }
    const bool geomRefl{s->is3d && !native && !mono && (enabled & A3DF_1ST_REFLECTIONS)
        && (s->renderMode & A3DRM_1ST_REFLECTIONS) && settings().maxReflections > 0};
    const bool reflOn{geomRefl || (s->is3d && !native && !mono && !s->manual.empty())};
    if(s->pendingPlay)
    {
        /* Set the reflection taps up first so they can be started in the
         * same breath as the source: that is what fixes their lag. */
        s->stopReflections();
        if(reflOn)
            updateReflections(s, S, dist, baseGain, eqGlobal * s->eq, freq, true, geomRefl);
        s->buf->Play(0, 0, s->loop ? DSBPLAY_LOOPING : 0);
        for(auto &v : s->refl)
            if(v.buf && !v.started)
            {
                v.buf->Play(0, 0, (s->loop && !v.padded) ? DSBPLAY_LOOPING : 0);
                v.started = true;
            }
        s->pendingPlay = false;
        s->buf->GetStatus(&status);
    }
    else
    {
        if((status & DSBSTATUS_PLAYING) && reflOn)
            updateReflections(s, S, dist, baseGain, eqGlobal * s->eq, freq, false, geomRefl);
    }

    const bool playing{(status & DSBSTATUS_PLAYING) != 0};
    s->audibility = playing ? std::min(1.0f, g * (0.5f + 0.5f*hf)) : 0.0f;
    s->occlusion = playing ? occl : 0.0f;

    if(!s->refl.empty() && (!reflOn || (!playing && s->loop)))
        s->stopReflections();
    else if(!s->refl.empty() && !playing)
    {
        /* A finished one-shot: let its reflection taps ring out, then free
         * them. */
        bool anyPlaying{false};
        for(auto &v : s->refl)
        {
            DWORD vs{0};
            if(v.buf) v.buf->GetStatus(&vs);
            anyPlaying |= (vs & DSBSTATUS_PLAYING) != 0;
        }
        if(!anyPlaying)
            s->stopReflections();
    }
}

/* A tap for a looping source is a duplicate of its buffer, played at an
 * offset. A one-shot gets its own copy with silence in front, so the tap can
 * start together with the source and still lag it without skipping the start
 * of the sound. */
bool Engine::createReflVoice(Source *s, ReflVoice &nv, float delay)
{
    if(s->loop)
    {
        if(HRESULT hr{ds->DuplicateSoundBuffer(s->buf, &nv.buf)}; FAILED(hr))
        {
            logf("reflection: DuplicateSoundBuffer failed %08lx", hr);
            return false;
        }
        nv.size = s->bytes;
    }
    else
    {
        const DWORD align{s->bufFmt().nBlockAlign};
        const float maxPitch{2.0f};
        nv.padBytes = DWORD(std::max(maxReflDelay, delay) * float(s->fmt.nSamplesPerSec) * maxPitch) * align;
        nv.size = nv.padBytes + s->bytes;
        DSBUFFERDESC desc{};
        desc.dwSize = sizeof(desc);
        desc.dwFlags = DSBCAPS_CTRLVOLUME | DSBCAPS_CTRLFREQUENCY | DSBCAPS_CTRL3D
            | DSBCAPS_GETCURRENTPOSITION2 | DSBCAPS_GLOBALFOCUS;
        desc.dwBufferBytes = nv.size;
        desc.lpwfxFormat = const_cast<WAVEFORMATEX*>(&s->bufFmt());
        if(HRESULT hr{ds->CreateSoundBuffer(&desc, &nv.buf, nullptr)}; FAILED(hr))
        {
            logf("reflection: CreateSoundBuffer failed %08lx", hr);
            return false;
        }
        void *d1{}, *d2{}, *s1{}, *s2{};
        DWORD dn1{}, dn2{}, sn1{}, sn2{};
        if(SUCCEEDED(nv.buf->Lock(0, 0, &d1, &dn1, &d2, &dn2, DSBLOCK_ENTIREBUFFER)))
        {
            std::memset(d1, s->bufFmt().wBitsPerSample == 8 ? 0x80 : 0, nv.padBytes);
            if(SUCCEEDED(s->buf->Lock(0, 0, &s1, &sn1, &s2, &sn2, DSBLOCK_ENTIREBUFFER)))
            {
                std::memcpy(static_cast<char*>(d1) + nv.padBytes, s1, std::min(sn1, dn1 - nv.padBytes));
                s->buf->Unlock(s1, sn1, s2, sn2);
            }
            nv.buf->Unlock(d1, dn1, d2, dn2);
        }
        nv.padded = true;
    }
    if(SUCCEEDED(nv.buf->QueryInterface(IID_IDirectSound3DBuffer, reinterpret_cast<void**>(&nv.b3d))))
    {
        nv.b3d->SetMode(DS3DMODE_HEADRELATIVE, DS3D_IMMEDIATE);
        nv.b3d->SetMinDistance(1.0f, DS3D_IMMEDIATE);
        nv.b3d->SetMaxDistance(1000000.0f, DS3D_IMMEDIATE);
    }
    if(s->eaxOk && SUCCEEDED(nv.buf->QueryInterface(IID_IKsPropertySet, reinterpret_cast<void**>(&nv.ps))))
    {
        constexpr GUID eax20buf{0x0306a6a7, 0xb224, 0x11d2,
            {0x99, 0xe5, 0x00, 0x00, 0xe8, 0xd8, 0xc7, 0x22}};
        LONG room{-10000};
        nv.ps->Set(eax20buf, 4, nullptr, 0, &room, sizeof(room));
    }
    return true;
}

void Engine::updateReflections(Source *s, const V3 &S, float directDist, float baseGain,
    float baseHF, DWORD freq, bool starting, bool geometric)
{
    struct Cand { DWORD key; float gain, hf, delay; V3 local; };
    std::vector<Cand> cands;
    const float upm{unitsPerMeter};
    const float gscale{reflGainScale * s->reflGainScale};
    const float dscale{reflDelayScale * s->reflDelayScale};

    for(size_t i{0};geometric && i < front.size();++i)
    {
        const Poly &p{front[i]};
        if(p.sub || !(p.mode & A3DPOLY_REFLECTIONS) || p.mat.reflLow <= 0.0f) continue;
        const float sd{dot(S - p.v[0], p.normal)};
        const float ld{dot(wP - p.v[0], p.normal)};
        if(!(sd*ld > 0.0f)) continue;
        const V3 I{S - p.normal*(2.0f*sd)};
        const float t{ld / (ld + sd)};
        const V3 X{wP + (I - wP)*t};
        if(!insidePoly(X, p)) continue;

        float o{0.0f};
        for(int c : children[i])
            if(insidePoly(X, front[size_t(c)]))
                o = std::max(o, front[size_t(c)].open());

        const V3 rel{I - wP};
        const V3 loc{V3{dot(rel, wR), dot(rel, wU), dot(rel, wF)} * (1.0f/upm)};
        const float r{length(loc)};
        const float refl{std::min(1.0f, p.mat.reflLow * gscale) * (1.0f - o)};
        const float gain{baseGain * distanceGain(s, r, dmsGlobal) * refl};
        if(gain < 0.001f) continue;

        const float delay{std::clamp((r - directDist) / SpeedOfSound * dscale, 0.0f, maxReflDelay)};
        cands.push_back({p.tag ? p.tag : (0x80000000u | DWORD(i)), gain,
            brightnessToHF(baseHF * p.mat.reflHigh * distanceBrightness(r, s->minDist, hfAbsorb)), delay, loc});
    }

    const size_t keep{std::min<size_t>(cands.size(), size_t(settings().maxReflections))};
    std::partial_sort(cands.begin(), cands.begin() + ptrdiff_t(keep), cands.end(),
        [](const Cand &a, const Cand &b) { return a.gain > b.gain; });
    cands.resize(keep);

    /* A3D 3.0 manual reflections: a position, delay, gain and EQ given by
     * the application. Their delay is taken as given (Aureal's sample uses
     * delays well beyond the 0.3 s geometric limit). */
    for(const Reflection *m : s->manual)
    {
        if(m->gainScale <= 0.0f) continue;
        V3 P{m->pos};
        if(m->xform == A3DXFORM_HEADRELATIVE)
        {
            const float sgn{leftHanded() ? -1.0f : 1.0f};
            P = wP + wR*P.x + wU*P.y + wF*(-sgn*P.z);
        }
        const V3 rel{P - wP};
        const V3 loc{V3{dot(rel, wR), dot(rel, wU), dot(rel, wF)} * (1.0f/upm)};
        const float r{length(loc)};
        const float gain{baseGain * distanceGain(s, r, dmsGlobal) * m->gainScale};
        if(gain < 0.0001f) continue;
        cands.push_back({0x40000000u | m->id, gain,
            brightnessToHF(baseHF * m->eq * distanceBrightness(r, s->minDist, hfAbsorb)),
            std::min(m->delay, 4.0f), loc});
    }

    for(auto &v : s->refl) v.used = false;

    DWORD srcPos{0}, srcWrite{0};
    s->buf->GetCurrentPosition(&srcPos, &srcWrite);
    const DWORD align{s->bufFmt().nBlockAlign};
    const DWORD size{s->bytes};

    for(const Cand &c : cands)
    {
        ReflVoice *v{nullptr};
        for(auto &x : s->refl)
            if(x.buf && x.key == c.key && !x.used) { v = &x; break; }
        if(!v)
        {
            if(reflVoicesInUse >= settings().maxReflectionVoices) continue;
            ReflVoice nv;
            if(!createReflVoice(s, nv, c.delay)) continue;
            nv.key = c.key;
            ++reflVoicesInUse;
            s->refl.push_back(nv);
            v = &s->refl.back();
            logf("reflection voice %08lx: delay %.1f ms gain %.3f%s", c.key, c.delay*1000.0f,
                c.gain, nv.padded ? " (padded)" : "");
        }
        v->used = true;
        if(v->b3d) v->b3d->SetPosition(c.local.x, c.local.y, c.local.z, DS3D_DEFERRED);
        v->buf->SetVolume(gainTo_mB(c.gain));
        v->buf->SetFrequency(freq);
        s->setDirectHF(v->ps, c.hf);

        DWORD db{DWORD(c.delay * float(freq)) * align};
        if(v->padded) db = std::min(db, v->padBytes);
        v->delayBytes = db;

        /* Where the tap should be now, given where the source is. */
        DWORD want{0};
        if(v->padded)
            want = (starting ? 0 : srcPos) + v->padBytes - db;
        else if(size > 0)
            want = ((starting ? 0 : srcPos) + size - db % size) % size;

        if(!v->started)
        {
            v->buf->SetCurrentPosition(want);
            if(!starting)
            {
                v->buf->Play(0, 0, v->padded ? 0 : DSBPLAY_LOOPING);
                v->started = true;
            }
        }
        else
        {
            /* Keep the lag right as the geometry moves. */
            DWORD vp{0}, vw{0};
            v->buf->GetCurrentPosition(&vp, &vw);
            const long tol{long(freq / 100) * long(align)};
            long diff{long(vp) - long(want)};
            if(!v->padded && size > 0)
            {
                diff %= long(size);
                if(diff > long(size)/2) diff -= long(size);
                if(diff < -long(size)/2) diff += long(size);
            }
            if(std::labs(diff) > tol && want < v->size)
                v->buf->SetCurrentPosition(want);
        }
    }

    for(auto it = s->refl.begin();it != s->refl.end();)
    {
        if(!it->used)
        {
            if(it->buf) --reflVoicesInUse;
            it->release();
            it = s->refl.erase(it);
        }
        else
            ++it;
    }
}

/* Starts delayed reflection taps on time, and keeps apps that never call
 * Flush (or stop calling it) from going silent. */
void Engine::workerLoop()
{
    while(!quit)
    {
        Sleep(10);
        Guard _{lock};
        if(!inited) continue;
        if(settings().autoFlush)
        {
            const DWORD since{GetTickCount() - lastFlushTick};
            bool pendingCmd{false};
            for(Source *s : sources) pendingCmd |= s->pendingPlay || s->pendingStop;
            if((flushCount == 0 && since >= 20) || (pendingCmd && since >= 100))
                flushFrame(false);
        }
    }
}

} // namespace a3d
