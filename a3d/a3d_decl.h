/* Interface declarations for the Aureal A3D 1.x and 2.0 COM APIs.
 *
 * Written for interoperability from the documented method order and the
 * binary layout that existing applications expect. Only what the emulation
 * needs is declared here.
 */
#pragma once

#include <windows.h>
#include <objbase.h>

using A3DVAL = float;
using LPA3DVAL = A3DVAL*;
using A3DMATRIX = A3DVAL[16];

/* Class and interface IDs. */
inline constexpr GUID CLSID_A3d_   {0xd8f1eee0,0xf634,0x11cf,{0x87,0x00,0x00,0xa0,0x24,0x5d,0x91,0x8b}};
inline constexpr GUID CLSID_A3dApi_{0x92fa2c24,0x253c,0x11d2,{0x90,0xfb,0x00,0x60,0x08,0xa1,0xf4,0x41}};
inline constexpr GUID IID_IA3d_    {0xd8f1eee1,0xf634,0x11cf,{0x87,0x00,0x00,0xa0,0x24,0x5d,0x91,0x8b}};
inline constexpr GUID IID_IA3d2_   {0xfb80d1e0,0x98d3,0x11d1,{0x90,0xfb,0x00,0x60,0x08,0xa1,0xf4,0x41}};
inline constexpr GUID IID_IA3d3_   {0xc398e560,0xd90b,0x11d1,{0x90,0xfb,0x00,0x60,0x08,0xa1,0xf4,0x41}};
inline constexpr GUID IID_IA3dGeom_{0xc398e561,0xd90b,0x11d1,{0x90,0xfb,0x00,0x60,0x08,0xa1,0xf4,0x41}};
inline constexpr GUID IID_IA3dSource_{0xc398e562,0xd90b,0x11d1,{0x90,0xfb,0x00,0x60,0x08,0xa1,0xf4,0x41}};
inline constexpr GUID IID_IA3dListener_{0xc398e563,0xd90b,0x11d1,{0x90,0xfb,0x00,0x60,0x08,0xa1,0xf4,0x41}};
inline constexpr GUID IID_IA3dList_{0xc398e564,0xd90b,0x11d1,{0x90,0xfb,0x00,0x60,0x08,0xa1,0xf4,0x41}};
inline constexpr GUID IID_IA3dMaterial_{0xc398e565,0xd90b,0x11d1,{0x90,0xfb,0x00,0x60,0x08,0xa1,0xf4,0x41}};
inline constexpr GUID IID_IA3d4_   {0xe4c40280,0xccba,0x11d2,{0x9d,0xcf,0x00,0x50,0x04,0x11,0x58,0x2f}};

/* Feature bits (Init, Enable, IsFeatureAvailable). */
enum : DWORD {
    A3DF_1ST_REFLECTIONS = 0x02,
    A3DF_DIRECT_PATH_A3D = 0x08,
    A3DF_DIRECT_PATH_GENERIC = 0x20,
    A3DF_OCCLUSIONS = 0x40,
    A3DF_DISABLE_SPLASHSCREEN = 0x80,
};

/* Geometry primitive types for Begin. */
enum : DWORD {
    A3DP_LINES = 2,
    A3DP_TRIANGLES = 3,
    A3DP_QUADS = 4,
    A3DP_MATERIAL = 5,
    A3DP_SUBFACE = 0x80000000u,
    A3DP_VERTEX_MASK = 0x0f,
};

enum : DWORD {
    A3DPLAY_SINGLE = 0,
    A3DPLAY_LOOPED = 1,

    A3DST_PLAYING = 0x1,
    A3DST_BUFFERLOST = 0x2,
    A3DST_LOOPING = 0x4,
    A3DST_WAITING_FOR_FLUSH = 0x1000,

    A3DCS_RIGHT_HANDED = 0,
    A3DCS_LEFT_HANDED = 1,

    A3DMAX_AUDIBLE = 0,
    A3DMAX_MUTE = 1,

