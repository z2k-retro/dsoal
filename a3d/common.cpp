#include <cstring>
#include <string>
#include "common.h"

#include <mutex>

#include <mmdeviceapi.h>
#include <audiopolicy.h>

namespace a3d {

namespace {

Settings gSettings;
bool gModeLate{false};
std::once_flag gSettingsOnce;
std::mutex gLogLock;
FILE *gLogFile{nullptr};

void exeDir(char *out, size_t size)
{
    char path[MAX_PATH]{};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    char *slash{std::strrchr(path, '\\')};
    if(!slash) slash = std::strrchr(path, '/');
    if(slash) slash[1] = '\0';
    else path[0] = '\0';
    snprintf(out, size, "%s", path);
}

/* Hand the speaker mode to OpenAL Soft. It reads the file named by
   ALSOFT_CONF after alsoft.ini, so the settings here override the user's.
   This must happen before DSOAL opens its OpenAL device, i.e. before the
   first DirectSoundCreate: the A3D DLLs create DirectSound themselves, so
   unless the program opened DirectSound on its own first, it does. */

/* CD music level.
 *
 * On the sound card the CD was an analogue input, mixed well below the
 * card's own full scale. On current Windows, CD audio (MCI, or a winmm
 * replacement playing music files) is digital, in the game's process, at
 * full level, so against the game's effects it comes out much louder than
 * it did. Nothing lets one process turn down only its CD stream, so this
 * turns the whole process's audio session down by cd-volume-db and has
 * OpenAL Soft turn the game's own sound up by the same amount: the effects
 * end where they were and everything else in the process is quieter.
 *
 * Windows remembers a session's volume per program, so the volume found at
 * start is kept in a file and put back when the last A3D object goes (or at
 * the next start, if the program never released it). */
const GUID kCLSID_MMDeviceEnumerator{0xbcde0395, 0xe52f, 0x467c,
    {0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e}};
const GUID kIID_IMMDeviceEnumerator{0xa95664d2, 0x9614, 0x4f35,
    {0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6}};
const GUID kIID_IAudioSessionManager{0xbfa971f1, 0x4d5e, 0x40bb,
    {0x93, 0x5e, 0x96, 0x70, 0x39, 0xbf, 0xbe, 0xe4}};

float gSessionOrig{-1.0f};
float gCdBoostDb{0.0f};
char gSessionFile[MAX_PATH]{};

/* Runs fn with the process's default-session volume control, on a thread of
   its own so the caller's COM apartment is left alone. */
template<typename F> struct SessionCtx { F fn; bool ok; };

template<typename F> DWORD WINAPI sessionThread(void *p)
{
    auto *c = static_cast<SessionCtx<F>*>(p);
    if(FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)))
        return 0;
    IMMDeviceEnumerator *en{};
    IMMDevice *dev{};
    IAudioSessionManager *mgr{};
    ISimpleAudioVolume *vol{};
    if(SUCCEEDED(CoCreateInstance(kCLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL,
            kIID_IMMDeviceEnumerator, reinterpret_cast<void**>(&en)))
        && SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev))
        && SUCCEEDED(dev->Activate(kIID_IAudioSessionManager, CLSCTX_ALL, nullptr,
            reinterpret_cast<void**>(&mgr)))
        && SUCCEEDED(mgr->GetSimpleAudioVolume(nullptr, FALSE, &vol)))
        c->ok = c->fn(vol);
    if(vol) vol->Release();
    if(mgr) mgr->Release();
    if(dev) dev->Release();
    if(en) en->Release();
    CoUninitialize();
    return 0;
}

/* The wait is bounded in case this is ever reached with the loader lock
   held (a program releasing A3D from a DllMain); the context is then left
   for the thread. */
template<typename F> bool withSessionVolume(F fn)
{
    auto *ctx = new SessionCtx<F>{std::move(fn), false};
    HANDLE th{CreateThread(nullptr, 0, sessionThread<F>, ctx, 0, nullptr)};
    if(!th) { delete ctx; return false; }
    const bool done{WaitForSingleObject(th, 3000) == WAIT_OBJECT_0};
    CloseHandle(th);
    if(!done) return false;
    const bool ok{ctx->ok};
    delete ctx;
    return ok;
}

