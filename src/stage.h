#pragma once
#include "common.h"
#include "core/spectrum.h"
#include <array>

namespace stage {
constexpr int count=4; // 0 is the original Memoirs composition.
const wchar_t* name(int mode);
constexpr int wingCount=5;
const wchar_t* wingName(int mode);
struct Memory {
    std::array<std::array<float,43>,24> ridges{};
    std::array<std::array<POINT,120>,6> traces{};
    int head=0, trace=0, samples=0;
    double last=-1;
    int track=-1;
    void advance(const memoirs::Spectrum&, int id, double seconds, bool running);
};
struct Palette { COLORREF field, grid, line, accent, frame; };
void paint(const Palette&,HDC,float scale,int mode,const memoirs::Spectrum&,const Memory&);
}
