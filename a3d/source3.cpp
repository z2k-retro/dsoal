/* A3D 3.0 additions to sources: IA3dSource2's new methods (volumetric
 * bounds, reverb mix, manual reflections, LoadFile with MP3) and
 * IA3dReflection. */
#include "engine.h"

#include <mmreg.h>
#include <msacm.h>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace a3d {

namespace {

using Guard = std::lock_guard<std::recursive_mutex>;

/* ------------------------------------------------------------------ MP3 */

struct Mp3Frame {
    int version;    /* 1 = MPEG-1, 2 = MPEG-2, 25 = MPEG-2.5 */
    int bitrate;    /* kbit/s */
    int rate;
    int channels;
};

/* Reads the MPEG audio frame header at p, if there is a Layer III one. */
bool parseMp3Header(const unsigned char *p, Mp3Frame &f)
{
    if(p[0] != 0xff || (p[1] & 0xe0) != 0xe0) return false;
    const int ver{(p[1] >> 3) & 3};
    const int layer{(p[1] >> 1) & 3};
    const int bri{p[2] >> 4};
    const int sri{(p[2] >> 2) & 3};
    if(ver == 1 || layer != 1 || bri == 0 || bri == 15 || sri == 3) return false;
    static constexpr int br1[15]{0,32,40,48,56,64,80,96,112,128,160,192,224,256,320};
    static constexpr int br2[15]{0,8,16,24,32,40,48,56,64,80,96,112,128,144,160};
    static constexpr int sr[3]{44100, 48000, 32000};
    f.version = ver == 3 ? 1 : ver == 2 ? 2 : 25;
    f.bitrate = f.version == 1 ? br1[bri] : br2[bri];
    f.rate = sr[sri] >> (f.version == 1 ? 0 : f.version == 2 ? 1 : 2);
    f.channels = (p[3] >> 6) == 3 ? 1 : 2;
    return true;
}

/* Decodes an MP3 file held in memory to 16-bit PCM with the system's MP3
 * codec (the ACM decoder every Windows since 98 has, and Wine). The A3D 3.0
 * library decodes static MP3 sources into memory the same way; streamed ones
 * are decoded whole here as well. */
bool decodeMp3(const unsigned char *data, size_t size, WAVEFORMATEX &out,
    std::vector<unsigned char> &pcm, A3DSourceMp3Info &info)
{
    size_t off{0};
    if(size >= 10 && std::memcmp(data, "ID3", 3) == 0)
    {
        off = 10 + (size_t(data[6] & 0x7f) << 21 | size_t(data[7] & 0x7f) << 14
            | size_t(data[8] & 0x7f) << 7 | size_t(data[9] & 0x7f));
        if(data[5] & 0x10) off += 10;
    }
    Mp3Frame f{};
    while(off + 4 <= size && !parseMp3Header(data + off, f))
        ++off;
    if(off + 4 > size) return false;

    MPEGLAYER3WAVEFORMAT in{};
    in.wfx.wFormatTag = WAVE_FORMAT_MPEGLAYER3;
    in.wfx.nChannels = WORD(f.channels);
    in.wfx.nSamplesPerSec = DWORD(f.rate);
    in.wfx.nAvgBytesPerSec = DWORD(f.bitrate) * 125;
    in.wfx.nBlockAlign = 1;
    in.wfx.cbSize = MPEGLAYER3_WFX_EXTRA_BYTES;
    in.wID = MPEGLAYER3_ID_MPEG;
    in.fdwFlags = MPEGLAYER3_FLAG_PADDING_OFF;
    in.nBlockSize = WORD((f.version == 1 ? 144 : 72) * f.bitrate * 1000 / f.rate);
    in.nFramesPerBlock = 1;
    in.nCodecDelay = 0;

    out = {};
    out.wFormatTag = WAVE_FORMAT_PCM;
    out.nChannels = WORD(f.channels);
    out.nSamplesPerSec = DWORD(f.rate);
    out.wBitsPerSample = 16;
    out.nBlockAlign = WORD(2 * f.channels);
    out.nAvgBytesPerSec = out.nSamplesPerSec * out.nBlockAlign;

    HACMSTREAM h{};
    if(MMRESULT r{acmStreamOpen(&h, nullptr, &in.wfx, &out, nullptr, 0, 0,
        ACM_STREAMOPENF_NONREALTIME)}; r != 0)
    {
        logf("LoadFile: no MP3 decoder (acmStreamOpen %u)", r);
        return false;
    }

    constexpr DWORD chunk{16384};
    DWORD outMax{0};
    acmStreamSize(h, chunk, &outMax, ACM_STREAMSIZEF_SOURCE);
    outMax = std::max<DWORD>(outMax, chunk * 16);
    std::vector<unsigned char> src(chunk), dst(outMax);
    DWORD have{0};
    bool ok{true};
    for(bool last{false};!last;)
    {
        const size_t take{std::min<size_t>(chunk - have, size - off)};
        std::memcpy(src.data() + have, data + off, take);
        off += take;
        have += DWORD(take);
        last = off >= size;

        ACMSTREAMHEADER hdr{};
        hdr.cbStruct = sizeof(hdr);
        hdr.pbSrc = src.data();
        hdr.cbSrcLength = have;
        hdr.pbDst = dst.data();
        hdr.cbDstLength = outMax;
        if(acmStreamPrepareHeader(h, &hdr, 0) != 0) { ok = false; break; }
        const MMRESULT r{acmStreamConvert(h, &hdr,
            ACM_STREAMCONVERTF_BLOCKALIGN | (last ? ACM_STREAMCONVERTF_END : 0))};
        acmStreamUnprepareHeader(h, &hdr, 0);
        if(r != 0) { ok = false; break; }
        pcm.insert(pcm.end(), dst.data(), dst.data() + hdr.cbDstLengthUsed);
        const DWORD left{have - hdr.cbSrcLengthUsed};
        std::memmove(src.data(), src.data() + hdr.cbSrcLengthUsed, left);
        if(hdr.cbSrcLengthUsed == 0 && hdr.cbDstLengthUsed == 0 && !last && left == chunk)
        { ok = false; break; }  /* not consuming anything: give up */
        have = left;
    }
    acmStreamClose(h, 0);
    if(!ok || pcm.empty())
    {
        logf("LoadFile: MP3 decode failed");
        return false;
    }

    info = {};
    info.dwSize = sizeof(info);
    info.nMpegLayer = 3;
    info.nMpegVersion = f.version == 25 ? 3 : f.version;
    info.nBitrate = f.bitrate * 1000;
    info.nChannels = f.channels;
    info.nSamplerate = f.rate;
    info.nBitsPerSample = 16;
    info.fTotalPlayLength = float(pcm.size() / out.nBlockAlign) / float(f.rate);
    return true;
}

DWORD detectFormat(const unsigned char *d, size_t n)
{
    if(n >= 12 && std::memcmp(d, "RIFF", 4) == 0 && std::memcmp(d+8, "WAVE", 4) == 0)
        return A3DFMT_WAVE;
    if(n >= 2 && d[0] == 0x0b && d[1] == 0x77)
        return A3DFMT_AC3;
    if(n >= 3 && std::memcmp(d, "ID3", 3) == 0)
        return A3DFMT_MP3;
    Mp3Frame f{};
    if(n >= 4 && parseMp3Header(d, f))
        return A3DFMT_MP3;
    return A3DFMT_AUTO;
}

/* Turns IA3dSource2::SetReverbMix's HiFreq (0 = no low-pass, 1 = full)
 * into the fraction of high frequencies removed. The curve is the
 * polynomial Aureal's library applies before passing the value on. */
float hiFreqCurve(float h)
{
    static constexpr float c[7]{3.72644f, -17.4498f, 43.6306f, -52.9992f,
        27.0877f, 0.777417f, -3.77317f};
    float v{0.0f}, p{h};
    for(float k : c) { v += k * p; p *= h; }
    return std::clamp(v, 0.0f, 1.0f);
}

} // namespace

