// scene.cpp - the shared SOUND MEMOIRS scene painter.
//
// Extracted verbatim from the former desktop ui.cpp painting functions
// (constants, coordinates, animation phases and paint order preserved) with
// three mechanical substitutions:
//   * Win32 GDI calls   -> the platform-independent gfx_* primitives
//   * PlayerApp globals -> the SceneState snapshot (g_state)
//   * fixed 2x desktop render -> the caller-provided scale (S() helper)
// Shell-owned overlays (library / settings CJK pages) run through the
// SceneHost hooks at the paint positions the desktop code used.
#include "scene.h"
#include "gfx.h"
#include "layout.h"
#include "themes.generated.h"
#include "themed_art_runs.h"
#include "core/game_tracks.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

using std::max;

namespace {

// ---- logical screen geometry (as in the former ui.cpp) -------------------
const int kLogicalW = scene::kLogicalWidth;
const int kLogicalH = scene::kLogicalHeight;

// spectrum window geometry constants used directly by the painter
const int kBars = memoirs::kSpectrumBands;   // 43 columns, 7px wide, pitch 8
const int kSegH = 2;                          // y1 = y2-2, step 3

// spectrum band-edge seam columns (logical x), tinted toward Th().seam
const int kSeamX[8] = { 171, 195, 243, 291, 339, 387, 435, 475 };
const char* kFreqLabel[8] = { "52", "110", "540", "1060", "2080", "4160", "12K", "22K" };

// run-length vector tables for title / axis (drawn as crisp scaled rects)
// come from themed_art_runs.h (kTitleRuns / kTitleRunsN).

// selection / pages
const int kExitIndex = -1;

// channel rail (page switcher) timing: fully visible while the user is
// active, fades out over 60 frames after 150 idle frames.
constexpr unsigned kRailIdleDelayFrames = 150;
constexpr unsigned kRailFadeFrames = 60;
constexpr int railX = 366, channelWidth = 40;

// ---- scale ---------------------------------------------------------------
// The scene is authored at 640x480 logical pixels and rendered into a
// backbuffer of 640*scale x 480*scale (desktop) or directly (WASM, scale 1).
float g_sceneScale = 1.0f;
static inline int S(int v) { return (int)(v * g_sceneScale + 0.5f); }
static inline int Sf(float v) { return (int)(v * g_sceneScale + 0.5f); }

const SceneState* g_state = nullptr;
static inline const Theme& Th() { return kThemes[g_state->theme]; }

// ---- LIB track-name pool ------------------------------------------------
// The shell (browser or desktop) pushes the filtered local-track names here
// so the archive page can list real file names. Fixed-width ASCII records:
// the browser writes count*40 bytes of NUL-terminated names at once.
constexpr int kLibNameMax = 64;
constexpr int kLibNameWidth = 40;
char g_libNames[kLibNameMax][kLibNameWidth] = {};
int g_libNameCount = 0;

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

// ---- primitive wrappers (kept so the extracted bodies read unchanged) -----
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
void panelBox(int x, int y, int r, int b, COLORREF c) {
    FillRectCol(S(x), S(y), S(r), S(b), c);
}

// ---- Unicode text helpers (SYS / LIB shell pages) -------------------------
// Convert an ASCII/UTF-8 string to UTF-32 code points and emit a gfx_text cmd.
static void textUtf8(const char* text, int x, int y, int right, int bottom,
                     COLORREF color, std::uint8_t align = GFX_ALIGN_LEFT) {
    if (!text) return;
    std::uint32_t cps[64];
    int n = 0;
    for (const unsigned char* p = (const unsigned char*)text; *p && n < 64;) {
        std::uint32_t cp = *p++;
        if (cp >= 0xC0 && cp < 0xE0 && *p) { cp = ((cp & 0x1F) << 6) | (*p++ & 0x3F); }
        else if (cp >= 0xE0 && cp < 0xF0 && *p && p[1]) {
            std::uint32_t b1 = *p++; std::uint32_t b2 = *p++;
            cp = ((cp & 0x0F) << 12) | ((b1 & 0x3F) << 6) | (b2 & 0x3F);
        }
        cps[n++] = cp;
    }
    gfx_text(cps, n, S(x), S(y), S(right), S(bottom), color, align);
}
// Emit text from a UTF-32 code point array stored in SceneState.
static void textCodePoints(const int* cps, int count, int x, int y, int right,
                           int bottom, COLORREF color,
                           std::uint8_t align = GFX_ALIGN_LEFT) {
    if (!cps || count <= 0) return;
    std::uint32_t buf[64];
    int n = 0;
    for (int i = 0; i < count && n < 64; ++i) buf[n++] = (std::uint32_t)cps[i];
    gfx_text(buf, n, S(x), S(y), S(right), S(bottom), color, align);
}

// Draw a run-length vector table (2x source coords) as crisp scaled rects.
// ox,oy = logical origin of the artwork; col0/col1 = theme colors for c=0/1.
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
            // never let rounding collapse a thin stroke to zero size
            if (right <= left) right = left + 1;
            if (bottom <= top) bottom = top + 1;
            gfx_rect(left, top, right, bottom, color);
        }
    }
}

// ---- fixed-pitch cell text -----------------------------------------------
// palette: 0 normal, 1 selection band, 2 dim, 3 no-underline (info lines),
// 4 orange frequency labels. The themed atlas (resource cells_<theme> on
// the desktop, wasm/assets/cells_<theme>.png in the browser) is blitted
// with nearest-neighbour scaling so the glyphs stay crisp at any scale.
void DrawCells(const char* text, int x, int y, int palette = 0) {
    static const char* cells = "ABCDEFGHIJKLNMOPQRSTUVWXYZ 0123456789_-.:;&<>*+";
    for (const char* p = text; *p; ++p) {
        const char* found = std::strchr(cells, *p);
        const int index = found ? (int)(found - cells)
                                : (int)(std::strchr(cells, ' ') - cells);
        gfx_blit(g_state->theme, index * 8, palette * 11, 8, 11,
                 S(x - 4), S(y - 5), S(8), S(11));
        x += 8;
    }
}

// LIB track titles: ORIGINAL font uses the 8x11 cell atlas (ASCII only,
// uppercased), PIXEL font uses the gfx_text path (dynamic CJK web font).
static void textLib(const char* text, int x, int y, int right, int bottom,
                    COLORREF color, std::uint8_t align = GFX_ALIGN_LEFT) {
    if (!text) return;
    if (g_state->libFont == 0) {
        (void)right; (void)bottom; (void)align;
        DrawCells(text, x + 4, y + 5, 0);
    } else {
        textUtf8(text, x, y, right, bottom, color, align);
    }
}

// ---- painting ------------------------------------------------------------

