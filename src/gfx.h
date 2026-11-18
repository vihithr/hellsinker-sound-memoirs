// gfx.h - platform-independent drawing primitives for SOUND MEMOIRS.
//
// The shared scene painter (scene.cpp) records a whole frame through this
// interface only.  Each platform provides one backend:
//   * src/gfx_win32.cpp        - desktop GDI; every primitive reproduces the
//                                exact GDI call sequence the former inline
//                                helpers in ui.cpp / stage.cpp performed.
//   * wasm/core/gfx_wasm.cpp   - encodes GfxCmd instructions replayed by
//                                wasm/scene.js on a Canvas 2D context.
//
// COLORREF keeps the Win32 0x00BBGGRR layout so src/themes.generated.h
// compiles verbatim on every platform: on Windows the native types are used,
// elsewhere minimal compatible definitions are provided.
#pragma once
#include <cstdint>
#include <cstddef>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>   // native COLORREF / RGB / GetRValue macros
#else
// Win32-compatible color types (same layout, same macro semantics).
using COLORREF = std::uint32_t;
#ifndef RGB
#define RGB(r,g,b) ((COLORREF)(((std::uint32_t)(r) & 0xFFu) | \
                               (((std::uint32_t)(g) & 0xFFu) << 8) | \
                               (((std::uint32_t)(b) & 0xFFu) << 16)))
#endif
inline int GetRValue(COLORREF c) { return (int)(std::uint8_t)c; }
inline int GetGValue(COLORREF c) { return (int)(std::uint8_t)(c >> 8); }
inline int GetBValue(COLORREF c) { return (int)(std::uint8_t)(c >> 16); }
#endif

struct GfxPoint { std::int16_t x, y; };

enum GfxOp : std::uint8_t {
    GFX_CLEAR = 1,
    GFX_RECT,
    GFX_ALPHA_RECT,
    GFX_LINE,
    GFX_POLYLINE,
    GFX_POLYGON,
    GFX_ELLIPSE,
    GFX_BLIT,
    GFX_PUSH_CLIP,
    GFX_POP_CLIP
};

// A run of CJK/Unicode text, addressed by UTF-32 code points. The desktop GDI
// backend renders it with the embedded Fusion Pixel font; the WASM backend
// records it for Canvas fillText with a dynamically loaded web font. Kept in a
// separate pool because its payload is variable-length code points.
struct GfxTextCmd {
    std::uint16_t codePointsOffset;  // into the gfx_text_points pool
    std::uint16_t count;             // number of code points
    std::int16_t  x, y, right, bottom;
    std::uint8_t  align;             // 0=left, 1=center, 2=right
    COLORREF      color;
};
constexpr std::uint8_t GFX_ALIGN_LEFT = 0, GFX_ALIGN_CENTER = 1, GFX_ALIGN_RIGHT = 2;

// One recorded drawing instruction (WASM backend transport; harmless to
// compile everywhere). Point payloads live in a separate pool referenced by
// pointsOffset/pointsCount so the struct stays a 20-byte POD.
struct GfxCmd {
    std::uint8_t  op;
    std::uint8_t  src;          // blit atlas index (theme cell atlas 0..4)
    std::uint8_t  width;        // line width in device pixels
    std::uint8_t  alpha;        // alpha rect opacity 0..255
    COLORREF      color;
    std::int16_t  x0, y0, x1, y1;
    std::uint16_t pointsOffset;
    std::uint16_t pointsCount;
};

// ---- frame lifecycle ------------------------------------------------------
// Begin a frame. The WASM backend clears the command buffer; the GDI backend
// has no cross-frame state to clear.
void gfx_reset();
// Fill the whole logical screen with a solid color.
void gfx_clear(COLORREF c);

// ---- primitives (device-pixel coordinates) --------------------------------
void gfx_rect(int x0, int y0, int x1, int y1, COLORREF c);
// Constant-alpha fill of a rect (GDI has no native alpha brush).
void gfx_alpha_rect(int x0, int y0, int x1, int y1, COLORREF c, int alpha);
void gfx_line(int x0, int y0, int x1, int y1, COLORREF c, int width);
void gfx_polyline(const GfxPoint* pts, int n, COLORREF c, int width);
void gfx_polygon(const GfxPoint* pts, int n, COLORREF c);
// Ellipse inscribed in [x0,y0]-[x1,y1] (border + fill in the same color).
void gfx_ellipse(int x0, int y0, int x1, int y1, COLORREF c);
// Atlas blit: copy cell (sx,sy,sw,sh) from atlas src to (dx,dy) sized
// (dw,dh). src is the themed cell-atlas index (0..4), matching the resource
// ids loaded by the desktop backend and wasm/assets/cells_<src>.png.
// src >= kGfxImageBase addresses full original artwork instead:
//   kGfxImageBase + 0 = title (resource 102 / wasm/assets/title.png)
//   kGfxImageBase + 1 = axis  (resource 103 / wasm/assets/axis.png)
void gfx_blit(int src, int sx, int sy, int sw, int sh, int dx, int dy,
              int dw, int dh);
constexpr int kGfxImageBase = 100;

// ---- rail fade (channelRail) ----------------------------------------------
// The page rail fades by blending the pre-rail background back over the
// freshly drawn rail. GDI: capture_begin BitBlits the region into a scratch
// bitmap, capture_blend AlphaBlends it back; when the region below the rail
// is a solid backdrop the blend is equivalent to an alpha rect of that color.
// Backends without readback encode only the blend (fallback color, cover),
// which is pixel-exact wherever the captured region equals the fallback.
void gfx_capture_begin(int x0, int y0, int x1, int y1);
void gfx_capture_blend(COLORREF fallback, int cover);

// ---- clip stack -----------------------------------------------------------
// Intersect the clip region with the rect / restore the previous region.
// GDI: SaveDC + IntersectClipRect / RestoreDC. WASM: recorded as
// GFX_PUSH_CLIP / GFX_POP_CLIP for the JS replay (clip does not survive a
// command buffer, so it has to be explicit state).
void gfx_push_clip(int x0, int y0, int x1, int y1);
void gfx_pop_clip();

// ---- Unicode text ---------------------------------------------------------
// Draw a line of text (UTF-32 code points) inside a logical-space rect.
void gfx_text(const std::uint32_t* codePoints, int count, int x, int y,
              int right, int bottom, COLORREF color, std::uint8_t align);

// ---- WASM backend transport -----------------------------------------------
const GfxCmd*   gfx_commands();
const GfxPoint* gfx_points();
int gfx_count();
const GfxTextCmd*  gfx_texts();
const std::uint32_t* gfx_text_points();
int gfx_text_count();

#ifdef _WIN32
// Desktop-only: bind the GDI backend to a device context. The shared painter
// never sees the HDC; the shell binds the backbuffer before sceneRender().
void gfx_bind(HDC dc);
// Desktop-only: themed cell atlas DC (resource cells_<theme>, base id 110),
// used by the shell's text path (DrawPanelText atlas argument).
HDC gfx_win32_atlas(int theme);
#endif
