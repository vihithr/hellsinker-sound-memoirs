// scene_state.h - platform-independent snapshot consumed by the shared
// scene painter (scene.cpp).
//
// The desktop shell packs this POD from PlayerApp each frame; the WASM
// branch has JS fill it at fixed byte offsets (app.js / verify_scene.mjs /
// dump_scene.mjs). The static_asserts below pin the layout so a change fails
// the build instead of silently corrupting frames. New fields are always
// appended at the end so every pre-existing offset stays untouched.
#pragma once
#include "core/spectrum.h"
#include <cstdint>
#include <cstddef>

struct SceneState {
    memoirs::Spectrum spectrum;   // 611 floats, standard layout
    float wing[2];                // stage::Memory::advance wing envelopes
                                   // (accumulated per instance; packers that
                                   // don't simulate them write zeros)
    int tick;                     // frame counter since screen entry (60 Hz)
    int selected;                 // -1 = EXIT
    int current;                  // -1 silence, 0..45 TR, 46 local file
    int available[46];            // per-track resolved flag
    int theme;                    // palette index, ORIGINAL = 0
    int stageMode;                // 0 spectrum, 1..3 stage compositions
    int wingMode;                 // 0 auto, 1..4 explicit wing style
    int layoutMode;               // 0 original, 1..4 field/stack/ring/cross
    int page;                     // 0 tracks, 1 library, 2 system
    int hoverChannel;             // -1 = none, else 0..2 rail channel
    double seconds;               // playback position for PLAY TIME
    int localFile;                // 1 when current is a local file
    int loading;                  // 1 while decoding / importing
    int railActivityTick;         // tick of the last rail user activity
    char localTitle[28];          // uppercased ASCII fallback of the local
                                   // file name ("" = no local track)
    int switchTick;               // tick of the last page switch (0 = none)
    int showGridOverlay;          // TAB grid overlay (Field/Cross layouts)
    double duration;              // current track duration in seconds
                                   // (Stack/Cross layout progress rails)
    // --- SYS / LIB shell state (web-completeness work) ---
    int language;                 // 0=EN, 1=ZH, 2=JA
    int volume;                   // 0..100 (SYS output level)
    int libFont;                  // 0=original atlas, 1=pixel font
    int settingRow;               // SYS active row 0..4
    int libCursor;                // LIB highlighted track index
    int libScroll;                // LIB first visible row
    int localOrder;               // 0 sequential, 1 repeat-one, 2 loop, 3 shuffle
    int libPaused;                // 1 when local playback paused
    int libPlaying;               // 1 when a local track is active
    int libPositionMs;            // LIB seek rail position in milliseconds
    int layoutName[16];           // UTF-32 code points of active layout name
    int layoutNameLen;
    int searchText[32];           // UTF-32 code points of LIB search query
    int searchTextLen;
};

// JS writes scalar fields at these fixed byte offsets. A layout change must
// fail the build here instead of silently corrupting the frame.
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
static_assert(offsetof(SceneState, railActivityTick) == 2688, "railActivityTick offset");
static_assert(offsetof(SceneState, localTitle) == 2692, "localTitle offset");
static_assert(offsetof(SceneState, switchTick) == 2720, "switchTick offset");
static_assert(offsetof(SceneState, showGridOverlay) == 2724, "showGridOverlay offset");
static_assert(offsetof(SceneState, duration) == 2728, "duration offset");
static_assert(offsetof(SceneState, language) == 2736, "language offset");
static_assert(offsetof(SceneState, volume) == 2740, "volume offset");
static_assert(offsetof(SceneState, libFont) == 2744, "libFont offset");
static_assert(offsetof(SceneState, settingRow) == 2748, "settingRow offset");
static_assert(offsetof(SceneState, libCursor) == 2752, "libCursor offset");
static_assert(offsetof(SceneState, libScroll) == 2756, "libScroll offset");
static_assert(offsetof(SceneState, localOrder) == 2760, "localOrder offset");
static_assert(offsetof(SceneState, libPaused) == 2764, "libPaused offset");
static_assert(offsetof(SceneState, libPlaying) == 2768, "libPlaying offset");
static_assert(offsetof(SceneState, libPositionMs) == 2772, "libPositionMs offset");
static_assert(offsetof(SceneState, layoutName) == 2776, "layoutName offset");
static_assert(offsetof(SceneState, layoutNameLen) == 2840, "layoutNameLen offset");
static_assert(offsetof(SceneState, searchText) == 2844, "searchText offset");
static_assert(offsetof(SceneState, searchTextLen) == 2972, "searchTextLen offset");
static_assert(sizeof(SceneState) == 2976, "SceneState size");
