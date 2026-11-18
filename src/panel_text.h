#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <cstdint>
#include <string>

enum class PanelTextMode {
    AtlasFirst,
    PixelOnly
};

// Draws a single line inside pixelRect. ASCII uses the supplied HellSinker
// atlas (or the embedded ORIGINAL atlas when atlas is null); non-ASCII text
// uses the embedded Fusion Pixel 12px CJK face. scale is the logical-to-pixel
// multiplier for the 8x11 atlas cells and the 12px CJK face. align accepts
// DT_LEFT, DT_CENTER, or DT_RIGHT.
void DrawPanelText(HDC dc, RECT pixelRect, const std::wstring& text,
                   COLORREF color, UINT align, float scale, HDC atlas,
                   PanelTextMode mode = PanelTextMode::AtlasFirst);

// Pixel-font-only line renderer used by the shared scene painter's gfx_text
// backend. codePoints is UTF-32; rect is in device pixels; align is 0/1/2.
void DrawCodepointLine(HDC dc, const std::uint32_t* codePoints, int count,
                       RECT pixelRect, COLORREF color, int align);
