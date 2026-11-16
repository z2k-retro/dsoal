// Drives a3dapi.dll through the A3D 3.0 interfaces (IA3d5, IA3dSource2,
// IA3dReverb, IA3dReflection) and prints a timeline, so the rendered WAV can
// be checked against it with a3d3_analyze.py. A short noise burst plays once
// a second from a looping source, through these phases (3 s each):
//   dry        no reverb bound
//   hangar     preset 10 (hangar) bound: long tails after each burst
//   mix0       same, source reverb mix 0: dry again
//   custom     custom (I3DL2) reverb, 2.5 s decay
//   inside     reverb off; volumetric box around the listener, source off to the right
//   right      the same source as a point: heard on the right
//   echo       manual reflection 0.4 s late, hard left, from a source in front
// An optional argument names an MP3 to load with LoadFile (format and length
// are printed).
#include "../a3d_decl.h"
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

using GetClassObjectFn = HRESULT(WINAPI*)(REFCLSID, REFIID, void**);

static std::chrono::steady_clock::time_point t0;
static double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); }

static void writeTestWave(const char *path)
{
    const int rate{22050}, n{rate};
    std::vector<int16_t> pcm(size_t(n), 0);
    uint32_t seed{12345};
    for(int i{0};i < rate/20;++i)
    {
        seed = seed*1664525u + 1013904223u;
        pcm[size_t(i)] = int16_t(int(seed >> 16) - 32768) / 2;
    }
    FILE *f{std::fopen(path, "wb")};
    const uint32_t data{uint32_t(n*2)}, riff{36 + data}, fmtLen{16}, sr{uint32_t(rate)}, br{uint32_t(rate*2)};
    const uint16_t tag{1}, ch{1}, align{2}, bits{16};
    std::fwrite("RIFF", 1, 4, f); std::fwrite(&riff, 4, 1, f); std::fwrite("WAVEfmt ", 1, 8, f);
    std::fwrite(&fmtLen, 4, 1, f); std::fwrite(&tag, 2, 1, f); std::fwrite(&ch, 2, 1, f);
    std::fwrite(&sr, 4, 1, f); std::fwrite(&br, 4, 1, f); std::fwrite(&align, 2, 1, f);
    std::fwrite(&bits, 2, 1, f); std::fwrite("data", 1, 4, f); std::fwrite(&data, 4, 1, f);
    std::fwrite(pcm.data(), 2, pcm.size(), f);
    std::fclose(f);
}

static void run(IA3d5X *a3d, double seconds)
{
    const double end{now() + seconds};
    while(now() < end) { a3d->Flush(); Sleep(20); }
}

