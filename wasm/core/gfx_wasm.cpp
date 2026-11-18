// wasm/core/gfx_wasm.cpp - WASM backend for the shared gfx interface.
//
// Encodes GfxCmd instructions for the JS Canvas replay (wasm/scene.js). The
// shared painter (src/scene.cpp / src/stage.cpp) calls the same primitives as
// the desktop GDI backend, so both platforms execute one drawing code path.
#include "gfx.h"
#include <cstring>

namespace {
constexpr int kMaxCmds = 16384;
constexpr int kMaxPoints = 32768;
GfxCmd g_cmds[kMaxCmds];
GfxPoint g_points[kMaxPoints];
int g_count = 0;
int g_pointCount = 0;
// Unicode text pool (separate from the drawing command buffer).
constexpr int kMaxTexts = 256;
constexpr int kMaxTextPoints = 4096;
GfxTextCmd g_texts[kMaxTexts];
std::uint32_t g_textPoints[kMaxTextPoints];
int g_textCount = 0;
int g_textPointCount = 0;
// Last capture region (rail fade): the replay cannot read back the canvas,
// so the blend is encoded against this remembered rectangle.
int g_capX0 = 0, g_capY0 = 0, g_capX1 = 0, g_capY1 = 0;

void push(const GfxCmd& cmd) {
    if (g_count < kMaxCmds) g_cmds[g_count++] = cmd;
}
int storePoints(const GfxPoint* pts, int n) {
    const int offset = g_pointCount;
    for (int i = 0; i < n && g_pointCount < kMaxPoints; ++i)
        g_points[g_pointCount++] = pts[i];
    return offset;
}
}

void gfx_reset() { g_count = 0; g_pointCount = 0; g_textCount = 0; g_textPointCount = 0; }
const GfxCmd* gfx_commands() { return g_cmds; }
const GfxPoint* gfx_points() { return g_points; }
int gfx_count() { return g_count; }
const GfxTextCmd* gfx_texts() { return g_texts; }
const std::uint32_t* gfx_text_points() { return g_textPoints; }
int gfx_text_count() { return g_textCount; }

void gfx_text(const std::uint32_t* cps, int count, int x, int y,
              int right, int bottom, COLORREF color, std::uint8_t align) {
    if (count <= 0 || !cps || g_textCount >= kMaxTexts) return;
    const int offset = g_textPointCount;
    for (int i = 0; i < count && g_textPointCount < kMaxTextPoints; ++i)
        g_textPoints[g_textPointCount++] = cps[i];
    GfxTextCmd cmd{};
    cmd.codePointsOffset = (std::uint16_t)offset;
    cmd.count = (std::uint16_t)count;
    cmd.x = (std::int16_t)x; cmd.y = (std::int16_t)y;
    cmd.right = (std::int16_t)right; cmd.bottom = (std::int16_t)bottom;
    cmd.align = align; cmd.color = color;
    g_texts[g_textCount++] = cmd;
}

