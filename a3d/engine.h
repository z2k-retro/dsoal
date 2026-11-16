/* A3D 2.0 engine: IA3d4 + IA3dGeom + IA3dListener on one object, with
 * IA3dSource, IA3dMaterial and IA3dList objects hanging off it.
 *
 * Rendering model (matches what the Vortex 2 driver does, within what plain
 * DirectSound can express):
 *  - every 3D source is a head-relative DirectSound 3D buffer; direction is
 *    left to the DirectSound implementation (DSOAL + OpenAL Soft HRTF with
 *    the Vortex 2 HRTF set);
 *  - distance, cone, gain, occlusion and Doppler are computed here and sent
 *    as volume/frequency, high-frequency loss as the EAX 2 DirectHF property;
 *  - first-order reflections are image sources played from duplicates of the
 *    source buffer, lagging by the extra path delay, as the hardware rereads
 *    the same sample memory for each reflection tap;
 *  - nothing happens until Flush, as the A3D 2.0 frame model requires.
 */
#pragma once

#include "a3d_decl.h"
#include "common.h"

#include <atomic>
#include <string>
#include <mutex>
#include <thread>
#include <vector>

namespace a3d {

class Engine;

struct MatProps {
    /* Defaults for polygons drawn with no material bound (a guess: a fairly
     * hard, nearly opaque wall). */
    float reflLow{0.9f}, reflHigh{0.8f};
    float transLow{0.1f}, transHigh{0.05f};
};

struct Poly {
    V3 v[4];
    int count{0};
    V3 normal;
    MatProps mat;
    DWORD mode{0};
    DWORD tag{0};
    bool sub{false};
    float opening{0.0f};
    const float *openingPtr{nullptr};
    int parent{-1};

    float open() const
    {
        const float o{openingPtr ? *openingPtr : opening};
        return std::clamp(o, 0.0f, 1.0f);
    }
};

class Material final : public IA3dMaterialX {
    std::atomic<LONG> mRef{1};
public:
    MatProps props;
    char name[64]{};

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override;
    ULONG STDMETHODCALLTYPE AddRef() override { return ++mRef; }
    ULONG STDMETHODCALLTYPE Release() override;

    HRESULT STDMETHODCALLTYPE Load(char *path) override;
    HRESULT STDMETHODCALLTYPE Save(char *path) override;
    HRESULT STDMETHODCALLTYPE UnSerialize(void *data, UINT size) override;
    HRESULT STDMETHODCALLTYPE Serialize(void **data, UINT *size) override;
    HRESULT STDMETHODCALLTYPE Duplicate(IA3dMaterialX **out) override;
    HRESULT STDMETHODCALLTYPE SetNameID(char *n) override;
    HRESULT STDMETHODCALLTYPE GetNameID(char *n, INT size) override;
    HRESULT STDMETHODCALLTYPE SelectPreset(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetClosestPreset(DWORD *p) override { if(p) *p = 0; return S_OK; }
    HRESULT STDMETHODCALLTYPE SetReflectance(A3DVAL g, A3DVAL hf) override;
    HRESULT STDMETHODCALLTYPE GetReflectance(A3DVAL *g, A3DVAL *hf) override;
    HRESULT STDMETHODCALLTYPE SetTransmittance(A3DVAL g, A3DVAL hf) override;
    HRESULT STDMETHODCALLTYPE GetTransmittance(A3DVAL *g, A3DVAL *hf) override;
private:
    std::vector<unsigned char> mBlob;
};

class List final : public IA3dListX {
    std::atomic<LONG> mRef{1};
    Engine *mEngine;
public:
    std::vector<Poly> polys;
    explicit List(Engine *e);
    ~List();

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override;
    ULONG STDMETHODCALLTYPE AddRef() override { return ++mRef; }
    ULONG STDMETHODCALLTYPE Release() override;

    HRESULT STDMETHODCALLTYPE Begin() override;
    HRESULT STDMETHODCALLTYPE End() override;
    HRESULT STDMETHODCALLTYPE Call() override;
    HRESULT STDMETHODCALLTYPE EnableBoundingVol() override { return S_OK; }
};

/* One delayed copy of a source for a first-order reflection. */
struct ReflVoice {
    IDirectSoundBuffer *buf{nullptr};
    IDirectSound3DBuffer *b3d{nullptr};
    IKsPropertySet *ps{nullptr};
    DWORD key{0};
    bool used{false};      /* matched this frame */
    bool started{false};
    bool padded{false};    /* one-shot: own copy with padBytes of silence in front */
    DWORD padBytes{0};
    DWORD size{0};
    DWORD delayBytes{0};
    void release();
};

class Source;
class Reverb;

/* A3D 3.0 manual reflection: a delayed copy of its source played from a
 * position the application chooses. The source keeps a list of its live
 * reflections; the application holds the only reference. */
class Reflection final : public IA3dReflectionX {
    std::atomic<LONG> mRef{1};
public:
    Engine *engine;
    Source *owner;          /* null once the source has freed it */
    DWORD id;
    float gainScale{1.0f}, delay{0.0f}, eq{1.0f};
    V3 pos;
    DWORD xform{A3DXFORM_NORMAL};

