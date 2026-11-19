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
#include "stage.h"
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
const stage::Memory* g_mem = nullptr;   // custom-layout dynamics (peaks, helix)
static inline const Theme& Th() { return kThemes[g_state->theme]; }

// Remapped 0..1 level of band k for the CUSTOM layouts (never ORIGINAL).
static inline float Lv(int k) { return stage::bandLevel(g_state->spectrum.barsLevel[k]); }

// Playback progress 0..1. TR tracks loop forever, so their progress is the
// position inside one minute; local files use the real decoded duration.
static double PlayProgress() {
    if (g_state->current < 0) return 0.0;
    const double dur = g_state->current == kTrackCount
        ? g_state->duration : 60.0;
    if (dur <= 0.0) return 0.0;
    double f = g_state->seconds / dur;
    if (f > 1.0) f = std::fmod(f, 1.0);
    return std::clamp(f, 0.0, 1.0);
}

// Beat impact 0..1 (fast attack / 240 ms release, from the low bands).
static inline float Impact() { return g_mem ? g_mem->impact : 0.f; }

// Transport word for the custom layouts' status corners.
static const char* TransportWord() {
    if (g_state->loading) return "LOAD";
    if (g_state->current < 0) return "STOP";
    if (g_state->libPaused) return "PAUSE";
    return "PLAY";
}

// ---- LIB track-name pool ------------------------------------------------
// The shell (browser or desktop) pushes the filtered local-track names here
// so the archive page can list real file names. Fixed-width ASCII records:
// the browser writes count*40 bytes of NUL-terminated names at once.
constexpr int kLibNameMax = 64;
constexpr int kLibNameWidth = 96;   // UTF-8 bytes: ~32 CJK chars per name
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

// ---- custom-layout chassis helpers ----------------------------------------
// True when the active theme has a light panel (ORIGINAL / PAPER).
static inline bool LightTheme() {
    const Theme& t = Th();
    return GetRValue(t.panel) + GetGValue(t.panel) + GetBValue(t.panel) > 384;
}