// Level meter (asm 0x7F1E43..0x7F20DA): count = abs(int(env/32767*90));
// env here is already normalized, so count = int(env*90).  When count > 0
// the loop draws i = 0..count INCLUSIVE (count+1 stripes); i > 44 only
// switches the packed color.  Left meter reads envL, right meter reads envR
// (matching the original array aliasing).
void PaintLevelMeter() {
    const int nL = (int)(g_state->spectrum.envL * 90.0f);
    const int nR = (int)(g_state->spectrum.envR * 90.0f);
    for (int i = 0; i <= nL && nL > 0; ++i) {
        const int y2 = 479 - 8 * i, y1 = 473 - 8 * i;
        FillRectCol(S(4), S(y1), S(138), S(y2), i > 44 ? kColStripHot : kColStrip);
    }
    for (int i = 0; i <= nR && nR > 0; ++i) {
        const int y2 = 479 - 8 * i, y1 = 473 - 8 * i;
        FillRectCol(S(502), S(y1), S(636), S(y2), i > 44 ? kColStripHot : kColStrip);
    }
}

// 240-point audio waveform polyline at x=70 (left) / x=570 (right),
// y = 2*i, i = 0..239 (sub_497ECC line calls inside sub_7EEAD8).
void PaintSideWave() {
    if (g_state->current < 0) return;
    GfxPoint pts[240];
    for (int side = 0; side < 2; ++side) {
        float base = side == 0 ? 70.0f : 570.0f;
        const float* wave = side == 0 ? g_state->spectrum.waveL
                                      : g_state->spectrum.waveR;
        for (int i = 0; i < 240; ++i) {
            float dx = wave[i];
            pts[i].x = (std::int16_t)S((int)(base + dx));
            pts[i].y = (std::int16_t)S(2 * i);
        }
        gfx_polyline(pts, 240, kColWave, 2);
    }
}

// Panel frame: four lines at x=320-t1-1 / 320+t1, y=240-t2 / 240+t2 where
// t1 = clamp(4*n32,0,128)/128*177 and t2 = clamp(4*n32-128,0,128)/128*236.
// n32 advances once per 60 fps frame; fully expanded after 64 frames.
void PaintFrame() {
    int c = (int)g_state->tick;
    if (c > 64) c = 64;
    int n1 = c * 4;
    if (n1 > 128) n1 = 128;
    int n2 = c * 4 - 128;
    if (n2 < 0) n2 = 0;
    if (n2 > 128) n2 = 128;
    int t1 = (int)((float)n1 / 128.0f * 177.0f);
    int t2 = (int)((float)n2 / 128.0f * 236.0f);
    int xl = 320 - t1 - 1, xr = 320 + t1;
    int yt = 240 - t2, yb = 240 + t2;
    VLine(S(yt), S(yb), S(xl), kColFrame, 2);
    VLine(S(yt), S(yb), S(xr), kColFrame, 2);
    HLine(S(xl), S(xr), S(yt), kColFrame, 2);
    HLine(S(xl), S(xr), S(yb), kColFrame, 2);
}

void PaintSpectrum(const layout::SpecGeom& g) {
    const int t = (int)g_state->tick;
    // entrance fades (n32-keyed alphas in the original):
    //   bg quad   clamp(3n32-128,0,128)   grid bands clamp(12n32-512,0,192)
    //   axis      clamp(4n32-256,0,255)   labels     clamp(20n32-800,0,255)
    float bgA = ClampI(3 * t - 128, 0, 128) / 128.0f;
    float seamA = ClampI(12 * t - 512, 0, 192) / 192.0f;
    int axisA = ClampI(4 * t - 256, 0, 255);
    int labelA = ClampI(20 * t - 800, 0, 255);

    COLORREF panel = LerpCol(kColOuter, kColPanel, bgA);
    FillRectCol(S(g.l), S(g.t), S(g.r), S(g.axisY), panel);

    // band seams: 1px columns tinted toward the theme seam color
    for (int i = 0; i < 8; ++i) {
        COLORREF c = LerpCol(panel, Th().seam, seamA);
        VLine(S(g.t), S(g.axisY), S(kSeamX[i]), c, max(1, S(1)));
    }

    // 43 columns: current level (tex 88) from the compressed level, decay
    // tail (tex 87) from the OLD smoothed state down to level/4.
    const int kBars = g.bars;
    for (int k = 0; k < kBars; ++k) {
        int level = (int)g_state->spectrum.barsLevel[k];
        int old = (int)g_state->spectrum.barsPrev[k];
        int x1 = S(g.l + g.pitch * k);
        int x2 = S(g.l + g.pitch * k + g.barW);
        if (level > 3) {
            int segs = (level >> 2) + 1;
            for (int m = 0; m < segs; ++m) {
                int y2 = g.barBase - 3 * m;
                int y1 = y2 - kSegH;
                FillRectCol(x1, S(y1), x2, S(y2), kColBar);
            }
        }
        if (old > 3 && (old >> 2) > level / 4) {
            int segs = (old >> 2) - level / 4 + 1;
            for (int m = 0; m < segs; ++m) {
                int mm = level / 4 + m;
                int y2 = g.barBase - 3 * mm;
                int y1 = y2 - kSegH;
                FillRectCol(x1, S(y1), x2, S(y2), kColBarLit);
            }
        }
    }

    // frequency axis: solid vector cyan line + chunky pixel-font labels
    // from the atlas orange row (palette 4), matching the original weight.
    if (axisA > 0) {
        COLORREF fc = LerpCol(panel, Th().frame, axisA / 255.0f);
        FillRectCol(S(g.l), S(g.axisY), S(g.r), S(g.axisY + 2), fc);
    }
    if (labelA > 0) {
        for (int i = 0; i < 8; ++i) {
            int len = (int)strlen(kFreqLabel[i]);
            DrawCells(kFreqLabel[i], kSeamX[i] - 4 * len + 4, g.axisY + 4, 4);
        }
    }
}

void PaintTitle() {
    // Blit the original 418x56 title artwork at half size. The run-vector path
    // below only kept two flat colors and lost the outline/shadow layers the
    // original wide, heavy glyphs rely on; the bitmap restores that 1:1 look.
    // Nearest-neighbour scaling is handled by each gfx backend (COLORONCOLOR
    // on GDI, imageSmoothingEnabled=false on Canvas).
    gfx_blit(kGfxImageBase + g_state->theme, 0, 0, 418, 56, 152, 18, 209, 28);
}

// Selection cursor: 56x16 cell.  Amber margins around the glyph band (the
// band itself is the atlas row drawn by PaintGrid).  Position jumps
// instantly; the original 5-frame pulse brightens the margins on 2..4.
void PaintSelection(const layout::TracksGeom& g, int col, int row) {
    int x = g.gridX - 12 + g.colW * col;
    int y = g.gridY - 8 + g.rowH * row;
    bool pulse = (g_state->tick % 5) >= 2;
    COLORREF base = pulse ? LerpCol(kColSelBase, RGB(255, 255, 255), 0.12f) : kColSelBase;
    COLORREF edge = pulse ? LerpCol(kColSelEdge, RGB(255, 255, 255), 0.12f) : kColSelEdge;
    COLORREF foot = pulse ? LerpCol(kColSelFoot, RGB(255, 255, 255), 0.12f) : kColSelFoot;
    FillRectCol(S(x), S(y), S(x + g.colW), S(y + g.rowH), base);
    // cream glyph band: ONE full-width horizontal strip across the cell
    FillRectCol(S(x), S(y + 3), S(x + g.colW), S(y + 14), Th().selBand);
    FillRectCol(S(x), S(y), S(x + g.colW), S(y + 1), edge);
    FillRectCol(S(x), S(y + 14), S(x + g.colW), S(y + 15), foot);
}

