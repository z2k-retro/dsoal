// Drives a3dapi.dll through IA3d4 / IA3dGeom / IA3dSource and prints a timeline
// of what it did, so the rendered WAV can be checked against it.
#include "../a3d_decl.h"
#include <cstdio>
#include <cmath>
#include <cstdint>
#include <vector>

using GetClassObjectFn = HRESULT(WINAPI*)(REFCLSID, REFIID, void**);

static void wait_ms(DWORD ms) { Sleep(ms); }

int main(int argc, char **argv)
{
    CoInitialize(nullptr);
    HMODULE m{LoadLibraryA("a3dapi.dll")};
    if(!m) { printf("no a3dapi.dll\n"); return 1; }
    auto gco = reinterpret_cast<GetClassObjectFn>(reinterpret_cast<void*>(GetProcAddress(m, "DllGetClassObject")));
    IClassFactory *cf{};
    if(FAILED(gco(CLSID_A3dApi_, IID_IClassFactory, reinterpret_cast<void**>(&cf)))) { printf("no factory\n"); return 1; }
    IA3d4X *a3d{};
    HRESULT hr{cf->CreateInstance(nullptr, IID_IA3d4_, reinterpret_cast<void**>(&a3d))};
    printf("CreateInstance %08lx\n", hr);
    hr = a3d->Init(nullptr, A3DF_OCCLUSIONS | A3DF_1ST_REFLECTIONS, 0);
    printf("Init %08lx occl=%ld refl=%ld\n", hr, a3d->IsFeatureAvailable(A3DF_OCCLUSIONS),
        a3d->IsFeatureAvailable(A3DF_1ST_REFLECTIONS));
    IA3dGeomX *geom{};
    IA3dListenerX *lis{};
    a3d->QueryInterface(IID_IA3dGeom_, reinterpret_cast<void**>(&geom));
    a3d->QueryInterface(IID_IA3dListener_, reinterpret_cast<void**>(&lis));

    /* 2 s of white noise, mono 22050 Hz 16-bit, looped. A click track on a
     * second source for the reflection test. */
    IA3dSourceX *src{};
    a3d->NewSource(0, &src);
    WAVEFORMATEX wf{WAVE_FORMAT_PCM, 1, 22050, 44100, 2, 16, 0};
    src->SetWaveFormat(&wf);
    const DWORD bytes{22050 * 2 * 2};
    hr = src->AllocateWaveData(bytes);
    printf("Allocate %08lx size %ld\n", hr, src->GetWaveSize());
    void *p1{}, *p2{};
    DWORD n1{}, n2{};
    src->Lock(0, 0, &p1, &n1, &p2, &n2, A3DLOCK_ENTIREBUFFER);
    uint32_t seed{12345};
    auto *s16 = static_cast<int16_t*>(p1);
    for(DWORD i{0};i < n1/2;++i)
    {
        seed = seed*1664525u + 1013904223u;
        s16[i] = int16_t(int32_t(seed >> 16) - 32768) / 4;
    }
    src->Unlock(p1, n1, p2, n2);

    IA3dSourceX *clk{};
    a3d->NewSource(0, &clk);
    clk->SetWaveFormat(&wf);
    clk->AllocateWaveData(22050 * 2 / 2); /* 0.5 s: one click at the start */
    clk->Lock(0, 0, &p1, &n1, &p2, &n2, A3DLOCK_ENTIREBUFFER);
    s16 = static_cast<int16_t*>(p1);
    for(DWORD i{0};i < n1/2;++i) s16[i] = (i < 40) ? int16_t((i & 1) ? 20000 : -20000) : 0;
    clk->Unlock(p1, n1, p2, n2);

    lis->SetPosition3f(0, 0, 0);
    lis->SetOrientationAngles3f(0, 0, 0);
    src->SetMinMaxDistance(1.0f, 100.0f, A3DMAX_AUDIBLE);
    src->Play(A3DPLAY_LOOPED);

    IA3dMaterialX *wallMat{};
    geom->NewMaterial(&wallMat);
    wallMat->SetTransmittance(0.25f, 0.05f);
    wallMat->SetReflectance(0.9f, 0.9f);

    const DWORD t0{GetTickCount()};
    auto now = [&]{ return float(GetTickCount() - t0) / 1000.0f; };
    /* Phase 1 (0-4 s): source circles the listener at 2 m, starting in front
     * (-z), going clockwise seen from above: front, right, back, left. */
    printf("PHASE sweep %.3f\n", now());
    while(now() < 4.0f)
    {
        const float a{now() / 4.0f * 6.2831853f};
        a3d->Clear();
        src->SetPosition3f(2.0f*std::sin(a), 0.0f, -2.0f*std::cos(a));
        a3d->Flush();
        wait_ms(10);
    }
    /* Phase 2 (4-6 s): source to the right; at 5 s a wall between. */
    printf("PHASE right %.3f\n", now());
    geom->Enable(A3DF_OCCLUSIONS);
    bool said{false};
    while(now() < 6.0f)
    {
        a3d->Clear();
        src->SetPosition3f(3.0f, 0.0f, 0.0f);
        if(now() >= 5.0f)
        {
            if(!said) { printf("PHASE wall %.3f\n", now()); said = true; }
            geom->BindMaterial(wallMat);
            geom->Begin(A3DP_QUADS);
            geom->Vertex3f(1.5f, -2, -2);
            geom->Vertex3f(1.5f, 2, -2);
            geom->Vertex3f(1.5f, 2, 2);
            geom->Vertex3f(1.5f, -2, 2);
            geom->End();
        }
        a3d->Flush();
        wait_ms(10);
    }
    /* Phase 3 (6-7 s): wall with an open door (subface, opening 1). */
    printf("PHASE door %.3f\n", now());
    float door{1.0f};
    while(now() < 7.0f)
    {
        a3d->Clear();
        geom->BindMaterial(wallMat);
        geom->Begin(A3DP_QUADS);
        geom->Vertex3f(1.5f, -2, -2); geom->Vertex3f(1.5f, 2, -2);
        geom->Vertex3f(1.5f, 2, 2); geom->Vertex3f(1.5f, -2, 2);
        geom->End();
        geom->SetOpeningFactorfv(&door);
        geom->Begin(A3DP_SUBFACE | A3DP_QUADS);
        geom->Vertex3f(1.5f, -1, -1); geom->Vertex3f(1.5f, 1, -1);
        geom->Vertex3f(1.5f, 1, 1); geom->Vertex3f(1.5f, -1, 1);
        geom->End();
        a3d->Flush();
        wait_ms(10);
    }
    src->Stop();
    a3d->Flush();
    geom->Disable(A3DF_OCCLUSIONS);
    /* Phase 4 (7.5-9.5 s): click in front, 2 m; reflecting wall far to the left
     * (x = -8.5): extra path ~ 16.6 m -> ~48 ms. One click every 0.5 s. */
    wait_ms(500);
    printf("PHASE refl %.3f\n", now());
    geom->Enable(A3DF_1ST_REFLECTIONS);
    clk->SetMinMaxDistance(1.0f, 100.0f, A3DMAX_AUDIBLE);
    float lastClick{-1.0f};
    while(now() < 9.6f)
    {
        a3d->Clear();
        geom->BindMaterial(wallMat);
        geom->Begin(A3DP_QUADS);
        geom->Vertex3f(-8.5f, -5, -20); geom->Vertex3f(-8.5f, 5, -20);
        geom->Vertex3f(-8.5f, 5, 20); geom->Vertex3f(-8.5f, -5, 20);
        geom->End();
        clk->SetPosition3f(0.0f, 0.0f, -2.0f);
        if(now() - lastClick >= 0.5f)
        {
            clk->Rewind();
            clk->Play(A3DPLAY_SINGLE);
            lastClick = now();
            printf("CLICK %.3f\n", lastClick);
        }
        a3d->Flush();
        wait_ms(5);
    }
    printf("PHASE end %.3f\n", now());
    wait_ms(300);
    clk->Release();
    src->Release();
    wallMat->Release();
    geom->Release();
    lis->Release();
    a3d->Release();
    cf->Release();
    printf("done\n");
    return 0;
}
