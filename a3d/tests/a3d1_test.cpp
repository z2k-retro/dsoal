#include "../a3d_decl.h"
#include <dsound.h>
#include <cstdio>
int main()
{
    CoInitialize(nullptr);
    /* The way A3D 1.x apps do it: CoCreateInstance(CLSID_A3d) + Initialize. */
    IDirectSound *ds{};
    HRESULT hr{CoCreateInstance(CLSID_A3d_, nullptr, CLSCTX_INPROC_SERVER, IID_IDirectSound, reinterpret_cast<void**>(&ds))};
    printf("CoCreateInstance(CLSID_A3d) %08lx\n", hr);
    if(FAILED(hr)) return 1;
    printf("Initialize %08lx\n", ds->Initialize(nullptr));
    IA3d2X *a3d2{};
    hr = ds->QueryInterface(IID_IA3d2_, reinterpret_cast<void**>(&a3d2));
    printf("QI IA3d2 %08lx\n", hr);
    A3DCaps1Software sc{sizeof(sc)};
    printf("RegisterVersion %08lx GetSoftwareCaps %08lx ver %lu\n", a3d2->RegisterVersion(12), a3d2->GetSoftwareCaps(&sc), sc.dwVersion);
    printf("SetOutputMode %08lx\n", a3d2->SetOutputMode(1, 1, 1));
    ds->SetCooperativeLevel(GetDesktopWindow(), DSSCL_PRIORITY);
    WAVEFORMATEX wf{WAVE_FORMAT_PCM, 1, 22050, 44100, 2, 16, 0};
    DSBUFFERDESC d{sizeof(d), DSBCAPS_CTRL3D | DSBCAPS_CTRLVOLUME, 44100, 0, &wf};
    IDirectSoundBuffer *b{};
    printf("CreateSoundBuffer 3D %08lx\n", ds->CreateSoundBuffer(&d, &b, nullptr));
    IDirectSound3DBuffer *b3{};
    printf("QI 3D buffer %08lx\n", b->QueryInterface(IID_IDirectSound3DBuffer, reinterpret_cast<void**>(&b3)));
    b3->Release(); b->Release(); a3d2->Release();
    printf("final release %lu\n", ds->Release());
    /* A3D 2.0 through the registry. */
    IA3d4X *a3d4{};
    hr = CoCreateInstance(CLSID_A3dApi_, nullptr, CLSCTX_INPROC_SERVER, IID_IA3d4_, reinterpret_cast<void**>(&a3d4));
    printf("CoCreateInstance(CLSID_A3dApi) %08lx\n", hr);
    if(SUCCEEDED(hr)) { printf("Init %08lx\n", a3d4->Init(nullptr, 0, 0)); a3d4->Release(); }
    return 0;
}
