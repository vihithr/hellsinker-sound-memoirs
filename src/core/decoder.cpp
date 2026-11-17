#include "decoder.h"
#include <cstring>

#define STB_VORBIS_HEADER_ONLY
#include "../../stb_vorbis.c"

namespace {

bool ResizeClamp(PcmBuffer& pcm, int maxFrames) {
    if (maxFrames <= 0) return true;
    if (pcm.frames() <= maxFrames) return true;
    pcm.samples.resize((size_t)maxFrames * 2);
    return true;
}

} // namespace

bool DecodeOgg(const unsigned char* data, unsigned long len, PcmBuffer& out,
               std::string& err, int maxFrames) {
    err.clear();
    out.clear();
    if (!data || len < 16) {
        err = "empty ogg payload";
        return false;
    }
    int vErr = 0;
    stb_vorbis* v = stb_vorbis_open_memory(data, (int)len, &vErr, NULL);
    if (!v) {
        err = "vorbis open failed (code " + std::to_string(vErr) + ")";
        return false;
    }

    stb_vorbis_info info = stb_vorbis_get_info(v);
    out.srcSampleRate = (int)info.sample_rate;
    out.srcChannels = (int)info.channels;
    out.sampleRate = (int)info.sample_rate;

    if (info.sample_rate <= 0 || info.channels <= 0) {
        stb_vorbis_close(v);
        err = "vorbis stream reports no audio";
        return false;
    }

    const int kChunk = 4096;
    std::vector<short> chunk((size_t)kChunk * 2);
    int limit = (maxFrames > 0) ? maxFrames : -1;

    for (;;) {
        if (limit >= 0 && out.frames() >= limit) break;
        int want = kChunk;
        if (limit >= 0) {
            int left = limit - out.frames();
            if (left < want) want = left;
        }
        int got = stb_vorbis_get_frame_short_interleaved(v, 2, &chunk[0], want * 2);
        if (got <= 0) break;
        out.samples.insert(out.samples.end(), chunk.begin(), chunk.begin() + (size_t)got * 2);
    }

    double total = stb_vorbis_stream_length_in_seconds(v);
    (void)total;
    stb_vorbis_close(v);

    if (out.frames() <= 0) {
        err = "vorbis stream decoded to no samples";
        out.clear();
        return false;
    }
    if (out.sampleRate != 44100) ResampleTo44100(out);
    ResizeClamp(out, maxFrames);
    return true;
}

namespace {

unsigned int RdU32(const unsigned char* p) {
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
           ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}
unsigned short RdU16(const unsigned char* p) {
    return (unsigned short)((unsigned int)p[0] | ((unsigned int)p[1] << 8));
}

short Clamp16(int v) {
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (short)v;
}

} // namespace