    Reflection(Engine *e, Source *s, DWORD i);
    ~Reflection();
    void detach();

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override;
    ULONG STDMETHODCALLTYPE AddRef() override { return ULONG(++mRef); }
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE SetGainScale(A3DVAL g) override;
    HRESULT STDMETHODCALLTYPE GetGainScale(A3DVAL *g) override;
    HRESULT STDMETHODCALLTYPE SetDelay(A3DVAL t) override;
    HRESULT STDMETHODCALLTYPE GetDelay(A3DVAL *t) override;
    HRESULT STDMETHODCALLTYPE SetPosition3f(A3DVAL x, A3DVAL y, A3DVAL z) override;
    HRESULT STDMETHODCALLTYPE GetPosition3f(A3DVAL *x, A3DVAL *y, A3DVAL *z) override;
    HRESULT STDMETHODCALLTYPE SetPosition3fv(A3DVAL *v) override;
    HRESULT STDMETHODCALLTYPE GetPosition3fv(A3DVAL *v) override;
    HRESULT STDMETHODCALLTYPE SetTransformMode(DWORD m) override;
    HRESULT STDMETHODCALLTYPE GetTransformMode(DWORD *m) override;
    HRESULT STDMETHODCALLTYPE SetEQ(A3DVAL e) override;
    HRESULT STDMETHODCALLTYPE GetEQ(A3DVAL *e) override;
};

/* A3D 3.0 reverb ("A3Dverb"): either one of the 26 EAX 1.0 environments
 * with a volume, decay time and damping, or a custom I3DL2 property set.
 * Bound to the world with IA3d5::BindReverb; takes effect at Flush. */
class Reverb final : public IA3dReverbX {
    std::atomic<LONG> mRef{1};
public:
    DWORD type{A3DREVERBTYPE_PRESET};
    DWORD env{0};
    float volume, decayTime, damping;
    A3DReverbCustom custom{};
    std::atomic<unsigned> gen{1};   /* bumped on every change */

    Reverb();
    void loadPreset(DWORD e);

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override;
    ULONG STDMETHODCALLTYPE AddRef() override { return ULONG(++mRef); }
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE SetReverbPreset(DWORD p) override;
    HRESULT STDMETHODCALLTYPE GetReverbPreset(DWORD *p) override;
    HRESULT STDMETHODCALLTYPE SetAllProperties(A3DReverbProperties *p) override;
    HRESULT STDMETHODCALLTYPE GetAllProperties(A3DReverbProperties *p) override;
    HRESULT STDMETHODCALLTYPE SetPresetVolume(A3DVAL v) override;
    HRESULT STDMETHODCALLTYPE GetPresetVolume(A3DVAL *v) override;
    HRESULT STDMETHODCALLTYPE SetPresetDecayTime(A3DVAL t) override;
    HRESULT STDMETHODCALLTYPE GetPresetDecayTime(A3DVAL *t) override;
    HRESULT STDMETHODCALLTYPE SetPresetDamping(A3DVAL d) override;
    HRESULT STDMETHODCALLTYPE GetPresetDamping(A3DVAL *d) override;
};

class Source final : public IA3dSource2X {
    std::atomic<LONG> mRef{1};
public:
    Engine *engine;
    DWORD type;

    /* Wave data. */
    WAVEFORMATEX fmt{};
    bool haveFmt{false};
    IDirectSoundBuffer *buf{nullptr};
    IDirectSound3DBuffer *b3d{nullptr};
    IKsPropertySet *ps{nullptr};
    bool eaxOk{false};
    bool is3d{false};
    DWORD bytes{0};       /* size of the DirectSound buffer */
    DWORD appBytes{0};    /* size the application sees */
    /* Stereo data on a 3D source is mixed to mono and positioned (as A3D
     * does): the application locks and reads a stereo shadow copy, and the
     * DirectSound buffer holds the mono mix. */
    bool mixdown{false};
    WAVEFORMATEX dsFmt{};
    std::vector<unsigned char> shadow;
    const unsigned char *lockBase{nullptr};