// The flashing white light is a centered translucent white bar whose height
// pulses 0,2,4,6,8 over the original 5-frame cycle = the selection blink.
void PaintSelectionStripe(const layout::TracksGeom& g) {
    int col, row;
    if (g_state->selected == kExitIndex) { col = 0; row = -1; }
    else { col = g_state->selected / g.rows; row = g_state->selected % g.rows; }
    int x = g.gridX - 12 + g.colW * col;
    int y = g.gridY - 8 + g.rowH * row;
    int h = 2 * (int)(g_state->tick % 5);
    if (h <= 0) return;
    int cy = y + 8;
    AlphaFill(S(x + 1), S(cy - h / 2), S(x + g.colW - 1), S(cy - h / 2 + h),
              Th().selStripe, 0x60);
}

void PaintExit(const layout::TracksGeom& g) {
    bool selected = g_state->selected == kExitIndex;
    if (selected) PaintSelection(g, 0, -1);
    DrawCells("EXIT", g.exitX, g.exitY, selected ? 1 : 0);
}

void PaintGridLabel(int idx, int x, int y, bool selected) {
    char label[8];
    snprintf(label, sizeof(label), "TR:%02d", idx);
    DrawCells(label, x, y, selected ? 1 : 0);
}

void PaintGrid(const layout::TracksGeom& g) {
    for (int col = 0; col < g.cols; ++col) {
        for (int row = 0; row < g.rows; ++row) {
            int idx = col * g.rows + row;
            if (idx >= kTrackCount) continue;
            if (g_state->selected == idx) PaintSelection(g, col, row);
            int x = g.gridX + g.colW * col;
            int y = g.gridY + g.rowH * row;
            PaintGridLabel(idx, x, y, g_state->selected == idx);
        }
    }
}

void PaintInfo(const layout::TracksGeom& g, SceneHost* host) {
    const SceneState& st = *g_state;
    const int x = g.infoX, y1 = g.infoY1, y2 = g.infoY2;

    std::string line1;
    if (st.loading) {
        line1 = "<LOADING>";
    } else if (st.current == kTrackCount) {
        line1 = "NOW PLAYING : ";
        line1 += st.localTitle[0] ? std::string(st.localTitle) : std::string("LOCAL FILE");
    } else if (st.current < 0) {
        line1 = "<SILENCE>";
    } else {
        char buf[32];
        snprintf(buf, sizeof(buf), "NOW PLAYING : TRACK %02d", st.current);
        line1 = buf;
    }

    char timeBuf[64];
    int secs = (int)st.seconds;
    if (secs < 0) secs = 0;
    snprintf(timeBuf, sizeof(timeBuf), "PLAY TIME   : %02dSEC", secs);
    std::string line2 = timeBuf;

    // Line 1 includes the "NOW PLAYING" prefix, so a long local title used
    // to make the underline wider than the content box; truncate like the
    // original desktop code.
    const int lineLeft = x - 4, lineRight = 486;
    const int line1Capacity = max(0, (lineRight - lineLeft) / 8);
    if ((int)line1.size() > line1Capacity) {
        if (line1Capacity >= 3) line1.replace(line1Capacity - 3, std::string::npos, "...");
        else line1.resize(line1Capacity);
    }

    // Non-ASCII local titles are the shell's to draw (the desktop uses its
    // Fusion Pixel font); pure-ASCII titles use the shared cell atlas.
    bool hostDrew = false;
    if (host && st.current == kTrackCount && st.localFile)
        hostDrew = host->drawLocalFileTitle();

    // no-underline cells; the underline is ONE continuous horizontal strip
    // per line drawn AFTER both lines (line 2's opaque cells would
    // otherwise overwrite line 1's baked underline).
    if (!hostDrew) DrawCells(line1.c_str(), x, y1, 3);
    DrawCells(line2.c_str(), x, y2, 3);
    int w1 = (int)line1.size() * 8;
    HLine(S(lineLeft), S(lineLeft) + S(w1), S(y1 + 4), Th().underline, max(1, S(1)));
    HLine(S(lineLeft), S(lineLeft) + S(19 * 8), S(y2 + 4), Th().underline, max(1, S(1)));
}

void PaintHints(const layout::TracksGeom& g) {
    // 0x7EF2DE: (dword_86690C/24)%2 picks the half-alpha blink variant.
    int palette = ((g_state->tick / 24) % 2) ? 2 : 0;
    DrawCells("<MAINSHOT: LOOPPLAY> <SUBWEAPON: STOP>", g.hintX, g.hintY, palette);
}

// ---- side wings ----------------------------------------------------------
void PaintWings(int style) {
    if (style == 1) { PaintLevelMeter(); PaintSideWave(); return; }
    const Theme& t = Th();
    for (int side = 0; side < 2; ++side) {
        int base = side ? 570 : 70;
        const float* wave = side ? g_state->spectrum.waveR : g_state->spectrum.waveL;
        if (style == 3) {
            GfxPoint upper[220], lower[220];
            for (int j = 0; j < 220; ++j) {
                float v = wave[j], lag = wave[(j + 12) % 220];
                int center = base + int(.58f * v), w = 3 + int(.37f * std::abs(v - lag));
                int y = 26 + 2 * j;
                upper[j] = { (std::int16_t)S(center - w), (std::int16_t)S(y) };
                lower[j] = { (std::int16_t)S(center + w), (std::int16_t)S(y) };
                if (j % 5 == 0)
                    FillRectCol(S(center - w), S(y), S(center + w + 1), S(y + 1), t.strip);
            }
            gfx_polyline(upper, 220, t.wave, 1);
            gfx_polyline(lower, 220, t.bar, 1);
        } else {
            const int span = 2 + int(61 * std::clamp(g_state->wing[side], 0.f, 1.f));
            for (int row = 0; row < 44; ++row) {
                float peak = 0;
                for (int j = 0; j < 5; ++j) peak = std::max(peak, std::abs(wave[row * 5 + j]));
                // Linear (not sqrt) mapping keeps the bar length responsive:
                // sqrt lifted quiet rows to near-full width and killed the bounce.
                const float contour = std::clamp(.16f + .84f * (peak / 110.f), 0.f, 1.f);
                int width = 2 + int((span - 2) * contour), y = 27 + row * 10;
                if (style == 2) {
                    const float heat = std::clamp(contour * .62f + g_state->wing[side] * .38f, 0.f, 1.f);
                    // Alternate rows were mixed *from* the cool wave colour, so the
                    // "red" row could never exceed 42% red and always looked dusty.
                    const COLORREF ink = row & 1
                        ? LerpCol(t.underline, t.wave, .16f - .12f * heat)
                        : LerpCol(t.bar, RGB(255, 255, 255), .10f + .22f * heat);
                    FillRectCol(S(base - width), S(y), S(base + width), S(y + 2), ink);
                    const int length = row % 5 == 0 ? 5 : 2;
                    const COLORREF tick = row % 5 == 0
                        ? t.bar
                        : LerpCol(t.underline, t.hint, .30f);
                    for (int edge : { -64, 63 }) {
                        const int x = base + edge;
                        FillRectCol(S(x), S(y), S(x + (edge < 0 ? length : -length)), S(y + 1), tick);
                    }
                } else {
                    for (int col = 0; col < 12; ++col) {
                        int x = base + (side ? -1 : 1) * (col * 10 - 55);
                        bool lit = col * 5 + 2 < width;
                        FillRectCol(S(x - 2), S(y - 1), S(x + (lit ? 3 : 0)), S(y + (lit ? 3 : 1)),
                                    lit ? (col > 8 ? t.bar : t.wave) : LerpCol(t.outer, t.wave, .30f));
                    }
                }
            }
        }
        DrawCells(side ? "CH : L" : "CH : R", base - 20, 13, 3);
    }
}

