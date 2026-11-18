// Platform-independent snapshot consumed by the derived painter.
// The desktop player keeps its own PlayerApp; this branch fills this POD from
// JS every frame and never touches the desktop global state.
#pragma once
#include "core/spectrum.h"
#include <cstdint>
#include <cstddef>

struct SceneState {
    memoirs::Spectrum spectrum;   // 611 floats, standard layout
    float wing[2];                // derived from stage::Memory::advance
    int tick;
    int selected;                 // -1 = EXIT
    int current;                  // -1 silence, 0..45 TR, 46 local file
    int available[46];
    int theme;
    int stageMode;
    int wingMode;
    int layoutMode;
    int page;                     // 0 = Tracks
    int hoverChannel;             // -1 = none
    double seconds;
    int localFile;                // 1 when current is a local file
    int loading;                  // 1 while decoding / importing
};

// JS writes scalar fields at these fixed byte offsets in app.js. A layout
// change must fail the build here instead of silently corrupting the frame.
static_assert(offsetof(SceneState, spectrum) == 0, "spectrum offset");
static_assert(offsetof(SceneState, wing) == 2444, "wing offset");
static_assert(offsetof(SceneState, tick) == 2452, "tick offset");
static_assert(offsetof(SceneState, selected) == 2456, "selected offset");
static_assert(offsetof(SceneState, current) == 2460, "current offset");
static_assert(offsetof(SceneState, available) == 2464, "available offset");
static_assert(offsetof(SceneState, theme) == 2648, "theme offset");
static_assert(offsetof(SceneState, stageMode) == 2652, "stageMode offset");
static_assert(offsetof(SceneState, wingMode) == 2656, "wingMode offset");
static_assert(offsetof(SceneState, layoutMode) == 2660, "layoutMode offset");
static_assert(offsetof(SceneState, page) == 2664, "page offset");
static_assert(offsetof(SceneState, hoverChannel) == 2668, "hoverChannel offset");
static_assert(offsetof(SceneState, seconds) == 2672, "seconds offset");
static_assert(offsetof(SceneState, localFile) == 2680, "localFile offset");
static_assert(offsetof(SceneState, loading) == 2684, "loading offset");
static_assert(sizeof(SceneState) == 2688, "SceneState size");
