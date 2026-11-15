/* Shared helpers for the A3D emulation DLLs: settings, logging, loading the
 * DirectSound implementation, and small math types. */
#pragma once

#define A3D_DSOAL_VERSION "0.1"

#include <windows.h>
#include <mmsystem.h>
#include <mmreg.h>
#include <dsound.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace a3d {

struct Settings {
    bool log{false};
    int maxReflections{4};        /* reflection voices per source */
    int maxReflectionVoices{48};  /* reflection voices in total */
    bool autoFlush{true};         /* keep sources moving for apps that never call Flush */
    float hfAbsorbPerMeter{0.037f};   /* distance treble loss, see engine.cpp */
    char dsoundPath[MAX_PATH]{"dsound.dll"};
    /* Output mode, as the A3D control panel's speaker setting:
       "headphones" (default), "speakers" (also "satellites"/"monitors"),
       or "quad". From -a3dmode on the command line, else a3d.ini. */
    char mode[16]{"headphones"};
};

const Settings &settings();
void logf(const char *fmt, ...);

/* DirectSoundCreate from whichever dsound.dll the settings name (by default
 * the normal search order, so a DSOAL dsound.dll next to the game wins). */
HRESULT createDirectSound(const GUID *dev, IDirectSound **out);

inline LONG gainTo_mB(float g)
{
    if(!(g > 0.00001f)) return -10000;
    const float mb{2000.0f * std::log10(g)};
    return static_cast<LONG>(std::clamp(mb, -10000.0f, 0.0f));
}

inline LONG gainTo_mB_signed(float g, LONG lo, LONG hi)
{
    if(!(g > 0.00001f)) return lo;
    const float mb{2000.0f * std::log10(g)};
    return static_cast<LONG>(std::clamp(mb, float(lo), float(hi)));
}

struct V3 {
    float x{0}, y{0}, z{0};
    V3 operator+(const V3 &o) const { return {x+o.x, y+o.y, z+o.z}; }
    V3 operator-(const V3 &o) const { return {x-o.x, y-o.y, z-o.z}; }
    V3 operator*(float s) const { return {x*s, y*s, z*s}; }
};
inline float dot(const V3 &a, const V3 &b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
inline V3 cross(const V3 &a, const V3 &b)
{ return {a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x}; }
inline float length(const V3 &a) { return std::sqrt(dot(a, a)); }
inline V3 normalize(const V3 &a)
{
    const float l{length(a)};
    return (l > 1e-12f) ? a*(1.0f/l) : V3{};
}

/* Column-major 4x4, as OpenGL and the A3D matrix calls use it. */
struct M4 {
    float m[16];
    static M4 identity()
    {
        M4 r{};
        r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
        return r;
    }
    V3 point(const V3 &v) const
    {
        return {m[0]*v.x + m[4]*v.y + m[8]*v.z + m[12],
                m[1]*v.x + m[5]*v.y + m[9]*v.z + m[13],
                m[2]*v.x + m[6]*v.y + m[10]*v.z + m[14]};
    }
    V3 dir(const V3 &v) const
    {
        return {m[0]*v.x + m[4]*v.y + m[8]*v.z,
                m[1]*v.x + m[5]*v.y + m[9]*v.z,
                m[2]*v.x + m[6]*v.y + m[10]*v.z};
    }
    M4 operator*(const M4 &b) const
    {
        M4 r{};
        for(int c{0};c < 4;++c)
            for(int rr{0};rr < 4;++rr)
            {
                float s{0};
                for(int k{0};k < 4;++k)
                    s += m[k*4 + rr] * b.m[c*4 + k];
                r.m[c*4 + rr] = s;
            }
        return r;
    }
    static M4 translate(float x, float y, float z)
    {
        M4 r{identity()};
        r.m[12] = x; r.m[13] = y; r.m[14] = z;
        return r;
    }
    static M4 scale(float x, float y, float z)
    {
        M4 r{identity()};
        r.m[0] = x; r.m[5] = y; r.m[10] = z;
        return r;
    }
    /* Counter-clockwise rotation by deg about (x,y,z), as glRotate. */
    static M4 rotate(float deg, float x, float y, float z)
    {
        const V3 a{normalize({x, y, z})};
        const float t{deg * 3.14159265358979f / 180.0f};
        const float c{std::cos(t)}, s{std::sin(t)}, k{1.0f - c};
        M4 r{identity()};
        r.m[0] = a.x*a.x*k + c;     r.m[4] = a.x*a.y*k - a.z*s; r.m[8]  = a.x*a.z*k + a.y*s;
        r.m[1] = a.y*a.x*k + a.z*s; r.m[5] = a.y*a.y*k + c;     r.m[9]  = a.y*a.z*k - a.x*s;
        r.m[2] = a.z*a.x*k - a.y*s; r.m[6] = a.z*a.y*k + a.x*s; r.m[10] = a.z*a.z*k + c;
        return r;
    }
};

/* Heading about Y, then pitch about X, then roll about Z, applied to the
 * default front (0,0,-1 right-handed, 0,0,1 left-handed) and up (0,1,0). */
inline void anglesToVectors(float h, float p, float r, bool leftHanded, V3 &front, V3 &up)
{
    const M4 rot{M4::rotate(h, 0, 1, 0) * M4::rotate(p, 1, 0, 0) * M4::rotate(r, 0, 0, 1)};
    front = rot.dir({0, 0, leftHanded ? 1.0f : -1.0f});
    up = rot.dir({0, 1, 0});
}

inline void vectorsToAngles(const V3 &f0, const V3 &u0, bool leftHanded, float &h, float &p, float &r)
{
    constexpr float todeg{180.0f / 3.14159265358979f};
    const float s{leftHanded ? -1.0f : 1.0f};
    const V3 f{normalize(f0)}, u{normalize(u0)};
    p = std::asin(std::clamp(s*f.y, -1.0f, 1.0f)) * todeg;
    h = std::atan2(-s*f.x, -s*f.z) * todeg;
    const M4 hp{M4::rotate(h, 0, 1, 0) * M4::rotate(p, 1, 0, 0)};
    const V3 x0{hp.dir({1, 0, 0})}, y0{hp.dir({0, 1, 0})};
    r = std::atan2(-dot(u, x0), dot(u, y0)) * todeg;
}

} // namespace a3d