// ---- composition: full-frame field ---------------------------------------
// No panel frame and no side wings: the spectrum becomes a symmetric
// full-width field with the title floating in a corner.  TAB raises the
// original TR grid on top so selection keeps the original coordinates.
void PaintFieldLayout() {
    const Theme& t = Th();
    FillRectCol(0, 0, S(kLogicalW), S(kLogicalH),
                LerpCol(t.outer, t.panel, 0.55f));

    const int baseY = 340;
    FillRectCol(0, S(baseY), S(kLogicalW), S(baseY + 2),
                LerpCol(t.frame, t.outer, 0.42f));

    const int cols = memoirs::kSpectrumBands;
    const int pitch = 14, barW = 12;
    const int x0 = (kLogicalW - cols * pitch) / 2;
    for (int k = 0; k < cols; ++k) {
        const float lv = std::clamp(g_state->spectrum.barsLevel[k] / 96.0f, 0.f, 1.f);
        const int x1 = x0 + k * pitch, x2 = x1 + barW;
        const int up = int(lv * 258);
        if (up > 1) {
            FillRectCol(S(x1), S(baseY - up), S(x2), S(baseY), t.bar);
            FillRectCol(S(x1), S(baseY - up), S(x2), S(baseY - up + 3),
                        LerpCol(t.bar, RGB(255, 255, 255), 0.45f));
        }
        const int down = int(lv * 46);
        if (down > 1) {
            FillRectCol(S(x1), S(baseY + 2), S(x2), S(baseY + 2 + down),
                        LerpCol(t.bar, t.outer, 0.30f));
        }
    }

    // frequency ticks and labels sit below the mirrored tail
    for (int i = 0; i < 8; ++i) {
        const int x = 48 + i * 78;
        FillRectCol(S(x), S(baseY + 4), S(x + 1), S(baseY + 50),
                    LerpCol(t.frame, t.outer, 0.55f));
        const int len = (int)strlen(kFreqLabel[i]);
        DrawCells(kFreqLabel[i], x - 4 * len + 4, baseY + 52, 4);
    }

    // Channel traces: left channel above the baseline, right below, so the
    // two never paint over each other.
    GfxPoint pts[120];
    for (int side = 0; side < 2; ++side) {
        const float* wv = side ? g_state->spectrum.waveR : g_state->spectrum.waveL;
        for (int i = 0; i < 120; ++i) {
            const float v = std::clamp(wv[i * 2] / 64.0f, -1.f, 1.f);
            pts[i].x = (std::int16_t)S(i * kLogicalW / 119);
            pts[i].y = (std::int16_t)S(baseY + (side ? 2 + int(v * 62) : -2 - int(v * 62)));
        }
        gfx_polyline(pts, 120, side ? LerpCol(t.wave, t.outer, 0.38f) : t.wave, 1);
    }

    DrawCells("SOUND MEMOIRS", 24, 24, 0);
    {
        const char* themeName = t.name;
        const int w = (int)strlen(themeName) * 8;
        DrawCells(themeName, 616 - w, 24, 2);
    }

    std::string now;
    if (g_state->current == kTrackCount) now = "NOW PLAYING : LOCAL FILE";
    else if (g_state->current >= 0) {
        char b[48];
        snprintf(b, sizeof(b), "NOW PLAYING : TRACK %02d", g_state->current);
        now = b;
    } else now = "<SILENCE>";
    DrawCells(now.c_str(), 24, 452, 3);
    {
        char tb[32];
        int secs = max(0, (int)g_state->seconds);
        snprintf(tb, sizeof(tb), "TIME : %02dSEC", secs);
        DrawCells(tb, 616 - (int)strlen(tb) * 8, 452, 3);
    }
    DrawCells("TAB GRID / F9 LAYOUT", 24, 466, 2);
    {
        char sb[16];
        snprintf(sb, sizeof(sb), "SEL:TR:%02d", std::max(0, g_state->selected));
        DrawCells(sb, 616 - (int)strlen(sb) * 8, 466, 2);
    }

    if (g_state->showGridOverlay) {
        AlphaFill(0, 0, S(kLogicalW), S(kLogicalH), RGB(10, 12, 16), 0x8C);
        FillRectCol(S(148), S(266), S(492), S(kLogicalH), t.outer);
        const layout::TracksGeom& g = layout::tracks(g_state->layoutMode);
        PaintExit(g);
        PaintGrid(g);
        PaintSelectionStripe(g);
        DrawCells("TAB CLOSE", 24, 466, 2);
    }
}

