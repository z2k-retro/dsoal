#include <cstring>
#include <string>
#include "common.h"

#include <mutex>

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
    applyMode(dir);

    if(gSettings.log)
    {
        char logpath[MAX_PATH]{};
        snprintf(logpath, sizeof(logpath), "%sa3d.log", dir);
        gLogFile = std::fopen(logpath, "a");
    }
    /* Not logf: that calls settings(), which is still running here. */
    if(gLogFile)
        std::fprintf(gLogFile, "[%lu] a3d: A3D on DSOAL, version " A3D_DSOAL_VERSION "\n", GetTickCount());
        std::fprintf(gLogFile, "[%lu] a3d: speaker mode %s%s\n", GetTickCount(), gSettings.mode,
            gModeLate ? " (dsound.dll was already loaded; if the program opened DirectSound"
                " first, OpenAL Soft keeps alsoft.ini's mode)" : "");
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
