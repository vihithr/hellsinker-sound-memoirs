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
    // High-frequency transient (bands 28..42): sharp attack 15 ms, release
    // 140 ms. Piano / cymbals / strings with weak lows still flash the ticks.
    float highSpark=0, highFloor=0, lastHigh=0;
    float wing[2]{};
    // Slow auto-gain for the XY phase figures: tracks the recent peak with a
    // fast attack / slow release so quiet passages expand instead of sitting
    // in a ball at the centre, and one loud spike cannot squash what follows.
    float xyGain=1.f;
    // Slow per-channel peak for the HELIX rack rails (hold ~0.8 s, fall 0.55/s).
    float meterPeak[2]{};
    int meterAge[2]{};
    // HELIX / SINK: per-channel level history, one rung every 1/15 s. The
    // lattice scrolls these upward while the view sinks; each rung keeps the
    // beat flag so strikes stay visible as they rise past the window.
    static constexpr int kHist=64;
    float histL[kHist]{}, histR[kHist]{};
    unsigned char histBeat[kHist]{};
    int histHead=0;
    double histLast=-1;
    // Signed waveform sample per rung (-1..1): SINK projects this onto the
    // strand's horizontal offset, so the wave itself draws the helix.
    float histWaveL[kHist]{}, histWaveR[kHist]{};
    // Running winding phase: advances with the signed wave (frequency sets
    // the crossing rate, amplitude the swing), so the strands keep twisting
    // even when a window mean cancels out.
    float windL=0, windR=0;
    // DIVE: frozen birth snapshot per layer (drive / high spark / winding
    // phase), keyed by the layer's birth index. A layer keeps its birth
    // imprint from spawn to screen exit, independent of the 8 Hz history.
    static constexpr int kDiveSlots=16;
    int diveLastBase=-1;
    float diveDrive[kDiveSlots]{}, diveSpark[kDiveSlots]{}, divePhase[kDiveSlots]{};
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