// ---- composition: vertical three-column rack -----------------------------
void PaintStackLayout() {
    const Theme& t = Th();
    FillRectCol(0, 0, S(kLogicalW), S(kLogicalH), LerpCol(t.outer, t.panel, 0.35f));

    FillRectCol(0, 0, S(kLogicalW), S(24), t.panel);
    FillRectCol(0, S(23), S(kLogicalW), S(24), t.frame);
    DrawCells("SOUND MEMOIRS", 16, 8, 0);
    { const char* nm = t.name; DrawCells(nm, 616 - (int)strlen(nm) * 8, 8, 2); }

    // left: two channel meters, bottom-up
    for (int side = 0; side < 2; ++side) {
        const int x1 = side ? 44 : 18, x2 = side ? 56 : 30;
        const int n = std::clamp(int((side ? g_state->spectrum.envR : g_state->spectrum.envL) * 60.0f), 0, 60);
        for (int i = 0; i < n; ++i) {
            const int y2 = 440 - i * 6;
            FillRectCol(S(x1), S(y2 - 5), S(x2), S(y2),
                        i > 44 ? t.stripHot : t.strip);
        }
        DrawCells(side ? "R" : "L", x1 + 1, 446, 2);
    }

    // centre display
    const int cl = 96, ct = 32, cr = 400, cb = 440;
    FillRectCol(S(cl), S(ct), S(cr), S(cb), LerpCol(t.panel, t.outer, 0.30f));
    FillRectCol(S(cl), S(ct), S(cr), S(ct + 1), t.frame);
    FillRectCol(S(cl), S(cb - 1), S(cr), S(cb), t.frame);
    FillRectCol(S(cl), S(ct), S(cl + 1), S(cb), t.frame);
    FillRectCol(S(cr - 1), S(ct), S(cr), S(cb), t.frame);

    const int baseY = 330;
    const int cols = memoirs::kSpectrumBands;
    const int pitch = 6, barW = 5;
    const int x0 = cl + 1 + ((cr - cl - 2) - cols * pitch) / 2;
    for (int k = 0; k < cols; ++k) {
        const float lv = std::clamp(g_state->spectrum.barsLevel[k] / 96.0f, 0.f, 1.f);
        const int x1 = x0 + k * pitch, x2 = x1 + barW;
        const int up = int(lv * 238);
        if (up > 1) {
            FillRectCol(S(x1), S(baseY - up), S(x2), S(baseY), t.bar);
            FillRectCol(S(x1), S(baseY - up), S(x2), S(baseY - up + 2),
                        LerpCol(t.bar, RGB(255, 255, 255), 0.45f));
        }
        const int down = int(lv * 22);
        if (down > 1)
            FillRectCol(S(x1), S(baseY + 1), S(x2), S(baseY + 1 + down),
                        LerpCol(t.bar, t.outer, 0.30f));
    }

    // lower half: the two channel traces
    GfxPoint pts[120];
    for (int side = 0; side < 2; ++side) {
        const float* wv = side ? g_state->spectrum.waveR : g_state->spectrum.waveL;
        const int cy = side ? 416 : 392;
        for (int i = 0; i < 120; ++i) {
            const float v = std::clamp(wv[i * 2] / 64.0f, -1.f, 1.f);
            pts[i].x = (std::int16_t)S(cl + 4 + i * (cr - cl - 8) / 119);
            pts[i].y = (std::int16_t)S(cy + int(v * 18));
        }
        gfx_polyline(pts, 120, side ? LerpCol(t.wave, t.bar, 0.45f) : t.wave, 1);
    }
    DrawCells("FIELD / 43 BAND", cl + 4, ct + 4, 2);

    // right: full track list, two columns of 23
    const int listX = 408, listY = 40, colW = 108, rowH = 17;
    for (int i = 0; i < kTrackCount; ++i) {
        const int col = i / 23, row = i % 23;
        const int x = listX + col * colW, y = listY + row * rowH;
        const bool sel = g_state->selected == i;
        if (sel) {
            FillRectCol(S(x), S(y), S(x + colW - 6), S(y + rowH - 2),
                        LerpCol(t.selBand, t.panel, 0.30f));
            FillRectCol(S(x), S(y), S(x + 3), S(y + rowH - 2), t.bar);
        }
        char b[8];
        snprintf(b, sizeof(b), "TR:%02d", i);
        DrawCells(b, x + 8, y + 3, sel ? 0 : 2);
    }

    // bottom rail: seek + time + hint
    FillRectCol(S(cl), S(452), S(cr), S(454), t.seam);
    const double dur = g_state->current == kTrackCount ? g_state->duration : 0.0;
    const double pos = dur > 0 ? g_state->seconds : 0.0;
    const int end = cl + int((cr - cl) * std::clamp(pos / (dur > 0 ? dur : 1.0), 0.0, 1.0));
    if (end > cl) FillRectCol(S(cl), S(452), S(end), S(454), t.bar);
    {
        char tb[32];
        snprintf(tb, sizeof(tb), "TIME : %02dSEC", std::max(0, int(pos)));
        DrawCells(tb, 632 - (int)strlen(tb) * 8, 460, 3);
    }
    DrawCells("F7 VISUAL / F8 WING / F9 LAYOUT", cl, 460, 2);
}

// ---- composition: circular instrument dial -------------------------------
void PaintRingLayout() {
    const Theme& t = Th();
    const float kPi = 3.14159265f;
    const int cx = 320, cy = 236;
    FillRectCol(0, 0, S(kLogicalW), S(kLogicalH), LerpCol(t.outer, t.panel, 0.42f));

    // concentric guide rings
    auto ring = [&](int radius, COLORREF c, int thick) {
        const int segs = 120;
        for (int i = 0; i < segs; ++i) {
            const float a = 2.f * kPi * i / segs;
            const int x = cx + int(radius * cosf(a));
            const int y = cy + int(radius * sinf(a));
            FillRectCol(S(x), S(y), S(x + thick), S(y + thick), c);
        }
    };
    ring(96, LerpCol(t.frame, t.outer, 0.35f), 1);
    ring(126, LerpCol(t.frame, t.outer, 0.20f), 1);
    ring(196, LerpCol(t.seam, t.outer, 0.30f), 1);

    // 43 radial band blocks, starting at 12 o'clock
    const int bands = memoirs::kSpectrumBands;
    for (int k = 0; k < bands; ++k) {
        const float lv = std::clamp(g_state->spectrum.barsLevel[k] / 96.0f, 0.f, 1.f);
        const int blocks = 2 + int(lv * 10.f);
        const float a = -kPi / 2.f + 2.f * kPi * k / bands;
        for (int b = 0; b < blocks; ++b) {
            const int r = 132 + b * 6;
            const int x = cx + int(r * cosf(a)) - 1;
            const int y = cy + int(r * sinf(a)) - 1;
            const COLORREF c = (b == blocks - 1) ? t.bar
                              : LerpCol(t.wave, t.bar, 0.28f);
            FillRectCol(S(x), S(y), S(x + 3), S(y + 3), c);
        }
    }

    // centre: phase diamond with up to three decayed traces
    {
        GfxPoint frame[] = { {(std::int16_t)S(cx), (std::int16_t)S(cy - 38)},
                             {(std::int16_t)S(cx + 46), (std::int16_t)S(cy)},
                             {(std::int16_t)S(cx), (std::int16_t)S(cy + 38)},
                             {(std::int16_t)S(cx - 46), (std::int16_t)S(cy)},
                             {(std::int16_t)S(cx), (std::int16_t)S(cy - 38)} };
        gfx_polyline(frame, 5, t.frame, 1);
        // XY plot of the two channels: the classic vector-display figure
        {
            GfxPoint pts2[120];
            for (int i = 0; i < 120; ++i) {
                const float l = std::clamp(g_state->spectrum.waveL[i * 2] / 64.f, -1.f, 1.f);
                const float r = std::clamp(g_state->spectrum.waveR[i * 2] / 64.f, -1.f, 1.f);
                pts2[i] = { (std::int16_t)S(cx + int(l * 34)), (std::int16_t)S(cy - int(r * 34)) };
            }
            gfx_polyline(pts2, 120, t.bar, 1);
        }
        FillRectCol(S(cx - 1), S(cy - 1), S(cx + 2), S(cy + 2), t.bar);
    }

    // 46 track ticks on the outside
    for (int i = 0; i < kTrackCount; ++i) {
        const float a = -kPi / 2.f + 2.f * kPi * i / kTrackCount;
        const int x = cx + int(206 * cosf(a));
        const int y = cy + int(206 * sinf(a));
        const bool sel = g_state->selected == i;
        const int halfW = sel ? 3 : 2, halfH = sel ? 6 : 3;
        FillRectCol(S(x - halfW), S(y - halfH), S(x + halfW), S(y + halfH),
                    sel ? t.bar : LerpCol(t.seam, t.ink, 0.55f));
    }

    DrawCells("SOUND MEMOIRS", 152, 14, 0);
    { const char* nm = t.name; DrawCells(nm, 632 - (int)strlen(nm) * 8, 14, 2); }

    {
        char b[16];
        snprintf(b, sizeof(b), "TR:%02d", std::max(0, g_state->selected));
        DrawCells(b, cx - (int)strlen(b) * 4, 446, g_state->selected < kTrackCount ? 0 : 2);
    }
    DrawCells("F7 VISUAL / F9 LAYOUT", 196, 462, 2);
}

