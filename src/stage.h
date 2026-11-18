#pragma once
#include "gfx.h"
#include "core/spectrum.h"
#include <array>

namespace stage {
constexpr int count=4; // 0 is the original Memoirs composition.
const wchar_t* name(int mode);
constexpr int wingCount=5;
const wchar_t* wingName(int mode);
struct Memory {
    std::array<std::array<float,43>,24> ridges{};
    std::array<std::array<GfxPoint,120>,6> traces{};
    float lowSustain=0, impact=0, lowFloor=0, lastLow=0;
    float wing[2]{};
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