    /* Application-side properties (app units, as given). */
    V3 pos, vel;
    V3 front{0, 0, -1}, up{0, 1, 0};
    float angles[3]{};
    M4 bound{M4::identity()};
    bool isBound{false};
    float coneInner{360.0f}, coneOuter{360.0f}, coneGain{1.0f};
    float minDist{1.0f}, maxDist{5000.0f};  /* meters */
    DWORD maxMode{A3DMAX_AUDIBLE};
    float gain{1.0f}, pitch{1.0f}, doppler{1.0f}, dms{1.0f}, eq{1.0f}, priority{0.5f};
    DWORD renderMode{A3DRM_DEFAULT};
    DWORD xform{A3DXFORM_NORMAL};
    float pan[2]{1.0f, 1.0f};
    float reflDelayScale{1.0f}, reflGainScale{1.0f};
    std::vector<DSBPOSITIONNOTIFY> events;
    bool eventsDirty{false};

    /* A3D 3.0. */
    float volBounds[3]{};           /* full box size, app units; all 0 = point */
    A3DVolSrcDampInfo volDamp{sizeof(A3DVolSrcDampInfo), 0.0f, 0.0f, 0.5f, 4, FALSE};
    float revLevel{1.0f}, revHiFreq{0.0f}, revBright{1.0f};
    LONG sentRoom{-1};              /* EAX room level last sent; -1 = none yet */
    std::vector<Reflection*> manual;
    std::string filePath;
    DWORD fileType{A3DFMT_WAVE};
    A3DSourceMp3Info mp3Info{};

    /* Deferred commands. */
    bool pendingPlay{false}, pendingStop{false};
    bool loop{false};

    /* Results of the last Flush. */
    float audibility{0.0f}, occlusion{0.0f};
    std::vector<ReflVoice> refl;

    Source(Engine *e, DWORD t);
    ~Source();

    bool wants3D() const
    { return !(renderMode & A3DRM_NATIVE) && haveFmt; }
    const WAVEFORMATEX &bufFmt() const { return mixdown ? dsFmt : fmt; }
    DWORD toApp(DWORD dsPos) const { return mixdown ? dsPos * 2 : dsPos; }
    DWORD toDS(DWORD appPos) const { return mixdown ? appPos / 2 : appPos; }
    void mixRange(DWORD appOff, DWORD n);
    HRESULT fillFromApp(const unsigned char *data, DWORD n);
    HRESULT createBuffer(DWORD size);
    void attachBuffer(IDirectSoundBuffer *b);
    void releaseBuffer();
    HRESULT rebuildBuffer();
    void stopReflections();
    void setDirectHF(IKsPropertySet *p, float hfGain);
    void setRoom(LONG mB);
    bool volumetric() const { return volBounds[0] > 0.0f || volBounds[1] > 0.0f || volBounds[2] > 0.0f; }
    void freeManual();

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override;
    ULONG STDMETHODCALLTYPE AddRef() override { return ++mRef; }
    ULONG STDMETHODCALLTYPE Release() override;

