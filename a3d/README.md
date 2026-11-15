# Aureal A3D on DSOAL

This directory builds two DLLs that let games written for Aureal A3D run on a
modern PC, on top of DSOAL and OpenAL Soft:

| DLL          | API      | How games reach it |
|--------------|----------|--------------------|
| `a3d.dll`    | A3D 1.x  | `CoCreateInstance(CLSID_A3d)` or the `A3dCreate` export. It is a DirectSound object that also answers to `IA3d` / `IA3d2`. 3D sound goes through ordinary DirectSound 3D buffers. |
| `a3dapi.dll` | A3D 2.0  | `CoCreateInstance(CLSID_A3dApi)`. Implements `IA3d4` (and `IA3d3`/`IA3d2`/`IA3d`), `IA3dGeom`, `IA3dListener`, `IA3dSource`, `IA3dMaterial` and `IA3dList`. |

Both load `dsound.dll` the normal way, so a DSOAL `dsound.dll` next to the
game is the one they use.

**Status: version 0.1, a first test release.** It has been built and run
against this directory's own test programs. Testing in games is only
starting, so expect problems, and please report what you find (with
`a3d.log`; see below). The direction filters are the Vortex 2's own, read
out of a real card as part of a reverse-engineering project,
[aureal-vortex2-tools](https://github.com/z2k-retro/aureal-vortex2-tools),
which also documents how the card and its driver treat each A3D setting.

## Install

Put these next to the game's `.exe`:

- `dsound.dll` (DSOAL) and `dsoal-aldrv.dll` (OpenAL Soft)
- `a3d.dll`, `a3dapi.dll`
- `Aureal Vortex 2.mhr` (from `hrtf/`)
- optionally `alsoft.ini` (copy of `alsoft.ini.sample`) and `a3d.ini` (copy of `a3d.ini.sample`)

The release zip has all of these, built for 32-bit Windows (A3D games are
32-bit). To see what the DLLs are doing, set `log=1` under `[a3d]` in
`a3d.ini`; they then write `a3d.log` next to the game, starting with the
version and the speaker mode.

## Speaker mode

The A3D control panel set the speaker type for the whole system; games had no
option for it. Here it comes from the game's command line, then `a3d.ini`
(`mode=`), with headphones as the default:

    hl.exe -a3dmode headphones     (default: the Vortex 2 HRTF)
    hl.exe -a3dmode speakers       (also "satellites" or "monitors")
    hl.exe -a3dmode quad           (four speakers)

The DLLs pass the mode to OpenAL Soft through a small settings file named by
`ALSOFT_CONF`, read after `alsoft.ini`, so it overrides that file's output
settings. On speakers the real card used its HRTF followed by a crosstalk
canceller; OpenAL Soft has no canceller, so speaker mode uses OpenAL Soft's
own stereo (UHJ) panning for now. The mode only takes effect if the A3D DLLs
create DirectSound before the program does, which is the normal case for A3D
games; `a3d.log` (with `log=1`) records the mode and says if it came too
late.

Games that use the A3D 2.0 helper library register `a3d.dll` and
`a3dapi.dll` themselves by plain file name, which Windows then finds in the
game's folder. For games that expect the driver to have done it, run
`regsvr32 a3d.dll` and `regsvr32 a3dapi.dll` once from the game's folder.

## How A3D 2.0 is rendered

The model follows what the Vortex 2 driver does with the chip, expressed
through DirectSound:

- **Direction** (the HRTF): each 3D source is a head-relative DirectSound 3D
  buffer. DSOAL hands it to OpenAL Soft, which filters it with the
  `Aureal Vortex 2` HRTF set: the card's own 56-tap direction filters and
  inter-aural delays, read out of the hardware on a 15° grid.
- **Distance**: computed here with the A3D model (no attenuation inside the
  minimum distance, then 6 dB per doubling, scaled by the source and global
  distance-model scales; `A3D_MUTE` silences beyond the maximum distance).
- **Cones, gain, output gain, pitch, Doppler**: computed here and sent as
  volume and frequency.
- **High frequencies**: the Vortex 2 driver folds every treble control
  into one brightness value, the product of the source and global `SetEq`,
  distance (falling as e^(−0.037 × metres beyond the minimum distance)) and
  the high-frequency transmittance of any walls in the way. It then
  programs a two-pole low-pass from it. The mapping here is the one read
  out of traces of the driver: `SetEq(0.5)` is about −14 dB at 5 kHz, and
  64 m is about −29 dB. It is sent as the EAX 2 `DirectHF` property, which
  DSOAL passes to OpenAL Soft's low-pass. Each buffer's EAX room send is
  switched off, since A3D 2.0 has no reverb.
- **Occlusion**: the listener-to-source line is tested against the polygons
  of the last flushed frame. Each wall crossed scales the level by its
  material's low-frequency transmittance and the brightness by its
  high-frequency transmittance, as the driver does. Subfaces (doors) with an opening factor let sound through
  in proportion, including `SetOpeningFactorfv` pointers kept in lists.
- **First-order reflections**: image sources. For each reflecting polygon
  the source is mirrored, the reflection point must fall inside the polygon,
  and the strongest few are played as extra voices at the image direction,
  delayed by the extra path length (clamped to the maximum reflection
  delay). One-shot sounds use a copy with silence in front so that the tap
  starts together with the source and lags it exactly; looping sounds use a
  duplicate of the buffer played at an offset.
- **Frames**: like the real library, nothing changes until `IA3d4::Flush`.
  `Play`/`Stop` are deferred to the next `Flush`. For programs that never
  call `Flush`, an automatic flush keeps them working (`auto-flush` in
  `a3d.ini`).

Native sources (`A3DSOURCE_INITIAL_RENDERMODE_NATIVE` or render mode
`NATIVE`) are 2D buffers panned with `SetPanValues`; only gain, pitch and pan
apply to them. Stereo data on an A3D source is mixed to mono and positioned,
as A3D does: the program locks and writes a stereo copy, and the mono mix
goes to DirectSound.

## Not modelled

- `IA3dEnvironment`, `SelectPreset` and the A3D 3.0 interfaces (`IA3d5`,
  reverb).
- Resource management and priorities: every source plays.
- Polygon bloat, update intervals and 2D line walls.
- OpenAL Soft's low-pass is matched to the card's two-pole filter at 5 kHz
  only; above that the card's filter falls off more steeply.

## Tests

`tests/` has two programs used during development, run under Wine with
OpenAL Soft's wave-file writer:

- `a3d2_scene_test.cpp` drives a scene: a source circling the listener, a
  wall that occludes it, a door that opens, and a click with a reflecting wall
  44 ms of extra path away. `analyze_wav.py` reports the left/right balance,
  treble and reflection timing.
- `a3d1_test.cpp` checks `CoCreateInstance(CLSID_A3d)`, `IA3d2`, and
  `CoCreateInstance(CLSID_A3dApi)` through the registry.

## HRTF data

`hrtf/Aureal Vortex 2.mhr` is the OpenAL Soft data set the DLLs use. It is
made from the direction filters the Vortex 2 driver loads into the card:
Aureal's "Ed's Ears" profile (`Ed_Riegelsberger1.hrtf`, measured by Ed
Riegelsberger), which comes with the Vortex 2 driver packages. It covers
elevations from −75° to +75° on a 15° grid plus straight up and straight down,
56 taps per ear at 48 kHz, with the card's inter-aural delays; filters
captured from a real SQ2500 match it to within ±2 units.

`hrtf/make_mhr.py` rebuilds it from the profile file (or from a .csv of a
captured table):

    python3 make_mhr.py Ed_Riegelsberger1.hrtf "Aureal Vortex 2.mhr"