    A3DXFORM_NORMAL = 0,
    A3DXFORM_HEADRELATIVE = 1,

    A3DRM_A3D = 0x00,
    A3DRM_MONO = 0x01,
    A3DRM_1ST_REFLECTIONS = 0x04,
    A3DRM_OCCLUSIONS = 0x08,
    A3DRM_NATIVE = 0x20,
    A3DRM_DEFAULT = A3DRM_A3D | A3DRM_1ST_REFLECTIONS | A3DRM_OCCLUSIONS,

    A3DPOLY_REFLECTIONS = 0x02,
    A3DPOLY_OCCLUSIONS = 0x40,

    A3DTYPE_NATIVE = 0x1,
    A3DTYPE_UNMANAGED = 0x2,
    A3DTYPE_STREAMED = 0x4,

    A3DLOCK_FROMWRITECURSOR = 0x1,
    A3DLOCK_ENTIREBUFFER = 0x2,

    A3DWAVEEVENT_STOP = 0xffffffffu,
};

#define A3DERR(code) MAKE_HRESULT(1, FACILITY_ITF, (code))
enum : HRESULT {
    A3DERR_MEMORY = A3DERR(1),
    A3DERR_INIT_DRIVER = A3DERR(4),
    A3DERR_FILE_OPEN = A3DERR(9),
    A3DERR_CREATE_SOUNDBUFFER = A3DERR(10),
    A3DERR_LOCK = A3DERR(12),
    A3DERR_UNLOCK = A3DERR(13),
    A3DERR_FORMAT = A3DERR(14),
    A3DERR_NO_WAVE_DATA = A3DERR(15),
    A3DERR_PLAYMODE = A3DERR(16),
    A3DERR_PLAY = A3DERR(17),
    A3DERR_NEEDS_FORMAT = A3DERR(19),
    A3DERR_NOT_VALID_SOURCE = A3DERR(21),
    A3DERR_DUPLICATION = A3DERR(22),
    A3DERR_FAILED_INIT = A3DERR(23),
    A3DERR_OUTSIDE_BEGIN_END = A3DERR(26),
    A3DERR_END_BEFORE_BEGIN = A3DERR(28),
    A3DERR_BEGIN_MODE = A3DERR(29),
    A3DERR_INVALID_ARG = A3DERR(30),
};

struct A3DCaps1Software { DWORD dwSize, dwVersion, dwFlags, dwReserved, dwReserved2; };
struct A3DCaps1Hardware { DWORD dwSize, dwFlags, dwReserved, dwReserved2; };
struct A3DCaps2Software {
    DWORD dwSize, dwVersion, dwFlags, dwReserved, dwReserved2;
    DWORD dwOutputChannels, dwMinSampleRate, dwMaxSampleRate, dwMax2DBuffers, dwMax3DBuffers;
};
struct A3DCaps2Hardware {
    DWORD dwSize, dwFlags, dwReserved, dwReserved2;
    DWORD dwOutputChannels, dwMinSampleRate, dwMaxSampleRate, dwMax2DBuffers, dwMax3DBuffers;
};

struct IA3dSourceX;
struct IA3dMaterialX;
struct IA3dListX;

#define A3DM(name) virtual HRESULT STDMETHODCALLTYPE name

/* IA3d / IA3d2 prefix; IA3d3 and IA3d4 extend the same vtable, so one
 * declaration of the longest one serves all four. */