    HRESULT STDMETHODCALLTYPE LoadWaveFile(char *path) override;
    HRESULT STDMETHODCALLTYPE LoadWaveData(void *data, DWORD size) override;
    HRESULT STDMETHODCALLTYPE AllocateWaveData(INT size) override;
    HRESULT STDMETHODCALLTYPE FreeWaveData() override;
    HRESULT STDMETHODCALLTYPE SetWaveFormat(void *f) override;
    HRESULT STDMETHODCALLTYPE GetWaveFormat(void *f) override;
    HRESULT STDMETHODCALLTYPE GetWaveSize() override;
    HRESULT STDMETHODCALLTYPE GetType(DWORD *t) override;
    HRESULT STDMETHODCALLTYPE Lock(DWORD off, DWORD n, void **p1, DWORD *n1, void **p2, DWORD *n2, DWORD flags) override;
    HRESULT STDMETHODCALLTYPE Unlock(void *p1, DWORD n1, void *p2, DWORD n2) override;
    HRESULT STDMETHODCALLTYPE Play(INT mode) override;
    HRESULT STDMETHODCALLTYPE Stop() override;
    HRESULT STDMETHODCALLTYPE Rewind() override;
    HRESULT STDMETHODCALLTYPE SetWaveTime(A3DVAL t) override;
    HRESULT STDMETHODCALLTYPE GetWaveTime(A3DVAL *t) override;
    HRESULT STDMETHODCALLTYPE SetWavePosition(DWORD p) override;
    HRESULT STDMETHODCALLTYPE GetWavePosition(DWORD *p) override;
    HRESULT STDMETHODCALLTYPE SetPosition3f(A3DVAL x, A3DVAL y, A3DVAL z) override;
    HRESULT STDMETHODCALLTYPE GetPosition3f(A3DVAL *x, A3DVAL *y, A3DVAL *z) override;
    HRESULT STDMETHODCALLTYPE SetPosition3fv(A3DVAL *v) override;
    HRESULT STDMETHODCALLTYPE GetPosition3fv(A3DVAL *v) override;
    HRESULT STDMETHODCALLTYPE SetOrientationAngles3f(A3DVAL h, A3DVAL p, A3DVAL r) override;
    HRESULT STDMETHODCALLTYPE GetOrientationAngles3f(A3DVAL *h, A3DVAL *p, A3DVAL *r) override;
    HRESULT STDMETHODCALLTYPE SetOrientationAngles3fv(A3DVAL *v) override;
    HRESULT STDMETHODCALLTYPE GetOrientationAngles3fv(A3DVAL *v) override;
    HRESULT STDMETHODCALLTYPE SetOrientation6f(A3DVAL fx, A3DVAL fy, A3DVAL fz, A3DVAL ux, A3DVAL uy, A3DVAL uz) override;
    HRESULT STDMETHODCALLTYPE GetOrientation6f(A3DVAL *fx, A3DVAL *fy, A3DVAL *fz, A3DVAL *ux, A3DVAL *uy, A3DVAL *uz) override;
    HRESULT STDMETHODCALLTYPE SetOrientation6fv(A3DVAL *v) override;
    HRESULT STDMETHODCALLTYPE GetOrientation6fv(A3DVAL *v) override;
    HRESULT STDMETHODCALLTYPE SetVelocity3f(A3DVAL x, A3DVAL y, A3DVAL z) override;
    HRESULT STDMETHODCALLTYPE GetVelocity3f(A3DVAL *x, A3DVAL *y, A3DVAL *z) override;
    HRESULT STDMETHODCALLTYPE SetVelocity3fv(A3DVAL *v) override;
    HRESULT STDMETHODCALLTYPE GetVelocity3fv(A3DVAL *v) override;
    HRESULT STDMETHODCALLTYPE SetCone(A3DVAL inner, A3DVAL outer, A3DVAL g) override;
    HRESULT STDMETHODCALLTYPE GetCone(A3DVAL *inner, A3DVAL *outer, A3DVAL *g) override;
    HRESULT STDMETHODCALLTYPE SetMinMaxDistance(A3DVAL mn, A3DVAL mx, DWORD b) override;
    HRESULT STDMETHODCALLTYPE GetMinMaxDistance(A3DVAL *mn, A3DVAL *mx, DWORD *b) override;
    HRESULT STDMETHODCALLTYPE SetGain(A3DVAL g) override;
    HRESULT STDMETHODCALLTYPE GetGain(A3DVAL *g) override;
    HRESULT STDMETHODCALLTYPE SetPitch(A3DVAL p) override;
    HRESULT STDMETHODCALLTYPE GetPitch(A3DVAL *p) override;
    HRESULT STDMETHODCALLTYPE SetDopplerScale(A3DVAL s) override;
    HRESULT STDMETHODCALLTYPE GetDopplerScale(A3DVAL *s) override;
    HRESULT STDMETHODCALLTYPE SetDistanceModelScale(A3DVAL s) override;
    HRESULT STDMETHODCALLTYPE GetDistanceModelScale(A3DVAL *s) override;
    HRESULT STDMETHODCALLTYPE SetEq(A3DVAL e) override;
    HRESULT STDMETHODCALLTYPE GetEq(A3DVAL *e) override;
    HRESULT STDMETHODCALLTYPE SetPriority(A3DVAL p) override;
    HRESULT STDMETHODCALLTYPE GetPriority(A3DVAL *p) override;
    HRESULT STDMETHODCALLTYPE SetRenderMode(DWORD m) override;
    HRESULT STDMETHODCALLTYPE GetRenderMode(DWORD *m) override;
    HRESULT STDMETHODCALLTYPE GetAudibility(A3DVAL *a) override;
    HRESULT STDMETHODCALLTYPE GetOcclusionFactor(A3DVAL *o) override;
    HRESULT STDMETHODCALLTYPE GetStatus(DWORD *s) override;
    HRESULT STDMETHODCALLTYPE SetPanValues(DWORD n, A3DVAL *g) override;
    HRESULT STDMETHODCALLTYPE GetPanValues(DWORD n, A3DVAL *g) override;
    HRESULT STDMETHODCALLTYPE SetWaveEvent(DWORD off, HANDLE ev) override;
    HRESULT STDMETHODCALLTYPE ClearWaveEvents() override;
    HRESULT STDMETHODCALLTYPE SetTransformMode(DWORD m) override;
    HRESULT STDMETHODCALLTYPE GetTransformMode(DWORD *m) override;
    HRESULT STDMETHODCALLTYPE SetReflectionDelayScale(A3DVAL s) override;
    HRESULT STDMETHODCALLTYPE GetReflectionDelayScale(A3DVAL *s) override;
    HRESULT STDMETHODCALLTYPE SetReflectionGainScale(A3DVAL s) override;
    HRESULT STDMETHODCALLTYPE GetReflectionGainScale(A3DVAL *s) override;

