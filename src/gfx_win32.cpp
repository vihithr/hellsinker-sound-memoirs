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
#include "panel_text.h"

#include <cmath>
#include <cstdint>
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

// Themed title artwork (resources 115..119 = title_<theme>.bmp), blitted
// 1:1 with per-theme recoloring produced by tools/make_themed_title.py.
constexpr int kTitleResBase = 115;
constexpr int kTitleImages = 5;
struct ImageSet {
    HDC dc[kTitleImages];
    HBITMAP bmp[kTitleImages];
    bool built;
    ImageSet() : built(false) {
        for (int i = 0; i < kTitleImages; ++i) { dc[i] = nullptr; bmp[i] = nullptr; }
    }
    ~ImageSet() {
        for (int i = 0; i < kTitleImages; ++i) {
            if (dc[i]) DeleteDC(dc[i]);
            if (bmp[i]) DeleteObject(bmp[i]);
        }
    }
    void ensure() {
        if (built) return;
        for (int i = 0; i < kTitleImages; ++i) {
            bmp[i] = LoadBitmapA(GetModuleHandleA(nullptr), MAKEINTRESOURCEA(kTitleResBase + i));
            if (!bmp[i]) continue;
            dc[i] = CreateCompatibleDC(nullptr);
            if (!dc[i]) { DeleteObject(bmp[i]); bmp[i] = nullptr; continue; }
            SelectObject(dc[i], bmp[i]);
        }
        built = true;
    }
};
ImageSet g_images;

HDC atlasDC(int theme) {
    g_atlas.ensure();
    if (theme < 0 || theme > 4 || !g_atlas.dc[theme]) theme = 0;
    return g_atlas.dc[theme];
}

HDC imageDC(int which) {
    g_images.ensure();
    if (which < 0 || which >= kTitleImages) return nullptr;
    return g_images.dc[which];
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

// Cached radial-glare sprites: building a DIB per call (hundreds per frame)
// stalls GDI, so we keep a small cache keyed by (size, color). Each sprite is
// a premultiplied radial-falloff disc; per-frame cost is just AlphaBlend.
namespace {
struct GlareSprite {
    int w = 0, h = 0;
    COLORREF col = 0;
    HDC dc = nullptr;
    HBITMAP bmp = nullptr;
};
GlareSprite g_glare[160];
int g_glareCount = 0;

int g_glareNext = 0;
GlareSprite* glareGet(int w, int h, COLORREF c) {
    for (int i = 0; i < g_glareCount; ++i)
        if (g_glare[i].w == w && g_glare[i].h == h && g_glare[i].col == c)
            return &g_glare[i];
    // ring eviction: reuse the oldest slot instead of disabling the glare
    int idx;
    if (g_glareCount < 160) idx = g_glareCount++;
    else { idx = g_glareNext; g_glareNext = (g_glareNext + 1) % 160; }
    GlareSprite& s = g_glare[idx];
    if (s.bmp) { DeleteObject(s.bmp); s.bmp = nullptr; }
    if (s.dc) { DeleteDC(s.dc); s.dc = nullptr; }
    s.w = w; s.h = h; s.col = c;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    s.dc = CreateCompatibleDC(nullptr);
    s.bmp = CreateDIBSection(s.dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!s.bmp) { DeleteDC(s.dc); s.dc = nullptr; s.w = 0; return nullptr; }
    SelectObject(s.dc, s.bmp);
    const float rx = w * 0.5f, ry = h * 0.5f;
    const int R = GetRValue(c), G = GetGValue(c), B = GetBValue(c);
    std::uint32_t* px = static_cast<std::uint32_t*>(bits);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float dx = (x + 0.5f - rx) / rx, dy = (y + 0.5f - ry) / ry;
            const float d = std::sqrt(dx * dx + dy * dy);
            float a = d >= 1.f ? 0.f : (1.f - d);
            a = a * a;                            // unit radial falloff
            px[y * w + x] = ((std::uint32_t)(a * 255.f) << 24)
                | ((std::uint32_t)(R * a) << 16)
                | ((std::uint32_t)(G * a) << 8)
                | (std::uint32_t)(B * a);
        }
    return &s;
}
} // namespace

void gfx_alpha_ellipse(int x0, int y0, int x1, int y1, COLORREF c, int alpha) {
    if (!g_dc || alpha <= 0 || x1 <= x0 || y1 <= y0) return;
    const int w = x1 - x0, h = y1 - y0;
    if (w > 128 || h > 128) return;
    GlareSprite* s = glareGet(w, h, c);
    if (!s) return;
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, (BYTE)(alpha > 255 ? 255 : alpha), AC_SRC_ALPHA };
    AlphaBlend(g_dc, x0, y0, w, h, s->dc, 0, 0, w, h, bf);
}

void gfx_blit(int src, int sx, int sy, int sw, int sh, int dx, int dy,
              int dw, int dh) {
    HDC atlas = src >= kGfxImageBase ? imageDC(src - kGfxImageBase) : atlasDC(src);
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

void gfx_text(const std::uint32_t* cps, int count, int x, int y,
              int right, int bottom, COLORREF color, std::uint8_t align) {
    if (!g_dc) return;
    RECT rect = { x, y, right, bottom };
    DrawCodepointLine(g_dc, cps, count, rect, color, align);
}

// The command-buffer transport is a WASM-only concept; the GDI backend
// executes immediately and reports nothing.
const GfxCmd* gfx_commands() { return nullptr; }
const GfxPoint* gfx_points() { return nullptr; }
int gfx_count() { return 0; }
const GfxTextCmd* gfx_texts() { return nullptr; }
const std::uint32_t* gfx_text_points() { return nullptr; }
int gfx_text_count() { return 0; }