// ---- composition: crosshair / vector display -----------------------------
void PaintCrossLayout() {
    const Theme& t = Th();
    FillRectCol(0, 0, S(kLogicalW), S(kLogicalH), LerpCol(t.outer, t.panel, 0.48f));

    const int cx = 320, cy = 240;
    const COLORREF axis = LerpCol(t.frame, t.outer, 0.66f);
    FillRectCol(S(cx - 1), S(24), S(cx + 1), S(456), axis);
    FillRectCol(S(24), S(cy - 1), S(616), S(cy + 1), axis);

    for (int y = 40; y < 456; y += 40) {
        FillRectCol(S(cx - 6), S(y), S(cx - 1), S(y + 1), t.seam);
        FillRectCol(S(cx + 1), S(y), S(cx + 6), S(y + 1), t.seam);
    }
    for (int x = 40; x < 616; x += 40) {
        FillRectCol(S(x), S(cy - 6), S(x + 1), S(cy - 1), t.seam);
        FillRectCol(S(x), S(cy + 1), S(x + 1), S(cy + 6), t.seam);
    }

    // channel traces ride above (left) and below (right) the horizontal axis
    GfxPoint pts[120];
    for (int side = 0; side < 2; ++side) {
        const float* wv = side ? g_state->spectrum.waveR : g_state->spectrum.waveL;
        const int sway = side ? 26 : -26;
        const int x0 = side ? 392 : 24;
        for (int i = 0; i < 120; ++i) {
            const float v = std::clamp(wv[i * 2] / 64.0f, -1.f, 1.f);
            pts[i].x = (std::int16_t)S(x0 + i * 224 / 119);
            pts[i].y = (std::int16_t)S(cy + sway + int(v * 16));
        }
        gfx_polyline(pts, 120, side ? LerpCol(t.wave, t.bar, 0.45f) : t.wave, 1);
    }

    // centre window: phase diamond over up to three decayed traces
    {
        GfxPoint frame[] = { {(std::int16_t)S(cx), (std::int16_t)S(cy - 40)},
                             {(std::int16_t)S(cx + 48), (std::int16_t)S(cy)},
                             {(std::int16_t)S(cx), (std::int16_t)S(cy + 40)},
                             {(std::int16_t)S(cx - 48), (std::int16_t)S(cy)},
                             {(std::int16_t)S(cx), (std::int16_t)S(cy - 40)} };
        gfx_polyline(frame, 5, t.frame, 1);
        // XY plot of the two channels inside the diamond
        {
            GfxPoint pts2[120];
            for (int i = 0; i < 120; ++i) {
                const float l = std::clamp(g_state->spectrum.waveL[i * 2] / 64.f, -1.f, 1.f);
                const float r = std::clamp(g_state->spectrum.waveR[i * 2] / 64.f, -1.f, 1.f);
                pts2[i] = { (std::int16_t)S(cx + int(l * 40)), (std::int16_t)S(cy - int(r * 40)) };
            }
            gfx_polyline(pts2, 120, t.bar, 1);
        }
    }

    // progress runs down the lower axis
    {
        const double dur = g_state->current == kTrackCount ? g_state->duration : 0.0;
        const double pos = dur > 0 ? g_state->seconds : 0.0;
        const int len = int(192.0 * std::clamp(pos / (dur > 0 ? dur : 1.0), 0.0, 1.0));
        FillRectCol(S(cx - 3), S(264), S(cx + 3), S(264 + len), t.bar);
    }

    DrawCells("SOUND MEMOIRS", 24, 20, 0);
    { const char* nm = t.name; DrawCells(nm, 616 - (int)strlen(nm) * 8, 20, 2); }
    DrawCells("EXIT", 24, 300, g_state->selected == kExitIndex ? 1 : 2);

    {
        char b[24];
        snprintf(b, sizeof(b), "TR:%02d", std::max(0, g_state->selected));
        DrawCells(b, cx - (int)strlen(b) * 4, 466, 0);
    }
    DrawCells("TAB GRID / F9 LAYOUT", 24, 466, 2);

    if (g_state->showGridOverlay) {
        AlphaFill(0, 0, S(kLogicalW), S(kLogicalH), RGB(10, 12, 16), 0x8C);
        FillRectCol(S(148), S(266), S(492), S(kLogicalH), t.outer);
        const layout::TracksGeom& g = layout::tracks(g_state->layoutMode);
        PaintExit(g);
        PaintGrid(g);
        PaintSelectionStripe(g);
        DrawCells("TAB CLOSE", 24, 466, 2);
    }
}

// ---- channel rail ---------------------------------------------------------
// The page rail stays visible while the user is around and fades out after
// 150 idle frames. While fading the pre-rail background is blended back
// over the fresh rail (gfx_capture_*: an exact snapshot on GDI, an
// equivalent solid-color blend on replay backends).
void channelRail(SceneHost* host) {
    const int left = S(railX - 4), top = S(272);
    const int right = S(railX + 3 * channelWidth + 4), bottom = S(288);
    float opacity = 1.0f;
    if (g_state->hoverChannel >= 0) {
        opacity = 1.0f;
    } else {
        const unsigned idle = (unsigned)(g_state->tick) - (unsigned)(g_state->railActivityTick);
        if (idle <= kRailIdleDelayFrames) opacity = 1.0f;
        else if (idle >= kRailIdleDelayFrames + kRailFadeFrames) opacity = 0.0f;
        else opacity = 1.0f - float(idle - kRailIdleDelayFrames) / float(kRailFadeFrames);
    }
    const bool fading = opacity < 0.999f;
    if (fading) gfx_capture_begin(left, top, right, bottom);
    static const char* labels[] = { "TR", "LIB", "SYS" };
    for (int i = 0; i < 3; ++i) {
        int x = railX + i * channelWidth;
        bool active = i == g_state->page;
        if (g_state->page == 2 && host) {
            host->drawChannelRailText(i, active);
        } else {
            DrawCells(labels[i], x + 8, 280, active ? 0 : 2);
        }
        if (i == g_state->hoverChannel && !active)
            FillRectCol(S(x + 4), S(286), S(x + 30), S(287), Th().underline);
        if (active) {
            FillRectCol(S(x), S(276), S(x + 2), S(284), Th().bar);
            FillRectCol(S(x), S(285), S(x + 5), S(286), Th().bar);
        }
    }
    if (fading) {
        const int cover = ClampI((int)((1.0f - opacity) * 255.0f + 0.5f), 0, 255);
        gfx_capture_blend(kColOuter, cover);
    }
}

} // namespace

void sceneAdvance(SceneState& s, stage::Memory& memory, int id, double seconds,
                  bool playing, bool paused) {
    memory.advance(s.spectrum, id, seconds, playing, paused);
    s.wing[0] = memory.wing[0];
    s.wing[1] = memory.wing[1];
}