void gfx_clear(COLORREF c) {
    GfxCmd cmd{}; cmd.op = GFX_CLEAR; cmd.color = c; push(cmd);
}
void gfx_rect(int x0, int y0, int x1, int y1, COLORREF c) {
    // GDI FillRect normalizes reversed rectangles before filling; the wing
    // tick rules rely on it. Keep the same semantics in the encoding so the
    // replay matches the desktop pixel for pixel.
    if (x0 > x1) { const int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { const int t = y0; y0 = y1; y1 = t; }
    if (x1 <= x0 || y1 <= y0) return;
    GfxCmd cmd{}; cmd.op = GFX_RECT; cmd.color = c;
    cmd.x0 = (std::int16_t)x0; cmd.y0 = (std::int16_t)y0;
    cmd.x1 = (std::int16_t)x1; cmd.y1 = (std::int16_t)y1;
    push(cmd);
}
void gfx_alpha_rect(int x0, int y0, int x1, int y1, COLORREF c, int alpha) {
    if (alpha <= 0 || x1 <= x0 || y1 <= y0) return;
    GfxCmd cmd{}; cmd.op = GFX_ALPHA_RECT; cmd.color = c;
    cmd.alpha = (std::uint8_t)(alpha > 255 ? 255 : alpha);
    cmd.x0 = (std::int16_t)x0; cmd.y0 = (std::int16_t)y0;
    cmd.x1 = (std::int16_t)x1; cmd.y1 = (std::int16_t)y1;
    push(cmd);
}
void gfx_line(int x0, int y0, int x1, int y1, COLORREF c, int width) {
    GfxCmd cmd{}; cmd.op = GFX_LINE; cmd.color = c;
    cmd.width = (std::uint8_t)(width > 0 ? width : 1);
    cmd.x0 = (std::int16_t)x0; cmd.y0 = (std::int16_t)y0;
    cmd.x1 = (std::int16_t)x1; cmd.y1 = (std::int16_t)y1;
    push(cmd);
}
void gfx_polyline(const GfxPoint* pts, int n, COLORREF c, int width) {
    if (n <= 0) return;
    GfxCmd cmd{}; cmd.op = GFX_POLYLINE; cmd.color = c;
    cmd.width = (std::uint8_t)(width > 0 ? width : 1);
    cmd.pointsOffset = (std::uint16_t)storePoints(pts, n);
    cmd.pointsCount = (std::uint16_t)n;
    push(cmd);
}
void gfx_polygon(const GfxPoint* pts, int n, COLORREF c) {
    if (n <= 0) return;
    GfxCmd cmd{}; cmd.op = GFX_POLYGON; cmd.color = c;
    cmd.pointsOffset = (std::uint16_t)storePoints(pts, n);
    cmd.pointsCount = (std::uint16_t)n;
    push(cmd);
}
void gfx_ellipse(int x0, int y0, int x1, int y1, COLORREF c) {
    GfxCmd cmd{}; cmd.op = GFX_ELLIPSE; cmd.color = c;
    cmd.x0 = (std::int16_t)x0; cmd.y0 = (std::int16_t)y0;
    cmd.x1 = (std::int16_t)x1; cmd.y1 = (std::int16_t)y1;
    push(cmd);
}
void gfx_blit(int src, int sx, int sy, int sw, int sh, int dx, int dy,
              int dw, int dh) {
    GfxCmd cmd{}; cmd.op = GFX_BLIT; cmd.src = (std::uint8_t)src;
    cmd.x0 = (std::int16_t)sx; cmd.y0 = (std::int16_t)sy;
    cmd.x1 = (std::int16_t)sw; cmd.y1 = (std::int16_t)sh;
    // Destination position and size live in the point payload: {dx,dy} and
    // {dw,dh}. A {0,0} size falls back to the source cell size on replay,
    // which keeps the revision-1 command streams valid.
    GfxPoint dst[2] = { { (std::int16_t)dx, (std::int16_t)dy },
                        { (std::int16_t)dw, (std::int16_t)dh } };
    cmd.pointsOffset = (std::uint16_t)storePoints(dst, 2);
    cmd.pointsCount = 2;
    push(cmd);
}

void gfx_capture_begin(int x0, int y0, int x1, int y1) {
    // A command buffer has no readback: the replay cannot snapshot the
    // destination. Wherever the captured region is a solid backdrop (the
    // page rail always sits on one), the fallback blend below is the exact
    // equivalent of the desktop snapshot blend.
    g_capX0 = x0; g_capY0 = y0; g_capX1 = x1; g_capY1 = y1;
}

void gfx_capture_blend(COLORREF fallback, int cover) {
    if (cover <= 0) return;
    GfxCmd cmd{}; cmd.op = GFX_ALPHA_RECT; cmd.color = fallback;
    cmd.alpha = (std::uint8_t)(cover > 255 ? 255 : cover);
    cmd.x0 = (std::int16_t)g_capX0; cmd.y0 = (std::int16_t)g_capY0;
    cmd.x1 = (std::int16_t)g_capX1; cmd.y1 = (std::int16_t)g_capY1;
    push(cmd);
}

void gfx_push_clip(int x0, int y0, int x1, int y1) {
    GfxCmd cmd{}; cmd.op = GFX_PUSH_CLIP;
    cmd.x0 = (std::int16_t)x0; cmd.y0 = (std::int16_t)y0;
    cmd.x1 = (std::int16_t)x1; cmd.y1 = (std::int16_t)y1;
    push(cmd);
}

void gfx_pop_clip() {
    GfxCmd cmd{}; cmd.op = GFX_POP_CLIP;
    push(cmd);
}
