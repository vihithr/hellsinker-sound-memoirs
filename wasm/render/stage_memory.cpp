// Derived from src/stage.cpp: Memory::advance(), restricted to the wing
// envelopes used by PaintWings (style 2/3). The desktop source is untouched.
#include "scene_state.h"
#include <algorithm>
#include <cmath>

namespace {
float follow(float current, float target, float dt, double attack, double release) {
    const double tau = target > current ? attack : release;
    return current + (target - current) * float(1.0 - std::exp(-dt / tau));
}
float smoothstep(float edge0, float edge1, float value) {
    const float t = std::clamp((value - edge0) / (edge1 - edge0), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}
float channelDrive(float env, const float* wave) {
    double sum = 0;
    for (int i = 0; i < 240; i += 4) {
        const double v = wave[i] / 64.0;
        sum += v * v;
    }
    const float rms = float(std::sqrt(sum / 60.0));
    return smoothstep(.02f, .78f, std::max(env * 2.7f, rms * 1.8f));
}
double g_motion = -1;
int g_track = -1;
}

void sceneAdvanceWing(SceneState& s, int id, double seconds, bool playing, bool paused) {
    if (id != g_track || (playing && g_motion >= 0 &&
        (seconds + .10 < g_motion || seconds - g_motion > 1.0))) {
        s.wing[0] = 0.f; s.wing[1] = 0.f; g_motion = -1; g_track = id;
    }
    if (!playing) {
        s.wing[0] = 0.f; s.wing[1] = 0.f; g_motion = -1; g_track = id;
        return;
    }
    if (paused) return;
    float dt = g_motion >= 0 ? float(seconds - g_motion) : 1.f / 60.f;
    if (dt <= 0) return;
    dt = std::clamp(dt, .001f, .12f);
    g_motion = seconds;

    s.wing[0] = follow(s.wing[0], channelDrive(s.spectrum.envL, s.spectrum.waveL),
                       dt, .030, .30);
    s.wing[1] = follow(s.wing[1], channelDrive(s.spectrum.envR, s.spectrum.waveR),
                       dt, .030, .30);
}
