#pragma once
#include "gfx.h"
#include "core/spectrum.h"
#include <array>
#include <cmath>

namespace stage {
constexpr int count=4; // 0 is the original Memoirs composition.
// Shared dynamic-range remap for the CUSTOM layouts (never ORIGINAL): the
// analyser already compresses levels, so a linear /96 map pins mid-volume
// music near the top. Drop a noise floor and apply gamma so quiet passages
// read quiet and peaks still reach full scale.
inline float bandLevel(float raw){
    const float x=raw<0.f?0.f:(raw>96.f?1.f:raw/96.f);
    const float y=(x-0.12f)/0.88f;
    return y<=0.f?0.f:(y>=1.f?1.f:y*std::sqrt(y));   // gamma 1.5
}
const wchar_t* name(int mode);
constexpr int wingCount=5;
const wchar_t* wingName(int mode);
struct Memory {
    std::array<std::array<float,43>,24> ridges{};
    std::array<std::array<GfxPoint,120>,6> traces{};
    float lowSustain=0, impact=0, lowFloor=0, lastLow=0;
    float wing[2]{};
    // Peak-hold caps for the custom layouts: rise instantly, hold ~0.45 s,
    // then fall at a constant 0.9/s. Values are the remapped 0..1 band level.
    float peak[43]{};
    int peakAge[43]{};
    // Slow per-channel peak for the meter rails (hold ~0.8 s, fall 0.55/s).
    float meterPeak[2]{};
    int meterAge[2]{};
    // HELIX torsion wave: a low-frequency attack launches a pulse at the left
    // bearing that crosses the shaft in ~160 ms and decays behind the head.
    float helixPulse=0;      // 0..1 strength of the travelling twist
    float helixHead=-1;      // 0..1 position along the shaft, -1 = idle
    int head=0, trace=0, samples=0;
    double last=-1;
    double motion=-1;
    int track=-1;
    void advance(const memoirs::Spectrum&, int id, double seconds, bool playing, bool paused);
};
struct Palette { COLORREF field, grid, line, accent, frame; };
// Paint one stage composition into the gfx backend. Logical 640x480
// coordinates are scaled by `scale` (the desktop backbuffer multiplier; the
// WASM replay passes 1.0).
void paint(const Palette&, float scale, int mode, const memoirs::Spectrum&, const Memory&);
}