    /* IA3dSource2 */
    HRESULT STDMETHODCALLTYPE SetVolumetricBounds(A3DVAL dx, A3DVAL dy, A3DVAL dz) override;
    HRESULT STDMETHODCALLTYPE GetVolumetricBounds(A3DVAL *dx, A3DVAL *dy, A3DVAL *dz) override;
    HRESULT STDMETHODCALLTYPE SetVolumetricDamping(A3DVolSrcDampInfo *i) override;
    HRESULT STDMETHODCALLTYPE GetVolumetricDamping(A3DVolSrcDampInfo *i) override;
    HRESULT STDMETHODCALLTYPE SetReverbMix(A3DVAL level, A3DVAL hiFreq) override;
    HRESULT STDMETHODCALLTYPE GetReverbMix(A3DVAL *level, A3DVAL *hiFreq) override;
    HRESULT STDMETHODCALLTYPE NewManualReflection(IA3dReflectionX **out) override;
    HRESULT STDMETHODCALLTYPE FreeManualReflections() override;
    HRESULT STDMETHODCALLTYPE GetNumManualReflections(int *n) override;
    HRESULT STDMETHODCALLTYPE LoadFile(char *path, DWORD format) override;
    HRESULT STDMETHODCALLTYPE GetCaps(A3DCapsSource *caps) override;
};

class Engine final : public IA3d5X, public IA3dGeom2X, public IA3dListenerX {
    std::atomic<LONG> mRef{1};
public:
    std::recursive_mutex lock;

    IDirectSound *ds{nullptr};
    IDirectSoundBuffer *primary{nullptr};
    IDirectSound3DListener *dsListener{nullptr};
    bool inited{false};
    DWORD available{0}, enabled{0};

    DWORD outFront{1}, outRear{1}, outQuad{1};
    DWORD rmMode{0}, version{0}, coop{DSSCL_NORMAL}, fallback{12};
    float hfAbsorb{1.0f}, maxReflDelay{0.3f}, outputGain{1.0f}, rmBias{0.5f};
    float unitsPerMeter{1.0f}, dopplerScale{1.0f}, dmsGlobal{1.0f}, eqGlobal{1.0f};
    DWORD coordSys{A3DCS_RIGHT_HANDED};

    /* Listener (app units, relative to its bound matrix). */
    V3 lPos, lVel, lFront{0, 0, -1}, lUp{0, 1, 0};
    float lAngles[3]{};
    M4 lBound{M4::identity()};

    /* Geometry state. */
    std::vector<M4> stack{M4::identity()};
    MatProps curMat;
    DWORD polyMode{A3DPOLY_REFLECTIONS | A3DPOLY_OCCLUSIONS};
    DWORD occlusionMode{0}, reflectionMode{0};
    float reflGainScale{1.0f}, reflDelayScale{1.0f}, bloat{0.0f};
    DWORD reflInterval{1}, occlInterval{1};
    bool inBegin{false};
    DWORD beginType{0};
    std::vector<V3> verts;
    bool haveNormal{false};
    V3 curNormal;
    DWORD curTag{0};
    float curOpening{0.0f};
    const float *curOpeningPtr{nullptr};
    List *recording{nullptr};
    std::vector<M4> savedStack;
    std::vector<Poly> back, front;
    std::vector<std::vector<int>> children;

    /* Listener frame in world space, worked out at each Flush. */
    V3 wP, wR{1, 0, 0}, wU{0, 1, 0}, wF{0, 0, -1}, wV;
    DWORD flushCount{0};

