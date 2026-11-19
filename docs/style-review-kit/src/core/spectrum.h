#pragma once
#include <cstddef>

namespace memoirs {
constexpr int kSpectrumBands = 43;
constexpr int kSpectrumFrames = 1024;
struct Spectrum {
    float bars[kSpectrumBands]{}, barsPrev[kSpectrumBands]{}, barsLevel[kSpectrumBands]{};
    float waveL[240]{}, waveR[240]{};
    float envL{}, envR{};
};
void analyze(Spectrum& state, const short* stereo, std::size_t frames, std::size_t first);
std::size_t visualCursor(double seconds, int rate, std::size_t frames);
}
