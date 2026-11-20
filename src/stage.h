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
    // Calibrated L/R level for the CUSTOM layouts' meters (no 2.7x overload
    // gain): normal music dances at 40..85%, only real peaks hit the top.
    // wing[] keeps its old hot curve for the original side wings.
    float meter[2]{};
    float coilAttack[2]{}; // transient envelope for the suspended DNA bonds
    // Slow auto-gain for the XY phase figures: tracks the recent peak with a
    // fast attack / slow release so quiet passages expand instead of sitting
    // in a ball at the centre, and one loud spike cannot squash what follows.
    float xyGain=1.f;
    // Slow per-channel peak for the HELIX rack rails (hold ~0.8 s, fall 0.55/s).
    float meterPeak[2]{};
    int meterAge[2]{};
    // Real acoustic telemetry: stereo phase correlation (-1..+1, smoothed)
    // and a 1 s clip latch per channel (env >= 0.94 or wave peak >= 61).
    float correlation=0.f;
    float clipHold[2]{};
    static constexpr int kHist=64;
    float histL[kHist]{}, histR[kHist]{};
    int histHead=0;
    double histClock=-1;
    int head=0, trace=0, samples=0;
    double last=-1;
    double motion=-1;
    int track=-1;
    // SINK / DIVE standby: animation clocks advance only while audible
    // (playing, not paused). Stopping freezes the shaft / tunnel in place;
    // resuming continues from the frozen phase instead of jumping ahead.
    double sinkClock=0, diveClock=0;
    // dt==0 compensation: when the audio position has not ticked between two
    // render frames, advance the visual clocks by 1/60 anyway (max 3 frames)
    // and debit the debt from the next real dt so long-term time never drifts.
    double clockComp=0;
    int sameSecFrames=0;
    // SONO: 88-row quantised spectrogram history, printed at 15 rows/s.
    static constexpr int kSonoRows=88;
    std::uint8_t sono[kSonoRows][43]{};
    int sonoHead=0;
    double sonoClock=0, sonoLast=-1;
    // TRACK: 32-row 8-channel hex pattern history at 12 rows/s.
    static constexpr int kTrackRows=32;
    std::uint8_t trackHex[kTrackRows][8]{};
    std::uint8_t trackBeat[kTrackRows]{};
    int trackHead=0, trackRowCounter=0;
    double trackClock=0, trackLast=-1;
    // DECK: reel rotation phase (rad), uniform 2.2 rad/s while audible.
    double deckClock=0;
    // POLY: uniform three-axis rotation phase.
    double polyClock=0;
    // WARP: terrain scroll clock (6 rows/s) plus the locked row-history ring.
    double warpClock=0, warpLast=-1.0;
    static constexpr int kWarpRows=16;
    static constexpr int kWarpCols=24;
    std::uint8_t warpRows[kWarpRows][kWarpCols]{};
    int warpHead=0;
    // MOIRE: uniform orbital phase of the three interference sources.
    double moireClock=0;
    // ---- audio modulation bus for the custom layouts -----------------------
    // Mid band 6..27 (melody / FM lead) with its own floor and attack, so a
    // phrase entry is distinguishable from a sustained pad.
    float midSustain=0, midAttack=0, midFloor=0, lastMid=0;
    // Stereo image (-1 left .. +1 right) and spectral centroid (0 dark .. 1
    // bright): both drive 3D bending and orbit speed in the custom layouts.
    float pan=0, centroid=0;
    // RASTER / TORUS clocks (elastic, same standby rules as the others).
    double rasterClock=0, torusClock=0;
    // MOIRE shockwave radius, normalised: >= 1.2 means "no wave active".
    float moirePulse=1.5f;
    // DIVE depth-delay history: one quantised waveform per tunnel layer.
    static constexpr int kDiveLayers=5;
    std::int8_t diveWaves[kDiveLayers][240]{};
    int diveWaveHead=0, diveLastLayer=-1;
    void advance(const memoirs::Spectrum&, int id, double seconds, bool playing, bool paused);
};
struct Palette { COLORREF field, grid, line, accent, frame; };
// Paint one stage composition into the gfx backend. Logical 640x480
// coordinates are scaled by `scale` (the desktop backbuffer multiplier; the
// WASM replay passes 1.0).
void paint(const Palette&, float scale, int mode, const memoirs::Spectrum&, const Memory&);
}