    std::vector<Source*> sources;
    int reflVoicesInUse{0};

    /* A3D 3.0. */
    DWORD requested{0};
    Reverb *reverb{nullptr};               /* bound reverb, referenced */
    Reverb *appliedReverb{nullptr};        /* identity only, not referenced */
    unsigned appliedGen{0};
    bool reverbWasOn{false};
    IDirectSoundBuffer *fxBuf{nullptr};    /* a silent buffer for listener EAX */
    IKsPropertySet *fxPs{nullptr};
    DWORD streamMs{900}, streamPrio{0}, maxHwSources{0};
    A3DGeomReverbParam geomReverb{sizeof(A3DGeomReverbParam), 1.0f, 1.0f, 1};
    DWORD nextReflId{1};
    bool loggedCustomExtras{false};
    DWORD lastFlushTick{0};
    std::thread worker;
    std::atomic<bool> quit{false};

    Engine();
    ~Engine();

    bool leftHanded() const { return coordSys == A3DCS_LEFT_HANDED; }
    std::vector<Poly> &target() { return recording ? recording->polys : back; }
    void emitPoly(int count);
    void flushFrame(bool fromApp);
    void processSource(Source *s);
    void updateReflections(Source *s, const V3 &S, float directDist, float baseGain,
        float baseHF, DWORD freq, bool starting, bool geometric);
    bool createReflVoice(Source *s, ReflVoice &nv, float delay);
    void applyReverb();
    void workerLoop();
    void shutdown();

    /* IUnknown */
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override;
    ULONG STDMETHODCALLTYPE AddRef() override { return ++mRef; }
    ULONG STDMETHODCALLTYPE Release() override;

