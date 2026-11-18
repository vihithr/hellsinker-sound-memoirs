// Derived from player/src/ui.cpp - SOUND MEMOIRS painting functions.
// Every constant, coordinate and animation phase is copied from the desktop
// implementation. Win32 GDI calls are replaced by the gfx instruction backend.
// The desktop source files are NOT modified.
#include "paint.h"
#include "gfx.h"
#include "layout_tables.h"

#include "../../src/themes.generated.h"
#include "../../src/themed_art_runs.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

// The browser renders the logical 640x480 screen directly, so scale is 1.
// This matches desktop --shot-scale 1, where S(v) == v.
static inline int S(int v) { return v; }
static inline int Sf(float v) { return (int)(v + 0.5f); }

const SceneState* g_state = nullptr;
static inline const Theme& Th() { return kThemes[g_state->theme]; }

#define kColOuter     (Th().outer)
#define kColPanel     (Th().panel)
#define kColStrip     (Th().strip)
#define kColStripHot  (Th().stripHot)
#define kColBar       (Th().bar)
#define kColBarLit    (Th().barLit)
#define kColFrame     (Th().frame)
#define kColWave      (Th().wave)
#define kColSelBase   (Th().selBase)
#define kColSelEdge   (Th().selEdge)
#define kColSelFoot   (Th().selFoot)

