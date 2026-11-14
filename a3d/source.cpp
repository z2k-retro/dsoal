/* IA3dSource, IA3dMaterial and IA3dList. */
#include "engine.h"

#include <ks.h>

namespace a3d {

namespace {

/* EAX 2.0 buffer property set (public EAX constants). */
constexpr GUID EAX20_BufferProps{0x0306a6a7, 0xb224, 0x11d2,
    {0x99, 0xe5, 0x00, 0x00, 0xe8, 0xd8, 0xc7, 0x22}};
enum : ULONG { EAX20BUF_DIRECT = 2, EAX20BUF_DIRECTHF = 3, EAX20BUF_ROOM = 4 };

using Guard = std::lock_guard<std::recursive_mutex>;

template<typename T> void safeRelease(T *&p) { if(p) { p->Release(); p = nullptr; } }

bool parseWave(const unsigned char *data, DWORD size, WAVEFORMATEX &fmt,
    const unsigned char *&pcm, DWORD &pcmSize)
{
    auto rd32 = [](const unsigned char *p) { return DWORD(p[0] | p[1]<<8 | p[2]<<16 | DWORD(p[3])<<24); };
    auto rd16 = [](const unsigned char *p) { return WORD(p[0] | p[1]<<8); };
    if(size < 12 || std::memcmp(data, "RIFF", 4) != 0 || std::memcmp(data+8, "WAVE", 4) != 0)
        return false;
    bool haveFmt{false};
    pcm = nullptr;
    DWORD off{12};
    while(off + 8 <= size)
    {
        const DWORD len{rd32(data+off+4)};
        const unsigned char *body{data + off + 8};
        const DWORD avail{std::min<DWORD>(len, size - off - 8)};
        if(std::memcmp(data+off, "fmt ", 4) == 0 && avail >= 16)
        {
            WORD tag{rd16(body)};
            if(tag == WAVE_FORMAT_EXTENSIBLE && avail >= 40)
                tag = rd16(body + 24); /* sub-format GUID starts with the format tag */
            if(tag != WAVE_FORMAT_PCM)
                return false;
            fmt = {};
            fmt.wFormatTag = WAVE_FORMAT_PCM;
            fmt.nChannels = rd16(body + 2);
            fmt.nSamplesPerSec = rd32(body + 4);
            fmt.wBitsPerSample = rd16(body + 14);
            fmt.nBlockAlign = WORD(fmt.nChannels * fmt.wBitsPerSample / 8);
            fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;
            haveFmt = true;
        }
        else if(std::memcmp(data+off, "data", 4) == 0)
        {
            pcm = body;
            pcmSize = avail;
        }
        off += 8 + len + (len & 1);
    }
    return haveFmt && pcm && fmt.nBlockAlign != 0;
}

} // namespace

void ReflVoice::release()
{
    if(buf) buf->Stop();
    safeRelease(ps);
    safeRelease(b3d);
    safeRelease(buf);
    started = false;
}

/* ---------------------------------------------------------------- Source */

Source::Source(Engine *e, DWORD t) : engine{e}, type{t}
{
    engine->AddRef();
    ++gObjects;
    if(t & A3DTYPE_NATIVE)
        renderMode = A3DRM_NATIVE;
    const bool lh{engine->leftHanded()};
    front = {0, 0, lh ? 1.0f : -1.0f};
    Guard _{engine->lock};
    engine->sources.push_back(this);
}

Source::~Source()
{
    {
        Guard _{engine->lock};
        stopReflections();
        releaseBuffer();
        auto &v = engine->sources;
        v.erase(std::remove(v.begin(), v.end(), this), v.end());
    }
    engine->Release();
    --gObjects;
}

HRESULT STDMETHODCALLTYPE Source::QueryInterface(REFIID riid, void **out)
{
    if(!out) return E_POINTER;
    if(riid == IID_IUnknown || riid == IID_IA3dSource_)
    {
        AddRef();
        *out = static_cast<IA3dSourceX*>(this);
        return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE Source::Release()
{
    const LONG r{--mRef};
    if(r == 0) delete this;
    return ULONG(r);
}

void Source::setDirectHF(IKsPropertySet *p, float hfGain)
{
    if(!p || !eaxOk) return;
    LONG mb{gainTo_mB(hfGain)};
    p->Set(EAX20_BufferProps, EAX20BUF_DIRECTHF, nullptr, 0, &mb, sizeof(mb));
}

void Source::attachBuffer(IDirectSoundBuffer *b)
{
    buf = b;
    DSBCAPS caps{};
    caps.dwSize = sizeof(caps);
    buf->GetCaps(&caps);
    bytes = caps.dwBufferBytes;
    is3d = (caps.dwFlags & DSBCAPS_CTRL3D) != 0;
    if(is3d && SUCCEEDED(buf->QueryInterface(IID_IDirectSound3DBuffer, reinterpret_cast<void**>(&b3d))))
    {
        b3d->SetMode(DS3DMODE_HEADRELATIVE, DS3D_IMMEDIATE);
        b3d->SetMinDistance(1.0f, DS3D_IMMEDIATE);
        b3d->SetMaxDistance(1000000.0f, DS3D_IMMEDIATE);
    }
    eaxOk = false;
    if(SUCCEEDED(buf->QueryInterface(IID_IKsPropertySet, reinterpret_cast<void**>(&ps))))
    {
        ULONG support{0};
        if(SUCCEEDED(ps->QuerySupport(EAX20_BufferProps, EAX20BUF_DIRECTHF, &support))
            && (support & KSPROPERTY_SUPPORT_SET))
        {
            eaxOk = true;
            /* A3D has no reverb send; keep EAX's default room effect off. */
            LONG room{-10000};
            ps->Set(EAX20_BufferProps, EAX20BUF_ROOM, nullptr, 0, &room, sizeof(room));
        }
    }
}

void Source::releaseBuffer()
{
    if(buf) buf->Stop();
    safeRelease(ps);
    safeRelease(b3d);
    safeRelease(buf);
    bytes = 0;
    is3d = false;
    eaxOk = false;
}

HRESULT Source::createBuffer(DWORD size)
{
    if(!engine->ds) return A3DERR_FAILED_INIT;
    if(!haveFmt) return A3DERR_NEEDS_FORMAT;
    if(size == 0) return E_INVALIDARG;
    releaseBuffer();

    const bool want3d{wants3D()};
    appBytes = size - size % fmt.nBlockAlign;
    mixdown = want3d && fmt.nChannels == 2;
    dsFmt = fmt;
    if(mixdown)
    {
        dsFmt.nChannels = 1;
        dsFmt.nBlockAlign = WORD(fmt.wBitsPerSample / 8);
        dsFmt.nAvgBytesPerSec = dsFmt.nSamplesPerSec * dsFmt.nBlockAlign;
        shadow.assign(appBytes, fmt.wBitsPerSample == 8 ? 0x80 : 0);
    }
    else
        shadow.clear();

    DSBUFFERDESC desc{};
    desc.dwSize = sizeof(desc);
    desc.dwFlags = DSBCAPS_CTRLVOLUME | DSBCAPS_CTRLFREQUENCY | DSBCAPS_CTRLPOSITIONNOTIFY
        | DSBCAPS_GETCURRENTPOSITION2 | DSBCAPS_GLOBALFOCUS
        | (want3d ? DSBCAPS_CTRL3D : DSBCAPS_CTRLPAN);
    desc.dwBufferBytes = toDS(appBytes);
    desc.lpwfxFormat = &dsFmt;
    IDirectSoundBuffer *b{nullptr};
    const HRESULT hr{engine->ds->CreateSoundBuffer(&desc, &b, nullptr)};
    if(FAILED(hr))
    {
        logf("CreateSoundBuffer(%lu bytes, %u ch, %lu Hz) failed: %08lx", desc.dwBufferBytes,
            dsFmt.nChannels, dsFmt.nSamplesPerSec, hr);
        mixdown = false;
        return A3DERR_CREATE_SOUNDBUFFER;
    }
    attachBuffer(b);
    eventsDirty = !events.empty();
    return S_OK;
}

/* Mix [appOff, appOff+n) of the stereo shadow copy down into the buffer. */
void Source::mixRange(DWORD appOff, DWORD n)
{
    if(!mixdown || !buf || n == 0) return;
    const DWORD fa{fmt.nBlockAlign};
    DWORD end{std::min(appBytes, appOff + n + fa - 1)};
    appOff -= appOff % fa;
    end -= end % fa;
    if(end <= appOff) return;
    void *p1{}, *p2{};
    DWORD n1{}, n2{};
    if(FAILED(buf->Lock(toDS(appOff), toDS(end - appOff), &p1, &n1, &p2, &n2, 0)))
        return;
    const unsigned char *src{shadow.data() + appOff};
    auto mixInto = [&](void *dst, DWORD bytes) {
        if(fmt.wBitsPerSample == 16)
        {
            auto *o = static_cast<int16_t*>(dst);
            const auto *i = reinterpret_cast<const int16_t*>(src);
            for(DWORD k{0};k < bytes/2;++k)
                o[k] = int16_t((int32_t(i[2*k]) + int32_t(i[2*k+1])) / 2);
            src += bytes * 2;
        }
        else
        {
            auto *o = static_cast<uint8_t*>(dst);
            for(DWORD k{0};k < bytes;++k)
                o[k] = uint8_t((unsigned(src[2*k]) + unsigned(src[2*k+1])) / 2);
            src += bytes * 2;
        }
    };
    mixInto(p1, n1);
    if(p2) mixInto(p2, n2);
    buf->Unlock(p1, n1, p2, n2);
}

/* Put application-format sample data into the source, whatever kind of
 * buffer it has now. */
HRESULT Source::fillFromApp(const unsigned char *data, DWORD n)
{
    if(!buf) return A3DERR_NO_WAVE_DATA;
    n = std::min(n, appBytes);
    if(mixdown)
    {
        std::memcpy(shadow.data(), data, n);
        mixRange(0, appBytes);
        return S_OK;
    }
    void *p1{}, *p2{};
    DWORD n1{}, n2{};
    if(FAILED(buf->Lock(0, 0, &p1, &n1, &p2, &n2, DSBLOCK_ENTIREBUFFER)))
        return A3DERR_LOCK;
    std::memcpy(p1, data, std::min(n1, n));
    if(n1 > n) std::memset(static_cast<char*>(p1) + n, fmt.wBitsPerSample == 8 ? 0x80 : 0, n1 - n);
    buf->Unlock(p1, n1, p2, n2);
    return S_OK;
}

/* Recreate the buffer when the render mode needs the other kind (3D vs
 * panned 2D), keeping the samples, position and play state. */
HRESULT Source::rebuildBuffer()
{
    if(!buf || wants3D() == is3d) return S_OK;
    std::vector<unsigned char> copy;
    if(mixdown)
        copy = shadow;
    else
    {
        copy.resize(appBytes);
        void *p1{}, *p2{};
        DWORD n1{}, n2{};
        if(SUCCEEDED(buf->Lock(0, 0, &p1, &n1, &p2, &n2, DSBLOCK_ENTIREBUFFER)))
        {
            std::memcpy(copy.data(), p1, std::min<DWORD>(n1, appBytes));
            buf->Unlock(p1, n1, p2, n2);
        }
    }
    DWORD status{0}, play{0}, write{0};
    buf->GetStatus(&status);
    buf->GetCurrentPosition(&play, &write);
    const DWORD appPos{toApp(play)};
    stopReflections();
    const HRESULT hr{createBuffer(DWORD(copy.size()))};
    if(FAILED(hr)) return hr;
    fillFromApp(copy.data(), DWORD(copy.size()));
    buf->SetCurrentPosition(toDS(appPos) - toDS(appPos) % bufFmt().nBlockAlign);
    if(status & DSBSTATUS_PLAYING)
        buf->Play(0, 0, (status & DSBSTATUS_LOOPING) ? DSBPLAY_LOOPING : 0);
    return S_OK;
}

void Source::stopReflections()
{
    for(auto &v : refl)
    {
        if(v.buf) --engine->reflVoicesInUse;
        v.release();
    }
    refl.clear();
}

HRESULT STDMETHODCALLTYPE Source::LoadWaveFile(char *path)
{
    if(!path) return E_POINTER;
    FILE *f{std::fopen(path, "rb")};
    if(!f)
    {
        logf("LoadWaveFile: cannot open %s", path);
        return A3DERR_FILE_OPEN;
    }
    std::vector<unsigned char> data;
    std::fseek(f, 0, SEEK_END);
    const long len{std::ftell(f)};
    std::fseek(f, 0, SEEK_SET);
    if(len > 0)
    {
        data.resize(size_t(len));
        data.resize(std::fread(data.data(), 1, data.size(), f));
    }
    std::fclose(f);
    return LoadWaveData(data.data(), DWORD(data.size()));
}

HRESULT STDMETHODCALLTYPE Source::LoadWaveData(void *data, DWORD size)
{
    if(!data) return E_POINTER;
    Guard _{engine->lock};
    WAVEFORMATEX wf{};
    const unsigned char *pcm{};
    DWORD pcmSize{};
    if(!parseWave(static_cast<const unsigned char*>(data), size, wf, pcm, pcmSize))
        return A3DERR_FORMAT;
    fmt = wf;
    haveFmt = true;
    if(HRESULT hr{createBuffer(pcmSize)}; FAILED(hr))
        return hr;
    return fillFromApp(pcm, pcmSize);
}

HRESULT STDMETHODCALLTYPE Source::AllocateWaveData(INT size)
{
    Guard _{engine->lock};
    if(size <= 0) return E_INVALIDARG;
    return createBuffer(DWORD(size));
}

HRESULT STDMETHODCALLTYPE Source::FreeWaveData()
{
    Guard _{engine->lock};
    if(!buf) return A3DERR_NO_WAVE_DATA;
    stopReflections();
    releaseBuffer();
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Source::SetWaveFormat(void *f)
{
    if(!f) return E_POINTER;
    Guard _{engine->lock};
    const auto *in = static_cast<const WAVEFORMATEX*>(f);
    if(in->wFormatTag != WAVE_FORMAT_PCM || in->nChannels == 0 || in->nChannels > 2)
        return A3DERR_FORMAT;
    fmt = *in;
    fmt.cbSize = 0;
    fmt.nBlockAlign = WORD(fmt.nChannels * fmt.wBitsPerSample / 8);
    fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;
    haveFmt = true;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Source::GetWaveFormat(void *f)
{
    if(!f) return E_POINTER;
    if(!haveFmt) return A3DERR_NEEDS_FORMAT;
    std::memcpy(f, &fmt, sizeof(WAVEFORMATEX));
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Source::GetWaveSize() { return HRESULT(buf ? appBytes : 0); }

HRESULT STDMETHODCALLTYPE Source::GetType(DWORD *t)
{
    if(!t) return E_POINTER;
    *t = type;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Source::Lock(DWORD off, DWORD n, void **p1, DWORD *n1, void **p2,
    DWORD *n2, DWORD flags)
{
    if(!buf) return A3DERR_NO_WAVE_DATA;
    void *dummy{};
    DWORD dummyN{};
    if(!p2) p2 = &dummy;
    if(!n2) n2 = &dummyN;
    if(mixdown)
    {
        if(!p1 || !n1) return E_POINTER;
        if(flags & A3DLOCK_ENTIREBUFFER)
        {
            off = 0;
            n = appBytes;
        }
        else if(flags & A3DLOCK_FROMWRITECURSOR)
        {
            DWORD play{0}, write{0};
            buf->GetCurrentPosition(&play, &write);
            off = toApp(write);
        }
        if(off >= appBytes || n > appBytes) return A3DERR_LOCK;
        *p1 = shadow.data() + off;
        *n1 = std::min(n, appBytes - off);
        *p2 = (n > *n1) ? shadow.data() : nullptr;
        *n2 = n - *n1;
        return S_OK;
    }
    DWORD dsflags{0};
    if(flags & A3DLOCK_FROMWRITECURSOR) dsflags |= DSBLOCK_FROMWRITECURSOR;
    if(flags & A3DLOCK_ENTIREBUFFER) dsflags |= DSBLOCK_ENTIREBUFFER;
    if(FAILED(buf->Lock(off, n, p1, n1, p2, n2, dsflags)))
        return A3DERR_LOCK;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Source::Unlock(void *p1, DWORD n1, void *p2, DWORD n2)
{
    if(!buf) return A3DERR_NO_WAVE_DATA;
    if(mixdown)
    {
        Guard _{engine->lock};
        const auto *base = shadow.data();
        if(p1) mixRange(DWORD(static_cast<unsigned char*>(p1) - base), n1);
        if(p2 && n2) mixRange(DWORD(static_cast<unsigned char*>(p2) - base), n2);
        return S_OK;
    }
    return FAILED(buf->Unlock(p1, n1, p2, n2)) ? A3DERR_UNLOCK : S_OK;
}

HRESULT STDMETHODCALLTYPE Source::Play(INT mode)
{
    Guard _{engine->lock};
    if(!buf) return A3DERR_NO_WAVE_DATA;
    if(mode != A3DPLAY_SINGLE && mode != A3DPLAY_LOOPED) return A3DERR_PLAYMODE;
    loop = (mode == A3DPLAY_LOOPED);
    pendingPlay = true;
    pendingStop = false;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Source::Stop()
{
    Guard _{engine->lock};
    pendingStop = true;
    pendingPlay = false;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Source::Rewind()
{
    return SetWavePosition(0);
}

HRESULT STDMETHODCALLTYPE Source::SetWaveTime(A3DVAL t)
{
    if(!haveFmt) return A3DERR_NO_WAVE_DATA;
    const DWORD frames{DWORD(std::max(0.0f, t) * float(fmt.nSamplesPerSec))};
    return SetWavePosition(frames * fmt.nBlockAlign);
}

HRESULT STDMETHODCALLTYPE Source::GetWaveTime(A3DVAL *t)
{
    if(!t) return E_POINTER;
    DWORD pos{0};
    if(HRESULT hr{GetWavePosition(&pos)}; FAILED(hr)) return hr;
    *t = float(pos / fmt.nBlockAlign) / float(fmt.nSamplesPerSec);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Source::SetWavePosition(DWORD p)
{
    Guard _{engine->lock};
    if(!buf) return A3DERR_NO_WAVE_DATA;
    DWORD d{toDS(p)};
    d -= d % bufFmt().nBlockAlign;
    buf->SetCurrentPosition(std::min(d, bytes ? bytes - bufFmt().nBlockAlign : 0));
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Source::GetWavePosition(DWORD *p)
{
    if(!p) return E_POINTER;
    if(!buf) return A3DERR_NO_WAVE_DATA;
    DWORD play{0}, write{0};
    buf->GetCurrentPosition(&play, &write);
    *p = toApp(play);
    return S_OK;
}

#define V3SET(field) do { Guard _{engine->lock}; field = {x, y, z}; return S_OK; } while(0)
#define V3GET(field) do { if(!x || !y || !z) return E_INVALIDARG; *x = field.x; *y = field.y; *z = field.z; return S_OK; } while(0)
#define V3SETV(field) do { if(!v) return E_INVALIDARG; Guard _{engine->lock}; field = {v[0], v[1], v[2]}; return S_OK; } while(0)
#define V3GETV(field) do { if(!v) return E_INVALIDARG; v[0] = field.x; v[1] = field.y; v[2] = field.z; return S_OK; } while(0)

HRESULT STDMETHODCALLTYPE Source::SetPosition3f(A3DVAL x, A3DVAL y, A3DVAL z) { V3SET(pos); }
HRESULT STDMETHODCALLTYPE Source::GetPosition3f(A3DVAL *x, A3DVAL *y, A3DVAL *z) { V3GET(pos); }
HRESULT STDMETHODCALLTYPE Source::SetPosition3fv(A3DVAL *v) { V3SETV(pos); }
HRESULT STDMETHODCALLTYPE Source::GetPosition3fv(A3DVAL *v) { V3GETV(pos); }
HRESULT STDMETHODCALLTYPE Source::SetVelocity3f(A3DVAL x, A3DVAL y, A3DVAL z) { V3SET(vel); }
HRESULT STDMETHODCALLTYPE Source::GetVelocity3f(A3DVAL *x, A3DVAL *y, A3DVAL *z) { V3GET(vel); }
HRESULT STDMETHODCALLTYPE Source::SetVelocity3fv(A3DVAL *v) { V3SETV(vel); }
HRESULT STDMETHODCALLTYPE Source::GetVelocity3fv(A3DVAL *v) { V3GETV(vel); }

HRESULT STDMETHODCALLTYPE Source::SetOrientationAngles3f(A3DVAL h, A3DVAL p, A3DVAL r)
{
    Guard _{engine->lock};
    angles[0] = h; angles[1] = p; angles[2] = r;
    anglesToVectors(h, p, r, engine->leftHanded(), front, up);
    return S_OK;
}
HRESULT STDMETHODCALLTYPE Source::GetOrientationAngles3f(A3DVAL *h, A3DVAL *p, A3DVAL *r)
{
    if(!h || !p || !r) return E_INVALIDARG;
    *h = angles[0]; *p = angles[1]; *r = angles[2];
    return S_OK;
}
HRESULT STDMETHODCALLTYPE Source::SetOrientationAngles3fv(A3DVAL *v)
{
    if(!v) return E_INVALIDARG;
    return SetOrientationAngles3f(v[0], v[1], v[2]);
}
HRESULT STDMETHODCALLTYPE Source::GetOrientationAngles3fv(A3DVAL *v)
{
    if(!v) return E_INVALIDARG;
    return GetOrientationAngles3f(&v[0], &v[1], &v[2]);
}
HRESULT STDMETHODCALLTYPE Source::SetOrientation6f(A3DVAL fx, A3DVAL fy, A3DVAL fz, A3DVAL ux, A3DVAL uy, A3DVAL uz)
{
    Guard _{engine->lock};
    front = {fx, fy, fz};
    up = {ux, uy, uz};
    vectorsToAngles(front, up, engine->leftHanded(), angles[0], angles[1], angles[2]);
    return S_OK;
}
HRESULT STDMETHODCALLTYPE Source::GetOrientation6f(A3DVAL *fx, A3DVAL *fy, A3DVAL *fz, A3DVAL *ux, A3DVAL *uy, A3DVAL *uz)
{
    if(!fx || !fy || !fz || !ux || !uy || !uz) return E_INVALIDARG;
    *fx = front.x; *fy = front.y; *fz = front.z;
    *ux = up.x; *uy = up.y; *uz = up.z;
    return S_OK;
}
HRESULT STDMETHODCALLTYPE Source::SetOrientation6fv(A3DVAL *v)
{
    if(!v) return E_INVALIDARG;
    return SetOrientation6f(v[0], v[1], v[2], v[3], v[4], v[5]);
}
HRESULT STDMETHODCALLTYPE Source::GetOrientation6fv(A3DVAL *v)
{
    if(!v) return E_INVALIDARG;
    return GetOrientation6f(&v[0], &v[1], &v[2], &v[3], &v[4], &v[5]);
}

HRESULT STDMETHODCALLTYPE Source::SetCone(A3DVAL inner, A3DVAL outer, A3DVAL g)
{
    Guard _{engine->lock};
    coneInner = std::clamp(inner, 0.0f, 360.0f);
    coneOuter = std::clamp(outer, 0.0f, 360.0f);
    coneGain = std::clamp(g, 0.0f, 1.0f);
    return S_OK;
}
HRESULT STDMETHODCALLTYPE Source::GetCone(A3DVAL *inner, A3DVAL *outer, A3DVAL *g)
{
    if(!inner || !outer || !g) return E_INVALIDARG;
    *inner = coneInner; *outer = coneOuter; *g = coneGain;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Source::SetMinMaxDistance(A3DVAL mn, A3DVAL mx, DWORD b)
{
    Guard _{engine->lock};
    const float upm{engine->unitsPerMeter};
    minDist = std::max(mn / upm, 0.0001f);
    maxDist = std::max(mx / upm, minDist);
    maxMode = b;
    return S_OK;
}
HRESULT STDMETHODCALLTYPE Source::GetMinMaxDistance(A3DVAL *mn, A3DVAL *mx, DWORD *b)
{
    const float upm{engine->unitsPerMeter};
    if(mn) *mn = minDist * upm;
    if(mx) *mx = maxDist * upm;
    if(b) *b = maxMode;
    return S_OK;
}

#define FSET(field, lo, hi) do { Guard _{engine->lock}; field = std::clamp(v, lo, hi); return S_OK; } while(0)
#define FGET(field) do { if(!v) return E_INVALIDARG; *v = field; return S_OK; } while(0)

HRESULT STDMETHODCALLTYPE Source::SetGain(A3DVAL v) { FSET(gain, 0.0f, 1.0f); }
HRESULT STDMETHODCALLTYPE Source::GetGain(A3DVAL *v) { FGET(gain); }
HRESULT STDMETHODCALLTYPE Source::SetPitch(A3DVAL v) { FSET(pitch, 0.01f, 100.0f); }
HRESULT STDMETHODCALLTYPE Source::GetPitch(A3DVAL *v) { FGET(pitch); }
HRESULT STDMETHODCALLTYPE Source::SetDopplerScale(A3DVAL v) { FSET(doppler, 0.0f, 100.0f); }
HRESULT STDMETHODCALLTYPE Source::GetDopplerScale(A3DVAL *v) { FGET(doppler); }
HRESULT STDMETHODCALLTYPE Source::SetDistanceModelScale(A3DVAL v) { FSET(dms, 0.0f, 1e6f); }
HRESULT STDMETHODCALLTYPE Source::GetDistanceModelScale(A3DVAL *v) { FGET(dms); }
HRESULT STDMETHODCALLTYPE Source::SetEq(A3DVAL v) { FSET(eq, 0.0f, 1.0f); }
HRESULT STDMETHODCALLTYPE Source::GetEq(A3DVAL *v) { FGET(eq); }
HRESULT STDMETHODCALLTYPE Source::SetPriority(A3DVAL v) { FSET(priority, 0.0f, 1.0f); }
HRESULT STDMETHODCALLTYPE Source::GetPriority(A3DVAL *v) { FGET(priority); }
HRESULT STDMETHODCALLTYPE Source::SetReflectionDelayScale(A3DVAL v) { FSET(reflDelayScale, 0.0f, 1e6f); }
HRESULT STDMETHODCALLTYPE Source::GetReflectionDelayScale(A3DVAL *v) { FGET(reflDelayScale); }
HRESULT STDMETHODCALLTYPE Source::SetReflectionGainScale(A3DVAL v) { FSET(reflGainScale, 0.0f, 1e6f); }
HRESULT STDMETHODCALLTYPE Source::GetReflectionGainScale(A3DVAL *v) { FGET(reflGainScale); }

HRESULT STDMETHODCALLTYPE Source::SetRenderMode(DWORD m)
{
    Guard _{engine->lock};
    renderMode = m;
    return S_OK;
}
HRESULT STDMETHODCALLTYPE Source::GetRenderMode(DWORD *m)
{
    if(!m) return E_INVALIDARG;
    *m = renderMode;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Source::GetAudibility(A3DVAL *a)
{
    if(!a) return E_INVALIDARG;
    *a = audibility;
    return S_OK;
}
HRESULT STDMETHODCALLTYPE Source::GetOcclusionFactor(A3DVAL *o)
{
    if(!o) return E_INVALIDARG;
    *o = occlusion;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Source::GetStatus(DWORD *s)
{
    if(!s) return E_INVALIDARG;
    Guard _{engine->lock};
    DWORD st{0};
    if(buf && !pendingStop)
    {
        DWORD ds{0};
        buf->GetStatus(&ds);
        if(ds & DSBSTATUS_PLAYING) st |= A3DST_PLAYING;
        if(ds & DSBSTATUS_LOOPING) st |= A3DST_LOOPING;
        if(ds & DSBSTATUS_BUFFERLOST) st |= A3DST_BUFFERLOST;
    }
    if(pendingPlay) st |= A3DST_WAITING_FOR_FLUSH;
    *s = st;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Source::SetPanValues(DWORD n, A3DVAL *g)
{
    if(!g || n == 0) return E_INVALIDARG;
    Guard _{engine->lock};
    pan[0] = std::clamp(g[0], 0.0f, 1.0f);
    pan[1] = std::clamp(n > 1 ? g[1] : g[0], 0.0f, 1.0f);
    return S_OK;
}
HRESULT STDMETHODCALLTYPE Source::GetPanValues(DWORD n, A3DVAL *g)
{
    if(!g) return E_INVALIDARG;
    for(DWORD i{0};i < n;++i) g[i] = pan[std::min<DWORD>(i, 1)];
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Source::SetWaveEvent(DWORD off, HANDLE ev)
{
    Guard _{engine->lock};
    DSBPOSITIONNOTIFY n{};
    n.dwOffset = (off == A3DWAVEEVENT_STOP) ? DSBPN_OFFSETSTOP : off;
    n.hEventNotify = ev;
    events.push_back(n);
    eventsDirty = true;
    return S_OK;
}
HRESULT STDMETHODCALLTYPE Source::ClearWaveEvents()
{
    Guard _{engine->lock};
    events.clear();
    eventsDirty = true;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Source::SetTransformMode(DWORD m)
{
    Guard _{engine->lock};
    xform = m;
    return S_OK;
}
HRESULT STDMETHODCALLTYPE Source::GetTransformMode(DWORD *m)
{
    if(!m) return E_INVALIDARG;
    *m = xform;
    return S_OK;
}

/* -------------------------------------------------------------- Material */

HRESULT STDMETHODCALLTYPE Material::QueryInterface(REFIID riid, void **out)
{
    if(!out) return E_POINTER;
    if(riid == IID_IUnknown || riid == IID_IA3dMaterial_)
    {
        AddRef();
        *out = static_cast<IA3dMaterialX*>(this);
        return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE Material::Release()
{
    const LONG r{--mRef};
    if(r == 0) { delete this; --gObjects; }
    return ULONG(r);
}

namespace {
struct MatBlob { char magic[4]; float rl, rh, tl, th; char name[64]; };
}

HRESULT STDMETHODCALLTYPE Material::Load(char *path)
{
    if(!path) return E_POINTER;
    FILE *f{std::fopen(path, "rb")};
    if(!f) return A3DERR_FILE_OPEN;
    MatBlob b{};
    const size_t n{std::fread(&b, 1, sizeof(b), f)};
    std::fclose(f);
    return UnSerialize(&b, UINT(n));
}

HRESULT STDMETHODCALLTYPE Material::Save(char *path)
{
    if(!path) return E_POINTER;
    void *data{};
    UINT size{};
    Serialize(&data, &size);
    FILE *f{std::fopen(path, "wb")};
    if(!f) return A3DERR_FILE_OPEN;
    std::fwrite(data, 1, size, f);
    std::fclose(f);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Material::UnSerialize(void *data, UINT size)
{
    if(!data || size < sizeof(MatBlob)) return E_INVALIDARG;
    MatBlob b{};
    std::memcpy(&b, data, sizeof(b));
    if(std::memcmp(b.magic, "A3DM", 4) != 0) return A3DERR_FORMAT;
    props = {b.rl, b.rh, b.tl, b.th};
    std::memcpy(name, b.name, sizeof(name));
    name[sizeof(name)-1] = '\0';
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Material::Serialize(void **data, UINT *size)
{
    if(!data || !size) return E_POINTER;
    MatBlob b{{'A','3','D','M'}, props.reflLow, props.reflHigh, props.transLow, props.transHigh, {}};
    std::memcpy(b.name, name, sizeof(b.name));
    mBlob.assign(reinterpret_cast<unsigned char*>(&b), reinterpret_cast<unsigned char*>(&b) + sizeof(b));
    *data = mBlob.data();
    *size = UINT(mBlob.size());
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Material::Duplicate(IA3dMaterialX **out)
{
    if(!out) return E_POINTER;
    auto *m = new Material{};
    ++gObjects;
    m->props = props;
    std::memcpy(m->name, name, sizeof(name));
    *out = m;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Material::SetNameID(char *n)
{
    if(!n) return E_POINTER;
    snprintf(name, sizeof(name), "%s", n);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Material::GetNameID(char *n, INT size)
{
    if(!n || size <= 0) return E_INVALIDARG;
    snprintf(n, size_t(size), "%s", name);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Material::SetReflectance(A3DVAL g, A3DVAL hf)
{
    props.reflLow = std::clamp(g, 0.0f, 1.0f);
    props.reflHigh = std::clamp(hf, 0.0f, 1.0f);
    return S_OK;
}
HRESULT STDMETHODCALLTYPE Material::GetReflectance(A3DVAL *g, A3DVAL *hf)
{
    if(g) *g = props.reflLow;
    if(hf) *hf = props.reflHigh;
    return S_OK;
}
HRESULT STDMETHODCALLTYPE Material::SetTransmittance(A3DVAL g, A3DVAL hf)
{
    props.transLow = std::clamp(g, 0.0f, 1.0f);
    props.transHigh = std::clamp(hf, 0.0f, 1.0f);
    return S_OK;
}
HRESULT STDMETHODCALLTYPE Material::GetTransmittance(A3DVAL *g, A3DVAL *hf)
{
    if(g) *g = props.transLow;
    if(hf) *hf = props.transHigh;
    return S_OK;
}

/* ------------------------------------------------------------------ List */

List::List(Engine *e) : mEngine{e}
{
    mEngine->AddRef();
    ++gObjects;
}

List::~List()
{
    {
        Guard _{mEngine->lock};
        if(mEngine->recording == this)
        {
            mEngine->recording = nullptr;
            mEngine->stack = mEngine->savedStack;
        }
    }
    mEngine->Release();
    --gObjects;
}

HRESULT STDMETHODCALLTYPE List::QueryInterface(REFIID riid, void **out)
{
    if(!out) return E_POINTER;
    if(riid == IID_IUnknown || riid == IID_IA3dList_)
    {
        AddRef();
        *out = static_cast<IA3dListX*>(this);
        return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE List::Release()
{
    const LONG r{--mRef};
    if(r == 0) delete this;
    return ULONG(r);
}

/* Recording stores polygons in the list's own coordinates: the matrix stack
 * starts from identity and is restored afterwards. Call places them with the
 * matrix current at the time of the call. */
HRESULT STDMETHODCALLTYPE List::Begin()
{
    Guard _{mEngine->lock};
    if(mEngine->recording) return E_FAIL;
    polys.clear();
    mEngine->savedStack = mEngine->stack;
    mEngine->stack.assign(1, M4::identity());
    mEngine->recording = this;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE List::End()
{
    Guard _{mEngine->lock};
    if(mEngine->recording != this) return E_FAIL;
    mEngine->recording = nullptr;
    mEngine->stack = mEngine->savedStack;
    return HRESULT(polys.size());
}

HRESULT STDMETHODCALLTYPE List::Call()
{
    Guard _{mEngine->lock};
    if(mEngine->recording) return E_FAIL;
    const M4 &m{mEngine->stack.back()};
    auto &out = mEngine->back;
    const int base{int(out.size())};
    for(const Poly &p : polys)
    {
        Poly q{p};
        for(int i{0};i < q.count;++i)
            q.v[i] = m.point(p.v[i]);
        const V3 n{normalize(cross(q.v[1] - q.v[0], q.v[2] - q.v[0]))};
        q.normal = (length(n) > 0.5f) ? n : normalize(m.dir(p.normal));
        if(q.parent >= 0) q.parent += base;
        out.push_back(q);
    }
    return S_OK;
}

} // namespace a3d