void cdLevelSetup(float db)
{
    if(!(db < -0.05f))
        return;
    char exe[MAX_PATH]{}, tmp[MAX_PATH]{};
    GetModuleFileNameA(nullptr, exe, sizeof(exe));
    const char *name{std::strrchr(exe, '\\')};
    name = name ? name+1 : exe;
    if(!GetTempPathA(sizeof(tmp), tmp))
        return;
    std::snprintf(gSessionFile, sizeof(gSessionFile), "%sa3d-session-%s.txt", tmp, name);

    /* A file left from a run that never restored holds the real original. */
    float saved{-1.0f};
    if(FILE *f{std::fopen(gSessionFile, "r")})
    {
        if(std::fscanf(f, "%f", &saved) != 1) saved = -1.0f;
        std::fclose(f);
    }
    const float factor{std::pow(10.0f, db / 20.0f)};
    static float orig{-1.0f};
    const bool ok{withSessionVolume([saved, factor](ISimpleAudioVolume *vol) -> bool {
        float cur{1.0f};
        if(FAILED(vol->GetMasterVolume(&cur))) return false;
        orig = (saved >= 0.0f && saved <= 1.0f) ? saved : cur;
        if(FILE *f{std::fopen(gSessionFile, "w")})
        {
            std::fprintf(f, "%f\n", orig);
            std::fclose(f);
        }
        return SUCCEEDED(vol->SetMasterVolume(orig * factor, nullptr));
    })};
    if(ok)
    {
        gSessionOrig = orig;
        gCdBoostDb = -db;
    }
}

void applyMode(const char *dir)
{
    const char *m{gSettings.mode};
    std::string conf{"[general]\nhrtf-paths = .\nfrequency = 48000\n"};
    if(!_stricmp(m, "speakers") || !_stricmp(m, "satellites") || !_stricmp(m, "monitors"))
    {
        /* The card used its HRTF plus a crosstalk canceller for speakers.
           OpenAL Soft has no canceller, so this uses its own stereo panning
           (UHJ), which holds the image better on speakers than HRTF alone. */
        conf += "channels = stereo\nstereo-mode = speakers\nstereo-encoding = uhj\n";
    }
    else if(!_stricmp(m, "quad") || !_stricmp(m, "four"))
        conf += "channels = quad\n";
    else
    {
        std::snprintf(gSettings.mode, sizeof(gSettings.mode), "headphones");
        conf += "channels = stereo\nstereo-mode = headphones\nstereo-encoding = hrtf\n"
            "default-hrtf = Aureal Vortex 2\n";
    }

    if(gCdBoostDb > 0.0f)
    {
        /* Make up for the session volume (see cdLevelSetup). Float output
           and no limiter, so the boosted mix reaches the session volume
           intact instead of being clipped or squashed on the way. */
        char line[96]{};
        std::snprintf(line, sizeof(line),
            "volume-adjust = %.2f\noutput-limiter = false\nsample-type = float32\n", gCdBoostDb);
        conf += line;
    }

    char path[MAX_PATH]{}, tmp[MAX_PATH]{};
    if(!GetTempPathA(sizeof(tmp), tmp))
        std::snprintf(tmp, sizeof(tmp), "%s", dir);
    std::snprintf(path, sizeof(path), "%sa3d-alsoft-%lu.ini", tmp, GetCurrentProcessId());
    if(FILE *f{std::fopen(path, "wb")})
    {
        std::fputs(conf.c_str(), f);
        std::fclose(f);
        SetEnvironmentVariableA("ALSOFT_CONF", path);
        _putenv_s("ALSOFT_CONF", path);
    }
    if(GetModuleHandleA("dsound.dll"))
        gModeLate = true;
}

