/* A3D 3.0 reverb (IA3dReverb), on EAX through DSOAL and OpenAL Soft.
 *
 * Aureal's library hands reverb to the sound card's driver through the EAX
 * 1.0 or I3DL2 property sets: a preset is an EAX 1.0 environment with a
 * volume, decay time and damping, and a custom reverb is the I3DL2 listener
 * property list. Here a preset goes out as EAX 1.0 listener properties and a
 * custom reverb as EAX 2.0 listener properties, which carry the same fields
 * as I3DL2 except density and the HF reference. */
#include "engine.h"

#include <ks.h>

#include <cmath>
#include <cstring>

namespace a3d {

namespace {

using Guard = std::lock_guard<std::recursive_mutex>;

/* The EAX 1.0 environments' default volume, decay time and damping
 * (EAX 1.0 SDK values), in A3DREVERB_PRESET order. */
struct PresetDefaults { float volume, decay, damping; };
constexpr PresetDefaults kPresets[A3DREVERB_PRESETS]{
    {0.500f,  1.493f, 0.500f},  /* generic */
    {0.250f,  0.100f, 0.000f},  /* padded cell */
    {0.417f,  0.400f, 0.666f},  /* room */
    {0.653f,  1.499f, 0.166f},  /* bathroom */
    {0.208f,  0.478f, 0.000f},  /* living room */
    {0.500f,  2.309f, 0.888f},  /* stone room */
    {0.403f,  4.279f, 0.500f},  /* auditorium */
    {0.500f,  3.961f, 0.500f},  /* concert hall */
    {0.500f,  2.886f, 1.304f},  /* cave */
    {0.361f,  7.284f, 0.332f},  /* arena */
    {0.500f, 10.000f, 0.300f},  /* hangar */
    {0.153f,  0.259f, 2.000f},  /* carpeted hallway */
    {0.361f,  1.493f, 0.000f},  /* hallway */
    {0.444f,  2.697f, 0.638f},  /* stone corridor */
    {0.250f,  1.752f, 0.776f},  /* alley */
    {0.111f,  3.145f, 0.472f},  /* forest */
    {0.111f,  2.767f, 0.224f},  /* city */
    {0.194f,  7.841f, 0.472f},  /* mountains */
    {1.000f,  1.499f, 0.500f},  /* quarry */
    {0.097f,  2.767f, 0.224f},  /* plain */
    {0.208f,  1.652f, 1.500f},  /* parking lot */
    {0.652f,  2.886f, 0.250f},  /* sewer pipe */
    {1.000f,  1.499f, 0.000f},  /* underwater */
    {0.875f,  8.392f, 1.388f},  /* drugged */
    {0.139f, 17.234f, 0.666f},  /* dizzy */
    {0.486f,  7.563f, 0.806f},  /* psychotic */
};

/* Public EAX property sets. */
constexpr GUID EAX10_Listener{0x4a4e6fc1, 0xc341, 0x11d1,
    {0xb7, 0x3a, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
constexpr GUID EAX20_Listener{0x0306a6a8, 0xb224, 0x11d2,
    {0x99, 0xe5, 0x00, 0x00, 0xe8, 0xd8, 0xc7, 0x22}};
enum : ULONG { EAX10REVERB_ALL = 0, EAX20LISTENER_ALLPARAMETERS = 1 };

struct Eax1Reverb { DWORD environment; float volume, decayTime, damping; };
struct Eax2Listener {
    LONG lRoom, lRoomHF;
    float flRoomRolloffFactor, flDecayTime, flDecayHFRatio;
    LONG lReflections;
    float flReflectionsDelay;
    LONG lReverb;
    float flReverbDelay;
    DWORD dwEnvironment;
    float flEnvironmentSize, flEnvironmentDiffusion, flAirAbsorptionHF;
    DWORD dwFlags;
};

bool validCustom(const A3DReverbCustom &c)
{
    return c.lRoom >= -10000 && c.lRoom <= 0 && c.lRoomHF >= -10000 && c.lRoomHF <= 0
        && c.flRoomRolloffFactor >= 0.0f && c.flRoomRolloffFactor <= 10.0f
        && c.flDecayTime >= 0.1f && c.flDecayTime <= 20.0f
        && c.flDecayHFRatio >= 0.1f && c.flDecayHFRatio <= 2.0f
        && c.lReflections >= -10000 && c.lReflections <= 1000
        && c.flReflectionsDelay >= 0.0f && c.flReflectionsDelay <= 0.3f
        && c.lReverb >= -10000 && c.lReverb <= 2000
        && c.flReverbDelay >= 0.0f && c.flReverbDelay <= 0.1f
        && c.flDiffusion >= 0.0f && c.flDiffusion <= 100.0f
        && c.flDensity >= 0.0f && c.flDensity <= 100.0f
        && c.flHFReference >= 20.0f && c.flHFReference <= 20000.0f;
}

} // namespace

/* ------------------------------------------------------------ IA3dReverb */

Reverb::Reverb()
{
    ++gObjects;
    loadPreset(0);
    /* The I3DL2 defaults. */
    custom.dwSize = sizeof(custom);
    custom.lRoom = -10000;
    custom.lRoomHF = 0;
    custom.flRoomRolloffFactor = 0.0f;
    custom.flDecayTime = 1.0f;
    custom.flDecayHFRatio = 0.5f;
    custom.lReflections = -10000;
    custom.flReflectionsDelay = 0.02f;
    custom.lReverb = -10000;
    custom.flReverbDelay = 0.04f;
    custom.flDiffusion = 100.0f;
    custom.flDensity = 100.0f;
    custom.flHFReference = 5000.0f;
}

void Reverb::loadPreset(DWORD e)
{
    type = A3DREVERBTYPE_PRESET;
    env = e;
    volume = kPresets[e].volume;
    decayTime = kPresets[e].decay;
    damping = kPresets[e].damping;
    ++gen;
}

HRESULT STDMETHODCALLTYPE Reverb::QueryInterface(REFIID riid, void **out)
{
    if(!out) return E_POINTER;
    if(riid == IID_IUnknown || riid == IID_IA3dReverb_)
    {
        AddRef();
        *out = static_cast<IA3dReverbX*>(this);
        return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE Reverb::Release()
{
    const LONG r{--mRef};
    if(r == 0)
    {
        --gObjects;
        delete this;
    }
    return ULONG(r);
}

HRESULT STDMETHODCALLTYPE Reverb::SetReverbPreset(DWORD p)
{
    if(p >= A3DREVERB_PRESETS) return A3DERR_INVALID_ARG;
    loadPreset(p);
    return S_OK;
}
HRESULT STDMETHODCALLTYPE Reverb::GetReverbPreset(DWORD *p)
{ if(!p) return E_POINTER; *p = env; return S_OK; }

HRESULT STDMETHODCALLTYPE Reverb::SetAllProperties(A3DReverbProperties *p)
{
    if(!p) return E_POINTER;
    if(p->dwSize < offsetof(A3DReverbProperties, uval)) return A3DERR_INVALID_ARG;
    if(p->dwType == A3DREVERBTYPE_PRESET)
    {
        const A3DReverbPreset &r{p->uval.preset};
        if(r.dwEnvPreset >= A3DREVERB_PRESETS || !(r.fVolume >= 0.0f && r.fVolume <= 1.0f)
            || !(r.fDecayTime >= 0.1f && r.fDecayTime <= 100.0f) || !(r.fDamping >= 0.0f && r.fDamping <= 100.0f))
            return A3DERR_INVALID_ARG;
        type = A3DREVERBTYPE_PRESET;
        env = r.dwEnvPreset;
        volume = r.fVolume;
        decayTime = r.fDecayTime;
        damping = r.fDamping;
    }
    else if(p->dwType == A3DREVERBTYPE_CUSTOM)
    {
        if(!validCustom(p->uval.custom)) return A3DERR_INVALID_ARG;
        type = A3DREVERBTYPE_CUSTOM;
        custom = p->uval.custom;
        custom.dwSize = sizeof(custom);
    }
    else
        return A3DERR_INVALID_ARG;
    ++gen;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Reverb::GetAllProperties(A3DReverbProperties *p)
{
    if(!p) return E_POINTER;
    if(p->dwSize < offsetof(A3DReverbProperties, uval)) return A3DERR_INVALID_ARG;
    p->dwType = type;
    if(type == A3DREVERBTYPE_PRESET)
    {
        p->uval.preset.dwSize = sizeof(A3DReverbPreset);
        p->uval.preset.dwEnvPreset = env;
        p->uval.preset.fVolume = volume;
        p->uval.preset.fDecayTime = decayTime;
        p->uval.preset.fDamping = damping;
    }
    else
        p->uval.custom = custom;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Reverb::SetPresetVolume(A3DVAL v)
{
    if(!(v >= 0.0f && v <= 1.0f)) return A3DERR_INVALID_ARG;
    volume = v; ++gen;
    return S_OK;
}
HRESULT STDMETHODCALLTYPE Reverb::GetPresetVolume(A3DVAL *v)
{ if(!v) return E_POINTER; *v = volume; return S_OK; }
HRESULT STDMETHODCALLTYPE Reverb::SetPresetDecayTime(A3DVAL t)
{
    if(!(t >= 0.1f && t <= 100.0f)) return A3DERR_INVALID_ARG;
    decayTime = t; ++gen;
    return S_OK;
}
HRESULT STDMETHODCALLTYPE Reverb::GetPresetDecayTime(A3DVAL *t)
{ if(!t) return E_POINTER; *t = decayTime; return S_OK; }
HRESULT STDMETHODCALLTYPE Reverb::SetPresetDamping(A3DVAL d)
{
    if(!(d >= 0.0f && d <= 100.0f)) return A3DERR_INVALID_ARG;
    damping = d; ++gen;
    return S_OK;
}
HRESULT STDMETHODCALLTYPE Reverb::GetPresetDamping(A3DVAL *d)
{ if(!d) return E_POINTER; *d = damping; return S_OK; }

/* -------------------------------------------------- engine-side reverb */

/* Sends the bound reverb to OpenAL Soft's EAX reverb when it has changed.
 * Called with the engine lock held, at Flush. */
void Engine::applyReverb()
{
    if(!fxPs) return;
    if(!reverb)
    {
        appliedReverb = nullptr;
        return;
    }
    const unsigned g{reverb->gen};
    if(reverb == appliedReverb && g == appliedGen) return;
    appliedReverb = reverb;
    appliedGen = g;

    HRESULT hr{};
    if(reverb->type == A3DREVERBTYPE_PRESET)
    {
        /* EAX 1.0's ranges are narrower than the ones A3D documents. */
        Eax1Reverb p{reverb->env, std::clamp(reverb->volume, 0.0f, 1.0f),
            std::clamp(reverb->decayTime, 0.1f, 20.0f), std::clamp(reverb->damping, 0.0f, 2.0f)};
        hr = fxPs->Set(EAX10_Listener, EAX10REVERB_ALL, nullptr, 0, &p, sizeof(p));
        logf("reverb: preset %lu, volume %.3f, decay %.3f s, damping %.3f (%08lx)",
            p.environment, p.volume, p.decayTime, p.damping, hr);
    }
    else
    {
        const A3DReverbCustom &c{reverb->custom};
        Eax2Listener p{c.lRoom, c.lRoomHF, c.flRoomRolloffFactor, c.flDecayTime, c.flDecayHFRatio,
            c.lReflections, c.flReflectionsDelay, c.lReverb, c.flReverbDelay,
            0, 7.5f, c.flDiffusion / 100.0f, -5.0f, 0x3f};
        hr = fxPs->Set(EAX20_Listener, EAX20LISTENER_ALLPARAMETERS, nullptr, 0, &p, sizeof(p));
        logf("reverb: custom, room %ld, decay %.3f s (%08lx)", c.lRoom, c.flDecayTime, hr);
        if(!loggedCustomExtras && (c.flDensity != 100.0f || c.flHFReference != 5000.0f))
        {
            logf("reverb: density and HF reference have no EAX 2.0 equivalent; ignored");
            loggedCustomExtras = true;
        }
    }
}

} // namespace a3d
