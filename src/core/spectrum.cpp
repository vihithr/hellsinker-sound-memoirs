#include "spectrum.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace memoirs {
namespace {
constexpr int kFftN = kSpectrumFrames;
constexpr int kBars = kSpectrumBands;
// ---- FFT ---------------------------------------------------------------
// Tables built by hs_dx9build.exe at 0x4A384C, 0x4A3CF8 and 0x4A3E60.
struct SpectrumTables {
    float window[kFftN];
    float binWeight[kFftN / 2];
    float sine[3 * kFftN / 4];

    SpectrumTables() {
        const double pi = 3.14159265;
        for (int i = 0; i < kFftN; ++i)
            window[i] = (float)(0.5 + 0.5 * cos(pi * (i - kFftN / 2) / kFftN));
        for (int i = 0; i < kFftN / 2; ++i) {
            float p = (float)pow((double)i / (kFftN / 2), 0.85);
            binWeight[i] = (float)(p + (1.0 - p) * 0.025) / 150.0f;
        }

        float halfStep = (float)sin(pi / kFftN);
        float dc = 2.0f * halfStep * halfStep;
        float ds = sqrtf((2.0f - dc) * dc);
        float step = 2.0f * dc, sn = 0.0f, cs = 1.0f;
        sine[0] = 0.0f;
        sine[kFftN / 4] = 1.0f;
        for (int i = 1; i < kFftN / 8; ++i) {
            cs -= dc;
            dc += step * cs;
            sn += ds;
            ds -= step * sn;
            sine[i] = sn;
            sine[kFftN / 4 - i] = cs;
        }
        sine[kFftN / 8] = sqrtf(0.5f);
        for (int i = 0; i < kFftN / 4; ++i) {
            sine[kFftN / 2 - i] = sine[i];
            sine[kFftN / 2 + i] = -sine[i];
        }
    }
};

void FFT(float* re, float* im, const SpectrumTables& table) {
    for (int i = 1, j = 0; i < kFftN; ++i) {
        int bit = kFftN >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
    }
    for (int len = 2; len <= kFftN; len <<= 1) {
        for (int j = 0; j < len / 2; ++j) {
            int phase = j * (kFftN / len);
            float cs = table.sine[phase + kFftN / 4], sn = table.sine[phase];
            for (int i = j; i < kFftN; i += len) {
                int h = i + len / 2;
                float vr = cs * re[h] - sn * im[h];
                float vi = cs * im[h] + sn * re[h];
                re[h] = re[i] - vr; im[h] = im[i] - vi;
                re[i] += vr; im[i] += vi;
            }
        }
    }
}

// bin range [start, count) for spectrum column k, exactly as sub_7EEAD8:
//   k 0..5 : 1 bin  (index k)
//   k 6..11: 3 bins starting 3k-12
//   k 12..17: 4 bins starting 4k-24
//   k 18..23: 8 bins starting 8k-96
//   k 24..29: 16 bins starting 16k-288
//   k 30..42: 64 bins starting 64k-1728
// The 1024-long magnitude array is interleaved: [mag0, mag1023, mag1, mag1022, ...]
static void BarBins(int k, int* start, int* count) {
    if (k < 6)       { *start = k;          *count = 1; }
    else if (k < 12) { *start = 3 * k - 12; *count = 3; }
    else if (k < 18) { *start = 4 * k - 24; *count = 4; }
    else if (k < 24) { *start = 8 * k - 96; *count = 8; }
    else if (k < 30) { *start = 16 * k - 288; *count = 16; }
    else             { *start = 64 * k - 1728; *count = 64; }
}

static float MeterEnvelope(float previous, short first, short second) {
    float level = (abs((int)first) + abs((int)second)) / (2.0f * 32767.0f);
    return level <= previous ? (5 * previous + level) / 6 : (7 * level + previous) / 8;
}

} // namespace

std::size_t visualCursor(double seconds,int rate,std::size_t total) {
    // Original cursor (0x7EFD2E): bytes = elapsed*176400, then
    // abs(bytes - 0x5622) &~3 % bufferSize — 125 ms compensation.
    // FFT, waveforms and meters all share this cursor.
    long long bytes = (long long)(seconds * rate) * 4 - 22050;
    if (bytes < 0) bytes = -bytes;
    return total ? (size_t)(bytes / 4) % total : 0;
}

void analyze(Spectrum& v,const short* pcm,std::size_t total,std::size_t first) {
    if (!pcm || !total) { v = {}; return; }
    first %= total;
    static const SpectrumTables table;
    float re[kFftN], im[kFftN], mag[kFftN];
    size_t frame = first;
    for (int i = 0; i < kFftN; ++i) {
        short left = pcm[2 * frame], right = pcm[2 * frame + 1];
        re[i] = left * table.window[i];
        im[i] = right * table.window[i];
        if ((i % 4) == 0 && i / 4 < 240) {
            v.waveL[i / 4] = (float)(right / 512);
            v.waveR[i / 4] = (float)(left / 512);
        }
        if (++frame == total) frame = 0;
    }
    FFT(re, im, table);
    for (int i = 0; i < kFftN / 2; ++i) {
        int mirror = kFftN - 1 - i;
        float scale = table.binWeight[i] / kFftN;
        mag[2 * i] = fabsf(re[i] * scale) + fabsf(im[i] * scale);
        mag[2 * i + 1] = fabsf(re[mirror] * scale) + fabsf(im[mirror] * scale);
    }
    for (int k = 0; k < kBars; ++k) {
        int start, count;
        BarBins(k, &start, &count);
        float sum = 0;
        for (int b = 0; b < count; ++b) sum += mag[start + b];
        int level = (int)(sum * 128.0f / count);
        int old = (int)v.bars[k];
        // groups 24..29 average with the old state BEFORE compression
        if (k >= 24 && k < 30) level = (old + level) / 2;
        if (level > 96) level = (level - 96) / 2 + 96;
        if (level > 64) level = (level - 64) / 2 + 64;
        v.bars[k] = (float)((level + 11 * old) / 12);
        v.barsPrev[k] = (float)old;
        v.barsLevel[k] = (float)level;
    }
    const size_t second = (first + 4) % total;
    v.envL = MeterEnvelope(v.envL, pcm[2 * first + 1], pcm[2 * second + 1]);
    v.envR = MeterEnvelope(v.envR, pcm[2 * first], pcm[2 * second]);
}

} // namespace memoirs

#ifdef __wasm__
static memoirs::Spectrum webSpectrum;
static float values[43*3+240*2+2];
extern "C" const float* sm_spectrum(const short* frames) {
    memoirs::analyze(webSpectrum,frames,memoirs::kSpectrumFrames,0);
    // A packed snapshot for the browser; keep native layout an implementation detail.
    auto p = std::copy_n(webSpectrum.bars,43,values);
    p = std::copy_n(webSpectrum.barsPrev,43,p);
    p = std::copy_n(webSpectrum.barsLevel,43,p);
    p = std::copy_n(webSpectrum.waveL,240,p);
    p = std::copy_n(webSpectrum.waveR,240,p);
    *p++ = webSpectrum.envL; *p = webSpectrum.envR;
    return values;
}
extern "C" void sm_spectrum_reset() { webSpectrum = {}; std::fill_n(values,611,0.f); }
#endif
