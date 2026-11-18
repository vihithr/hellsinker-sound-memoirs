// WASM drawing backend: records POD instructions for the JS Canvas renderer.
#include "gfx.h"
#include <cstring>

namespace {
constexpr int kMaxCmds = 16384;
constexpr int kMaxPoints = 32768;
GfxCmd g_cmds[kMaxCmds];
GfxPoint g_points[kMaxPoints];
int g_count = 0;
int g_pointCount = 0;

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

void gfx_reset() { g_count = 0; g_pointCount = 0; }
const GfxCmd* gfx_commands() { return g_cmds; }
const GfxPoint* gfx_points() { return g_points; }
int gfx_count() { return g_count; }

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
void gfx_blit(int src, int sx, int sy, int sw, int sh, int dx, int dy) {
    GfxCmd cmd{}; cmd.op = GFX_BLIT; cmd.src = (std::uint8_t)src;
    cmd.x0 = (std::int16_t)sx; cmd.y0 = (std::int16_t)sy;
    cmd.x1 = (std::int16_t)sw; cmd.y1 = (std::int16_t)sh;
    // destination is carried in the point payload (two points)
    GfxPoint dst[2] = { { (std::int16_t)dx, (std::int16_t)dy }, { 0, 0 } };
    cmd.pointsOffset = (std::uint16_t)storePoints(dst, 2);
    cmd.pointsCount = 2;
    push(cmd);
}