void loadSettings()
{
    char dir[MAX_PATH]{}, ini[MAX_PATH]{};
    exeDir(dir, sizeof(dir));
    snprintf(ini, sizeof(ini), "%sa3d.ini", dir);

    gSettings.log = GetPrivateProfileIntA("a3d", "log", 0, ini) != 0;
    if(const char *env{std::getenv("A3D_LOG")}; env && *env && *env != '0')
        gSettings.log = true;
    gSettings.maxReflections = std::clamp<int>(
        GetPrivateProfileIntA("a3d", "reflections-per-source", 4, ini), 0, 16);
    gSettings.maxReflectionVoices = std::clamp<int>(
        GetPrivateProfileIntA("a3d", "reflection-voices", 48, ini), 0, 256);
    gSettings.autoFlush = GetPrivateProfileIntA("a3d", "auto-flush", 1, ini) != 0;
    char buf[64]{};
    GetPrivateProfileStringA("a3d", "hf-absorb-per-meter", "0.037", buf, sizeof(buf), ini);
    gSettings.hfAbsorbPerMeter = std::clamp(float(std::atof(buf)), 0.0f, 10.0f);
    GetPrivateProfileStringA("a3d", "dsound", "dsound.dll", gSettings.dsoundPath,
        sizeof(gSettings.dsoundPath), ini);

    /* Speaker mode. Programs had no launcher option for this (the A3D
       control panel set it system-wide), so it comes from the command line
       ("-a3dmode headphones|speakers|satellites|monitors|quad"), then
       a3d.ini ("mode="), with headphones as the default. */
    GetPrivateProfileStringA("a3d", "mode", "headphones", gSettings.mode,
        sizeof(gSettings.mode), ini);
    if(const char *cmd{GetCommandLineA()})
    {
        if(const char *opt{std::strstr(cmd, "-a3dmode")})
        {
            opt += 8;
            while(*opt == ' ' || *opt == '=' || *opt == ':') ++opt;
            size_t n{0};
            while(opt[n] && opt[n] != ' ' && opt[n] != '"' && n < sizeof(gSettings.mode)-1)
                ++n;
            if(n > 0)
            {
                std::memcpy(gSettings.mode, opt, n);
                gSettings.mode[n] = '\0';
            }
        }
    }
    /* CD music level, in dB against the game's own sound (0 = leave alone). */
    GetPrivateProfileStringA("a3d", "cd-volume-db", "-12", buf, sizeof(buf), ini);
    gSettings.cdVolumeDb = std::clamp(float(std::atof(buf)), -24.0f, 0.0f);
    cdLevelSetup(gSettings.cdVolumeDb);
    applyMode(dir);

    if(gSettings.log)
    {
        char logpath[MAX_PATH]{};
        snprintf(logpath, sizeof(logpath), "%sa3d.log", dir);
        gLogFile = std::fopen(logpath, "a");
    }
    /* Not logf: that calls settings(), which is still running here. */
    if(gLogFile)
    {
        std::fprintf(gLogFile, "[%lu] a3d: A3D on DSOAL, version " A3D_DSOAL_VERSION "\n", GetTickCount());
        std::fprintf(gLogFile, "[%lu] a3d: speaker mode %s%s\n", GetTickCount(), gSettings.mode,
            gModeLate ? " (dsound.dll was already loaded; if the program opened DirectSound"
                " first, OpenAL Soft keeps alsoft.ini's mode)" : "");
        if(gCdBoostDb > 0.0f)
            std::fprintf(gLogFile, "[%lu] a3d: CD level %.1f dB: session volume %.3f -> %.3f, "
                "OpenAL Soft +%.1f dB\n", GetTickCount(), -gCdBoostDb, gSessionOrig,
                gSessionOrig * std::pow(10.0f, -gCdBoostDb / 20.0f), gCdBoostDb);
        else if(gSettings.cdVolumeDb < -0.05f)
            std::fprintf(gLogFile, "[%lu] a3d: CD level not applied (no audio session control)\n",
                GetTickCount());
        std::fflush(gLogFile);
    }
}

} // namespace

const Settings &settings()
{
    std::call_once(gSettingsOnce, loadSettings);
    return gSettings;
}

void logf(const char *fmt, ...)
{
    if(!settings().log || !gLogFile)
        return;
    std::lock_guard<std::mutex> _{gLogLock};
    std::fprintf(gLogFile, "[%lu] ", GetTickCount());
    va_list args;
    va_start(args, fmt);
    std::vfprintf(gLogFile, fmt, args);
    va_end(args);
    std::fputc('\n', gLogFile);
    std::fflush(gLogFile);
}

void restoreSessionVolume()
{
    if(gSessionOrig < 0.0f)
        return;
    const float orig{gSessionOrig};
    gSessionOrig = -1.0f;
    if(withSessionVolume([orig](ISimpleAudioVolume *vol) -> bool
        { return SUCCEEDED(vol->SetMasterVolume(orig, nullptr)); }))
        DeleteFileA(gSessionFile);
    logf("session volume restored to %.3f", orig);
}

HRESULT createDirectSound(const GUID *dev, IDirectSound **out)
{
    using CreateFn = HRESULT(WINAPI*)(const GUID*, IDirectSound**, IUnknown*);
    static HMODULE mod{nullptr};
    static CreateFn create{nullptr};
    if(!create)
    {
        mod = LoadLibraryA(settings().dsoundPath);
        if(!mod && std::strcmp(settings().dsoundPath, "dsound.dll") != 0)
            mod = LoadLibraryA("dsound.dll");
        if(mod)
            create = reinterpret_cast<CreateFn>(
                reinterpret_cast<void*>(GetProcAddress(mod, "DirectSoundCreate")));
        char name[MAX_PATH]{};
        if(mod) GetModuleFileNameA(mod, name, MAX_PATH);
        logf("dsound: %s", mod ? name : "(failed to load)");
    }
    if(!create)
        return DSERR_NODRIVER;
    return create(dev, out, nullptr);
}

} // namespace a3d