// Depth fade for the custom layouts: far geometry dissolves toward the
// recessed panel, but never fully vanishes. Dark themes cap at 0.78; light
// themes cap at 0.58 and drift toward seam so distant lines keep a pale-ink
// print feel instead of washing out into chalk smudges.
static inline COLORREF DepthInk(COLORREF fg, float depth) {
    const Theme& t = Th();
    float d = std::clamp(depth, 0.f, 1.f);
    if (LightTheme()) {
        const COLORREF bg = LerpCol(t.panel, t.seam, 0.35f);
        return LerpCol(fg, bg, std::min(d, 0.58f));
    }
    return LerpCol(fg, t.panel, std::min(d, 0.78f));
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

// Recessed instrument window: pure panel fill inside a 1px frame border.
// Light themes get an inner hairline toward ink so the boundary stays legible
// even where the frame hue sits close to the panel value.
static void InsetPanel(int x, int y, int r, int b) {
    const Theme& t = Th();
    FillRectCol(S(x), S(y), S(r), S(b), t.panel);
    FillRectCol(S(x), S(y), S(r), S(y + 1), t.frame);
    FillRectCol(S(x), S(b - 1), S(r), S(b), t.frame);
    FillRectCol(S(x), S(y), S(x + 1), S(b), t.frame);
    FillRectCol(S(r - 1), S(y), S(r), S(b), t.frame);
    if (LightTheme()) {
        const COLORREF inner = LerpCol(t.frame, t.ink, 0.25f);
        FillRectCol(S(x + 1), S(y + 1), S(r - 1), S(y + 2), inner);
        FillRectCol(S(x + 1), S(b - 2), S(r - 1), S(b - 1), inner);
        FillRectCol(S(x + 1), S(y + 1), S(x + 2), S(b - 1), inner);
        FillRectCol(S(r - 2), S(y + 1), S(r - 1), S(b - 1), inner);
    }
}

// ---- Unicode text helpers (SYS / LIB shell pages) -------------------------
// Convert an ASCII/UTF-8 string to UTF-32 code points and emit a gfx_text cmd.
static void textUtf8(const char* text, int x, int y, int right, int bottom,
                     COLORREF color, std::uint8_t align = GFX_ALIGN_LEFT) {
    if (!text) return;
    std::uint32_t cps[128];
    int n = 0;
    for (const unsigned char* p = (const unsigned char*)text; *p && n < 128;) {
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
    // The 8x11 atlas only carries ASCII glyphs; a CJK file name has to use the
    // pixel-font path even when the ORIGINAL (atlas) lib font is selected.
    bool ascii = true;
    for (const unsigned char* p = (const unsigned char*)text; *p; ++p) {
        if (*p >= 0x80) { ascii = false; break; }
    }
    if (g_state->libFont == 0 && ascii) {
        (void)right; (void)bottom; (void)align;
        DrawCells(text, x + 4, y + 5, 0);
    } else {
        textUtf8(text, x, y, right, bottom, color, align);
    }
}

// ---- shared custom-layout chassis -----------------------------------------
// Every custom composition mounts into the same chassis: a solid header
// strip (y 0..28) carrying the machine plate and transport badge, and a
// footer strip (y 452..480) with the full-width seek rail, now-playing line
// and key hints. No loose readouts float in the corners any more.
void PaintCustomHeader(const char* layoutCode) {
    const Theme& t = Th();
    FillRectCol(0, 0, S(kLogicalW), S(28), t.panel);
    FillRectCol(0, S(27), S(kLogicalW), S(28), t.frame);
    if (LightTheme())
        FillRectCol(0, S(26), S(kLogicalW), S(27), LerpCol(t.frame, t.ink, 0.25f));
    // machine plate: 3px signal block + heavy title + composition code
    FillRectCol(S(12), S(8), S(15), S(21), t.bar);
    DrawCells("SOUND MEMOIRS", 22, 10, 0);
    DrawCells(layoutCode, 136, 10, 2);
    // right: theme name + transport badge with a play indicator lamp
    const char* nm = t.name;
    DrawCells(nm, 536 - (int)strlen(nm) * 8, 10, 2);
    const char* tw = TransportWord();
    const bool playing = g_state->current >= 0 && !g_state->libPaused && !g_state->loading;
    const int tx = 616 - (int)strlen(tw) * 8;
    DrawCells(tw, tx, 10, playing ? 0 : 2);
    const bool blink = (g_state->tick % 30) < 20;
    if (playing || (g_state->libPaused && blink))
        FillRectCol(S(tx - 10), S(13), S(tx - 6), S(17), t.accentHot);
}

void PaintCustomFooter(bool showGridHint) {
    const Theme& t = Th();
    FillRectCol(0, S(452), S(kLogicalW), S(kLogicalH), t.panel);
    FillRectCol(0, S(452), S(kLogicalW), S(453), t.frame);
    if (LightTheme())
        FillRectCol(0, S(453), S(kLogicalW), S(454), LerpCol(t.frame, t.ink, 0.25f));
    // full-width seek rail embedded in the footer's top edge
    FillRectCol(S(16), S(454), S(624), S(457), t.seam);
    const int end = 16 + int(608 * PlayProgress());
    if (end > 16) FillRectCol(S(16), S(454), S(end), S(457), t.bar);
    const bool blink = (g_state->tick % 30) < 20;
    if (g_state->current >= 0 && (!g_state->libPaused || blink))
        FillRectCol(S(end - 1), S(453), S(end + 2), S(460),
                    LerpCol(t.accentHot, t.ink, 0.25f));
    // now playing (local files keep their real CJK title via textLib)
    if (g_state->current == kTrackCount && g_state->libCurrentRow >= 0 &&
        g_state->libCurrentRow < g_libNameCount && g_libNames[g_state->libCurrentRow][0]) {
        DrawCells("NOW :", 16, 461, 3);
        textLib(g_libNames[g_state->libCurrentRow], 64, 460, 400, 472, t.digit);
    } else {
        char b[48];
        if (g_state->current == kTrackCount) snprintf(b, sizeof(b), "NOW : LOCAL FILE");
        else if (g_state->current >= 0) snprintf(b, sizeof(b), "NOW : TRACK %02d", g_state->current);
        else snprintf(b, sizeof(b), "NOW : <SILENCE>");
        DrawCells(b, 16, 461, 3);
    }
    {
        char tb[32];
        const int secs = std::max(0, int(g_state->seconds));
        snprintf(tb, sizeof(tb), "%02d:%02d", secs / 60, secs % 60);
        DrawCells(tb, 616 - (int)strlen(tb) * 8, 461, 3);
    }
    DrawCells(showGridHint ? "TAB GRID / F9" : "F7 VISUAL / F9", 16, 473, 2);
    {
        char sb[16];
        snprintf(sb, sizeof(sb), "SEL:TR:%02d", std::max(0, g_state->selected));
        DrawCells(sb, 616 - (int)strlen(sb) * 8, 473, 2);
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
            // edges heat toward stripHot with the channel envelope: quiet
            // stays cool wave/bar, a strong passage visibly loads the ribbon
            const float heat = std::clamp(g_state->wing[side], 0.f, 1.f);
            const COLORREF uc = LerpCol(t.wave, t.stripHot, heat * 0.85f);
            const COLORREF lc = LerpCol(t.bar, t.stripHot, heat * 0.55f);
            GfxPoint upper[220], lower[220];
            for (int j = 0; j < 220; ++j) {
                float v = wave[j], lag = wave[(j + 12) % 220];
                int center = base + int(.58f * v), w = 3 + int(.37f * std::abs(v - lag));
                int y = 26 + 2 * j;
                upper[j] = { (std::int16_t)S(center - w), (std::int16_t)S(y) };
                lower[j] = { (std::int16_t)S(center + w), (std::int16_t)S(y) };
                if (j % 5 == 0)
                    FillRectCol(S(center - w), S(y), S(center + w + 1), S(y + 1),
                                LerpCol(t.strip, t.stripHot, heat * 0.7f));
            }
            gfx_polyline(upper, 220, uc, 1);
            gfx_polyline(lower, 220, lc, 1);
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
                    // matrix: lit cells heat toward stripHot with the envelope
                    const float heat = std::clamp(g_state->wing[side], 0.f, 1.f);
                    for (int col = 0; col < 12; ++col) {
                        int x = base + (side ? -1 : 1) * (col * 10 - 55);
                        bool lit = col * 5 + 2 < width;
                        const COLORREF litCol = col > 8
                            ? LerpCol(t.bar, t.stripHot, heat * 0.6f)
                            : LerpCol(t.wave, t.stripHot, heat * 0.8f);
                        FillRectCol(S(x - 2), S(y - 1), S(x + (lit ? 3 : 0)), S(y + (lit ? 3 : 1)),
                                    lit ? litCol : LerpCol(t.outer, t.wave, .30f));
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
    const float imp = Impact();
    const float spark = g_mem ? g_mem->highSpark : 0.f;
    FillRectCol(0, 0, S(kLogicalW), S(kLogicalH), t.outer);

    // main display window: the single visual protagonist of this layout
    InsetPanel(16, 34, 624, 446);

    // ---- upper spectrum field (y 44..326): 43 flat two-tone columns -------
    const int baseY = 326;
    // sparse dB reference dotted lines (3, not a full-screen dot matrix)
    const COLORREF ref = LerpCol(t.seam, t.panel, 0.30f);
    for (int r = 0; r < 3; ++r) {
        const int y = baseY - 66 * (r + 1);
        for (int x = 28; x < 612; x += 8)
            FillRectCol(S(x), S(y), S(x + 2), S(y + 1), ref);
    }
    const int cols = memoirs::kSpectrumBands;
    const int pitch = 14, barW = 12;
    const int x0 = (kLogicalW - cols * pitch) / 2;
    for (int k = 0; k < cols; ++k) {
        const float lv = Lv(k);
        const int x1 = x0 + k * pitch, x2 = x1 + barW;
        const int up = int(lv * 264);
        for (int y = baseY - up; y < baseY; y += 3)
            FillRectCol(S(x1), S(y), S(x2), S(std::min(y + 2, baseY)), t.bar);
        const int prevUp = int(stage::bandLevel(g_state->spectrum.barsPrev[k]) * 264);
        for (int y = baseY - prevUp; y < baseY - up; y += 3)
            FillRectCol(S(x1), S(y), S(x2), S(std::min(y + 2, baseY - up)), t.barLit);
    }
    // beat: the baseline steps 1px -> 3px solid accent; HF spark lights the
    // frequency ticks below (three-band division of labour)
    if (imp > 0.08f)
        FillRectCol(S(24), S(baseY), S(616), S(baseY + 3), t.accentHot);
    else
        FillRectCol(S(24), S(baseY), S(616), S(baseY + 1),
                    LerpCol(t.frame, t.panel, 0.35f));
    for (int i = 0; i < 8; ++i) {
        const int x = 48 + i * 78;
        FillRectCol(S(x), S(baseY + 3), S(x + 1), S(baseY + 9),
                    spark > 0.15f ? t.accentHot : LerpCol(t.frame, t.panel, 0.45f));
        const int len = (int)strlen(kFreqLabel[i]);
        DrawCells(kFreqLabel[i], x - 4 * len + 4, baseY + 12, 4);
    }

    // ---- lower independent stereo scope slot (y 346..436) -----------------
    // L above, R below; 2px traces that never cross the spectrum columns.
    FillRectCol(S(24), S(346), S(616), S(347), LerpCol(t.frame, t.panel, 0.45f));
    GfxPoint pts[120];
    for (int side = 0; side < 2; ++side) {
        const float* wv = side ? g_state->spectrum.waveR : g_state->spectrum.waveL;
        const int cy = side ? 414 : 370;
        FillRectCol(S(56), S(cy), S(616), S(cy + 1), LerpCol(t.seam, t.panel, 0.30f));
        for (int i = 0; i < 120; ++i) {
            const float v = std::clamp(wv[i * 2] / 64.0f, -1.f, 1.f);
            pts[i].x = (std::int16_t)S(56 + i * 560 / 119);
            pts[i].y = (std::int16_t)S(cy + int(v * 20));
        }
        gfx_polyline(pts, 120,
                     side ? LerpCol(t.wave, t.bar, 0.40f)
                          : LerpCol(t.wave, t.accentHot, imp * 0.6f), 2);
        DrawCells(side ? "CH:R" : "CH:L", 22, cy - 5, 2);
    }

    PaintCustomHeader("FIELD / 01");
    PaintCustomFooter(true);

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
    FillRectCol(0, 0, S(kLogicalW), S(kLogicalH), t.outer);
    const float imp = Impact();

    // ---- left rack: L/R level meters in a compact framed bay (x 12..68) ---
    InsetPanel(12, 34, 68, 446);
    const int mTop = 60, mBot = 420;                 // 60 cells of 6px
    for (int side = 0; side < 2; ++side) {
        const int x1 = side ? 48 : 18, x2 = x1 + 14;
        const float drive = std::clamp((side ? g_state->spectrum.envR : g_state->spectrum.envL) * 2.2f, 0.f, 1.f);
        const int n = std::clamp(int(drive * 60.0f), 0, 60);
        for (int i = 0; i < n; ++i) {
            const int y2 = mBot - i * 6;
            FillRectCol(S(x1), S(y2 - 5), S(x2), S(y2),
                        i > 44 ? t.accentHot : t.strip);
        }
        for (int i = n; i < 60; i += 6) {
            const int y2 = mBot - i * 6;
            FillRectCol(S(x1 + 4), S(y2 - 1), S(x2 - 4), S(y2),
                        LerpCol(t.seam, t.panel, 0.35f));
        }
        // hard-edge peak-hold tick (0.8 s hold, then slow fall)
        if (g_mem && g_mem->meterPeak[side] > 0.02f) {
            const int py = mBot - int(g_mem->meterPeak[side] * 360.f);
            FillRectCol(S(x1), S(py - 2), S(x2), S(py), t.accentHot);
        }
        DrawCells(side ? "R" : "L", x1 + 3, 428, 2);
    }
    // dB scale in the 6px gutter between the meters: 0 / -6 / -12 / -24
    {
        const int db[4] = { 0, 6, 12, 24 };
        for (int i = 0; i < 4; ++i) {
            const float lv = std::pow(10.f, -db[i] / 20.f);
            const int y = mBot - int(lv * 360.f);
            FillRectCol(S(36), S(y), S(44), S(y + 1), LerpCol(t.frame, t.panel, 0.30f));
        }
        DrawCells("DB", 33, 44, 2);
    }

    // ---- centre display: the protagonist (x 76..452, 376px wide) ----------
    const int cl = 76, ct = 34, cr = 452, cb = 446;
    InsetPanel(cl, ct, cr, cb);
    const int baseY = 344;
    const int cols = memoirs::kSpectrumBands;
    const int pitch = 8, barW = 6;                   // near-original proportions
    const int x0 = cl + 1 + ((cr - cl - 2) - cols * pitch) / 2;
    // sparse dB reference dotted lines
    const COLORREF ref = LerpCol(t.seam, t.panel, 0.30f);
    for (int r = 0; r < 3; ++r) {
        const int y = baseY - 70 * (r + 1);
        for (int x = cl + 8; x < cr - 8; x += 8)
            FillRectCol(S(x), S(y), S(x + 2), S(y + 1), ref);
    }
    for (int k = 0; k < cols; ++k) {
        const float lv = Lv(k);
        const int x1 = x0 + k * pitch, x2 = x1 + barW;
        const int up = int(lv * 268);
        for (int y = baseY - up; y < baseY; y += 3)
            FillRectCol(S(x1), S(y), S(x2), S(std::min(y + 2, baseY)), t.bar);
        const int prevUp = int(stage::bandLevel(g_state->spectrum.barsPrev[k]) * 268);
        for (int y = baseY - prevUp; y < baseY - up; y += 3)
            FillRectCol(S(x1), S(y), S(x2), S(std::min(y + 2, baseY - up)), t.barLit);
    }
    if (imp > 0.08f)
        FillRectCol(S(cl + 4), S(baseY), S(cr - 4), S(baseY + 3), t.accentHot);
    else
        FillRectCol(S(cl + 4), S(baseY), S(cr - 4), S(baseY + 1),
                    LerpCol(t.frame, t.panel, 0.35f));

    // lower 90px: independent stereo scope area
    FillRectCol(S(cl + 4), S(352), S(cr - 4), S(353), LerpCol(t.frame, t.panel, 0.45f));
    GfxPoint pts[120];
    for (int side = 0; side < 2; ++side) {
        const float* wv = side ? g_state->spectrum.waveR : g_state->spectrum.waveL;
        const int cy = side ? 420 : 378;
        FillRectCol(S(cl + 34), S(cy), S(cr - 6), S(cy + 1), LerpCol(t.seam, t.panel, 0.30f));
        for (int i = 0; i < 120; ++i) {
            const float v = std::clamp(wv[i * 2] / 64.0f, -1.f, 1.f);
            pts[i].x = (std::int16_t)S(cl + 34 + i * (cr - cl - 40) / 119);
            pts[i].y = (std::int16_t)S(cy + int(v * 18));
        }
        gfx_polyline(pts, 120, side ? LerpCol(t.wave, t.bar, 0.45f) : t.wave, 2);
        DrawCells(side ? "R" : "L", cl + 8, cy - 5, 2);
    }
    DrawCells("43 BAND", cl + 6, ct + 6, 2);

    // ---- right: compact track index (x 460..628, 26% of the width) --------
    InsetPanel(456, 34, 628, 446);
    const int listX = 460, listY = 40, colW = 80, rowH = 16;
    const bool blink = (g_state->tick % 30) < 20;
    for (int i = 0; i < kTrackCount; ++i) {
        const int col = i / 23, row = i % 23;
        const int x = listX + col * colW, y = listY + row * rowH;
        const bool sel = g_state->selected == i;
        const bool playing = g_state->current == i;
        if (sel) {
            FillRectCol(S(x), S(y), S(x + colW - 6), S(y + rowH - 2),
                        LerpCol(t.selBand, t.panel, 0.30f));
            FillRectCol(S(x), S(y), S(x + 3), S(y + rowH - 2), t.bar);
        }
        if (playing && blink)
            FillRectCol(S(x + colW - 12), S(y + 5), S(x + colW - 8), S(y + 9), t.accentHot);
        char b[8];
        snprintf(b, sizeof(b), "TR:%02d", i);
        DrawCells(b, x + 8, y + 3, playing ? 0 : sel ? 0 : 2);
    }

    PaintCustomHeader("STACK / 02");
    PaintCustomFooter(false);
}

// ---- composition: circular instrument dial -------------------------------
// One radial spectrum, no second one: 43 solid hard-edge wedges (4-point
// trapezoid segments, 3px radial block + 2px gap, 70% angular duty) grow out
// of a continuous base ring. The centre carries the XY phase figure; the
// outer rim keeps only the 46 track ticks. Corners stay sparse on purpose.
void PaintRingLayout() {
    const Theme& t = Th();
    const float kPi = 3.14159265f;
    const float imp = Impact();
    const float spark = g_mem ? g_mem->highSpark : 0.f;
    const int cx = 320, cy = 236;
    FillRectCol(0, 0, S(kLogicalW), S(kLogicalH), t.outer);

    auto dot = [&](float a, int r, int half, COLORREF c) {
        const int x = cx + int(r * cosf(a)), y = cy + int(r * sinf(a));
        FillRectCol(S(x - half), S(y - half), S(x + half + 1), S(y + half + 1), c);
    };
    // continuous closed ring (96-segment polyline, no dot-string breaks)
    auto ringLine = [&](int radius, COLORREF c) {
        GfxPoint pts[97];
        for (int i = 0; i <= 96; ++i) {
            const float a = 2.f * kPi * i / 96;
            pts[i] = { (std::int16_t)S(cx + int(radius * cosf(a))),
                       (std::int16_t)S(cy + int(radius * sinf(a))) };
        }
        gfx_polyline(pts, 97, c, 1);
    };

    // dial furniture: continuous base + boundary rings, 12 radial ticks
    ringLine(104, LerpCol(t.frame, t.outer, 0.10f));
    ringLine(196, LerpCol(t.seam, t.ink, 0.45f));
    for (int i = 0; i < 12; ++i) {
        const float a = 2.f * kPi * i / 12;
        GfxPoint seg[2] = {
            { (std::int16_t)S(cx + int(188 * cosf(a))), (std::int16_t)S(cy + int(188 * sinf(a))) },
            { (std::int16_t)S(cx + int(196 * cosf(a))), (std::int16_t)S(cy + int(196 * sinf(a))) } };
        gfx_polyline(seg, 2, LerpCol(t.frame, t.outer, 0.15f), 1);
    }

    // progress arc: sweeps the inner dial as the track plays
    {
        const float prog = float(PlayProgress());
        const int lit = int(prog * 144);
        for (int i = 0; i < 144; ++i) {
            const float a = -kPi / 2.f + 2.f * kPi * i / 144;
            const bool on = g_state->current >= 0 && i < lit;
            dot(a, 96, on ? 1 : 0,
                on ? t.bar : LerpCol(t.seam, t.outer, 0.30f));
        }
        if (g_state->current >= 0 && lit > 0) {
            const float a = -kPi / 2.f + 2.f * kPi * (lit - 1) / 144;
            dot(a, 96, 2, LerpCol(t.accentHot, t.ink, 0.25f));
        }
    }

    // 43 solid radial wedge segments: each segment is a 4-point trapezoid
    // (r..r+3, 70% of the angular step). Level in bar, decay tail in barLit;
    // a beat expands the base ring and heats the innermost two segments.
    const int bands = memoirs::kSpectrumBands;
    const int r0 = 108 + int(imp * 8.f);
    const float step = 2.f * kPi / bands;
    const float hw = step * 0.35f;
    const bool beat = imp > 0.08f;
    auto wedge = [&](float a, int r, COLORREF c) {
        const float c0 = cosf(a - hw), s0 = sinf(a - hw);
        const float c1 = cosf(a + hw), s1 = sinf(a + hw);
        GfxPoint q[4] = {
            { (std::int16_t)S(cx + int(r * c0)),       (std::int16_t)S(cy + int(r * s0)) },
            { (std::int16_t)S(cx + int((r + 3) * c0)), (std::int16_t)S(cy + int((r + 3) * s0)) },
            { (std::int16_t)S(cx + int((r + 3) * c1)), (std::int16_t)S(cy + int((r + 3) * s1)) },
            { (std::int16_t)S(cx + int(r * c1)),       (std::int16_t)S(cy + int(r * s1)) } };
        gfx_polygon(q, 4, c);
    };
    for (int k = 0; k < bands; ++k) {
        const float lv = Lv(k);
        const float a = -kPi / 2.f + step * k;
        const int len = int(lv * 84.f);
        for (int r = r0; r < r0 + len; r += 5)
            wedge(a, r, beat && r < r0 + 10 ? t.accentHot : t.bar);
        const int prevLen = int(stage::bandLevel(g_state->spectrum.barsPrev[k]) * 84.f);
        for (int r = r0 + len; r < r0 + prevLen; r += 5)
            wedge(a, r, t.barLit);
    }

    // centre: large XY phase figure with a dim afterglow of the previous trace
    {
        GfxPoint frame[] = { {(std::int16_t)S(cx), (std::int16_t)S(cy - 72)},
                             {(std::int16_t)S(cx + 80), (std::int16_t)S(cy)},
                             {(std::int16_t)S(cx), (std::int16_t)S(cy + 72)},
                             {(std::int16_t)S(cx - 80), (std::int16_t)S(cy)},
                             {(std::int16_t)S(cx), (std::int16_t)S(cy - 72)} };
        gfx_polyline(frame, 5, LerpCol(t.frame, t.outer, 0.20f), 1);
        const float g = g_mem ? g_mem->xyGain : 1.f;
        if (g_mem && g_mem->samples > 0) {
            const int prev = (g_mem->trace + 5) % 6;
            GfxPoint ghost[120];
            for (int i = 0; i < 120; ++i) {
                const GfxPoint& p = g_mem->traces[prev][i];
                ghost[i] = { (std::int16_t)S(cx + ClampI(int(p.x * g), -66, 66)),
                             (std::int16_t)S(cy - ClampI(int(p.y * g), -60, 60)) };
            }
            gfx_polyline(ghost, 120, DepthInk(t.wave, 0.55f), 1);
        }
        GfxPoint pts2[120];
        for (int i = 0; i < 120; ++i) {
            const float l = g_state->spectrum.waveL[i * 2], r = g_state->spectrum.waveR[i * 2];
            pts2[i] = { (std::int16_t)S(cx + ClampI(int((l - r) * .92f * g), -66, 66)),
                        (std::int16_t)S(cy - ClampI(int((l + r) * .72f * g), -60, 60)) };
        }
        gfx_polyline(pts2, 120, t.bar, 1);
        FillRectCol(S(cx - 1), S(cy - 1), S(cx + 2), S(cy + 2), t.frame);
        // HF transient: crosshair pips light the four diamond vertices
        if (spark > 0.2f) {
            FillRectCol(S(cx - 1), S(cy - 73), S(cx + 2), S(cy - 70), t.accentHot);
            FillRectCol(S(cx + 78), S(cy - 1), S(cx + 81), S(cy + 2), t.accentHot);
            FillRectCol(S(cx - 1), S(cy + 70), S(cx + 2), S(cy + 73), t.accentHot);
            FillRectCol(S(cx - 81), S(cy - 1), S(cx - 78), S(cy + 2), t.accentHot);
        }
    }

    // 46 track ticks on the outside: selected 3x3 selEdge, playing 5x5
    // accentHot with a 1px box; idle ticks stay small and quiet
    const bool blink = (g_state->tick % 30) < 20;
    for (int i = 0; i < kTrackCount; ++i) {
        const float a = -kPi / 2.f + 2.f * kPi * i / kTrackCount;
        const bool sel = g_state->selected == i;
        const bool playing = g_state->current == i;
        if (playing) {
            const int x = cx + int(206 * cosf(a)), y = cy + int(206 * sinf(a));
            FillRectCol(S(x - 4), S(y - 4), S(x + 5), S(y + 5), t.frame);
            FillRectCol(S(x - 2), S(y - 2), S(x + 3), S(y + 3),
                        blink ? t.accentHot : t.bar);
        } else {
            dot(a, 206, sel ? 1 : 0,
                sel ? t.selEdge : LerpCol(t.seam, t.ink, 0.45f));
        }
    }

    // bottom-left: compact 8px twin vertical meters with peak-hold ticks
    for (int side = 0; side < 2; ++side) {
        const int x = 24 + side * 14;
        const float drive = std::clamp(g_state->wing[side], 0.f, 1.f);
        const int n = int(drive * 26);
        for (int i = 0; i < n; ++i) {
            const int y = 428 - i * 6;
            FillRectCol(S(x), S(y - 4), S(x + 8), S(y),
                        i > 19 ? t.accentHot : t.strip);
        }
        if (g_mem && g_mem->meterPeak[side] > 0.02f) {
            const int py = 428 - int(g_mem->meterPeak[side] * 156.f);
            FillRectCol(S(x), S(py - 2), S(x + 8), S(py), t.accentHot);
        }
        DrawCells(side ? "R" : "L", x, 438, 2);
    }
    // bottom-right: three clean readout rows, nothing else
    {
        const int lx = 548;
        char b[24];
        snprintf(b, sizeof(b), "TR:%02d", std::max(0, g_state->selected));
        DrawCells(b, lx, 396, g_state->selected < kTrackCount ? 0 : 2);
        static const char* orders[4] = { "ORDER", "ONE", "LOOP", "SHUFFLE" };
        DrawCells(orders[g_state->localOrder % 4], lx, 412, 2);
        snprintf(b, sizeof(b), "PEAK:%02d", g_mem ? int(g_mem->meterPeak[0] * 99.f) : 0);
        DrawCells(b, lx, 428, 3);
    }

    PaintCustomHeader("RING / 03");
    PaintCustomFooter(false);
}

// ---- composition: asymmetric precision cross ------------------------------
// 65/35 vertical split (axis x=416), 62/38 horizontal split (axis y=292):
// the top-left 43-band spectrum is the undisputed protagonist; top-right a
// large square XY scope; bottom-left stacked full-width L/R scopes;
// bottom-right a spacious transport bay. No duplicated instruments.
void PaintCrossLayout() {
    const Theme& t = Th();
    FillRectCol(0, 0, S(kLogicalW), S(kLogicalH), t.outer);
    const float imp = Impact();
    const float spark = g_mem ? g_mem->highSpark : 0.f;

    const int cx = 416, cy = 292;
    // main axes: 1px at rest, 3px accentHot step on a beat
    const int aw = imp > 0.08f ? 3 : 1;
    const COLORREF axis = imp > 0.08f ? t.accentHot : LerpCol(t.frame, t.outer, 0.25f);
    FillRectCol(S(cx - aw / 2), S(34), S(cx - aw / 2 + aw), S(446), axis);
    FillRectCol(S(16), S(cy - aw / 2), S(624), S(cy - aw / 2 + aw), axis);
    for (int y = 50; y < 446; y += 32) {
        FillRectCol(S(cx - 5), S(y), S(cx - 1), S(y + 1), t.seam);
        FillRectCol(S(cx + 1), S(y), S(cx + 5), S(y + 1), t.seam);
    }
    for (int x = 32; x < 624; x += 32) {
        FillRectCol(S(x), S(cy - 5), S(x + 1), S(cy - 1), t.seam);
        FillRectCol(S(x), S(cy + 1), S(x + 1), S(cy + 5), t.seam);
    }

    // ---- top-left protagonist: large 43-band vertical spectrum ------------
    {
        const int lx = 16, rx = 408, baseY = 262;
        const COLORREF ref = LerpCol(t.seam, t.outer, 0.30f);
        for (int r = 0; r < 3; ++r) {
            const int y = baseY - 60 * (r + 1);
            for (int x = lx + 8; x < rx - 8; x += 8)
                FillRectCol(S(x), S(y), S(x + 2), S(y + 1), ref);
        }
        const int cols = memoirs::kSpectrumBands;
        const int pitch = 9, barW = 7;
        const int x0 = lx + ((rx - lx) - cols * pitch) / 2;
        for (int k = 0; k < cols; ++k) {
            const float lv = Lv(k);
            const int x1 = x0 + k * pitch, x2 = x1 + barW;
            const int up = int(lv * 204);
            for (int y = baseY - up; y < baseY; y += 3)
                FillRectCol(S(x1), S(y), S(x2), S(std::min(y + 2, baseY)), t.bar);
            const int prevUp = int(stage::bandLevel(g_state->spectrum.barsPrev[k]) * 204);
            for (int y = baseY - prevUp; y < baseY - up; y += 3)
                FillRectCol(S(x1), S(y), S(x2), S(std::min(y + 2, baseY - up)), t.barLit);
        }
        if (imp > 0.08f)
            FillRectCol(S(lx + 4), S(baseY), S(rx - 4), S(baseY + 3), t.accentHot);
        else
            FillRectCol(S(lx + 4), S(baseY), S(rx - 4), S(baseY + 1),
                        LerpCol(t.frame, t.outer, 0.35f));
        for (int i = 0; i < 8; ++i) {
            const int x = 34 + i * 52;
            FillRectCol(S(x), S(baseY + 3), S(x + 1), S(baseY + 8),
                        spark > 0.15f ? t.accentHot : LerpCol(t.frame, t.outer, 0.45f));
            const int len = (int)strlen(kFreqLabel[i]);
            DrawCells(kFreqLabel[i], x - 4 * len + 4, baseY + 11, 4);
        }
        DrawCells("43 BAND", lx + 4, 40, 2);
    }

    // ---- top-right: large square XY phase scope (19px grid + afterglow) ---
    {
        const int wx = 424, wy = 60, wr = 624, wb = 260;
        InsetPanel(wx, wy, wr, wb);
        const int mx = (wx + wr) / 2, my = (wy + wb) / 2;
        const COLORREF grid = LerpCol(t.seam, t.panel, 0.30f);
        for (int j = -5; j <= 5; ++j) {
            const int y = my + j * 19;
            if (y > wy + 2 && y < wb - 3)
                FillRectCol(S(wx + 3), S(y), S(wr - 3), S(y + 1), grid);
        }
        for (int j = -5; j <= 5; ++j) {
            const int x = mx + j * 19;
            if (x > wx + 2 && x < wr - 3)
                FillRectCol(S(x), S(wy + 3), S(x + 1), S(wb - 3), grid);
        }
        const float g = g_mem ? g_mem->xyGain : 1.f;
        if (g_mem && g_mem->samples > 0) {
            const int prev = (g_mem->trace + 5) % 6;
            GfxPoint ghost[120];
            for (int i = 0; i < 120; ++i) {
                const GfxPoint& p = g_mem->traces[prev][i];
                ghost[i] = { (std::int16_t)S(mx + ClampI(int(p.x * g), -94, 94)),
                             (std::int16_t)S(my - ClampI(int(p.y * g), -92, 92)) };
            }
            gfx_polyline(ghost, 120, DepthInk(t.wave, 0.55f), 1);
        }
        GfxPoint xy[120];
        for (int i = 0; i < 120; ++i) {
            const float l = g_state->spectrum.waveL[i * 2], r = g_state->spectrum.waveR[i * 2];
            xy[i] = { (std::int16_t)S(mx + ClampI(int((l - r) * .92f * g), -94, 94)),
                      (std::int16_t)S(my - ClampI(int((l + r) * .72f * g), -92, 92)) };
        }
        gfx_polyline(xy, 120, t.bar, 1);
        FillRectCol(S(mx - 1), S(my - 1), S(mx + 2), S(my + 2), t.frame);
        DrawCells("XY PHASE", wx + 6, wy + 6, 2);
        if (spark > 0.2f)
            FillRectCol(S(wr - 10), S(wy + 6), S(wr - 6), S(wy + 10), t.accentHot);
    }

    // ---- bottom-left: stacked full-width L / R scopes ----------------------
    {
        GfxPoint pts[120];
        for (int side = 0; side < 2; ++side) {
            const float* wv = side ? g_state->spectrum.waveR : g_state->spectrum.waveL;
            const int midY = side ? 408 : 336;
            FillRectCol(S(64), S(midY), S(404), S(midY + 1),
                        LerpCol(t.seam, t.outer, 0.30f));
            for (int i = 0; i < 120; ++i) {
                const float v = std::clamp(wv[i * 2] / 64.0f, -1.f, 1.f);
                pts[i].x = (std::int16_t)S(64 + i * 340 / 119);
                pts[i].y = (std::int16_t)S(midY + int(v * 30));
            }
            gfx_polyline(pts, 120, side ? LerpCol(t.wave, t.bar, 0.40f) : t.wave, 2);
            DrawCells(side ? "CH:R" : "CH:L", 18, midY - 5, 2);
        }
    }

    // ---- bottom-right: spacious transport bay ------------------------------
    {
        const int px = 432;
        DrawCells(TransportWord(), px, 306, g_state->current >= 0 && !g_state->libPaused ? 0 : 2);
        {
            char b[24];
            snprintf(b, sizeof(b), "TR:%02d", std::max(0, g_state->current));
            DrawCells(b, px, 326, g_state->current >= 0 ? 0 : 2);
            static const char* orders[4] = { "ORDER", "ONE", "LOOP", "SHUFFLE" };
            DrawCells(orders[g_state->localOrder % 4], px + 72, 326, 2);
        }
        DrawCells("EXIT", px, 348, g_state->selected == kExitIndex ? 1 : 2);
        // horizontal L/R level bars with hard peak-hold ticks
        for (int side = 0; side < 2; ++side) {
            const int y = 396 + side * 20;
            const float drive = std::clamp(g_state->wing[side], 0.f, 1.f);
            FillRectCol(S(px + 24), S(y), S(616), S(y + 6), LerpCol(t.seam, t.outer, 0.30f));
            const int w = int(drive * 160.f);
            if (w > 0) FillRectCol(S(px + 24), S(y), S(px + 24 + w), S(y + 6), t.strip);
            if (g_mem && g_mem->meterPeak[side] > 0.02f) {
                const int pxx = px + 24 + int(g_mem->meterPeak[side] * 160.f);
                FillRectCol(S(pxx), S(y - 2), S(pxx + 2), S(y + 8), t.accentHot);
            }
            DrawCells(side ? "R" : "L", px, y - 2, 2);
        }
    }

    // hub: small diamond badge where the axes cross
    {
        GfxPoint hub[] = { {(std::int16_t)S(cx), (std::int16_t)S(cy - 10)},
                           {(std::int16_t)S(cx + 12), (std::int16_t)S(cy)},
                           {(std::int16_t)S(cx), (std::int16_t)S(cy + 10)},
                           {(std::int16_t)S(cx - 12), (std::int16_t)S(cy)},
                           {(std::int16_t)S(cx), (std::int16_t)S(cy - 10)} };
        gfx_polyline(hub, 5, t.frame, 1);
        const bool blink = (g_state->tick % 30) < 20;
        if (g_state->current >= 0 && (!g_state->libPaused || blink))
            FillRectCol(S(cx - 2), S(cy - 2), S(cx + 3), S(cy + 3), t.bar);
    }

    PaintCustomHeader("CROSS / 04");
    PaintCustomFooter(true);

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

// ---- composition: SINK descent shaft (HELIX slot) --------------------------
// Two angular strands, one per channel, zigzag down a tall shaft and cross
// into X / # lattices. Every rung is a 1/15 s sample of that channel's drive,
// so the lattice is a core sample of the music. The rungs scroll upward
// while the depth gauge counts down: the view is sinking through the track.
// Beats mark their rung in the hot color and kick the descent speed.
void PaintHelixLayout() {
    const Theme& t = Th();
    FillRectCol(0, 0, S(kLogicalW), S(kLogicalH), t.outer);

    const bool active = g_state->current >= 0;
    const int H = stage::Memory::kHist;

    // shaft geometry
    const int wl = 210, wr = 430, wt = 56, wb = 440;
    const int cx = 320, pitch = 10, maxOff = 86;
    const COLORREF wc = t.frame;
    FillRectCol(S(wl - 14), S(wt), S(wr + 14), S(wb), t.panel);
    FillRectCol(S(wl - 14), S(wt), S(wl - 13), S(wb), wc);
    FillRectCol(S(wr + 13), S(wt), S(wr + 14), S(wb), wc);
    if (LightTheme()) {
        const COLORREF inner = LerpCol(t.frame, t.ink, 0.25f);
        FillRectCol(S(wl - 13), S(wt), S(wl - 12), S(wb), inner);
        FillRectCol(S(wr + 12), S(wt), S(wr + 13), S(wb), inner);
    }
    // shaft wall ticks every rung, longer every 5th
    for (int y = wt; y <= wb; y += pitch) {
        const int len = ((y - wt) / pitch) % 5 == 0 ? 8 : 4;
        FillRectCol(S(wl - 14), S(y), S(wl - 14 + len), S(y + 1), LerpCol(t.frame, t.outer, 0.35f));
        FillRectCol(S(wr + 14 - len), S(y), S(wr + 14), S(y + 1), LerpCol(t.frame, t.outer, 0.35f));
    }
    // faint dot grid inside the shaft
    const COLORREF grid = LerpCol(t.seam, t.panel, 0.45f);
    for (int y = wt + 6; y < wb; y += 12)
        for (int x = wl - 4; x < wr + 4; x += 12)
            FillRectCol(S(x), S(y), S(x + 1), S(y + 1), grid);

    // rung positions: continuous upward scroll locked to playback time
    // (8 rungs/s at 10px = a calm 80px/s descent, no strobing)
    const double scroll = active ? g_state->seconds * 8.0 : 0.0;
    const int base = int(std::floor(scroll));
    const int frac = int((scroll - base) * pitch);
    const int rows = (wb - wt) / pitch + 2;

    // history lookup: row 0 = newest sample at the bottom of the shaft
    auto hist = [&](int row, int side) -> float {
        if (!g_mem || row < 0 || row >= H) return 0.f;
        const int idx = (g_mem->histHead - row + H * 4) % H;
        return side ? g_mem->histR[idx] : g_mem->histL[idx];
    };
    auto wave = [&](int row, int side) -> float {
        if (!g_mem || row < 0 || row >= H) return 0.f;
        const int idx = (g_mem->histHead - row + H * 4) % H;
        return side ? g_mem->histWaveR[idx] : g_mem->histWaveL[idx];
    };
    auto beat = [&](int row) -> bool {
        if (!g_mem || row < 0 || row >= H) return false;
        const int idx = (g_mem->histHead - row + H * 4) % H;
        return g_mem->histBeat[idx] != 0;
    };
    // strand offset = the waveform sample itself, scaled by that moment's
    // drive: the wave draws the helix, loud passages swing wide. No zigzag,
    // no beat modulation - vertical motion is the only movement and it is
    // strictly uniform.
    auto strandX = [&](int row, int side) -> int {
        const float drive = hist(row, side);
        return cx + int(wave(row, side) * (18.f + drive * (maxOff - 18.f)));
    };

    gfx_push_clip(S(wl - 14), S(wt + 1), S(wr + 14), S(wb - 1));
    // rungs + cross pins, oldest at the top
    for (int j = rows; j >= 0; --j) {
        const int y = wb - j * pitch + frac;
        if (y < wt - pitch || y > wb + pitch) continue;
        const int row = j;                      // history age (0 = newest)
        const int xl2 = strandX(row, 0), xr2 = strandX(row, 1);
        const bool hot = beat(row);
        const bool fresh = row == 0;
        // cross pin between the strands: the # bars (1px, quiet)
        const int pl = std::min(xl2, xr2), pr = std::max(xl2, xr2);
        if (pr - pl > 3) {
            const COLORREF pc = hot ? t.accentHot : fresh ? t.bar : t.barLit;
            FillRectCol(S(pl + 2), S(y), S(pr - 1), S(y + 1), pc);
        }
        // rung nodes: 3x3 blocks; a beat only heats the node, no full flare
        const COLORREF lc = hot ? LerpCol(t.wave, t.accentHot, 0.7f) : fresh ? t.bar : t.wave;
        const COLORREF rc = hot ? LerpCol(t.wave, t.accentHot, 0.7f) : fresh ? t.bar
                          : LerpCol(t.wave, t.bar, 0.45f);
        FillRectCol(S(xl2 - 1), S(y - 1), S(xl2 + 2), S(y + 2), lc);
        FillRectCol(S(xr2 - 1), S(y - 1), S(xr2 + 2), S(y + 2), rc);
    }
    // strand segments between consecutive rung nodes, drawn after the pins
    // so the strands read on top
    for (int j = rows; j >= 1; --j) {
        const int y0 = wb - j * pitch + frac, y1 = wb - (j - 1) * pitch + frac;
        if (y1 < wt - pitch || y0 > wb + pitch) continue;
        for (int side = 0; side < 2; ++side) {
            GfxPoint seg[2] = {
                { (std::int16_t)S(strandX(j, side)), (std::int16_t)S(y0) },
                { (std::int16_t)S(strandX(j - 1, side)), (std::int16_t)S(y1) } };
            const COLORREF sc = side ? LerpCol(t.wave, t.bar, 0.45f) : t.wave;
            gfx_polyline(seg, 2, sc, 1);
        }
    }
    gfx_pop_clip();

    // ---- depth gauge (left): playback position as descent depth -----------
    {
        const int gx = 150, gt = 70, gb = 430;
        FillRectCol(S(gx), S(gt), S(gx + 1), S(gb), LerpCol(t.frame, t.outer, 0.25f));
        for (int i = 0; i <= 10; ++i) {
            const int y = gt + i * (gb - gt) / 10;
            FillRectCol(S(gx - (i % 5 == 0 ? 8 : 4)), S(y), S(gx), S(y + 1),
                        LerpCol(t.frame, t.outer, 0.30f));
        }
        const int my = gt + int(PlayProgress() * (gb - gt));
        FillRectCol(S(gx - 12), S(my - 3), S(gx + 4), S(my + 4), t.bar);
        FillRectCol(S(gx - 12), S(my - 3), S(gx + 4), S(my - 1), t.accentHot);
        DrawCells("DEPTH", gx - 40, 58, 2);
        char db[24];
        snprintf(db, sizeof(db), "-%04dM", int(PlayProgress() * 9999));
        DrawCells(db, gx - 44, 444, 3);
    }

    // ---- right column: live channel meters + low-band ladder --------------
    for (int side = 0; side < 2; ++side) {
        const int x = 470 + side * 20;
        const float drive = std::clamp(g_state->wing[side], 0.f, 1.f);
        const int n = int(drive * 24);
        for (int i = 0; i < n; ++i) {
            const int y = 300 - i * 6;
            FillRectCol(S(x), S(y - 4), S(x + 12), S(y), i > 18 ? t.accentHot : t.strip);
        }
        DrawCells(side ? "R" : "L", x + 2, 310, 2);
        // hard-edge peak-hold tick over the meter column
        if (g_mem && g_mem->meterPeak[side] > 0.02f) {
            const int py = 300 - int(g_mem->meterPeak[side] * 144.f);
            FillRectCol(S(x), S(py - 2), S(x + 12), S(py), t.accentHot);
        }
    }
    {
        const int lx = 470, ly = 336;
        for (int k = 0; k < 8; ++k) {
            const int y = ly + k * 8;
            const int w = int(Lv(k) * 120);
            if (w > 0) FillRectCol(S(lx), S(y), S(lx + w), S(y + 3), t.bar);
            const int pw = int(stage::bandLevel(g_state->spectrum.barsPrev[k]) * 120);
            if (pw > w) FillRectCol(S(lx + w), S(y), S(lx + pw), S(y + 3), t.barLit);
        }
        DrawCells("LOW", lx, 326, 2);
        static const char* orders[4] = { "ORDER", "ONE", "LOOP", "SHUFFLE" };
        DrawCells(orders[g_state->localOrder % 4], lx, 412, 2);
    }

    // header / status
    DrawCells("SOUND MEMOIRS", 24, 20, 0);
    { const char* nm = t.name; DrawCells(nm, 616 - (int)strlen(nm) * 8, 20, 2); }
    DrawCells("SINK : 05", 24, 36, 3);
    DrawCells(TransportWord(), 616 - (int)strlen(TransportWord()) * 8, 36,
              active && !g_state->libPaused ? 0 : 2);

    // footer: track + name + time
    {
        char b[48];
        if (g_state->current == kTrackCount) snprintf(b, sizeof(b), "NOW : LOCAL FILE");
        else if (active) snprintf(b, sizeof(b), "NOW : TRACK %02d", g_state->current);
        else snprintf(b, sizeof(b), "NOW : <SILENCE>");
        DrawCells(b, 24, 462, 3);
        char tb[32];
        const int secs = std::max(0, int(g_state->seconds));
        snprintf(tb, sizeof(tb), "%02d:%02d", secs / 60, secs % 60);
        DrawCells(tb, 616 - (int)strlen(tb) * 8, 462, 3);
    }
    if (g_state->current == kTrackCount && g_state->libCurrentRow >= 0 &&
        g_state->libCurrentRow < g_libNameCount && g_libNames[g_state->libCurrentRow][0]) {
        textLib(g_libNames[g_state->libCurrentRow], 200, 456, 440, 472, t.digit);
    }
}

// ---- composition: DIVE, first-person fall through a # / DNA tunnel --------
// Each layer is a # frame whose four edges are DNA double helices: two
// strands wind around each edge line with cross rungs between them. The
// frame grows at a strictly uniform rate while the whole layer rotates; the
// helix swing width is that moment's spectrum/waveform. Four bright marker
// dots ride the edge midpoints so the plane reads in 3D. Frames are born
// small at the centre, pass the viewer and leave past the screen edge.
void PaintDiveLayout() {
    const Theme& t = Th();
    const float kPi = 3.14159265f;
    FillRectCol(0, 0, S(kLogicalW), S(kLogicalH), t.outer);

    const bool active = g_state->current >= 0;
    const int cx = 320, cy = 240;
    const int K = 6;                       // layers in flight: continuous stream
    const int DS = stage::Memory::kDiveSlots;

    // fall phase: strictly uniform - one layer born every 1/0.32 s. Growth is
    // true perspective (below), so a layer is already far off-screen before
    // its slot recycles: the stream reads as one layer passing after another.
    const double f = active ? g_state->seconds * 0.32 : 0.35;
    const int base = int(std::floor(f));
    const float frac = float(f - base);

    // birth snapshots: frozen drive / phase per layer, indexed by birth
    // number - never scrolls with the 8 Hz history, never ages out.
    auto snapDrive = [&](int age) -> float {
        if (!g_mem || age < 0) return 0.f;
        return g_mem->diveDrive[((age % DS) + DS) % DS];
    };
    auto snapPhase = [&](int age) -> float {
        if (!g_mem || age < 0) return 0.f;
        return g_mem->divePhase[((age % DS) + DS) % DS];
    };

    struct Frame { float half; float ang; float u; int age; float drive; };
    // K visible slots + 1 extra "just-recycled" anchor so the rail between
    // the outermost visible layer and the one that just left stays drawn.
    Frame fr[7];
    for (int k = 0; k <= K; ++k) {
        const float u = (k + frac) / K;              // 0 new/far .. >1 left
        const int age = base - k;
        fr[k].u = u;
        fr[k].age = age;
        // true perspective: size ~ 1/z. The extra anchor (u>1) keeps growing
        // past the frame so its rail endpoint is far off-screen, not snapped.
        const float z = std::max(0.008f, 1.f - u);
        fr[k].half = std::min(6000.f, 13.f / z);
        fr[k].drive = std::clamp(snapDrive(age), 0.f, 1.f);
        fr[k].ang = age * 0.5f + (active ? float(g_state->seconds) * 0.55f : 0.f);
    }

    // tunnel-wall rails: connect each layer's 8 vertices to the next layer's
    // corresponding vertex. Odd vertices carry the L waveform, even the R,
    // displaced perpendicular to the rail - the walls are live scopes.
    for (int k = 0; k + 1 <= K; ++k) {
        const Frame& A = fr[k];      // farther
        const Frame& B = fr[k + 1];  // nearer (may be the recycled anchor)
        // No cull: the rail to the just-recycled layer keeps tracing to the
        // screen edge until that anchor itself is gone, so the scope line
        // between the last two layers never breaks.
        const float depth = std::min(1.f, A.u);
        const float fade = std::min(0.96f, std::pow(1.f - depth, 1.6f));
        const COLORREF railC = DepthInk(t.frame, fade);
        for (int c = 0; c < 8; ++c) {
            const float aa = kPi / 8.f + c * kPi / 4.f;
            const float ax = A.half * cosf(aa + A.ang), ay = A.half * sinf(aa + A.ang);
            const float bx = B.half * cosf(aa + B.ang), by = B.half * sinf(aa + B.ang);
            const float dx = bx - ax, dy = by - ay;
            const float len = std::sqrt(dx * dx + dy * dy);
            if (len < 2.f) continue;
            const float nx = -dy / len, ny = dx / len;
            const float* wv = (c % 2) ? g_state->spectrum.waveL : g_state->spectrum.waveR;
            const COLORREF wc = DepthInk((c % 2) ? t.wave : LerpCol(t.wave, t.bar, 0.45f),
                                         fade);
            const int RS = 16;
            GfxPoint rail[17];
            for (int i = 0; i <= RS; ++i) {
                const float tt = float(i) / RS;
                const float v = std::clamp(wv[(i * 14) % 240] / 64.f, -1.f, 1.f);
                const float disp = v * std::min(34.f, len * 0.22f);
                rail[i] = { (std::int16_t)S(cx + int(ax + dx * tt + nx * disp)),
                            (std::int16_t)S(cy + int(ay + dy * tt + ny * disp)) };
            }
            gfx_polyline(rail, RS + 1, wc, 1);
        }
    }

    // painter's order: far (small, transparent) first, near (large, solid)
    for (int k = 0; k < K; ++k) {
        const Frame& F = fr[k];
        // a layer's own octagon / # / helix is culled once fully off-screen;
        // the inter-layer waveform rails (drawn above, no cull) keep tracing
        // to the screen edge so the scope line never breaks.
        if (F.half * 0.55f > 430.f) continue;
        const float depth = F.u;
        // depth cue: far/small layers nearly dissolve into the backdrop
        // (power curve), near layers solid - monotonic, no reversal
        const float fade = std::min(0.96f, std::pow(1.f - depth, 1.6f));
        const COLORREF strandA = DepthInk(t.wave, fade);
        const COLORREF strandB = DepthInk(LerpCol(t.wave, t.bar, 0.45f), fade);
        // rungs: hot bonds pushed toward ink so they contrast on EVERY theme
        // (light or dark), then the same far-transparent / near-solid ramp
        const COLORREF rungHot = LerpCol(t.accentHot, t.ink, 0.3f);
        const COLORREF rungC = DepthInk(rungHot, std::min(0.85f, fade + 0.05f));
        const int w = 1;                             // hairline strands always
        // helix swing: tight around the edge line; drive still sets the width
        const float amp = F.half * (0.03f + 0.07f * F.drive);
        const float bias = snapPhase(F.age);
        const float ca = cosf(F.ang), sa = sinf(F.ang);
        auto pt = [&](float lx, float ly) -> GfxPoint {
            return { (std::int16_t)S(cx + int(lx * ca - ly * sa)),
                     (std::int16_t)S(cy + int(lx * sa + ly * ca)) };
        };
        // the positioning frame is a REGULAR octagon of 8 hard beacon lights
        // at radius half. The # is four INFINITE lines clipped by that
        // octagon: each edge runs along its line until it hits the boundary,
        // and the DNA helix winds uniformly over that whole visible span.
        float ovx[8], ovy[8];
        GfxPoint oct[8];
        for (int c = 0; c < 8; ++c) {
            const float a = kPi / 8.f + c * kPi / 4.f;
            ovx[c] = F.half * cosf(a);
            ovy[c] = F.half * sinf(a);
            oct[c] = pt(ovx[c], ovy[c]);
        }
        // faint octagon rim: the clipping boundary
        for (int c = 0; c < 8; ++c) {
            GfxPoint rim[2] = { oct[c], oct[(c + 1) % 8] };
            gfx_polyline(rim, 2, DepthInk(t.frame, 0.45f + fade * 0.4f), 1);
        }
        // clip the infinite line P + t*D against the convex octagon by
        // intersecting it with every rim segment (robust, winding-agnostic)
        auto clipLine = [&](float px, float py, float dx, float dy,
                            float& t0, float& t1) -> bool {
            t0 = 1e9f; t1 = -1e9f;
            bool hit = false;
            for (int c = 0; c < 8; ++c) {
                const int n = (c + 1) % 8;
                const float ax = ovx[c], ay = ovy[c];
                const float ex = ovx[n] - ax, ey = ovy[n] - ay;
                // solve P + t*D = A + s*E
                const float den = dx * ey - dy * ex;
                if (std::abs(den) < 1e-6f) continue;       // parallel
                const float tt = ((ax - px) * ey - (ay - py) * ex) / den;
                const float ss = ((ax - px) * dy - (ay - py) * dx) / den;
                if (ss < -0.001f || ss > 1.001f) continue; // outside segment
                t0 = std::min(t0, tt); t1 = std::max(t1, tt);
                hit = true;
            }
            return hit && t0 < t1;
        };
        const float off = F.half * 0.42f;      // # bar offset: big centre hole
        const float sideAmp = std::min(amp, F.half * 0.16f);
        const int ES = 40;                     // samples along the clipped span
        for (int e = 0; e < 4; ++e) {
            const bool horiz = e < 2;
            const float eo = (e % 2) ? off : -off;
            const float px = horiz ? 0.f : eo, py = horiz ? eo : 0.f;
            const float dx = horiz ? 1.f : 0.f, dy = horiz ? 0.f : 1.f;
            float t0, t1;
            if (!clipLine(px, py, dx, dy, t0, t1)) continue;
            auto edgePt = [&](float tt, float q) -> GfxPoint {
                const float p = t0 + (t1 - t0) * tt;
                return horiz ? pt(p, eo + q) : pt(eo + q, p);
            };
            // base-pair bonds every 3rd sample, uniform winding (no taper)
            for (int i = 0; i <= ES; i += 3) {
                const float tt = float(i) / ES;
                const float h = sideAmp * sinf(2.f * kPi * 2.5f * tt + bias + e * 0.7f);
                GfxPoint seg[2] = { edgePt(tt, h), edgePt(tt, -h) };
                gfx_polyline(seg, 2, rungC, 1);
            }
            // the two strands over the full clipped span, cut at the rim
            GfxPoint sA[41], sB[41];
            for (int i = 0; i <= ES; ++i) {
                const float tt = float(i) / ES;
                const float h = sideAmp * sinf(2.f * kPi * 2.5f * tt + bias + e * 0.7f);
                sA[i] = edgePt(tt, h);
                sB[i] = edgePt(tt, -h);
            }
            gfx_polyline(sA, ES + 1, strandA, w);
            gfx_polyline(sB, ES + 1, strandB, w);
        }
        // beacon glare: a natural flash, not a blinking square. Brightness
        // and edge blur breathe together on a sharp pulse - at the peak the
        // halo blooms wide and the core overexposes toward white, dazzling
        // enough to hide the octagon's actual corner; at the trough it shrinks
        // to a crisp point. Layered alpha squares approximate the radial glow.
        {
            const float phase = float(g_state->tick) / 60.f * 2.f * kPi / 1.8f + k * 1.3f;
            float pulse = 0.5f + 0.5f * sinf(phase);
            pulse = pulse * pulse * pulse;                 // sharp flash peak
            // never fully dark: a resting ember stays lit between flashes
            const float glow = 0.22f + 0.78f * pulse;
            const float blur = 3.f + 11.f * pulse;         // glare radius
            // theme-coherent: halo is the hot accent, core lifts toward the
            // theme ink (not raw white) so the glare keeps the palette's hue
            const COLORREF halo = DepthInk(t.accentHot, fade * 0.4f);
            const COLORREF core = LerpCol(halo, t.ink, 0.25f + 0.45f * pulse);
            for (int c = 0; c < 8; ++c) {
                const int bx = oct[c].x, by = oct[c].y;
                const int b = int(blur);
                // true radial discs (alpha ellipse): round soft glare, no
                // square body or hard edge at any phase
                gfx_alpha_ellipse(bx - b - 3, by - b - 3, bx + b + 4, by + b + 4,
                                  halo, int((26 + 50 * pulse) * glow));
                gfx_alpha_ellipse(bx - b, by - b, bx + b + 1, by + b + 1,
                                  halo, int((60 + 100 * pulse) * glow));
                const int cs = 1 + int(2 * pulse);
                gfx_alpha_ellipse(bx - cs - 1, by - cs - 1, bx + cs + 2, by + cs + 2,
                                  core, int((120 + 110 * pulse) * glow));
            }
        }
    }

    // the vanishing point: the centre dot every frame is born from
    FillRectCol(S(cx - 2), S(cy - 2), S(cx + 3), S(cy + 3), t.ink);
    FillRectCol(S(cx - 1), S(cy - 1), S(cx + 2), S(cy + 2), t.frame);

    // corner readouts keep the tunnel readable as an instrument
    DrawCells("SOUND MEMOIRS", 24, 20, 0);
    { const char* nm = t.name; DrawCells(nm, 616 - (int)strlen(nm) * 8, 20, 2); }
    DrawCells("DIVE : 06", 24, 36, 3);
    DrawCells(TransportWord(), 616 - (int)strlen(TransportWord()) * 8, 36,
              active && !g_state->libPaused ? 0 : 2);
    {
        char b[48];
        if (g_state->current == kTrackCount) snprintf(b, sizeof(b), "NOW : LOCAL FILE");
        else if (active) snprintf(b, sizeof(b), "NOW : TRACK %02d", g_state->current);
        else snprintf(b, sizeof(b), "NOW : <SILENCE>");
        DrawCells(b, 24, 462, 3);
        char tb[32];
        const int secs = std::max(0, int(g_state->seconds));
        snprintf(tb, sizeof(tb), "%02d:%02d", secs / 60, secs % 60);
        DrawCells(tb, 616 - (int)strlen(tb) * 8, 462, 3);
    }
    // thin progress rail along the very bottom
    FillRectCol(S(24), S(474), S(616), S(476), t.seam);
    {
        const int end = 24 + int(592 * PlayProgress());
        if (end > 24) FillRectCol(S(24), S(474), S(end), S(476), t.bar);
    }
    if (g_state->current == kTrackCount && g_state->libCurrentRow >= 0 &&
        g_state->libCurrentRow < g_libNameCount && g_libNames[g_state->libCurrentRow][0]) {
        textLib(g_libNames[g_state->libCurrentRow], 200, 456, 440, 472, t.digit);
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
    g_mem = &memory;
    g_sceneScale = scale;
    gfx_reset();
    gfx_clear(kColOuter);

    const int layoutMode = s.layoutMode;
    if (s.page == 0) {  // Tracks page: full-page compositions
        if (layoutMode == layout::Field) { PaintFieldLayout(); return; }
        if (layoutMode == layout::Stack) { PaintStackLayout(); return; }
        if (layoutMode == layout::Ring) { PaintRingLayout(); return; }
        if (layoutMode == layout::Cross) { PaintCrossLayout(); return; }
        if (layoutMode == layout::Helix) { PaintHelixLayout(); return; }
        if (layoutMode == layout::Dive) { PaintDiveLayout(); return; }
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

// Width of one LIB name record, so the shell never hardcodes the stride.
int sceneLocalNameWidth() { return kLibNameWidth; }

// Shell entry point: replace the LIB list with `count` fixed-width UTF-8
// records read from `pool` (count * kLibNameWidth bytes). Names are copied
// on code-point boundaries so a truncated CJK name never ends mid-sequence.
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
            int n = 0;
            while (n < kLibNameWidth - 1) {
                const unsigned char c = (unsigned char)src[n];
                if (c == 0) break;
                const int len = (c < 0x80) ? 1 : (c >= 0xF0) ? 4
                              : (c >= 0xE0) ? 3 : (c >= 0xC0) ? 2 : 1;
                if (n + len > kLibNameWidth - 1) break;   // keep whole glyphs
                n += len;
            }
            std::memcpy(g_libNames[i], src, (std::size_t)n);
            g_libNames[i][n] = 0;
        } else {
            g_libNames[i][0] = 0;
        }
    }
}
