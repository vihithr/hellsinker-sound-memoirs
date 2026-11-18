// gfx_win32.cpp - GDI backend for the platform-independent gfx interface.
//
// Every primitive reproduces the exact GDI call sequence the former inline
// helpers in ui.cpp / stage.cpp performed, so the shared scene painter
// (scene.cpp) produces byte-identical pixels on the desktop:
//   gfx_rect          <- FillRectCol  (CreateSolidBrush + FillRect)
//   gfx_alpha_rect    <- AlphaFill    (1x1 bitmap + AlphaBlend)
//   gfx_line          <- HLine / VLine (CreatePen + MoveToEx + LineTo)
//   gfx_polyline      <- Polyline paths (cosmetic/geometric pen, same widths)
//   gfx_polygon       <- stage Paper polygon (DC_PEN + DC_BRUSH + Polygon)
//   gfx_ellipse       <- stage Paper disc   (DC_PEN + DC_BRUSH + Ellipse)
//   gfx_blit          <- DrawCells    (COLORONCOLOR StretchBlt, atlas)
//   gfx_capture_*     <- channelRail fade (BitBlt snapshot + AlphaBlend back)
//   gfx_push/pop_clip  <- stage paint clip (SaveDC/IntersectClipRect/RestoreDC)
#include "gfx.h"

#include <cstring>

namespace {

HDC g_dc = nullptr;

// Largest polyline the scene painter records is 240 points (side waveform).
constexpr int kMaxStackPoints = 512;

// ---- themed bitmap art (was ui.cpp ArtSet) -------------------------------
// Glyph shapes come from the 8x11 cell atlas; per-theme art is pre-rendered
// into BMP resources (base id 110 = cells_<theme>). NEAREST-NEIGHBOR
// stretching keeps it crisp; COLORONCOLOR drops pixels when scaling down.
struct AtlasSet {
    HDC dc[5];
    HBITMAP bmp[5];
    bool built;
    AtlasSet() : built(false) {
        for (int i = 0; i < 5; ++i) { dc[i] = nullptr; bmp[i] = nullptr; }
    }
    ~AtlasSet() {
        for (int i = 0; i < 5; ++i) {
            if (dc[i]) { DeleteDC(dc[i]); dc[i] = nullptr; }
            if (bmp[i]) { DeleteObject(bmp[i]); bmp[i] = nullptr; }
        }
    }
    void ensure() {
        if (built) return;
        for (int t = 0; t < 5; ++t) {
            bmp[t] = LoadBitmapA(GetModuleHandleA(nullptr), MAKEINTRESOURCEA(110 + t));
            if (!bmp[t]) continue;
            dc[t] = CreateCompatibleDC(nullptr);
            if (!dc[t]) { DeleteObject(bmp[t]); bmp[t] = nullptr; continue; }
            SelectObject(dc[t], bmp[t]);
        }
        built = true;
    }
};

AtlasSet g_atlas;

HDC atlasDC(int theme) {
    g_atlas.ensure();
    if (theme < 0 || theme > 4 || !g_atlas.dc[theme]) theme = 0;
    return g_atlas.dc[theme];
}

// ---- rail fade snapshot (was channelRail's savedDC/savedBmp) --------------
HDC g_savedDC = nullptr;
HBITMAP g_savedBmp = nullptr;
int g_savedW = 0, g_savedH = 0;
int g_capX0 = 0, g_capY0 = 0, g_capX1 = 0, g_capY1 = 0;

// ---- clip stack -----------------------------------------------------------
int g_clipSave[16];
int g_clipDepth = 0;

} // namespace

void gfx_bind(HDC dc) { g_dc = dc; }

HDC gfx_win32_atlas(int theme) { return atlasDC(theme); }

void gfx_reset() {
    // The GDI backend keeps no per-frame state; snapshots and clip pushes are
    // always paired by the painter within a single frame.
}

void gfx_clear(COLORREF c) {
    if (!g_dc) return;
    RECT rc = { 0, 0, 32768, 32768 };   // GDI clips to the surface
    HBRUSH br = CreateSolidBrush(c);
    FillRect(g_dc, &rc, br);
    DeleteObject(br);
}

void gfx_rect(int x0, int y0, int x1, int y1, COLORREF c) {
    if (!g_dc) return;
    // GDI FillRect normalizes reversed rectangles before filling (the wing
    // tick rules rely on it); match that instead of dropping the call.
    if (x0 > x1) { const int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { const int t = y0; y0 = y1; y1 = t; }
    if (x1 <= x0 || y1 <= y0) return;
    RECT rc;
    rc.left = x0; rc.top = y0; rc.right = x1; rc.bottom = y1;
    HBRUSH br = CreateSolidBrush(c);
    FillRect(g_dc, &rc, br);
    DeleteObject(br);
}

void gfx_alpha_rect(int x0, int y0, int x1, int y1, COLORREF c, int alpha) {
    if (!g_dc || alpha <= 0 || x1 <= x0 || y1 <= y0) return;
    static HDC srcDC = nullptr;
    static HBITMAP srcBmp = nullptr;
    if (!srcDC) {
        srcDC = CreateCompatibleDC(nullptr);
        srcBmp = CreateBitmap(1, 1, 1, 32, nullptr);
        SelectObject(srcDC, srcBmp);
    }
    SetPixel(srcDC, 0, 0, c);
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, (BYTE)(alpha > 255 ? 255 : alpha), 0 };
    AlphaBlend(g_dc, x0, y0, x1 - x0, y1 - y0, srcDC, 0, 0, 1, 1, bf);
}