struct IA3d4X : IUnknown {
    /* IA3d */
    A3DM(SetOutputMode)(DWORD front, DWORD rear, DWORD quad) = 0;
    A3DM(GetOutputMode)(DWORD *front, DWORD *rear, DWORD *quad) = 0;
    A3DM(SetResourceManagerMode)(DWORD mode) = 0;
    A3DM(GetResourceManagerMode)(DWORD *mode) = 0;
    A3DM(SetHFAbsorbFactor)(float factor) = 0;
    A3DM(GetHFAbsorbFactor)(float *factor) = 0;
    /* IA3d2 */
    A3DM(RegisterVersion)(DWORD version) = 0;
    A3DM(GetSoftwareCaps)(void *caps) = 0;
    A3DM(GetHardwareCaps)(void *caps) = 0;
    /* IA3d3 */
    A3DM(Clear)() = 0;
    A3DM(Flush)() = 0;
    A3DM(Compat)(DWORD mode, DWORD value) = 0;
    A3DM(Init)(GUID *device, DWORD features, DWORD prefs) = 0;
    A3DM(IsFeatureAvailable)(DWORD feature) = 0;
    A3DM(NewSource)(DWORD type, IA3dSourceX **out) = 0;
    A3DM(DuplicateSource)(IA3dSourceX *orig, IA3dSourceX **out) = 0;
    A3DM(SetCooperativeLevel)(HWND hwnd, DWORD level) = 0;
    A3DM(GetCooperativeLevel)(DWORD *level) = 0;
    A3DM(SetMaxReflectionDelayTime)(A3DVAL seconds) = 0;
    A3DM(GetMaxReflectionDelayTime)(A3DVAL *seconds) = 0;
    A3DM(SetCoordinateSystem)(DWORD cs) = 0;
    A3DM(GetCoordinateSystem)(DWORD *cs) = 0;
    A3DM(SetOutputGain)(A3DVAL gain) = 0;
    A3DM(GetOutputGain)(A3DVAL *gain) = 0;
    A3DM(SetNumFallbackSources)(DWORD num) = 0;
    A3DM(GetNumFallbackSources)(DWORD *num) = 0;
    /* IA3d4 */
    A3DM(SetRMPriorityBias)(A3DVAL bias) = 0;
    A3DM(GetRMPriorityBias)(A3DVAL *bias) = 0;
    A3DM(DisableViewer)() = 0;
    A3DM(SetUnitsPerMeter)(A3DVAL units) = 0;
    A3DM(GetUnitsPerMeter)(A3DVAL *units) = 0;
    A3DM(SetDopplerScale)(A3DVAL scale) = 0;
    A3DM(GetDopplerScale)(A3DVAL *scale) = 0;
    A3DM(SetDistanceModelScale)(A3DVAL scale) = 0;
    A3DM(GetDistanceModelScale)(A3DVAL *scale) = 0;
    A3DM(SetEq)(A3DVAL eq) = 0;
    A3DM(GetEq)(A3DVAL *eq) = 0;
    A3DM(Shutdown)() = 0;
    A3DM(RegisterApp)(REFIID app) = 0;
};

/* IA3d / IA3d2 alone (used by the A3D 1.x DirectSound object). */
struct IA3d2X : IUnknown {
    A3DM(SetOutputMode)(DWORD front, DWORD rear, DWORD quad) = 0;
    A3DM(GetOutputMode)(DWORD *front, DWORD *rear, DWORD *quad) = 0;
    A3DM(SetResourceManagerMode)(DWORD mode) = 0;
    A3DM(GetResourceManagerMode)(DWORD *mode) = 0;
    A3DM(SetHFAbsorbFactor)(float factor) = 0;
    A3DM(GetHFAbsorbFactor)(float *factor) = 0;
    A3DM(RegisterVersion)(DWORD version) = 0;
    A3DM(GetSoftwareCaps)(void *caps) = 0;
    A3DM(GetHardwareCaps)(void *caps) = 0;
};