static void PaintSystemPanel() {
    const Theme& t = Th();
    const COLORREF ink = t.panel, frame = t.frame, accent = t.bar;
    const COLORREF helpInk = t.ink, buttonInk = t.outer;
    const COLORREF stripBase = LerpCol(t.panel, t.strip, 0.16f);
    const COLORREF stripEdge = LerpCol(t.panel, t.frame, 0.45f);
    const COLORREF stripShade = t.seam;
    const int L = 144, T = 249, R = 495, B = 475;
    panelBox(L, T, R, B, ink);
    panelBox(L, T, R, T + 1, frame);
    panelBox(L, T + 1, L + 1, B, frame);
    panelBox(R - 1, T + 1, R, B, frame);
    panelBox(L, B - 1, R, B, frame);
    // Three-language UI strings (ported from the desktop config_panel.cpp
    // Localize() tables so the browser SYS page honors the language setting).
    const int L3 = g_state->language % 3;
    static const char* kSysTitle[3] = { "SETTINGS", "系统设置", "設定" };
    static const char* kSysLabels[3][5] = {
        { "OUTPUT LEVEL", "PALETTE", "LANGUAGE", "LIB FONT", "COMPOSITION" },
        { "输出音量", "画面配色", "界面语言", "曲库字体", "画面布局" },
        { "出力音量", "配色", "言語", "ライブラリ書体", "画面構成" },
    };
    static const char* kSysHelp[3][5] = {
        { "Master output level", "Interface and signal palette",
          "Interface and help language", "Library track title typeface",
          "Screen composition" },
        { "主音量（最终输出）", "界面与信号配色",
          "界面与帮助语言", "曲库曲目名的显示字体", "画面布局的选择" },
        { "主音量（最終出力）", "画面と信号の配色",
          "表示とヘルプの言語", "音楽庫の曲名書体", "画面構成の選択" },
    };
    static const char* kLangNames[3] = { "ENGLISH", "简体中文", "日本語" };
    static const char* kDefaultLine[3] = { "Default: 80%", "默认值: 80%", "初期値: 80%" };
    textUtf8(kSysTitle[L3], 154, 249, 285, 271, helpInk);
    panelBox(154, 268, 355, 269, stripEdge);
    const char* const* labels = kSysLabels[L3];
    const char* const* helpText = kSysHelp[L3];
    const int active = g_state->settingRow, y0 = 283, pitch = 31;
    char valueBuf[32];
    for (int row = 0; row < 5; ++row) {
        const int y = y0 + pitch * row, top = y + 5, bottom = y + pitch - 4;
        const bool isActive = row == active;
        const COLORREF rowStrip = isActive ? LerpCol(stripBase, t.stripHot, 0.15f) : stripBase;
        if (isActive) { panelBox(154, top - 3, 486, top - 1, frame); panelBox(154, bottom, 486, bottom + 2, frame); }
        panelBox(154, top, 486, bottom, rowStrip);
        panelBox(154, top, 486, top + 1, stripEdge);
        panelBox(154, bottom - 1, 486, bottom, stripShade);
        panelBox(156, y + 10, 158, y + 24, accent);
        textUtf8(labels[row], 161, y + 7, 260, y + 26, t.ink);
        panelBox(161, y + 27, 248, y + 29, accent);
        const COLORREF btnEdge = isActive ? frame : LerpCol(frame, t.frame, 0.18f);
        panelBox(269, y + 7, 294, y + 26, buttonInk);
        panelBox(269, y + 7, 294, y + 8, btnEdge);
        panelBox(269, y + 25, 294, y + 26, stripShade);
        panelBox(462, y + 7, 485, y + 26, buttonInk);
        panelBox(462, y + 7, 485, y + 8, btnEdge);
        panelBox(462, y + 25, 485, y + 26, stripShade);
        const int mid = y + 16;
        panelBox(277, mid - 1, 286, mid + 1, t.ink); panelBox(281, mid - 4, 283, mid + 5, t.ink);
        panelBox(469, mid - 1, 478, mid + 1, t.ink); panelBox(473, mid - 4, 475, mid + 5, t.ink);
        if (row == 0) std::snprintf(valueBuf, sizeof(valueBuf), "%d%%", g_state->volume);
        else if (row == 1) std::snprintf(valueBuf, sizeof(valueBuf), "%s", Th().name);
        else if (row == 2) std::snprintf(valueBuf, sizeof(valueBuf), "%s", kLangNames[L3]);
        else if (row == 3) std::snprintf(valueBuf, sizeof(valueBuf), "%s", g_state->libFont ? "PIXEL" : "ORIGINAL");
        else { textCodePoints(g_state->layoutName, g_state->layoutNameLen, 303, y + 6, 454, y + 27, t.digit, GFX_ALIGN_RIGHT); continue; }
        textUtf8(valueBuf, 303, y + 6, 454, y + 27, t.digit, GFX_ALIGN_RIGHT);
        if (row == 0) {
            for (int seg = 0; seg < 20; ++seg) { const int x = 304 + seg * 7;
                panelBox(x, y + 27, x + 4, y + 29, seg * 5 < g_state->volume ? accent : LerpCol(rowStrip, ink, 0.38f)); }
        } else if (row == 1) {
            const COLORREF sw[4] = { frame, t.digit, t.bar, t.selEdge };
            for (int i = 0; i < 4; ++i) panelBox(418 + i * 9, y + 27, 425 + i * 9, y + 30, sw[i]);
        }
    }
    panelBox(154, 442, 486, 475, t.outer);
    panelBox(154, 442, 486, 443, frame);
    panelBox(157, 447, 160, 471, frame);
    textUtf8(helpText[active % 5], 167, 444, 481, 460, helpInk);
    textUtf8(kDefaultLine[L3], 167, 458, 481, 474, t.digit);
}