int main(int argc, char **argv)
{
    CoInitialize(nullptr);
    HMODULE m{LoadLibraryA("a3dapi.dll")};
    if(!m) { printf("no a3dapi.dll\n"); return 1; }
    auto gco = reinterpret_cast<GetClassObjectFn>(reinterpret_cast<void*>(GetProcAddress(m, "DllGetClassObject")));
    IClassFactory *cf{};
    if(FAILED(gco(CLSID_A3dApi_, IID_IClassFactory, reinterpret_cast<void**>(&cf)))) { printf("no factory\n"); return 1; }
    IA3d5X *a3d{};
    HRESULT hr{cf->CreateInstance(nullptr, IID_IA3d5_, reinterpret_cast<void**>(&a3d))};
    printf("CreateInstance(IA3d5) %08lx\n", hr);
    if(FAILED(hr)) return 1;

    hr = a3d->InitEx(nullptr, A3DF_REVERB | A3DF_1ST_REFLECTIONS | A3DF_OCCLUSIONS, 0, GetDesktopWindow(), 1);
    printf("InitEx %08lx reverb available %ld\n", hr, long(a3d->IsFeatureAvailable(A3DF_REVERB)));

    IA3dListenerX *lis{};
    a3d->QueryInterface(IID_IA3dListener_, reinterpret_cast<void**>(&lis));
    IA3dGeom2X *geom{};
    printf("QI IA3dGeom2 %08lx\n", a3d->QueryInterface(IID_IA3dGeom2_, reinterpret_cast<void**>(&geom)));
    lis->SetPosition3f(0, 0, 0);
    lis->SetOrientation6f(0, 0, -1, 0, 1, 0);

    if(argc > 1)
    {
        IA3dSourceX *s1{};
        a3d->NewSource(0, &s1);
        IA3dSource2X *mp3{};
        s1->QueryInterface(IID_IA3dSource2_, reinterpret_cast<void**>(&mp3));
        hr = mp3->LoadFile(argv[1], A3DFMT_AUTO);
        A3DCapsSource caps{};
        caps.dwSize = sizeof(caps);
        HRESULT hc{mp3->GetCaps(&caps)};
        printf("LoadFile(%s) %08lx GetCaps %08lx type %lu: MPEG %d layer %d, %d Hz, %d ch, %d bit/s, %.2f s\n",
            argv[1], hr, hc, caps.dwType, caps.data.mp3Info.nMpegVersion, caps.data.mp3Info.nMpegLayer,
            caps.data.mp3Info.nSamplerate, caps.data.mp3Info.nChannels, caps.data.mp3Info.nBitrate,
            double(caps.data.mp3Info.fTotalPlayLength));
        mp3->Release();
        s1->Release();
    }

    writeTestWave("burst.wav");
    IA3dSourceX *s0{};
    a3d->NewSource(0, &s0);
    IA3dSource2X *src{};
    printf("QI IA3dSource2 %08lx\n", s0->QueryInterface(IID_IA3dSource2_, reinterpret_cast<void**>(&src)));
    s0->Release();
    printf("LoadFile(burst.wav) %08lx\n", src->LoadFile(const_cast<char*>("burst.wav"), A3DFMT_WAVE));
    src->SetPosition3f(0, 0, -2);
    src->SetMinMaxDistance(1, 1000, A3DMAX_AUDIBLE);

    IA3dReverbX *rv{};
    printf("NewReverb %08lx\n", a3d->NewReverb(&rv));
    if(!rv) { printf("no reverb object\n"); return 1; }
    A3DVAL v{}, d{}, dm{};
    rv->SetReverbPreset(10);
    rv->GetPresetVolume(&v); rv->GetPresetDecayTime(&d); rv->GetPresetDamping(&dm);
    printf("hangar preset: volume %.3f decay %.3f damping %.3f\n", double(v), double(d), double(dm));

    t0 = std::chrono::steady_clock::now();
    src->Play(A3DPLAY_LOOPED);
    printf("PHASE dry %.3f\n", now()); fflush(stdout);
    run(a3d, 3.0);

    printf("BindReverb %08lx\n", a3d->BindReverb(rv));
    printf("PHASE hangar %.3f\n", now()); fflush(stdout);
    run(a3d, 3.0);

    printf("SetReverbMix(0,0) %08lx\n", src->SetReverbMix(0.0f, 0.0f));
    printf("PHASE mix0 %.3f\n", now()); fflush(stdout);
    run(a3d, 3.0);

    src->SetReverbMix(1.0f, 0.0f);
    A3DReverbProperties p{};
    p.dwSize = sizeof(p);
    p.dwType = A3DREVERBTYPE_CUSTOM;
    A3DReverbCustom &c{p.uval.custom};
    c = {sizeof(c), -1000, -100, 0.0f, 2.5f, 0.8f, -1000, 0.02f, 200, 0.03f, 100.0f, 100.0f, 5000.0f};
    printf("SetAllProperties(custom) %08lx\n", rv->SetAllProperties(&p));
    printf("PHASE custom %.3f\n", now()); fflush(stdout);
    run(a3d, 3.0);

    printf("BindReverb(null) %08lx\n", a3d->BindReverb(nullptr));
    src->SetPosition3f(4, 0, 0);
    printf("SetVolumetricBounds %08lx\n", src->SetVolumetricBounds(12, 4, 12));
    printf("PHASE inside %.3f\n", now()); fflush(stdout);
    run(a3d, 3.0);

    src->SetVolumetricBounds(0, 0, 0);
    printf("PHASE right %.3f\n", now()); fflush(stdout);
    run(a3d, 3.0);

    src->SetPosition3f(0, 0, -2);
    IA3dReflectionX *r{};
    printf("NewManualReflection %08lx\n", src->NewManualReflection(&r));
    r->SetPosition3f(-2, 0, 0);
    r->SetDelay(0.4f);
    r->SetGainScale(1.0f);
    int nr{};
    src->GetNumManualReflections(&nr);
    printf("manual reflections %d\n", nr);
    printf("PHASE echo %.3f\n", now()); fflush(stdout);
    run(a3d, 3.0);

    printf("PHASE end %.3f\n", now()); fflush(stdout);
    src->Stop();
    a3d->Flush();
    r->Release();
    rv->Release();
    src->Release();
    geom->Release();
    lis->Release();
    printf("final release %lu\n", a3d->Release());
    return 0;
}