static inline int ClampI(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

COLORREF LerpCol(COLORREF a, COLORREF b, float t) {
    if (t <= 0.0f) return a;
    if (t >= 1.0f) return b;
    return RGB((int)(GetRValue(a) + (GetRValue(b) - GetRValue(a)) * t),
               (int)(GetGValue(a) + (GetGValue(b) - GetGValue(a)) * t),
               (int)(GetBValue(a) + (GetBValue(b) - GetBValue(a)) * t));
}

void FillRectCol(int l, int t, int r, int b, COLORREF c) {
    gfx_rect(l, t, r, b, c);
}
void AlphaFill(int l, int t, int r, int b, COLORREF c, int alpha) {
    gfx_alpha_rect(l, t, r, b, c, alpha);
}
void HLine(int x0, int x1, int y, COLORREF c, int w = 1) {
    gfx_line(x0, y, x1, y, c, w);
}
void VLine(int y0, int y1, int x, COLORREF c, int w = 1) {
    gfx_line(x, y0, x, y1, c, w);
}

// Draw a run-length vector table (2x source coords) as crisp rects.
// Derived verbatim from DrawRuns in ui.cpp, including the 1px stroke guard.
void DrawRuns(const ArtRun* runs, int n, float ox, float oy,
              COLORREF col0, COLORREF col1) {
    for (int c = 0; c < 2; ++c) {
        const COLORREF color = c == 0 ? col0 : col1;
        for (int i = 0; i < n; ++i) {
            if (runs[i].c != c) continue;
            int left = Sf(ox + runs[i].x * 0.5f);
            int top = Sf(oy + runs[i].y * 0.5f);
            int right = Sf(ox + (runs[i].x + runs[i].w) * 0.5f);
            int bottom = Sf(oy + (runs[i].y + 1) * 0.5f);
            if (right <= left) right = left + 1;
            if (bottom <= top) bottom = top + 1;
            gfx_rect(left, top, right, bottom, color);
        }
    }
}

// Fixed-pitch 8x11 atlas text. palette: 0 normal, 1 selection, 2 dim,
// 3 no-underline info, 4 orange frequency label.
void DrawCells(const char* text, int x, int y, int palette = 0) {
    static const char* cells = "ABCDEFGHIJKLNMOPQRSTUVWXYZ 0123456789_-.:;&<>*+";
    for (const char* p = text; *p; ++p) {
        const char* found = std::strchr(cells, *p);
        const int index = found ? (int)(found - cells) : (int)std::strchr(cells, ' ') - (int)cells;
        gfx_blit(g_state->theme, index * 8, palette * 11, 8, 11, x - 4, y - 5);
        x += 8;
    }
}

void PaintBackdrop() {
    FillRectCol(0, 0, kLogicalW, kLogicalH, kColOuter);
}

// Level meter: count = int(env*90); loop draws i=0..count INCLUSIVE.
// i>44 switches to the packed hot color. Left reads RIGHT channel, right
// reads LEFT channel (matches the desktop / original array aliasing).
void PaintLevelMeter() {
    const int nL = (int)(g_state->spectrum.envL * 90.0f);
    const int nR = (int)(g_state->spectrum.envR * 90.0f);
    for (int i = 0; i <= nL && nL > 0; ++i) {
        const int y2 = 479 - 8 * i, y1 = 473 - 8 * i;
        FillRectCol(4, y1, 138, y2, i > 44 ? kColStripHot : kColStrip);
    }
    for (int i = 0; i <= nR && nR > 0; ++i) {
        const int y2 = 479 - 8 * i, y1 = 473 - 8 * i;
        FillRectCol(502, y1, 636, y2, i > 44 ? kColStripHot : kColStrip);
    }
}

// 240-point waveform polyline at x=70 (left) / x=570 (right), y=2*i.
void PaintSideWave() {
    // Desktop guard: the settled --shot frame has current < 0 and draws no
    // waveform at all. Without this, two spurious 2px vertical lines appear.
    if (g_state->current < 0) return;
    for (int side = 0; side < 2; ++side) {
        const float base = side == 0 ? 70.0f : 570.0f;
        const float* wave = side == 0 ? g_state->spectrum.waveL : g_state->spectrum.waveR;
        GfxPoint pts[240];
        for (int i = 0; i < 240; ++i) {
            pts[i].x = (std::int16_t)(int)(base + wave[i]);
            pts[i].y = (std::int16_t)(2 * i);
        }
        gfx_polyline(pts, 240, kColWave, 2);
    }
}

// Panel frame: expands over 64 ticks, fully open after ~1.07 s.
void PaintFrame() {
    int c = (int)g_state->tick;
    if (c > 64) c = 64;
    int n1 = c * 4;
    if (n1 > 128) n1 = 128;
    int n2 = c * 4 - 128;
    if (n2 < 0) n2 = 0;
    if (n2 > 128) n2 = 128;
    const int t1 = (int)((float)n1 / 128.0f * 177.0f);
    const int t2 = (int)((float)n2 / 128.0f * 236.0f);
    const int xl = 320 - t1 - 1, xr = 320 + t1;
    const int yt = 240 - t2, yb = 240 + t2;
    VLine(yt, yb, xl, kColFrame, 2);
    VLine(yt, yb, xr, kColFrame, 2);
    HLine(xl, xr, yt, kColFrame, 2);
    HLine(xl, xr, yb, kColFrame, 2);
}

void PaintSpectrum(const SpecGeom& g) {
    const int t = (int)g_state->tick;
    const float bgA = ClampI(3 * t - 128, 0, 128) / 128.0f;
    const float seamA = ClampI(12 * t - 512, 0, 192) / 192.0f;
    const int axisA = ClampI(4 * t - 256, 0, 255);
    const int labelA = ClampI(20 * t - 800, 0, 255);

    const COLORREF panel = LerpCol(kColOuter, kColPanel, bgA);
    FillRectCol(g.l, g.t, g.r, g.axisY, panel);
    for (int i = 0; i < 8; ++i)
        VLine(g.t, g.axisY, kSeamX[i], LerpCol(panel, Th().seam, seamA), 1);

    for (int k = 0; k < g.bars; ++k) {
        const int level = (int)g_state->spectrum.barsLevel[k];
        const int old = (int)g_state->spectrum.barsPrev[k];
        const int x1 = g.l + g.pitch * k;
        const int x2 = x1 + g.barW;
        if (level > 3) {
            const int segs = (level >> 2) + 1;
            for (int m = 0; m < segs; ++m) {
                const int y2 = g.barBase - 3 * m;
                FillRectCol(x1, y2 - kSegH, x2, y2, kColBar);
            }
        }
        if (old > 3 && (old >> 2) > level / 4) {
            const int segs = (old >> 2) - level / 4 + 1;
            for (int m = 0; m < segs; ++m) {
                const int mm = level / 4 + m;
                const int y2 = g.barBase - 3 * mm;
                FillRectCol(x1, y2 - kSegH, x2, y2, kColBarLit);
            }
        }
    }

    if (axisA > 0) {
        const COLORREF fc = LerpCol(panel, Th().frame, axisA / 255.0f);
        FillRectCol(g.l, g.axisY, g.r, g.axisY + 2, fc);
    }
    if (labelA > 0) {
        for (int i = 0; i < 8; ++i) {
            const int len = (int)std::strlen(kFreqLabel[i]);
            DrawCells(kFreqLabel[i], kSeamX[i] - 4 * len + 4, g.axisY + 4, 4);
        }
    }
}

void PaintTitle() {
    DrawRuns(kTitleRuns, kTitleRunsN, 152, 18, Th().title, Th().titleBlock);
}

void PaintSelection(const TracksGeom& g, int col, int row) {
    const int x = g.gridX - 12 + g.colW * col;
    const int y = g.gridY - 8 + g.rowH * row;
    const bool pulse = (g_state->tick % 5) >= 2;
    const COLORREF base = pulse ? LerpCol(kColSelBase, RGB(255,255,255), .12f) : kColSelBase;
    const COLORREF edge = pulse ? LerpCol(kColSelEdge, RGB(255,255,255), .12f) : kColSelEdge;
    const COLORREF foot = pulse ? LerpCol(kColSelFoot, RGB(255,255,255), .12f) : kColSelFoot;
    FillRectCol(x, y, x + g.colW, y + g.rowH, base);
    FillRectCol(x, y + 3, x + g.colW, y + 14, Th().selBand);
    FillRectCol(x, y, x + g.colW, y + 1, edge);
    FillRectCol(x, y + 14, x + g.colW, y + 15, foot);
}

void PaintSelectionStripe(const TracksGeom& g) {
    int col, row;
    if (g_state->selected == kExitIndex) { col = 0; row = -1; }
    else { col = g_state->selected / g.rows; row = g_state->selected % g.rows; }
    const int x = g.gridX - 12 + g.colW * col;
    const int y = g.gridY - 8 + g.rowH * row;
    const int h = 2 * (int)(g_state->tick % 5);
    if (h <= 0) return;
    const int cy = y + 8;
    AlphaFill(x + 1, cy - h / 2, x + g.colW - 1, cy - h / 2 + h,
              Th().selStripe, 0x60);
}

void PaintExit(const TracksGeom& g) {
    const bool selected = g_state->selected == kExitIndex;
    if (selected) PaintSelection(g, 0, -1);
    DrawCells("EXIT", g.exitX, g.exitY, selected ? 1 : 0);
}

void PaintGridLabel(int idx, int x, int y, bool selected) {
    char label[8];
    std::snprintf(label, sizeof(label), "TR:%02d", idx);
    DrawCells(label, x, y, selected ? 1 : 0);
}

void PaintGrid(const TracksGeom& g) {
    for (int col = 0; col < g.cols; ++col) {
        for (int row = 0; row < g.rows; ++row) {
            const int idx = col * g.rows + row;
            if (idx >= kTrackCount) continue;
            if (g_state->selected == idx) PaintSelection(g, col, row);
            PaintGridLabel(idx, g.gridX + g.colW * col, g.gridY + g.rowH * row,
                           g_state->selected == idx);
        }
    }
}

void PaintInfo(const TracksGeom& g) {
    const int x = g.infoX, y1 = g.infoY1, y2 = g.infoY2;

    char line1[64];
    if (g_state->loading) {
        std::snprintf(line1, sizeof(line1), "%s", "<LOADING>");
    } else if (g_state->current < 0) {
        std::snprintf(line1, sizeof(line1), "%s", "<SILENCE>");
    } else if (g_state->current >= kTrackCount) {
        std::snprintf(line1, sizeof(line1), "%s", "NOW PLAYING : LOCAL FILE");
    } else {
        std::snprintf(line1, sizeof(line1), "NOW PLAYING : TRACK %02d", g_state->current);
    }

    char timeBuf[64];
    int secs = (int)g_state->seconds;
    if (secs < 0) secs = 0;
    std::snprintf(timeBuf, sizeof(timeBuf), "PLAY TIME   : %02dSEC", secs);

    const int lineLeft = x - 4, lineRight = 486;
    const int line1Capacity = std::max(0, (lineRight - lineLeft) / 8);
    if ((int)std::strlen(line1) > line1Capacity) {
        if (line1Capacity >= 3) {
            line1[line1Capacity - 3] = '.';
            line1[line1Capacity - 2] = '.';
            line1[line1Capacity - 1] = '.';
            line1[line1Capacity] = 0;
        } else {
            line1[line1Capacity] = 0;
        }
    }

    DrawCells(line1, x, y1, 3);
    DrawCells(timeBuf, x, y2, 3);
    const int w1 = (int)std::strlen(line1) * 8;
    HLine(lineLeft, lineLeft + w1, y1 + 4, Th().underline, std::max(1, S(1)));
    HLine(lineLeft, lineLeft + 19 * 8, y2 + 4, Th().underline, std::max(1, S(1)));
}

void PaintHints(const TracksGeom& g) {
    const int palette = ((g_state->tick / 24) % 2) ? 2 : 0;
    DrawCells("<MAINSHOT: LOOPPLAY> <SUBWEAPON: STOP>", g.hintX, g.hintY, palette);
}

void PaintWings(int style) {
    if (style == 1) { PaintLevelMeter(); PaintSideWave(); return; }
    const Theme& t = Th();
    for (int side = 0; side < 2; ++side) {
        const int base = side ? 570 : 70;
        const float* wave = side ? g_state->spectrum.waveR : g_state->spectrum.waveL;
        if (style == 3) {
            GfxPoint upper[220], lower[220];
            for (int j = 0; j < 220; ++j) {
                const float v = wave[j], lag = wave[(j + 12) % 220];
                const int center = base + (int)(.58f * v);
                const int w = 3 + (int)(.37f * std::abs(v - lag));
                const int y = 26 + 2 * j;
                upper[j] = { (std::int16_t)(center - w), (std::int16_t)y };
                lower[j] = { (std::int16_t)(center + w), (std::int16_t)y };
                if (j % 5 == 0)
                    FillRectCol(center - w, y, center + w + 1, y + 1, t.strip);
            }
            gfx_polyline(upper, 220, t.wave, 1);
            gfx_polyline(lower, 220, t.bar, 1);
        } else {
            const int span = 2 + (int)(61 * std::clamp(g_state->wing[side], 0.f, 1.f));
            for (int row = 0; row < 44; ++row) {
                float peak = 0;
                for (int j = 0; j < 5; ++j)
                    peak = std::max(peak, std::abs(wave[row * 5 + j]));
                const float contour = .34f + .66f * std::sqrt(std::min(1.f, peak / 64.f));
                const int width = 2 + (int)((span - 2) * contour);
                const int y = 27 + row * 10;
                if (style == 2) {
                    const float heat = std::clamp(contour * .62f + g_state->wing[side] * .38f, 0.f, 1.f);
                    const COLORREF ink = row & 1
                        ? LerpCol(t.wave, t.underline, .62f + .16f * heat)
                        : LerpCol(t.wave, t.bar, .08f + .34f * heat);
                    FillRectCol(base - width, y, base + width, y + 2, ink);
                    const int length = row % 5 == 0 ? 5 : 2;
                    const COLORREF tick = row % 5 == 0
                        ? LerpCol(t.hint, t.bar, .34f + .34f * heat)
                        : LerpCol(t.hint, t.underline, .42f + .22f * heat);
                    for (int edge : { -64, 63 }) {
                        const int px = base + edge;
                        FillRectCol(px, y, px + (edge < 0 ? length : -length), y + 1, tick);
                    }
                } else {
                    for (int col = 0; col < 12; ++col) {
                        const int px = base + (side ? -1 : 1) * (col * 10 - 55);
                        const bool lit = col * 5 + 2 < width;
                        FillRectCol(px - 2, y - 1, px + (lit ? 3 : 0), y + (lit ? 3 : 1),
                                    lit ? (col > 8 ? t.bar : t.wave)
                                        : LerpCol(t.outer, t.wave, .30f));
                    }
                }
            }
        }
        DrawCells(side ? "CH : L" : "CH : R", base - 20, 13, 3);
    }
}

void panelBox(int x, int y, int r, int b, COLORREF c) { FillRectCol(x, y, r, b, c); }

// Desktop page rail fades after 150 idle ticks. lastRailActivityTick starts at
// 0 and --shot uses tick 200, so the settled baseline frame captures the rail
// mid-fade (opacity ~0.167). Reproduce that blend exactly.
void channelRail() {
    const int left = kTracks.railX - 4, top = 272;
    const int right = kTracks.railX + 3 * kTracks.channelW + 4, bottom = 288;
    const unsigned idle = (unsigned)g_state->tick;  // lastRailActivityTick = 0
    float opacity = 1.0f;
    if (idle > 150 && idle < 210)
        opacity = 1.0f - float(idle - 150) / 60.0f;
    else if (idle >= 210)
        opacity = 0.0f;
    const bool fading = opacity < 0.999f;
    static const char* labels[] = { "TR", "LIB", "SYS" };
    for (int i = 0; i < 3; ++i) {
        const int x = kTracks.railX + i * kTracks.channelW;
        const bool active = i == g_state->page;
        DrawCells(labels[i], x + 8, 280, active ? 0 : 2);
        if (i == g_state->hoverChannel && !active)
            panelBox(x + 4, 286, x + 30, 287, Th().underline);
        if (active) {
            panelBox(x, 276, x + 2, 284, Th().bar);
            panelBox(x, 285, x + 5, 286, Th().bar);
        }
    }
    if (fading) {
        // Desktop AlphaBlends the saved pre-rail background over the rail;
        // this region is plain backdrop, so an outer-colored alpha rect is
        // pixel-equivalent to that cover blend.
        const int cover = ClampI((int)((1.0f - opacity) * 255.0f + 0.5f), 0, 255);
        AlphaFill(left, top, right, bottom, kColOuter, cover);
    }
}

} // namespace

void sceneRender(const SceneState& s) {
    g_state = &s;
    gfx_reset();
    gfx_clear(kColOuter);

    const int stageMode = s.stageMode;
    const int wingStyle = s.wingMode ? s.wingMode : stageMode + 1;
    PaintWings(wingStyle);
    PaintFrame();
    if (!stageMode) {
        PaintSpectrum(kSpec);
    } else {
        // Additional stage compositions are out of scope for revision 1.
        PaintSpectrum(kSpec);
    }
    PaintTitle();
    PaintInfo(kTracks);
    PaintExit(kTracks);
    PaintGrid(kTracks);
    PaintSelectionStripe(kTracks);
    PaintHints(kTracks);
    channelRail();

    // Entrance near-black tint: clamp(8*tick,0,255), fading over 32 frames.
    const int t = (int)s.tick;
    if (t < 32)
        AlphaFill(0, 0, kLogicalW, kLogicalH, RGB(8,4,16), 255 - ClampI(8 * t, 0, 255));
}
