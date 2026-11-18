// WASM branch drawing primitives. Derived support code: the desktop sources
// under player/src are NOT modified; this header only supplies Win32-compatible
// color types so the desktop theme table can be included read-only.
#pragma once
#include <cstdint>
#include <cstddef>

// COLORREF / RGB layout is 0x00BBGGRR. Keeping the same layout lets
// src/themes.generated.h compile unchanged in the WASM build.
using COLORREF = std::uint32_t;
#define RGB(r,g,b) ((COLORREF)(((std::uint32_t)(r) & 0xFFu) | \
                               (((std::uint32_t)(g) & 0xFFu) << 8) | \
                               (((std::uint32_t)(b) & 0xFFu) << 16)))
inline int GetRValue(COLORREF c) { return (int)(std::uint8_t)c; }
inline int GetGValue(COLORREF c) { return (int)(std::uint8_t)(c >> 8); }
inline int GetBValue(COLORREF c) { return (int)(std::uint8_t)(c >> 16); }

struct GfxPoint { std::int16_t x, y; };

enum GfxOp : std::uint8_t {
    GFX_CLEAR = 1,
    GFX_RECT,
    GFX_ALPHA_RECT,
    GFX_LINE,
    GFX_POLYLINE,
    GFX_POLYGON,
    GFX_ELLIPSE,
    GFX_BLIT
};

// One drawing instruction. Point payloads live in a separate pool referenced
// by pointsOffset/pointsCount so the struct stays small and POD.
struct GfxCmd {
    std::uint8_t  op;
    std::uint8_t  src;          // blit atlas: 0 = cells, 1 = title, 2 = axis
    std::uint8_t  width;        // line width
    std::uint8_t  alpha;        // alpha rect opacity
    COLORREF      color;
    std::int16_t  x0, y0, x1, y1;
    std::uint16_t pointsOffset;
    std::uint16_t pointsCount;
};

void gfx_reset();
void gfx_clear(COLORREF c);
void gfx_rect(int x0, int y0, int x1, int y1, COLORREF c);
void gfx_alpha_rect(int x0, int y0, int x1, int y1, COLORREF c, int alpha);
void gfx_line(int x0, int y0, int x1, int y1, COLORREF c, int width);
void gfx_polyline(const GfxPoint* pts, int n, COLORREF c, int width);
void gfx_polygon(const GfxPoint* pts, int n, COLORREF c);
void gfx_ellipse(int x0, int y0, int x1, int y1, COLORREF c);
void gfx_blit(int src, int sx, int sy, int sw, int sh, int dx, int dy);

const GfxCmd*   gfx_commands();
const GfxPoint* gfx_points();
int gfx_count();