void gfx_line(int x0, int y0, int x1, int y1, COLORREF c, int width) {
    if (!g_dc) return;
    if (width <= 0) width = 1;
    HPEN p = CreatePen(PS_SOLID, width, c);
    HGDIOBJ old = SelectObject(g_dc, p);
    MoveToEx(g_dc, x0, y0, nullptr);
    LineTo(g_dc, x1, y1);
    SelectObject(g_dc, old);
    DeleteObject(p);
}

void gfx_polyline(const GfxPoint* pts, int n, COLORREF c, int width) {
    if (!g_dc || !pts || n <= 0 || n > kMaxStackPoints) return;
    if (width <= 0) width = 1;
    POINT buf[kMaxStackPoints];
    for (int i = 0; i < n; ++i) { buf[i].x = pts[i].x; buf[i].y = pts[i].y; }
    HPEN p = CreatePen(PS_SOLID, width, c);
    HGDIOBJ old = SelectObject(g_dc, p);
    Polyline(g_dc, buf, n);
    SelectObject(g_dc, old);
    DeleteObject(p);
}

void gfx_polygon(const GfxPoint* pts, int n, COLORREF c) {
    if (!g_dc || !pts || n <= 0 || n > kMaxStackPoints) return;
    POINT buf[kMaxStackPoints];
    for (int i = 0; i < n; ++i) { buf[i].x = pts[i].x; buf[i].y = pts[i].y; }
    SelectObject(g_dc, GetStockObject(DC_PEN));
    SelectObject(g_dc, GetStockObject(DC_BRUSH));
    SetDCPenColor(g_dc, c);
    SetDCBrushColor(g_dc, c);
    Polygon(g_dc, buf, n);
}

void gfx_ellipse(int x0, int y0, int x1, int y1, COLORREF c) {
    if (!g_dc) return;
    SelectObject(g_dc, GetStockObject(DC_PEN));
    SelectObject(g_dc, GetStockObject(DC_BRUSH));
    SetDCPenColor(g_dc, c);
    SetDCBrushColor(g_dc, c);
    Ellipse(g_dc, x0, y0, x1, y1);
}

void gfx_blit(int src, int sx, int sy, int sw, int sh, int dx, int dy,
              int dw, int dh) {
    HDC atlas = atlasDC(src);
    if (!g_dc || !atlas) return;
    if (dw <= 0 || dh <= 0) { dw = sw; dh = sh; }
    SetStretchBltMode(g_dc, COLORONCOLOR);
    StretchBlt(g_dc, dx, dy, dw, dh, atlas, sx, sy, sw, sh, SRCCOPY);
}

void gfx_capture_begin(int x0, int y0, int x1, int y1) {
    if (!g_dc || x1 <= x0 || y1 <= y0) return;
    const int width = x1 - x0, height = y1 - y0;
    if (g_savedW != width || g_savedH != height) {
        if (g_savedBmp) { DeleteObject(g_savedBmp); g_savedBmp = nullptr; }
        if (g_savedDC) { DeleteDC(g_savedDC); g_savedDC = nullptr; }
        g_savedDC = CreateCompatibleDC(g_dc);
        if (!g_savedDC) return;
        g_savedBmp = CreateCompatibleBitmap(g_dc, width, height);
        if (!g_savedBmp) { DeleteDC(g_savedDC); g_savedDC = nullptr; return; }
        SelectObject(g_savedDC, g_savedBmp);
        g_savedW = width; g_savedH = height;
    }
    BitBlt(g_savedDC, 0, 0, width, height, g_dc, x0, y0, SRCCOPY);
    g_capX0 = x0; g_capY0 = y0; g_capX1 = x1; g_capY1 = y1;
}

void gfx_capture_blend(COLORREF fallback, int cover) {
    (void)fallback;   // the GDI backend blends the true snapshot
    if (!g_dc || !g_savedDC || cover <= 0) return;
    cover = cover > 255 ? 255 : cover;
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, (BYTE)cover, 0 };
    AlphaBlend(g_dc, g_capX0, g_capY0, g_capX1 - g_capX0, g_capY1 - g_capY0,
               g_savedDC, 0, 0, g_savedW, g_savedH, blend);
}

void gfx_push_clip(int x0, int y0, int x1, int y1) {
    if (!g_dc || g_clipDepth >= 16) return;
    int save = SaveDC(g_dc);
    if (!save) return;
    g_clipSave[g_clipDepth++] = save;
    IntersectClipRect(g_dc, x0, y0, x1, y1);
}

void gfx_pop_clip() {
    if (!g_dc || g_clipDepth <= 0) return;
    RestoreDC(g_dc, g_clipSave[--g_clipDepth]);
}

// The command-buffer transport is a WASM-only concept; the GDI backend
// executes immediately and reports nothing.
const GfxCmd* gfx_commands() { return nullptr; }
const GfxPoint* gfx_points() { return nullptr; }
int gfx_count() { return 0; }