/* IsEnabled returns BOOL and the matrix stack calls return the depth. */
struct IA3dGeomX : IUnknown {
    A3DM(Enable)(DWORD feature) = 0;
    A3DM(Disable)(DWORD feature) = 0;
    virtual BOOL STDMETHODCALLTYPE IsEnabled(DWORD feature) = 0;
    A3DM(SetOcclusionMode)(DWORD mode) = 0;
    A3DM(GetOcclusionMode)(DWORD *mode) = 0;
    A3DM(SetReflectionMode)(DWORD mode) = 0;
    A3DM(GetReflectionMode)(DWORD *mode) = 0;
    A3DM(SetReflectionGainScale)(A3DVAL scale) = 0;
    A3DM(GetReflectionGainScale)(A3DVAL *scale) = 0;
    A3DM(SetReflectionDelayScale)(A3DVAL scale) = 0;
    A3DM(GetReflectionDelayScale)(A3DVAL *scale) = 0;
    virtual ULONG STDMETHODCALLTYPE PushMatrix() = 0;
    virtual ULONG STDMETHODCALLTYPE PopMatrix() = 0;
    A3DM(LoadIdentity)() = 0;
    A3DM(LoadMatrix)(A3DVAL *m) = 0;
    A3DM(GetMatrix)(A3DVAL *m) = 0;
    A3DM(MultMatrix)(A3DVAL *m) = 0;
    A3DM(Translate3f)(A3DVAL x, A3DVAL y, A3DVAL z) = 0;
    A3DM(Translate3fv)(A3DVAL *v) = 0;
    A3DM(Rotate3f)(A3DVAL deg, A3DVAL x, A3DVAL y, A3DVAL z) = 0;
    A3DM(Rotate3fv)(A3DVAL deg, A3DVAL *v) = 0;
    A3DM(Scale3f)(A3DVAL x, A3DVAL y, A3DVAL z) = 0;
    A3DM(Scale3fv)(A3DVAL *v) = 0;
    A3DM(Begin)(DWORD type) = 0;
    A3DM(End)() = 0;
    A3DM(Vertex3f)(A3DVAL x, A3DVAL y, A3DVAL z) = 0;
    A3DM(Vertex3fv)(A3DVAL *v) = 0;
    A3DM(Normal3f)(A3DVAL x, A3DVAL y, A3DVAL z) = 0;
    A3DM(Normal3fv)(A3DVAL *v) = 0;
    A3DM(Tag)(DWORD tag) = 0;
    A3DM(SetOpeningFactorf)(A3DVAL f) = 0;
    A3DM(SetOpeningFactorfv)(A3DVAL *f) = 0;
    A3DM(NewMaterial)(IA3dMaterialX **out) = 0;
    A3DM(BindMaterial)(IA3dMaterialX *mat) = 0;
    A3DM(NewList)(IA3dListX **out) = 0;
    A3DM(BindListener)() = 0;
    A3DM(BindSource)(IA3dSourceX *src) = 0;
    A3DM(NewEnvironment)(void **out) = 0;
    A3DM(BindEnvironment)(void *env) = 0;
    A3DM(SetRenderMode)(DWORD mode) = 0;
    A3DM(GetRenderMode)(DWORD *mode) = 0;
    A3DM(SetPolygonBloatFactor)(A3DVAL f) = 0;
    A3DM(GetPolygonBloatFactor)(A3DVAL *f) = 0;
    A3DM(SetReflectionUpdateInterval)(DWORD n) = 0;
    A3DM(GetReflectionUpdateInterval)(DWORD *n) = 0;
    A3DM(SetOcclusionUpdateInterval)(DWORD n) = 0;
    A3DM(GetOcclusionUpdateInterval)(DWORD *n) = 0;
};