// Minimal RIFF/WAVE reader: PCM 8/16/24/32, IEEE float 32/64 and WAVE_FORMAT_EXTENSIBLE.
bool DecodeWav(const unsigned char* data, unsigned long len, PcmBuffer& out,
               std::string& err, int maxFrames) {
    err.clear();
    out.clear();
    if (!data || len < 44) {
        err = "wav payload too small";
        return false;
    }
    if (memcmp(data, "RIFF", 4) != 0 || memcmp(data + 8, "WAVE", 4) != 0) {
        err = "not a RIFF/WAVE file";
        return false;
    }

    unsigned short fmtTag = 0, channels = 0, bits = 0, blockAlign = 0;
    unsigned int sampleRate = 0;
    const unsigned char* pcmData = NULL;
    unsigned long pcmLen = 0;

    unsigned long pos = 12;
    while (pos + 8 <= len) {
        const unsigned char* ck = data + pos;
        unsigned int ckLen = RdU32(ck + 4);
        pos += 8;
        if (pos > len) break;
        if (ckLen > len - pos) ckLen = (unsigned int)(len - pos);

        if (memcmp(ck, "fmt ", 4) == 0 && ckLen >= 16) {
            fmtTag = RdU16(data + pos);
            channels = RdU16(data + pos + 2);
            sampleRate = RdU32(data + pos + 4);
            blockAlign = RdU16(data + pos + 12);
            bits = RdU16(data + pos + 14);
            if (fmtTag == 0xFFFE && ckLen >= 40) {
                fmtTag = RdU16(data + pos + 24); // SubFormat GUID first two bytes
            }
        } else if (memcmp(ck, "data", 4) == 0) {
            pcmData = data + pos;
            pcmLen = ckLen;
        }
        pos += ckLen;
        if (ckLen & 1) ++pos; // RIFF chunk padding
    }

    if (!pcmData) {
        err = "wav has no data chunk";
        return false;
    }
    if (channels == 0 || sampleRate == 0 || blockAlign == 0) {
        err = "wav fmt chunk incomplete";
        return false;
    }
    const bool isFloat = (fmtTag == 3);
    if (fmtTag != 1 && fmtTag != 3) {
        err = "unsupported wav codec (tag " + std::to_string(fmtTag) + ")";
        return false;
    }
    if (!isFloat && bits != 8 && bits != 16 && bits != 24 && bits != 32) {
        err = "unsupported wav bit depth";
        return false;
    }
    if (isFloat && bits != 32 && bits != 64) {
        err = "unsupported float wav bit depth";
        return false;
    }

    const int bytesPerSample = bits / 8;
    if (blockAlign < channels * bytesPerSample) {
        err = "wav block alignment mismatch";
        return false;
    }
    unsigned long frames = pcmLen / blockAlign;
    if (frames == 0) {
        err = "wav holds no frames";
        return false;
    }
    if (maxFrames > 0 && frames > (unsigned long)maxFrames) frames = (unsigned long)maxFrames;

    out.srcSampleRate = (int)sampleRate;
    out.srcChannels = channels;
    out.sampleRate = (int)sampleRate;
    out.samples.resize((size_t)frames * 2);

    const int outCh = 2;
    for (unsigned long f = 0; f < frames; ++f) {
        const unsigned char* fr = pcmData + f * blockAlign;
        int vals[2] = { 0, 0 };
        for (int c = 0; c < outCh; ++c) {
            int srcCh = (c < channels) ? c : channels - 1;
            const unsigned char* sp = fr + srcCh * bytesPerSample;
            int v = 0;
            if (isFloat) {
                if (bits == 32) {
                    float fl;
                    memcpy(&fl, sp, 4);
                    double d = (double)fl;
                    if (d > 1.0) d = 1.0;
                    if (d < -1.0) d = -1.0;
                    v = (int)(d * 32767.0);
                } else {
                    double d;
                    memcpy(&d, sp, 8);
                    if (d > 1.0) d = 1.0;
                    if (d < -1.0) d = -1.0;
                    v = (int)(d * 32767.0);
                }
            } else if (bits == 8) {
                v = ((int)sp[0] - 128) << 8;
            } else if (bits == 16) {
                v = (int)(short)RdU16(sp);
            } else if (bits == 24) {
                int raw = (int)sp[0] | ((int)sp[1] << 8) | ((int)sp[2] << 16);
                if (raw & 0x800000) raw -= 0x1000000;
                v = raw >> 8;
            } else { // 32-bit int
                int raw = (int)RdU32(sp);
                v = raw >> 16;
            }
            vals[c] = Clamp16(v);
        }
        out.samples[(size_t)f * 2 + 0] = (short)vals[0];
        out.samples[(size_t)f * 2 + 1] = (short)vals[1];
    }

    if (out.sampleRate != 44100) ResampleTo44100(out);
    return true;
}

void ResampleTo44100(PcmBuffer& pcm) {
    if (pcm.sampleRate == 44100 || pcm.srcSampleRate == 0 || pcm.frames() == 0) {
        pcm.sampleRate = 44100;
        return;
    }
    const int inFrames = pcm.frames();
    const double ratio = 44100.0 / (double)pcm.srcSampleRate;
    long outFrames = (long)((double)inFrames * ratio + 0.5);
    if (outFrames < 1) outFrames = 1;

    std::vector<short> out((size_t)outFrames * 2);
    for (long i = 0; i < outFrames; ++i) {
        double srcPos = (double)i / ratio;
        long i0 = (long)srcPos;
        if (i0 >= inFrames) i0 = inFrames - 1;
        long i1 = i0 + 1;
        if (i1 >= inFrames) i1 = inFrames - 1;
        double frac = srcPos - (double)i0;
        for (int c = 0; c < 2; ++c) {
            double a = pcm.samples[(size_t)i0 * 2 + c];
            double b = pcm.samples[(size_t)i1 * 2 + c];
            double v = a + (b - a) * frac;
            if (v > 32767.0) v = 32767.0;
            if (v < -32768.0) v = -32768.0;
            out[(size_t)i * 2 + c] = (short)(v < 0 ? v - 0.5 : v + 0.5);
        }
    }
    pcm.samples.swap(out);
    pcm.sampleRate = 44100;
}
