// config_panel.h - independently compiled system settings panel.
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include "library.h"

namespace config_panel {

inline constexpr int kSettingCount = 5;

struct Palette {
    COLORREF panel;
    COLORREF frame;
    COLORREF bar;
    COLORREF ink;
    COLORREF strip;
    COLORREF stripHot;
    COLORREF seam;
    COLORREF outer;
    COLORREF digit;
    COLORREF selectedEdge;
};

struct DrawContext {
    HDC dc;
    float scale;
    HDC atlas;
    Palette palette;
};

struct State {
    int activeRow;
    int volume;
    const char* paletteName;
    lib::Lang language;
    int libraryFont;
    const wchar_t* layoutName;
};

enum class HitAction {
    Select,
    Decrease,
    Increase
};

struct HitResult {
    int row;
    HitAction action;

    explicit operator bool() const { return row >= 0; }
};

// Coordinates are in the panel's unscaled 640x480 logical space.
HitResult HitTest(int logicalX, int logicalY);

void Paint(const DrawContext& context, const State& state);

} // namespace config_panel