struct IA3dListenerX : IUnknown {
    A3DM(SetPosition3f)(A3DVAL x, A3DVAL y, A3DVAL z) = 0;
    A3DM(GetPosition3f)(A3DVAL *x, A3DVAL *y, A3DVAL *z) = 0;
    A3DM(SetPosition3fv)(A3DVAL *v) = 0;
    A3DM(GetPosition3fv)(A3DVAL *v) = 0;
    A3DM(SetOrientationAngles3f)(A3DVAL h, A3DVAL p, A3DVAL r) = 0;
    A3DM(GetOrientationAngles3f)(A3DVAL *h, A3DVAL *p, A3DVAL *r) = 0;
    A3DM(SetOrientationAngles3fv)(A3DVAL *v) = 0;
    A3DM(GetOrientationAngles3fv)(A3DVAL *v) = 0;
    A3DM(SetOrientation6f)(A3DVAL fx, A3DVAL fy, A3DVAL fz, A3DVAL ux, A3DVAL uy, A3DVAL uz) = 0;
    A3DM(GetOrientation6f)(A3DVAL *fx, A3DVAL *fy, A3DVAL *fz, A3DVAL *ux, A3DVAL *uy, A3DVAL *uz) = 0;
    A3DM(SetOrientation6fv)(A3DVAL *v) = 0;
    A3DM(GetOrientation6fv)(A3DVAL *v) = 0;
    A3DM(SetVelocity3f)(A3DVAL x, A3DVAL y, A3DVAL z) = 0;
    A3DM(GetVelocity3f)(A3DVAL *x, A3DVAL *y, A3DVAL *z) = 0;
    A3DM(SetVelocity3fv)(A3DVAL *v) = 0;
    A3DM(GetVelocity3fv)(A3DVAL *v) = 0;
};