    /* IA3d4 */
    HRESULT STDMETHODCALLTYPE SetOutputMode(DWORD f, DWORD r, DWORD q) override;
    HRESULT STDMETHODCALLTYPE GetOutputMode(DWORD *f, DWORD *r, DWORD *q) override;
    HRESULT STDMETHODCALLTYPE SetResourceManagerMode(DWORD m) override;
    HRESULT STDMETHODCALLTYPE GetResourceManagerMode(DWORD *m) override;
    HRESULT STDMETHODCALLTYPE SetHFAbsorbFactor(float f) override;
    HRESULT STDMETHODCALLTYPE GetHFAbsorbFactor(float *f) override;
    HRESULT STDMETHODCALLTYPE RegisterVersion(DWORD v) override;
    HRESULT STDMETHODCALLTYPE GetSoftwareCaps(void *c) override;
    HRESULT STDMETHODCALLTYPE GetHardwareCaps(void *c) override;
    HRESULT STDMETHODCALLTYPE Clear() override;
    HRESULT STDMETHODCALLTYPE Flush() override;
    HRESULT STDMETHODCALLTYPE Compat(DWORD, DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Init(GUID *dev, DWORD features, DWORD prefs) override;
    HRESULT STDMETHODCALLTYPE IsFeatureAvailable(DWORD f) override;
    HRESULT STDMETHODCALLTYPE NewSource(DWORD type, IA3dSourceX **out) override;
    HRESULT STDMETHODCALLTYPE DuplicateSource(IA3dSourceX *orig, IA3dSourceX **out) override;
    HRESULT STDMETHODCALLTYPE SetCooperativeLevel(HWND hwnd, DWORD level) override;
    HRESULT STDMETHODCALLTYPE GetCooperativeLevel(DWORD *level) override;
    HRESULT STDMETHODCALLTYPE SetMaxReflectionDelayTime(A3DVAL s) override;
    HRESULT STDMETHODCALLTYPE GetMaxReflectionDelayTime(A3DVAL *s) override;
    HRESULT STDMETHODCALLTYPE SetCoordinateSystem(DWORD cs) override;
    HRESULT STDMETHODCALLTYPE GetCoordinateSystem(DWORD *cs) override;
    HRESULT STDMETHODCALLTYPE SetOutputGain(A3DVAL g) override;
    HRESULT STDMETHODCALLTYPE GetOutputGain(A3DVAL *g) override;
    HRESULT STDMETHODCALLTYPE SetNumFallbackSources(DWORD n) override;
    HRESULT STDMETHODCALLTYPE GetNumFallbackSources(DWORD *n) override;
    HRESULT STDMETHODCALLTYPE SetRMPriorityBias(A3DVAL b) override;
    HRESULT STDMETHODCALLTYPE GetRMPriorityBias(A3DVAL *b) override;
    HRESULT STDMETHODCALLTYPE DisableViewer() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetUnitsPerMeter(A3DVAL u) override;
    HRESULT STDMETHODCALLTYPE GetUnitsPerMeter(A3DVAL *u) override;
    HRESULT STDMETHODCALLTYPE SetDopplerScale(A3DVAL s) override;
    HRESULT STDMETHODCALLTYPE GetDopplerScale(A3DVAL *s) override;
    HRESULT STDMETHODCALLTYPE SetDistanceModelScale(A3DVAL s) override;
    HRESULT STDMETHODCALLTYPE GetDistanceModelScale(A3DVAL *s) override;
    HRESULT STDMETHODCALLTYPE SetEq(A3DVAL e) override;
    HRESULT STDMETHODCALLTYPE GetEq(A3DVAL *e) override;
    HRESULT STDMETHODCALLTYPE Shutdown() override;
    HRESULT STDMETHODCALLTYPE RegisterApp(REFIID) override { return S_OK; }

    /* IA3d5 */
    HRESULT STDMETHODCALLTYPE InitEx(GUID *dev, DWORD features, DWORD prefs, HWND hwnd, DWORD coop) override;
    HRESULT STDMETHODCALLTYPE NewReverb(IA3dReverbX **out) override;
    HRESULT STDMETHODCALLTYPE BindReverb(IA3dReverbX *r) override;
    HRESULT STDMETHODCALLTYPE GetStreamingProperties(DWORD *ms, DWORD *prio) override;
    HRESULT STDMETHODCALLTYPE SetStreamingProperties(DWORD ms, DWORD prio) override;
    HRESULT STDMETHODCALLTYPE A3dEnumerate(A3DEnumCallback cb, void *ctx) override;
    HRESULT STDMETHODCALLTYPE UnlockFallbackAC3Decoder(char *, DWORD) override;
    HRESULT STDMETHODCALLTYPE SetMaxHardwareSources(DWORD n) override;
    HRESULT STDMETHODCALLTYPE GetMaxHardwareSources(DWORD *n) override;

    /* IA3dGeom */
    HRESULT STDMETHODCALLTYPE Enable(DWORD f) override;
    HRESULT STDMETHODCALLTYPE Disable(DWORD f) override;
    BOOL STDMETHODCALLTYPE IsEnabled(DWORD f) override;
    HRESULT STDMETHODCALLTYPE SetOcclusionMode(DWORD m) override;
    HRESULT STDMETHODCALLTYPE GetOcclusionMode(DWORD *m) override;
    HRESULT STDMETHODCALLTYPE SetReflectionMode(DWORD m) override;
    HRESULT STDMETHODCALLTYPE GetReflectionMode(DWORD *m) override;
    HRESULT STDMETHODCALLTYPE SetReflectionGainScale(A3DVAL s) override;
    HRESULT STDMETHODCALLTYPE GetReflectionGainScale(A3DVAL *s) override;
    HRESULT STDMETHODCALLTYPE SetReflectionDelayScale(A3DVAL s) override;
    HRESULT STDMETHODCALLTYPE GetReflectionDelayScale(A3DVAL *s) override;
    ULONG STDMETHODCALLTYPE PushMatrix() override;
    ULONG STDMETHODCALLTYPE PopMatrix() override;
    HRESULT STDMETHODCALLTYPE LoadIdentity() override;
    HRESULT STDMETHODCALLTYPE LoadMatrix(A3DVAL *m) override;
    HRESULT STDMETHODCALLTYPE GetMatrix(A3DVAL *m) override;
    HRESULT STDMETHODCALLTYPE MultMatrix(A3DVAL *m) override;
    HRESULT STDMETHODCALLTYPE Translate3f(A3DVAL x, A3DVAL y, A3DVAL z) override;
    HRESULT STDMETHODCALLTYPE Translate3fv(A3DVAL *v) override;
    HRESULT STDMETHODCALLTYPE Rotate3f(A3DVAL d, A3DVAL x, A3DVAL y, A3DVAL z) override;
    HRESULT STDMETHODCALLTYPE Rotate3fv(A3DVAL d, A3DVAL *v) override;
    HRESULT STDMETHODCALLTYPE Scale3f(A3DVAL x, A3DVAL y, A3DVAL z) override;
    HRESULT STDMETHODCALLTYPE Scale3fv(A3DVAL *v) override;
    HRESULT STDMETHODCALLTYPE Begin(DWORD type) override;
    HRESULT STDMETHODCALLTYPE End() override;
    HRESULT STDMETHODCALLTYPE Vertex3f(A3DVAL x, A3DVAL y, A3DVAL z) override;
    HRESULT STDMETHODCALLTYPE Vertex3fv(A3DVAL *v) override;
    HRESULT STDMETHODCALLTYPE Normal3f(A3DVAL x, A3DVAL y, A3DVAL z) override;
    HRESULT STDMETHODCALLTYPE Normal3fv(A3DVAL *v) override;
    HRESULT STDMETHODCALLTYPE Tag(DWORD t) override;
    HRESULT STDMETHODCALLTYPE SetOpeningFactorf(A3DVAL f) override;
    HRESULT STDMETHODCALLTYPE SetOpeningFactorfv(A3DVAL *f) override;
    HRESULT STDMETHODCALLTYPE NewMaterial(IA3dMaterialX **out) override;
    HRESULT STDMETHODCALLTYPE BindMaterial(IA3dMaterialX *m) override;
    HRESULT STDMETHODCALLTYPE NewList(IA3dListX **out) override;
    HRESULT STDMETHODCALLTYPE BindListener() override;
    HRESULT STDMETHODCALLTYPE BindSource(IA3dSourceX *s) override;
    HRESULT STDMETHODCALLTYPE NewEnvironment(void **out) override;
    HRESULT STDMETHODCALLTYPE BindEnvironment(void *) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetRenderMode(DWORD m) override;
    HRESULT STDMETHODCALLTYPE GetRenderMode(DWORD *m) override;
    HRESULT STDMETHODCALLTYPE SetPolygonBloatFactor(A3DVAL f) override;
    HRESULT STDMETHODCALLTYPE GetPolygonBloatFactor(A3DVAL *f) override;
    HRESULT STDMETHODCALLTYPE SetReflectionUpdateInterval(DWORD n) override;
    HRESULT STDMETHODCALLTYPE GetReflectionUpdateInterval(DWORD *n) override;
    HRESULT STDMETHODCALLTYPE SetOcclusionUpdateInterval(DWORD n) override;
    HRESULT STDMETHODCALLTYPE GetOcclusionUpdateInterval(DWORD *n) override;
    /* IA3dGeom2 */
    HRESULT STDMETHODCALLTYPE SetGeomReverbParam(A3DGeomReverbParam *p) override;
    HRESULT STDMETHODCALLTYPE GetGeomReverbParam(A3DGeomReverbParam *p) override;

    /* IA3dListener */
    HRESULT STDMETHODCALLTYPE SetPosition3f(A3DVAL x, A3DVAL y, A3DVAL z) override;
    HRESULT STDMETHODCALLTYPE GetPosition3f(A3DVAL *x, A3DVAL *y, A3DVAL *z) override;
    HRESULT STDMETHODCALLTYPE SetPosition3fv(A3DVAL *v) override;
    HRESULT STDMETHODCALLTYPE GetPosition3fv(A3DVAL *v) override;
    HRESULT STDMETHODCALLTYPE SetOrientationAngles3f(A3DVAL h, A3DVAL p, A3DVAL r) override;
    HRESULT STDMETHODCALLTYPE GetOrientationAngles3f(A3DVAL *h, A3DVAL *p, A3DVAL *r) override;
    HRESULT STDMETHODCALLTYPE SetOrientationAngles3fv(A3DVAL *v) override;
    HRESULT STDMETHODCALLTYPE GetOrientationAngles3fv(A3DVAL *v) override;
    HRESULT STDMETHODCALLTYPE SetOrientation6f(A3DVAL fx, A3DVAL fy, A3DVAL fz, A3DVAL ux, A3DVAL uy, A3DVAL uz) override;
    HRESULT STDMETHODCALLTYPE GetOrientation6f(A3DVAL *fx, A3DVAL *fy, A3DVAL *fz, A3DVAL *ux, A3DVAL *uy, A3DVAL *uz) override;
    HRESULT STDMETHODCALLTYPE SetOrientation6fv(A3DVAL *v) override;
    HRESULT STDMETHODCALLTYPE GetOrientation6fv(A3DVAL *v) override;
    HRESULT STDMETHODCALLTYPE SetVelocity3f(A3DVAL x, A3DVAL y, A3DVAL z) override;
    HRESULT STDMETHODCALLTYPE GetVelocity3f(A3DVAL *x, A3DVAL *y, A3DVAL *z) override;
    HRESULT STDMETHODCALLTYPE SetVelocity3fv(A3DVAL *v) override;
    HRESULT STDMETHODCALLTYPE GetVelocity3fv(A3DVAL *v) override;
};

extern std::atomic<LONG> gObjects;

} // namespace a3d