/* ----------------------------------------------------------- IA3dSource2 */

HRESULT STDMETHODCALLTYPE Source::SetVolumetricBounds(A3DVAL dx, A3DVAL dy, A3DVAL dz)
{
    if(!(dx >= 0.0f) || !(dy >= 0.0f) || !(dz >= 0.0f)) return E_INVALIDARG;
    Guard _{engine->lock};
    volBounds[0] = dx; volBounds[1] = dy; volBounds[2] = dz;
    return S_OK;
}
HRESULT STDMETHODCALLTYPE Source::GetVolumetricBounds(A3DVAL *dx, A3DVAL *dy, A3DVAL *dz)
{
    if(!dx || !dy || !dz) return E_POINTER;
    *dx = volBounds[0]; *dy = volBounds[1]; *dz = volBounds[2];
    return S_OK;
}

/* Kept and returned; the damping settings shape how A3D occludes a box
 * partly hidden behind a wall, which is not modelled here (a volumetric
 * source is occluded along the line to its nearest point). */
HRESULT STDMETHODCALLTYPE Source::SetVolumetricDamping(A3DVolSrcDampInfo *i)
{
    if(!i) return E_POINTER;
    if(i->dwSize < sizeof(A3DVolSrcDampInfo) || i->nTestPointsMax < 1) return E_INVALIDARG;
    volDamp = *i;
    return S_OK;
}
HRESULT STDMETHODCALLTYPE Source::GetVolumetricDamping(A3DVolSrcDampInfo *i)
{
    if(!i) return E_POINTER;
    if(i->dwSize < sizeof(A3DVolSrcDampInfo)) return E_INVALIDARG;
    *i = volDamp;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Source::SetReverbMix(A3DVAL level, A3DVAL hiFreq)
{
    if(!(level >= 0.0f && level <= 1.0f) || !(hiFreq >= 0.0f && hiFreq <= 1.0f))
        return A3DERR_INVALID_ARG;
    if(!(engine->requested & A3DF_REVERB)) return A3DERR_FEATURE_NOT_REQUESTED;
    Guard _{engine->lock};
    revLevel = level;
    revHiFreq = hiFreq;
    revBright = 1.0f - hiFreqCurve(hiFreq);
    return (renderMode & A3DRM_NATIVE) ? A3DERR_SOURCE_IN_NATIVE_MODE : S_OK;
}
HRESULT STDMETHODCALLTYPE Source::GetReverbMix(A3DVAL *level, A3DVAL *hiFreq)
{
    if(!level || !hiFreq) return E_POINTER;
    if(!(engine->requested & A3DF_REVERB)) return A3DERR_FEATURE_NOT_REQUESTED;
    *level = revLevel;
    *hiFreq = revHiFreq;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Source::NewManualReflection(IA3dReflectionX **out)
{
    if(!out) return E_POINTER;
    Guard _{engine->lock};
    auto *r = new Reflection{engine, this, engine->nextReflId++};
    manual.push_back(r);
    *out = r;
    return S_OK;
}

void Source::freeManual()
{
    for(Reflection *r : manual)
        r->owner = nullptr;
    manual.clear();
}

HRESULT STDMETHODCALLTYPE Source::FreeManualReflections()
{
    Guard _{engine->lock};
    freeManual();
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Source::GetNumManualReflections(int *n)
{
    if(!n) return E_POINTER;
    *n = int(manual.size());
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Source::LoadFile(char *path, DWORD format)
{
    if(!path) return E_POINTER;
    FILE *f{std::fopen(path, "rb")};
    if(!f)
    {
        logf("LoadFile: cannot open %s", path);
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

    DWORD kind{format & 0xffffu};
    if(kind == A3DFMT_AUTO)
        kind = detectFormat(data.data(), data.size());
    logf("LoadFile(%s, %#lx): %s%s", path, format,
        kind == A3DFMT_WAVE ? "WAV" : kind == A3DFMT_MP3 ? "MP3" : kind == A3DFMT_AC3 ? "AC-3" : "unknown",
        (format & A3DFMT_STREAMING) ? ", streamed (loaded whole here)" : "");

    HRESULT hr{A3DERR_FORMAT};
    if(kind == A3DFMT_WAVE)
        hr = LoadWaveData(data.data(), DWORD(data.size()));
    else if(kind == A3DFMT_MP3)
    {
        WAVEFORMATEX wf{};
        std::vector<unsigned char> pcm;
        A3DSourceMp3Info info{};
        if(!decodeMp3(data.data(), data.size(), wf, pcm, info))
            return A3DERR_FORMAT;
        Guard _{engine->lock};
        fmt = wf;
        haveFmt = true;
        hr = createBuffer(DWORD(pcm.size()));
        if(SUCCEEDED(hr))
            hr = fillFromApp(pcm.data(), DWORD(pcm.size()));
        if(SUCCEEDED(hr))
            mp3Info = info;
    }
    else if(kind == A3DFMT_AC3)
        logf("LoadFile: AC-3 is not supported");

    if(SUCCEEDED(hr))
    {
        Guard _{engine->lock};
        filePath = path;
        fileType = kind;
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE Source::GetCaps(A3DCapsSource *caps)
{
    if(!caps) return E_POINTER;
    if(caps->dwSize < offsetof(A3DCapsSource, data)) return E_INVALIDARG;
    if(!haveFmt) return A3DERR_NO_WAVE_DATA;
    const DWORD size{std::min<DWORD>(caps->dwSize, sizeof(A3DCapsSource))};
    A3DCapsSource c{};
    c.dwSize = caps->dwSize;
    c.dwType = fileType;
    c.szFilename = filePath.empty() ? nullptr : filePath.data();
    if(fileType == A3DFMT_MP3)
        c.data.mp3Info = mp3Info;
    else
    {
        c.data.waveFormat.dwSize = sizeof(A3DSourceWaveFormat);
        c.data.waveFormat.nChannels = fmt.nChannels;
        c.data.waveFormat.nSamplesPerSec = fmt.nSamplesPerSec;
        c.data.waveFormat.nAvgBytesPerSec = fmt.nAvgBytesPerSec;
        c.data.waveFormat.nBlockAlign = fmt.nBlockAlign;
        c.data.waveFormat.wBitsPerSample = fmt.wBitsPerSample;
    }
    std::memcpy(caps, &c, size);
    return S_OK;
}

/* --------------------------------------------------------- IA3dReflection */

Reflection::Reflection(Engine *e, Source *s, DWORD i) : engine{e}, owner{s}, id{i}
{
    engine->AddRef();
    ++gObjects;
}

Reflection::~Reflection()
{
    engine->Release();
    --gObjects;
}

void Reflection::detach()
{
    if(!owner) return;
    auto &v = owner->manual;
    v.erase(std::remove(v.begin(), v.end(), this), v.end());
    owner = nullptr;
}

HRESULT STDMETHODCALLTYPE Reflection::QueryInterface(REFIID riid, void **out)
{
    if(!out) return E_POINTER;
    if(riid == IID_IUnknown || riid == IID_IA3dReflection_)
    {
        AddRef();
        *out = static_cast<IA3dReflectionX*>(this);
        return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE Reflection::Release()
{
    const LONG r{--mRef};
    if(r == 0)
    {
        {
            Guard _{engine->lock};
            detach();
        }
        delete this;
    }
    return ULONG(r);
}

HRESULT STDMETHODCALLTYPE Reflection::SetGainScale(A3DVAL g)
{
    if(!(g >= 0.0f)) return E_INVALIDARG;
    Guard _{engine->lock};
    gainScale = g;
    return S_OK;
}
HRESULT STDMETHODCALLTYPE Reflection::GetGainScale(A3DVAL *g)
{ if(!g) return E_POINTER; *g = gainScale; return S_OK; }

HRESULT STDMETHODCALLTYPE Reflection::SetDelay(A3DVAL t)
{
    if(!(t >= 0.0f)) return E_INVALIDARG;
    Guard _{engine->lock};
    delay = t;
    return S_OK;
}
HRESULT STDMETHODCALLTYPE Reflection::GetDelay(A3DVAL *t)
{ if(!t) return E_POINTER; *t = delay; return S_OK; }

HRESULT STDMETHODCALLTYPE Reflection::SetPosition3f(A3DVAL x, A3DVAL y, A3DVAL z)
{
    Guard _{engine->lock};
    pos = {x, y, z};
    return S_OK;
}
HRESULT STDMETHODCALLTYPE Reflection::GetPosition3f(A3DVAL *x, A3DVAL *y, A3DVAL *z)
{
    if(!x || !y || !z) return E_POINTER;
    *x = pos.x; *y = pos.y; *z = pos.z;
    return S_OK;
}
HRESULT STDMETHODCALLTYPE Reflection::SetPosition3fv(A3DVAL *v)
{ if(!v) return E_POINTER; return SetPosition3f(v[0], v[1], v[2]); }
HRESULT STDMETHODCALLTYPE Reflection::GetPosition3fv(A3DVAL *v)
{ if(!v) return E_POINTER; return GetPosition3f(&v[0], &v[1], &v[2]); }

HRESULT STDMETHODCALLTYPE Reflection::SetTransformMode(DWORD m)
{
    if(m != A3DXFORM_NORMAL && m != A3DXFORM_HEADRELATIVE) return E_INVALIDARG;
    Guard _{engine->lock};
    xform = m;
    return S_OK;
}
HRESULT STDMETHODCALLTYPE Reflection::GetTransformMode(DWORD *m)
{ if(!m) return E_POINTER; *m = xform; return S_OK; }

HRESULT STDMETHODCALLTYPE Reflection::SetEQ(A3DVAL e)
{
    if(!(e >= 0.0f && e <= 1.0f)) return E_INVALIDARG;
    Guard _{engine->lock};
    eq = e;
    return S_OK;
}
HRESULT STDMETHODCALLTYPE Reflection::GetEQ(A3DVAL *e)
{ if(!e) return E_POINTER; *e = eq; return S_OK; }

} // namespace a3d