struct IA3dSourceX : IUnknown {
    A3DM(LoadWaveFile)(char *path) = 0;
    A3DM(LoadWaveData)(void *data, DWORD size) = 0;
    A3DM(AllocateWaveData)(INT size) = 0;
    A3DM(FreeWaveData)() = 0;
    A3DM(SetWaveFormat)(void *fmt) = 0;
    A3DM(GetWaveFormat)(void *fmt) = 0;
    A3DM(GetWaveSize)() = 0;
    A3DM(GetType)(DWORD *type) = 0;
    A3DM(Lock)(DWORD offset, DWORD bytes, void **p1, DWORD *n1, void **p2, DWORD *n2, DWORD flags) = 0;
    A3DM(Unlock)(void *p1, DWORD n1, void *p2, DWORD n2) = 0;
    A3DM(Play)(INT mode) = 0;
    A3DM(Stop)() = 0;
    A3DM(Rewind)() = 0;
    A3DM(SetWaveTime)(A3DVAL t) = 0;
    A3DM(GetWaveTime)(A3DVAL *t) = 0;
    A3DM(SetWavePosition)(DWORD pos) = 0;
    A3DM(GetWavePosition)(DWORD *pos) = 0;
    A3DM(SetPosition3f)(A3DVAL x, A3DVAL y, A3DVAL z) = 0;
    A3DM(GetPosition3f)(A3DVAL *x, A3DVAL *y, A3DVAL *z) = 0;
    A3DM(SetPosition3fv)(A3DVAL *v) = 0;
    A3DM(GetPosition3fv)(A3DVAL *v) = 0;
    A3DM(SetOrientationAngles3f)(A3DVAL h, A3DVAL p, A3DVAL r) = 0;
    A3DM(GetOrientationAngles3f)(A3DVAL *h, A3DVAL *p, A3DVAL *r) = 0;
    A3DM(SetOrientationAngles3fv)(A3DVAL *v) = 0;
    A3DM(GetOrientationAngles3fv)(A3DVAL *v) = 0;
    A3DM(SetOrientation6f)(A3DVAL fx, A3DVAL fy, A3DVAL fz, A3DVAL ux, A3DVAL uy, A3DVAL uz) = 0;
    A3DM(GetOrientation6f)(A3DVAL *fx, A3DVAL *fy, A3DVAL *fz, A3DVAL *ux, A3DVAL *uy, A3DVAL *uz) = 0;
    A3DM(SetOrientation6fv)(A3DVAL *v) = 0;
    A3DM(GetOrientation6fv)(A3DVAL *v) = 0;
    A3DM(SetVelocity3f)(A3DVAL x, A3DVAL y, A3DVAL z) = 0;
    A3DM(GetVelocity3f)(A3DVAL *x, A3DVAL *y, A3DVAL *z) = 0;
    A3DM(SetVelocity3fv)(A3DVAL *v) = 0;
    A3DM(GetVelocity3fv)(A3DVAL *v) = 0;
    A3DM(SetCone)(A3DVAL inner, A3DVAL outer, A3DVAL gain) = 0;
    A3DM(GetCone)(A3DVAL *inner, A3DVAL *outer, A3DVAL *gain) = 0;
    A3DM(SetMinMaxDistance)(A3DVAL mn, A3DVAL mx, DWORD behaviour) = 0;
    A3DM(GetMinMaxDistance)(A3DVAL *mn, A3DVAL *mx, DWORD *behaviour) = 0;
    A3DM(SetGain)(A3DVAL g) = 0;
    A3DM(GetGain)(A3DVAL *g) = 0;
    A3DM(SetPitch)(A3DVAL p) = 0;
    A3DM(GetPitch)(A3DVAL *p) = 0;
    A3DM(SetDopplerScale)(A3DVAL s) = 0;
    A3DM(GetDopplerScale)(A3DVAL *s) = 0;
    A3DM(SetDistanceModelScale)(A3DVAL s) = 0;
    A3DM(GetDistanceModelScale)(A3DVAL *s) = 0;
    A3DM(SetEq)(A3DVAL eq) = 0;
    A3DM(GetEq)(A3DVAL *eq) = 0;
    A3DM(SetPriority)(A3DVAL p) = 0;
    A3DM(GetPriority)(A3DVAL *p) = 0;
    A3DM(SetRenderMode)(DWORD mode) = 0;
    A3DM(GetRenderMode)(DWORD *mode) = 0;
    A3DM(GetAudibility)(A3DVAL *a) = 0;
    A3DM(GetOcclusionFactor)(A3DVAL *o) = 0;
    A3DM(GetStatus)(DWORD *status) = 0;
    A3DM(SetPanValues)(DWORD count, A3DVAL *gains) = 0;
    A3DM(GetPanValues)(DWORD count, A3DVAL *gains) = 0;
    A3DM(SetWaveEvent)(DWORD offset, HANDLE event) = 0;
    A3DM(ClearWaveEvents)() = 0;
    A3DM(SetTransformMode)(DWORD mode) = 0;
    A3DM(GetTransformMode)(DWORD *mode) = 0;
    A3DM(SetReflectionDelayScale)(A3DVAL s) = 0;
    A3DM(GetReflectionDelayScale)(A3DVAL *s) = 0;
    A3DM(SetReflectionGainScale)(A3DVAL s) = 0;
    A3DM(GetReflectionGainScale)(A3DVAL *s) = 0;
};

struct IA3dListX : IUnknown {
    A3DM(Begin)() = 0;
    A3DM(End)() = 0;
    A3DM(Call)() = 0;
    A3DM(EnableBoundingVol)() = 0;
};

struct IA3dMaterialX : IUnknown {
    A3DM(Load)(char *path) = 0;
    A3DM(Save)(char *path) = 0;
    A3DM(UnSerialize)(void *data, UINT size) = 0;
    A3DM(Serialize)(void **data, UINT *size) = 0;
    A3DM(Duplicate)(IA3dMaterialX **out) = 0;
    A3DM(SetNameID)(char *name) = 0;
    A3DM(GetNameID)(char *name, INT size) = 0;
    A3DM(SelectPreset)(DWORD preset) = 0;
    A3DM(GetClosestPreset)(DWORD *preset) = 0;
    A3DM(SetReflectance)(A3DVAL gain, A3DVAL hf) = 0;
    A3DM(GetReflectance)(A3DVAL *gain, A3DVAL *hf) = 0;
    A3DM(SetTransmittance)(A3DVAL gain, A3DVAL hf) = 0;
    A3DM(GetTransmittance)(A3DVAL *gain, A3DVAL *hf) = 0;
};

#undef A3DM