static void PaintArchivePage() {
    const Theme& t = Th();
    const bool local = g_state->current >= kTrackCount;
    // Three-language LIB strings (from the desktop archivePaint tr() calls).
    const int L3 = g_state->language % 3;
    static const char* kLibTitle[3] = { "LOCAL ARCHIVE", "本地曲库", "音楽庫" };
    static const char* kLibSearch[3] = { "Search tracks", "搜索曲名、艺人或专辑", "曲・アーティスト検索" };
    static const char* kLibAdd[3] = { "Add files", "添加音乐", "曲を追加" };
    static const char* kLibPause[3] = { "PAUSE", "暂停", "一時停止" };
    static const char* kLibPlay[3] = { "PLAY", "播放", "再生" };
    static const char* kLibStop[3] = { "Stop", "停止", "停止" };
    // Status line: HOLD / ON AIR, now-playing title, time / duration.
    textUtf8(g_state->libPaused ? "HOLD" : "ON AIR", 154, 249, 201, 264, t.digit);
    char now[48];
    if (g_state->current >= 0 && g_state->current < kTrackCount)
        std::snprintf(now, sizeof(now), "TR / %02d", g_state->current);
    else std::snprintf(now, sizeof(now), "LOCAL FILE");
    textUtf8(now, 204, 249, 383, 264, t.ink);
    char timeBuf[48];
    const int pos = (int)(g_state->libPositionMs / 1000.0);
    const int dur = (int)g_state->duration;
    std::snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d/%02d:%02d",
                  pos / 60, pos % 60, dur / 60, dur % 60);
    textUtf8(timeBuf, 384, 249, 486, 264, t.digit, GFX_ALIGN_RIGHT);
    // seek rail
    panelBox(154, 266, 486, 268, t.seam);
    if (dur > 0) {
        const int end = 154 + int((486 - 154) * (std::min(1.0, std::max(0.0, (double)pos / dur))) + .5);
        if (end > 154) panelBox(154, 266, end, 268, t.bar);
    }
    textUtf8(kLibTitle[L3], 154, 271, 330, 289, t.ink);
    // search + add row
    panelBox(154, 293, 405, 309, t.strip);
    if (g_state->searchTextLen > 0)
        textCodePoints(g_state->searchText, g_state->searchTextLen, 159, 293, 400, 309, t.ink);
    else textUtf8(kLibSearch[L3], 159, 293, 400, 309, t.ink);
    textUtf8(kLibAdd[L3], 406, 293, 486, 309, t.digit, GFX_ALIGN_CENTER);
    // ruled slots
    for (int row = 0; row < 7; ++row) {
        const int y = 330 + row * 18;
        panelBox(154, y, 158, y + 1, t.frame);
        panelBox(160, y, 486, y + 1, t.strip);
    }
    // track rows (real file names from the shell's LIB pool)
    const int first = g_state->libScroll;
    const int total = g_libNameCount;
    if (total == 0) {
        // No local tracks: say so instead of showing fake placeholder rows.
        static const char* kLibEmpty[3] = {
            "Drop music here or press + to add files",
            "拖入音乐，或点 + 添加文件",
            "音楽をドロップ、または + で追加",
        };
        textUtf8(kLibEmpty[L3], 154, 349, 486, 375, t.ink, GFX_ALIGN_CENTER);
    }
    for (int row = 0; row < 7; ++row) {
        const int idx = first + row;
        const int y = 313 + row * 18;
        if (idx >= total) break;
        const bool selected = idx == g_state->libCursor;
        if (selected) panelBox(154, y + 2, 157, y + 15, t.selEdge);
        char num[8]; std::snprintf(num, sizeof(num), "%03d", idx + 1);
        textUtf8(num, 161, y, 188, y + 17, t.digit);
        textLib(g_libNames[idx], 192, y, 464, y + 17, selected ? t.digit : t.ink);
        if (idx == g_state->libCurrentRow) {
            for (int j = 0; j < 3; ++j) {
                const int h = 2 + (j == 1 ? (int)g_state->spectrum.barsLevel[3 + j * 4] / 10 : 0);
                panelBox(471 + j * 4, y + 14 - h, 473 + j * 4, y + 14, t.bar);
            }
        }
    }
    panelBox(154, 440, 486, 441, t.frame);
    static const char* orders[4] = { "ORDER", "ONE", "LOOP", "SHUFFLE" };
    textUtf8(orders[g_state->localOrder % 4], 154, 443, 212, 460, t.digit);
    textUtf8(g_state->libPlaying && !g_state->libPaused ? kLibPause[L3] : kLibPlay[L3], 213, 443, 277, 460, t.digit);
    textUtf8(kLibStop[L3], 278, 443, 331, 460, t.digit);
    textUtf8("<", 341, 443, 375, 460, t.digit, GFX_ALIGN_CENTER);
    {
        const int pages = total > 0 ? (total + 6) / 7 : 0;
        const int shown = pages > 0 ? (first / 7) + 1 : 0;
        char pageBuf[16];
        std::snprintf(pageBuf, sizeof(pageBuf), "%d / %d", shown, pages);
        textUtf8(pageBuf, 375, 443, 453, 460, t.ink, GFX_ALIGN_CENTER);
    }
    textUtf8(">", 453, 443, 486, 460, t.digit, GFX_ALIGN_CENTER);
}

void sceneRender(const SceneState& s, float scale, const stage::Memory& memory,
                 SceneHost* host) {
    g_state = &s;
    g_sceneScale = scale;
    gfx_reset();
    gfx_clear(kColOuter);

    const int layoutMode = s.layoutMode;
    if (s.page == 0) {  // Tracks page: full-page compositions
        if (layoutMode == layout::Field) { PaintFieldLayout(); return; }
        if (layoutMode == layout::Stack) { PaintStackLayout(); return; }
        if (layoutMode == layout::Ring) { PaintRingLayout(); return; }
        if (layoutMode == layout::Cross) { PaintCrossLayout(); return; }
    }

    const int mode = s.stageMode;
    PaintWings(s.wingMode ? s.wingMode : mode + 1);
    PaintFrame();
    if (!mode) {
        PaintSpectrum(layout::spec(s.layoutMode));
    } else {
        const Theme& t = Th();
        stage::paint({ t.panel, LerpCol(t.seam, t.wave, .48f), t.wave, t.bar, t.frame },
                     scale, mode, s.spectrum, memory);
        const char* marks[] = { "", "ORBIT : 01", "STRATA : 02", "PHASE : 03" };
        DrawCells(marks[mode], 152, 74, 3);
    }
    PaintTitle();

    if (s.page == 2) {          // System settings (shared painter)
        PaintSystemPanel();
    } else if (s.page == 1) {   // Local archive (shared painter)
        PaintArchivePage();
    } else {
        const layout::TracksGeom& g = layout::tracks(s.layoutMode);
        PaintInfo(g, host);
        PaintExit(g);
        PaintGrid(g);
        PaintSelectionStripe(g);
        PaintHints(g);
    }

    channelRail(host);
    if (s.hoverChannel >= 0) {
        FillRectCol(S(144), S(462), S(495), S(478), Th().outer);
        if (host) host->drawHoverHint(s.hoverChannel);
    }
    if (s.switchTick) {
        const unsigned age = (unsigned)s.tick - (unsigned)s.switchTick;
        if (age < 12) {
            const int y = 290 + int(age) * 14;
            FillRectCol(S(150), S(y), S(490), S(y + 1), Th().frame);
        }
    }
    // entrance: near-black tint (packed 0x100408) fading out over the first
    // 32 frames via fullscreen quad alpha clamp(8*n32,0,255).  The original
    // also has a late white flash (tex 290, mode 14) not reproduced here.
    const int t = (int)s.tick;
    if (t < 32)
        AlphaFill(0, 0, S(kLogicalW), S(kLogicalH), RGB(8, 4, 16), 255 - ClampI(8 * t, 0, 255));
}

// Shell entry point: replace the LIB list with `count` fixed-width ASCII
// records read from `pool` (count * kLibNameWidth bytes).
void sceneSetLocalNames(const char* pool, int count) {
    if (!pool || count <= 0) {
        g_libNameCount = 0;
        for (int i = 0; i < kLibNameMax; ++i) g_libNames[i][0] = 0;
        return;
    }
    if (count > kLibNameMax) count = kLibNameMax;
    g_libNameCount = count;
    for (int i = 0; i < kLibNameMax; ++i) {
        if (i < count) {
            const char* src = pool + (std::size_t)i * kLibNameWidth;
            std::snprintf(g_libNames[i], kLibNameWidth, "%s", src);
        } else {
            g_libNames[i][0] = 0;
        }
    }
}
