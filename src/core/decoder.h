// decoder.h - ogg/wav -> 44.1 kHz 16-bit stereo PCM.
#pragma once

#include <string>
#include <vector>

struct PcmBuffer {
    std::vector<short> samples; // interleaved stereo, 44100 Hz
    int sampleRate;
    int srcSampleRate;
    int srcChannels;

    PcmBuffer() : sampleRate(44100), srcSampleRate(0), srcChannels(0) {}
    int frames() const { return (int)(samples.size() / 2); }
    double seconds() const { return sampleRate > 0 ? (double)frames() / (double)sampleRate : 0.0; }
    void clear() {
        samples.clear();
        sampleRate = 44100;
        srcSampleRate = 0;
        srcChannels = 0;
    }
};

// maxFrames <= 0 decodes everything; otherwise stops after that many output frames.
bool DecodeOgg(const unsigned char* data, unsigned long len, PcmBuffer& out,
               std::string& err, int maxFrames);
bool DecodeWav(const unsigned char* data, unsigned long len, PcmBuffer& out,
               std::string& err, int maxFrames);
void ResampleTo44100(PcmBuffer& pcm);
