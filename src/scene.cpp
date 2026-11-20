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

// ---- unit-circle lookup tables --------------------------------------------
// Hot loops (moire rings, 64 segments per ring) must not call cosf/sinf per
// vertex. Two fixed resolutions cover every caller; the 32-point table is
// enough for small radii, where adjacent samples already land on the same
// pixel.
template <int N>
struct UnitCircleLut {
    float c[N + 1];
    float s[N + 1];
    UnitCircleLut() {
        constexpr float kTwoPi = 6.283185307179586f;
        for (int i = 0; i <= N; ++i) {
            const float a = kTwoPi * i / float(N);
            c[i] = std::cos(a);
            s[i] = std::sin(a);
        }
    }
};
static const UnitCircleLut<64> kCircle64;
static const UnitCircleLut<32> kCircle32;

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

static double PlayProgress() {
    if (g_state->current < 0 || g_state->duration <= 0.0) return 0.0;
    return std::clamp(g_state->seconds / g_state->duration, 0.0, 1.0);
}

// Beat impact 0..1 (fast attack / 240 ms release, from the low bands).
static inline float Impact() { return g_mem ? g_mem->impact : 0.f; }

// Transport word for the custom layouts' status corners.
static const char* PlaybackOrderWord() {
    static const char* orders[] = { "ORDER", "ONE", "LOOP", "SHUFFLE" };
    if (g_state->current < 0) return "--";
    return g_state->current == kTrackCount ? orders[std::clamp(g_state->localOrder, 0, 3)] : "TR LOOP";
}

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

// Contrast-boosted chassis colours for the CUSTOM layouts on light themes:
// the ORIGINAL palette's outer/panel/strip/frame sit within a few points of
// each other, so light themes sink the panel slightly, press the frame into
// a crisp teal-ink line and deepen the meter strips. Dark themes unchanged.
static inline COLORREF CustomPanelCol() {
    const Theme& t = Th();
    return LightTheme() ? LerpCol(t.panel, t.ink, 0.05f) : t.panel;
}
static inline COLORREF CustomFrameCol() {
    const Theme& t = Th();
    return LightTheme() ? LerpCol(t.frame, t.ink, 0.42f) : t.frame;
}
static inline COLORREF CustomStripCol() {
    const Theme& t = Th();
    return LightTheme() ? LerpCol(t.strip, t.ink, 0.32f) : t.strip;
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

// Recessed instrument window: sunken panel fill inside a 1px chassis frame.
// On light themes CustomPanelCol/CustomFrameCol pull the two apart so the
// window reads as a real recess instead of a flat wash.
static void InsetPanel(int x, int y, int r, int b) {
    const Theme& t = Th();
    const COLORREF panel = CustomPanelCol();
    const COLORREF frame = LerpCol(CustomFrameCol(), panel, 0.32f);
    FillRectCol(S(x), S(y), S(r), S(b), panel);
    FillRectCol(S(x), S(y), S(r), S(y + 1), frame);
    FillRectCol(S(x), S(b - 1), S(r), S(b), frame);
    FillRectCol(S(x), S(y), S(x + 1), S(b), frame);
    FillRectCol(S(r - 1), S(y), S(r), S(b), frame);
    // 1px physical inner fold: the glass sits *inside* the metal chamfer.
    const COLORREF innerShade = LightTheme()
        ? LerpCol(panel, t.ink, 0.12f)
        : LerpCol(panel, RGB(0, 0, 0), 0.35f);
    FillRectCol(S(x + 1), S(y + 1), S(r - 1), S(y + 2), innerShade);
    FillRectCol(S(x + 1), S(y + 1), S(x + 2), S(b - 1), innerShade);
}

// Four L-shaped registration corners inside a recessed window. Single source
// of truth for the chassis marks: every caller paints the same eight arms, so
// a layout can no longer drop three corners through a hand-rolled loop.
static void PaintCornerBrackets(int l, int t, int r, int b, int arm) {
    const COLORREF fc = CustomFrameCol();
    panelBox(l, t, l + arm, t + 1, fc);         panelBox(l, t, l + 1, t + arm, fc);
    panelBox(r - arm, t, r, t + 1, fc);         panelBox(r - 1, t, r, t + arm, fc);
    panelBox(l, b - 1, l + arm, b, fc);         panelBox(l, b - arm, l + 1, b, fc);
    panelBox(r - arm, b - 1, r, b, fc);         panelBox(r - 1, b - arm, r, b, fc);
}

// ---- handheld-scope hardware: graticule + telemetry bay -------------------
static void DrawCellsTL(const char* text, int x, int y, int palette);
// The custom 3D layouts share one instrument chassis: a left scope window
// (16..524) with a printed graticule, and a right telemetry bay (532..624).
// Signal motion is mapped onto sliding cursors on the rails instead of moving
// the whole body, which is what kept the old versions looking like screensavers.
static void PaintScopeGraticule(int l, int t, int r, int b,
                                bool drawCross, bool showPan, bool showCent) {
    const Theme& th = Th();
    const int cx = (l + r) / 2, cy = (t + b) / 2;
    const COLORREF tickC = LerpCol(CustomFrameCol(), CustomPanelCol(), 0.35f);
    const COLORREF axisC = LerpCol(CustomFrameCol(), CustomPanelCol(), 0.58f);

    // four-sided scale: 3px tick every 12px, 6px major every 48px
    for (int x = l + 12, i = 1; x < r - 8; x += 12, ++i) {
        const int len = (i % 4 == 0) ? 6 : 3;
        FillRectCol(S(x), S(t + 1), S(x + 1), S(t + 1 + len), tickC);
        FillRectCol(S(x), S(b - 1 - len), S(x + 1), S(b - 1), tickC);
    }
    for (int y = t + 12, i = 1; y < b - 8; y += 12, ++i) {
        const int len = (i % 4 == 0) ? 6 : 3;
        FillRectCol(S(l + 1), S(y), S(l + 1 + len), S(y + 1), tickC);
        FillRectCol(S(r - 1 - len), S(y), S(r - 1), S(y + 1), tickC);
    }

    // optional faint crosshair, 14px clear at the centre
    if (drawCross) {
        FillRectCol(S(l + 10), S(cy), S(cx - 7), S(cy + 1), axisC);
        FillRectCol(S(cx + 8), S(cy), S(r - 10), S(cy + 1), axisC);
        FillRectCol(S(cx), S(t + 10), S(cx + 1), S(cy - 7), axisC);
        FillRectCol(S(cx), S(cy + 8), S(cx + 1), S(b - 10), axisC);
    }

    // top-edge PAN cursor (stereo image)
    if (showPan && g_mem) {
        const int px = cx + int(std::clamp(g_mem->pan, -1.f, 1.f) * float((r - l) / 2 - 28));
        const COLORREF pc = std::abs(g_mem->pan) > 0.28f ? th.accentHot : th.bar;
        FillRectCol(S(px - 4), S(t + 1), S(px + 5), S(t + 4), pc);
        FillRectCol(S(px), S(t + 4), S(px + 1), S(t + 8), pc);
    }

    // right-edge CENT cursor (spectral centroid: low at the bottom)
    if (showCent && g_mem) {
        const float cent = std::clamp(g_mem->centroid, 0.f, 1.f);
        const int py = (b - 14) - int(cent * float((b - t) - 28));
        FillRectCol(S(r - 4), S(py - 3), S(r - 1), S(py + 4), th.bar);
        FillRectCol(S(r - 8), S(py), S(r - 4), S(py + 1), th.bar);
    }

    PaintCornerBrackets(l + 6, t + 6, r - 6, b - 6, 8);
}

// Right-hand telemetry bay: four strictly separated zones, no overlapping
// text or rules. Shared by every handheld-scope layout so the chassis reads
// as one product line.
static void PaintScopeTelemetryBay(const char* modeTag, const char* subTag) {
    const Theme& t = Th();
    const int bx = 532, br = 624;
    InsetPanel(bx, 34, br, 446);
    const COLORREF sep = LerpCol(CustomFrameCol(), CustomPanelCol(), 0.45f);

    // Zone 1 (y 40..84): mode code + TRIG latch lamp
    DrawCellsTL(modeTag, bx + 6, 40, 2);
    DrawCellsTL(subTag, bx + 6, 54, 3);
    const bool trig = Impact() > 0.08f;
    FillRectCol(S(bx + 6), S(70), S(bx + 12), S(76), CustomFrameCol());
    FillRectCol(S(bx + 7), S(71), S(bx + 11), S(75), trig ? t.accentHot : t.seam);
    DrawCellsTL("TRIG", bx + 16, 68, trig ? 0 : 2);
    FillRectCol(S(bx + 4), S(84), S(br - 4), S(85), sep);

    // Zone 2 (y 90..284): L/R meters with CLIP lamps and peak holds
    const int mBot = 266;  // 28 cells x 6px = 168px (y 98..266)
    for (int side = 0; side < 2; ++side) {
        const int x = side ? 588 : 548;
        const bool clip = g_mem && g_mem->clipHold[side] > 0.f;
        FillRectCol(S(x + 4), S(90), S(x + 10), S(96), CustomFrameCol());
        FillRectCol(S(x + 5), S(91), S(x + 9), S(95), clip ? t.accentHot : t.seam);
        const float drive = g_mem ? g_mem->meter[side] : 0.f;
        const int n = std::clamp(int(drive * 28.f), 0, 28);
        for (int i = 0; i < n; ++i) {
            const int y = mBot - i * 6;
            FillRectCol(S(x), S(y - 4), S(x + 14), S(y),
                        i > 21 ? t.accentHot : CustomStripCol());
        }
        for (int i = n; i < 28; i += 4) {
            const int y = mBot - i * 6;
            FillRectCol(S(x + 4), S(y - 1), S(x + 10), S(y),
                        LerpCol(t.seam, CustomPanelCol(), 0.35f));
        }
        if (g_mem && g_mem->meterPeak[side] > 0.02f) {
            const int py = mBot - int(g_mem->meterPeak[side] * 168.f);
            FillRectCol(S(x), S(py - 2), S(x + 14), S(py), t.accentHot);
        }
        DrawCellsTL(side ? "R" : "L", x + 3, 270, 2);
    }
    // centre-gap dB reference ticks
    for (int i = 0; i < 4; ++i) {
        const int y = 104 + i * 48;
        FillRectCol(S(568), S(y), S(582), S(y + 1), sep);
    }
    FillRectCol(S(bx + 4), S(286), S(br - 4), S(287), sep);

    // Zone 3 (y 292..354): LOW / MID / HI bars
    const float bands[3] = {
        g_mem ? g_mem->lowSustain : 0.f,
        g_mem ? g_mem->midSustain : 0.f,
        g_mem ? g_mem->highSpark : 0.f
    };
    static const char* bTag[3] = { "LO", "MD", "HI" };
    for (int i = 0; i < 3; ++i) {
        const int y = 294 + i * 20;
        DrawCellsTL(bTag[i], bx + 6, y, 2);
        FillRectCol(S(bx + 28), S(y + 2), S(br - 6), S(y + 8), t.seam);
        const int w = int(std::clamp(bands[i], 0.f, 1.f) * float((br - 6) - (bx + 28)));
        if (w > 0) FillRectCol(S(bx + 28), S(y + 2), S(bx + 28 + w), S(y + 8),
                               i == 0 ? t.bar : i == 1 ? t.wave : t.accentHot);
    }
    FillRectCol(S(bx + 4), S(356), S(br - 4), S(357), sep);

    // Zone 4 (y 362..442): numeric telemetry
    char buf[16];
    const int corr = g_mem ? int(g_mem->correlation * 99.f) : 0;
    snprintf(buf, sizeof(buf), "COR%c%02d", corr < 0 ? '-' : '+', std::abs(corr));
    DrawCellsTL(buf, bx + 6, 364, 3);
    const int panV = g_mem ? int(g_mem->pan * 99.f) : 0;
    snprintf(buf, sizeof(buf), "PAN%c%02d", panV < 0 ? '-' : '+', std::abs(panV));
    DrawCellsTL(buf, bx + 6, 382, 3);
    const int cenV = g_mem ? int(g_mem->centroid * 99.f) : 0;
    snprintf(buf, sizeof(buf), "CEN:%02d", cenV);
    DrawCellsTL(buf, bx + 6, 400, 2);
    DrawCellsTL(PlaybackOrderWord(), bx + 6, 424, 2);
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

// Top-left anchored cell text: DrawCells subtracts (4,5) internally (its
// callers pass centre-ish anchors); TL callers pass the exact glyph box
// origin so text never straddles a rule or chews through a frame.
static inline void DrawCellsTL(const char* text, int x, int y, int palette = 0) {
    DrawCells(text, x + 4, y + 5, palette);
}

// ---- retro-hardware primitives ---------------------------------------------
// Oscilloscope trigger: first rising zero-crossing in the head of the window
// (Tektronix-style lock - a steady tone stands still instead of jittering).
static int TriggerOffset(const float* wave) {
    for (int i = 1; i <= 60; ++i)
        if (wave[i - 1] <= 0.f && wave[i] > 0.f) return i;
    return 0;
}

// Stereo phase correlation meter: -1 .. 0 .. +1 slot with a centre-out
// cursor; out-of-phase (< -0.2) warns in accentHot.
static void PaintCorrelationBar(int x, int y, int w) {
    const Theme& t = Th();
    FillRectCol(S(x), S(y), S(x + w), S(y + 6), t.seam);
    const int mid = x + w / 2;
    FillRectCol(S(mid), S(y - 1), S(mid + 1), S(y + 7), t.frame);
    DrawCellsTL("-1", x - 18, y - 2, 2);
    DrawCellsTL("+1", x + w + 4, y - 2, 2);
    const float corr = g_mem ? g_mem->correlation : 0.f;
    const int reach = int(corr * (w / 2 - 2));
    const COLORREF cc = corr < -0.2f ? t.accentHot : t.bar;
    if (reach >= 0) FillRectCol(S(mid + 1), S(y + 1), S(mid + 1 + reach), S(y + 5), cc);
    else FillRectCol(S(mid + reach), S(y + 1), S(mid), S(y + 5), cc);
}

// CLIP latch LED: a restrained 4x4 hardware lamp inside a 6x6 frame, with a
// small PK label to the right - no more clunky text-on-colour-box.
static void PaintClipLed(int x, int y, int side) {
    const Theme& t = Th();
    const bool hot = g_mem && g_mem->clipHold[side] > 0.f;
    FillRectCol(S(x), S(y), S(x + 6), S(y + 6), t.frame);
    FillRectCol(S(x + 1), S(y + 1), S(x + 5), S(y + 5), hot ? t.accentHot : t.seam);
    DrawCellsTL("PK", x + 10, y - 2, hot ? 3 : 2);
}

// ---- shared custom-layout chassis -----------------------------------------
// Every custom composition mounts into the same chassis: a solid header
// strip (y 0..28) and a footer strip (y 448..480). All text uses DrawCellsTL
// (exact glyph-box origin) so nothing straddles a rule or chews a frame.
void PaintCustomHeader(const char* layoutCode) {
    const Theme& t = Th();
    FillRectCol(0, 0, S(kLogicalW), S(28), CustomPanelCol());
    FillRectCol(0, S(27), S(kLogicalW), S(28), LerpCol(CustomFrameCol(), CustomPanelCol(), 0.4f));
    // machine plate: 3px signal block + heavy title + composition code
    FillRectCol(S(12), S(8), S(15), S(20), t.bar);
    DrawCellsTL("SOUND MEMOIRS", 22, 9, 0);
    DrawCellsTL(layoutCode, 136, 9, 2);
    // right: theme name + transport badge with a play indicator lamp
    const char* nm = t.name;
    DrawCellsTL(nm, 536 - (int)strlen(nm) * 8, 9, 2);
    const char* tw = TransportWord();
    const bool playing = g_state->current >= 0 && !g_state->libPaused && !g_state->loading;
    const int tx = 616 - (int)strlen(tw) * 8;
    DrawCellsTL(tw, tx, 9, playing ? 0 : 2);
    const bool blink = (g_state->tick % 30) < 20;
    if (playing || (g_state->libPaused && blink))
        FillRectCol(S(tx - 12), S(12), S(tx - 8), S(16), t.accentHot);
}

void PaintCustomFooter(bool showGridHint) {
    const Theme& t = Th();
    FillRectCol(0, S(452), S(kLogicalW), S(kLogicalH), CustomPanelCol());
    FillRectCol(0, S(452), S(kLogicalW), S(453), LerpCol(CustomFrameCol(), CustomPanelCol(), 0.4f));
    // full-width seek rail: y 454..456, playhead 453..457 (never reaches text)
    FillRectCol(S(16), S(454), S(624), S(456), t.seam);
    const int end = 16 + int(608 * PlayProgress());
    if (end > 16) FillRectCol(S(16), S(454), S(end), S(456), t.bar);
    const bool blink = (g_state->tick % 30) < 20;
    if (g_state->current >= 0 && (!g_state->libPaused || blink))
        FillRectCol(S(end - 1), S(453), S(end + 2), S(457),
                    LerpCol(t.accentHot, t.ink, 0.25f));
    // single clean text line at y 460..471: NOW + hint left, SEL + time right
    if (g_state->current == kTrackCount && g_state->libCurrentRow >= 0 &&
        g_state->libCurrentRow < g_libNameCount && g_libNames[g_state->libCurrentRow][0]) {
        DrawCellsTL("NOW :", 16, 460, 3);
        textLib(g_libNames[g_state->libCurrentRow], 64, 460, 336, 471, t.digit);
    } else {
        char b[48];
        if (g_state->current == kTrackCount) snprintf(b, sizeof(b), "NOW : LOCAL FILE");
        else if (g_state->current >= 0) snprintf(b, sizeof(b), "NOW : TRACK %02d", g_state->current);
        else snprintf(b, sizeof(b), "NOW : <SILENCE>");
        DrawCellsTL(b, 16, 460, 3);
    }
    const unsigned idle = unsigned(g_state->tick) - unsigned(g_state->railActivityTick);
    if (idle < 150 || g_state->current < 0)
        DrawCellsTL(showGridHint ? "TAB GRID / F9" : "F9 LAYOUT", 346, 460, 2);
    {
        char sb[16];
        if (g_state->selected == kExitIndex) snprintf(sb, sizeof(sb), "SEL:EXIT");
        else snprintf(sb, sizeof(sb), "SEL:TR:%02d", g_state->selected);
        DrawCellsTL(sb, 468, 460, 2);
    }
    {
        char tb[32];
        const int secs = std::max(0, int(g_state->seconds));
        snprintf(tb, sizeof(tb), "%02d:%02d", secs / 60, secs % 60);
        DrawCellsTL(tb, 616 - (int)strlen(tb) * 8, 460, 3);
    }
}

// ---- reversing spec plate + layout-switch calibration sweep ---------------
// A solid plate with inverted text is the loudest quiet element available:
// one per layout is enough to anchor the hierarchy.
static void PaintSpecPlate(int x, int y, const char* text, bool hot = false) {
    const Theme& t = Th();
    const int w = (int)std::strlen(text) * 8 + 10;
    const COLORREF bg = hot ? t.accentHot
                            : LerpCol(CustomFrameCol(), CustomPanelCol(), 0.25f);
    FillRectCol(S(x), S(y), S(x + w), S(y + 13), bg);
    FillRectCol(S(x), S(y), S(x + 3), S(y + 13), hot ? t.ink : t.bar);
    DrawCellsTL(text, x + 6, y + 1, hot ? 1 : 0);
}

int g_lastCustomLayout = -1;
unsigned g_layoutSwitchTick = 0;

// Ten-frame horizontal calibration line on every custom-layout change; the
// plate flashes for the first six frames. Deliberately outside the standby
// clocks: it is a UI acknowledgement, not music.
static void PaintLayoutSwitchSweep() {
    if (!g_state) return;
    if (g_state->layoutMode != g_lastCustomLayout) {
        g_lastCustomLayout = g_state->layoutMode;
        g_layoutSwitchTick = g_state->tick;
    }
    const unsigned age = g_state->tick - g_layoutSwitchTick;
    if (age >= 10) return;
    const Theme& t = Th();
    const int y = 34 + int(age) * 41;
    FillRectCol(S(16), S(y), S(624), S(y + 2), t.bar);
    if (age < 6) PaintSpecPlate(24, 38, "CAL // LOCK", age < 2);
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
    PaintCornerBrackets(22, 40, 618, 440, 10);

    // ---- upper spectrum field (y 44..316): 43 flat two-tone columns -------
    const int baseY = 316;
    // sparse dB reference dotted lines + 8 faint band-divider verticals
    const COLORREF ref = LerpCol(t.seam, CustomPanelCol(), 0.30f);
    for (int r = 0; r < 3; ++r) {
        const int y = baseY - 62 * (r + 1);
        for (int x = 28; x < 612; x += 8)
            FillRectCol(S(x), S(y), S(x + 2), S(y + 1), ref);
    }
    const COLORREF divider = LerpCol(CustomPanelCol(), t.seam, 0.55f);
    for (int i = 0; i < 8; ++i) {
        const int x = 48 + i * 78;
        FillRectCol(S(x), S(56), S(x + 1), S(baseY), divider);
    }
    const int cols = memoirs::kSpectrumBands;
    const int pitch = 14, barW = 12;
    const int x0 = (kLogicalW - cols * pitch) / 2;
    // Backlit spectrum silhouette with a separate peak trace. Keep the
    // large fill quiet so short transients read against the sustained body.
    GfxPoint profile[memoirs::kSpectrumBands + 2], peaks[memoirs::kSpectrumBands];
    const COLORREF body = LerpCol(CustomPanelCol(), t.wave, 0.23f);
    for (int k = 0; k < cols; ++k) {
        const int x = x0 + k * pitch + barW / 2;
        const int y = baseY - int(252 * Lv(k));
        const int peakY = baseY - int(252 * stage::bandLevel(g_state->spectrum.barsPrev[k]));
        profile[k] = { (std::int16_t)S(x), (std::int16_t)S(y) };
        peaks[k] = { (std::int16_t)S(x), (std::int16_t)S(std::min(y, peakY)) };
    }
    profile[cols] = { profile[cols-1].x, (std::int16_t)S(baseY) };
    profile[cols+1] = { profile[0].x, (std::int16_t)S(baseY) };
    gfx_polygon(profile, cols+2, body);
    gfx_polyline(peaks, cols, LerpCol(t.wave, CustomPanelCol(), 0.35f), S(1));
    gfx_polyline(profile, cols, t.bar, S(2));
    // beat: the baseline steps 1px -> 3px solid accent; HF spark lights the
    // frequency ticks below (three-band division of labour)
    if (imp > 0.08f)
        FillRectCol(S(24), S(baseY), S(616), S(baseY + 3), t.accentHot);
    else
        FillRectCol(S(24), S(baseY), S(616), S(baseY + 1),
                    LerpCol(CustomFrameCol(), CustomPanelCol(), 0.35f));
    for (int i = 0; i < 8; ++i) {
        const int x = 48 + i * 78;
        FillRectCol(S(x), S(baseY + 3), S(x + 1), S(baseY + 7),
                    spark > 0.15f ? t.accentHot : LerpCol(CustomFrameCol(), CustomPanelCol(), 0.45f));
        const int len = (int)strlen(kFreqLabel[i]);
        DrawCellsTL(kFreqLabel[i], x - 4 * len, baseY + 8, 4);
    }

    // ---- mid telemetry belt (y 336..352): LOW/MID/HI mini bars + CORR -----
    {
        FillRectCol(S(24), S(334), S(616), S(335), LerpCol(CustomFrameCol(), CustomPanelCol(), 0.45f));
        FillRectCol(S(24), S(352), S(616), S(353), LerpCol(CustomFrameCol(), CustomPanelCol(), 0.45f));
        static const char* tag[3] = { "LOW", "MID", "HI" };
        static const int lo[3] = { 0, 12, 28 }, hi[3] = { 7, 23, 42 };
        for (int b = 0; b < 3; ++b) {
            float m = 0.f;
            for (int k = lo[b]; k <= hi[b]; ++k) m = std::max(m, Lv(k));
            const int x = 24 + b * 66;
            DrawCellsTL(tag[b], x, 338, 2);
            FillRectCol(S(x + 26), S(341), S(x + 62), S(347), t.seam);
            const int w = int(m * 36.f);
            if (w > 0) FillRectCol(S(x + 26), S(341), S(x + 26 + w), S(347),
                                   b == 0 ? t.bar : b == 1 ? t.wave : t.accentHot);
        }
        PaintCorrelationBar(486, 340, 96);
    }

    // ---- lower independent stereo scope slot (y 360..438) -----------------
    // L above, R below; 2px traces that never cross the spectrum columns.
    GfxPoint pts[120];
    for (int side = 0; side < 2; ++side) {
        const float* wv = side ? g_state->spectrum.waveR : g_state->spectrum.waveL;
        const int cy = side ? 416 : 380;
        const int off = TriggerOffset(wv);
        FillRectCol(S(56), S(cy), S(616), S(cy + 1), LerpCol(t.seam, CustomPanelCol(), 0.30f));
        for (int i = 0; i < 120; ++i) {
            const float v = std::clamp(wv[(off + i * 3 / 2) % 240] / 64.0f, -1.f, 1.f);
            pts[i].x = (std::int16_t)S(56 + i * 560 / 119);
            pts[i].y = (std::int16_t)S(cy + int(v * 18));
        }
        gfx_polyline(pts, 120,
                     side ? LerpCol(t.wave, t.bar, 0.40f)
                          : LerpCol(t.wave, t.accentHot, imp * 0.6f), 2);
        DrawCellsTL(side ? "CH:R" : "CH:L", 22, cy - 5, 2);
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
    const int mBot = 420;                            // 60 cells of 6px
    for (int side = 0; side < 2; ++side) {
        const int x1 = side ? 48 : 18, x2 = x1 + 14;
        const float drive = g_mem ? g_mem->meter[side] : 0.f;
        const int n = std::clamp(int(drive * 60.0f), 0, 60);
        for (int i = 0; i < n; ++i) {
            const int y2 = mBot - i * 6;
            FillRectCol(S(x1), S(y2 - 5), S(x2), S(y2),
                        i > 44 ? t.accentHot : CustomStripCol());
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
    // dB scale in the gutter between the meters, with tiny numeric labels
    {
        const int db[4] = { 0, 6, 12, 24 };
        for (int i = 0; i < 4; ++i) {
            const float lv = std::pow(10.f, -db[i] / 20.f);
            const int y = mBot - int(lv * 360.f);
            FillRectCol(S(34), S(y), S(46), S(y + 1), LerpCol(CustomFrameCol(), CustomPanelCol(), 0.30f));
        }
        // bay head: a clean CLIP label above two 4x4 L/R latch LEDs
        DrawCellsTL("CLIP", 24, 38, 2);
        for (int side = 0; side < 2; ++side) {
            const bool hot = g_mem && g_mem->clipHold[side] > 0.f;
            const int x = 24 + side * 22;
            FillRectCol(S(x), S(52), S(x + 6), S(58), CustomFrameCol());
            FillRectCol(S(x + 1), S(53), S(x + 5), S(57), hot ? t.accentHot : t.seam);
        }
        // chassis micro-corners inside the bay
        PaintCornerBrackets(16, 66, 64, 442, 6);
    }

    // ---- centre display: the protagonist (x 76..452, 376px wide) ----------
    const int cl = 76, ct = 34, cr = 452, cb = 446;
    InsetPanel(cl, ct, cr, cb);
    // compact sub-header strip: SPEC identity + live CORR / PEAK telemetry
    {
        DrawCellsTL("SPEC // 43-BAND", cl + 6, 40, 2);
        char b[24];
        const int corr = g_mem ? int(g_mem->correlation * 99.f) : 0;
        snprintf(b, sizeof(b), "COR%c%02d", corr < 0 ? '-' : '+', std::abs(corr));
        DrawCellsTL(b, cr - 132, 40, 3);
        snprintf(b, sizeof(b), "PK%02d", g_mem ? int(g_mem->meterPeak[0] * 99.f) : 0);
        DrawCellsTL(b, cr - 56, 40, 3);
        FillRectCol(S(cl + 4), S(53), S(cr - 4), S(54), LerpCol(CustomFrameCol(), CustomPanelCol(), 0.45f));
    }
    const int baseY = 344;
    const int cols = memoirs::kSpectrumBands;
    const int pitch = 8, barW = 6;                   // near-original proportions
    const int x0 = cl + 1 + ((cr - cl - 2) - cols * pitch) / 2;
    // sparse dB reference dotted lines + 8 faint band-divider verticals
    const COLORREF ref = LerpCol(t.seam, CustomPanelCol(), 0.30f);
    for (int r = 0; r < 3; ++r) {
        const int y = baseY - 70 * (r + 1);
        for (int x = cl + 8; x < cr - 8; x += 8)
            FillRectCol(S(x), S(y), S(x + 2), S(y + 1), ref);
    }
    const COLORREF divider = LerpCol(CustomPanelCol(), t.seam, 0.55f);
    for (int i = 0; i < 8; ++i) {
        const int x = x0 + 2 + i * 43;
        FillRectCol(S(x), S(60), S(x + 1), S(baseY), divider);
        FillRectCol(S(x), S(baseY + 3), S(x + 1), S(baseY + 6),
                    LerpCol(CustomFrameCol(), CustomPanelCol(), 0.40f));
    }
    for (int k = 0; k < cols; ++k) {
        const float lv = Lv(k);
        const int x1 = x0 + k * pitch, x2 = x1 + barW;
        const int up = int(lv * 268);
        const int prevUp = int(stage::bandLevel(g_state->spectrum.barsPrev[k]) * 268);
        for (int y = baseY - up; y < baseY; y += 3)
            FillRectCol(S(x1), S(y), S(x2), S(std::min(y + 2, baseY)), t.bar);
        for (int y = baseY - prevUp; y < baseY - up; y += 3)
            FillRectCol(S(x1), S(y), S(x2), S(std::min(y + 2, baseY - up)), t.barLit);
    }
    if (imp > 0.08f)
        FillRectCol(S(cl + 4), S(baseY), S(cr - 4), S(baseY + 3), t.accentHot);
    else
        FillRectCol(S(cl + 4), S(baseY), S(cr - 4), S(baseY + 1),
                    LerpCol(CustomFrameCol(), CustomPanelCol(), 0.35f));

    // lower 90px: independent stereo scope area
    FillRectCol(S(cl + 4), S(352), S(cr - 4), S(353), LerpCol(t.frame, t.panel, 0.45f));
    GfxPoint pts[120];
    for (int side = 0; side < 2; ++side) {
        const float* wv = side ? g_state->spectrum.waveR : g_state->spectrum.waveL;
        const int cy = side ? 420 : 378;
        const int off = TriggerOffset(wv);
        FillRectCol(S(cl + 34), S(cy), S(cr - 6), S(cy + 1), LerpCol(t.seam, t.panel, 0.30f));
        for (int i = 0; i < 120; ++i) {
            const float v = std::clamp(wv[(off + i * 3 / 2) % 240] / 64.0f, -1.f, 1.f);
            pts[i].x = (std::int16_t)S(cl + 34 + i * (cr - cl - 40) / 119);
            pts[i].y = (std::int16_t)S(cy + int(v * 18));
        }
        gfx_polyline(pts, 120, side ? LerpCol(t.wave, t.bar, 0.45f) : t.wave, 2);
        DrawCells(side ? "R" : "L", cl + 8, cy - 5, 2);
    }
    // ---- right: compact track index (x 460..628, 26% of the width) --------
    InsetPanel(456, 34, 628, 446);
    // rack sub-header + column separator + 5-track index pips
    DrawCellsTL("ARCHIVE // 46", 462, 40, 2);
    FillRectCol(S(460), S(53), S(624), S(54), LerpCol(CustomFrameCol(), CustomPanelCol(), 0.45f));
    FillRectCol(S(540), S(58), S(541), S(430), LerpCol(CustomPanelCol(), t.seam, 0.55f));
    const int listX = 460, listY = 58, colW = 80, rowH = 16;
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
        // index pip every 5th track: rack-panel rhythm marker
        if (i % 5 == 0)
            FillRectCol(S(x + 1), S(y + 6), S(x + 3), S(y + 8),
                        LerpCol(CustomFrameCol(), CustomPanelCol(), 0.25f));
        char b[8];
        snprintf(b, sizeof(b), "TR:%02d", i);
        DrawCellsTL(b, x + 8, y + 3, playing ? 0 : sel ? 0 : 2);
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

    // centre: free circular XY phase scope - no diamond cage, no centre dot;
    // radius-clipped trace + faint crosshair with a hollow 12px centre
    {
        const COLORREF cross = LerpCol(CustomFrameCol(), t.outer, 0.55f);
        FillRectCol(S(cx - 82), S(cy), S(cx - 6), S(cy + 1), cross);
        FillRectCol(S(cx + 6), S(cy), S(cx + 82), S(cy + 1), cross);
        FillRectCol(S(cx), S(cy - 82), S(cx + 1), S(cy - 6), cross);
        FillRectCol(S(cx), S(cy + 6), S(cx + 1), S(cy + 82), cross);
        // circular clip: pull any point beyond r=82 back onto the circle
        auto circ = [&](float dx, float dy, int& ox, int& oy) {
            const float d = std::sqrt(dx * dx + dy * dy);
            if (d > 82.f) { dx *= 82.f / d; dy *= 82.f / d; }
            ox = cx + int(dx); oy = cy - int(dy);
        };
        const float g = g_mem ? g_mem->xyGain : 1.f;
        if (g_mem && g_mem->samples > 0) {
            const int prev = (g_mem->trace + 5) % 6;
            GfxPoint ghost[120];
            for (int i = 0; i < 120; ++i) {
                const GfxPoint& p = g_mem->traces[prev][i];
                int ox, oy;
                circ(p.x * g, p.y * g, ox, oy);
                ghost[i] = { (std::int16_t)S(ox), (std::int16_t)S(oy) };
            }
            gfx_polyline(ghost, 120, DepthInk(t.wave, 0.55f), 1);
        }
        GfxPoint pts2[120];
        for (int i = 0; i < 120; ++i) {
            const float l = g_state->spectrum.waveL[i * 2], r = g_state->spectrum.waveR[i * 2];
            int ox, oy;
            circ((l - r) * .92f * g, (l + r) * .72f * g, ox, oy);
            pts2[i] = { (std::int16_t)S(ox), (std::int16_t)S(oy) };
        }
        gfx_polyline(pts2, 120, t.bar, 1);
    }

    // live outer rim: L/R waveform rings breathe on the boundary circle so
    // the rim itself is alive, not a dead black frame
    {
        const float reach = 6.f + 6.f * imp;
        for (int side = 0; side < 2; ++side) {
            const float* wv = side ? g_state->spectrum.waveR : g_state->spectrum.waveL;
            GfxPoint pts[121];
            for (int i = 0; i <= 120; ++i) {
                const float a = -kPi / 2.f + 2.f * kPi * i / 120;
                const float v = std::clamp(wv[(i % 120) * 2] / 64.f, -1.f, 1.f);
                const int r = 196 + int(v * reach);
                pts[i] = { (std::int16_t)S(cx + int(r * cosf(a))),
                           (std::int16_t)S(cy + int(r * sinf(a))) };
            }
            gfx_polyline(pts, 121, side ? LerpCol(t.wave, t.bar, 0.45f) : t.wave, 1);
        }
    }

    // 46 track ticks further out (r=212, clear of the live waveform rim):
    // selected 3x3 selEdge, playing 5x5 accentHot with a 1px box
    const bool blink = (g_state->tick % 30) < 20;
    for (int i = 0; i < kTrackCount; ++i) {
        const float a = -kPi / 2.f + 2.f * kPi * i / kTrackCount;
        const bool sel = g_state->selected == i;
        const bool playing = g_state->current == i;
        if (playing) {
            const int x = cx + int(212 * cosf(a)), y = cy + int(212 * sinf(a));
            FillRectCol(S(x - 4), S(y - 4), S(x + 5), S(y + 5), CustomFrameCol());
            FillRectCol(S(x - 2), S(y - 2), S(x + 3), S(y + 3),
                        blink ? t.accentHot : t.bar);
        } else {
            dot(a, 212, sel ? 1 : 0,
                sel ? t.selEdge : LerpCol(t.seam, t.ink, 0.45f));
        }
    }

    // bottom-left: compact 8px twin vertical meters with peak-hold ticks
    for (int side = 0; side < 2; ++side) {
        const int x = 24 + side * 14;
        const float drive = g_mem ? g_mem->meter[side] : 0.f;
        const int n = int(drive * 26);
        for (int i = 0; i < n; ++i) {
            const int y = 428 - i * 6;
            FillRectCol(S(x), S(y - 4), S(x + 8), S(y),
                        i > 19 ? t.accentHot : CustomStripCol());
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
        if (g_state->selected == kExitIndex) snprintf(b, sizeof(b), "EXIT");
        else snprintf(b, sizeof(b), "TR:%02d", g_state->selected);
        DrawCells(b, lx, 396, g_state->selected < kTrackCount ? 0 : 2);
        DrawCells(PlaybackOrderWord(), lx, 412, 2);
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
    // main axes: permanently calm 1px chassis lines - never flash or fatten
    // on beats (only the small hub core pulses); precision engineering rules
    const COLORREF axis = CustomFrameCol();
    FillRectCol(S(cx), S(34), S(cx + 1), S(446), axis);
    FillRectCol(S(16), S(cy), S(624), S(cy + 1), axis);
    const COLORREF tickC = LerpCol(CustomFrameCol(), t.outer, 0.35f);
    for (int y = 38; y < 442; y += 8) {
        const int len = (y - 38) % 40 == 0 ? 7 : 3;
        FillRectCol(S(cx - len), S(y), S(cx), S(y + 1), tickC);
        FillRectCol(S(cx + 1), S(y), S(cx + 1 + len), S(y + 1), tickC);
    }
    for (int x = 20; x < 620; x += 8) {
        const int len = (x - 20) % 40 == 0 ? 7 : 3;
        FillRectCol(S(x), S(cy - len), S(x + 1), S(cy), tickC);
        FillRectCol(S(x), S(cy + 1), S(x + 1), S(cy + 1 + len), tickC);
    }

    // ---- top-left protagonist: large 43-band vertical spectrum ------------
    {
        const int lx = 16, rx = 408, baseY = 262;
        InsetPanel(lx, 34, rx, 286);
        const COLORREF ref = LerpCol(t.seam, t.panel, 0.30f);
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
            const int prevUp = int(stage::bandLevel(g_state->spectrum.barsPrev[k]) * 204);
            for (int y = baseY - up; y < baseY; y += 3)
                FillRectCol(S(x1), S(y), S(x2), S(std::min(y + 2, baseY)), t.bar);
            for (int y = baseY - prevUp; y < baseY - up; y += 3)
                FillRectCol(S(x1), S(y), S(x2), S(std::min(y + 2, baseY - up)), t.barLit);
        }
        if (imp > 0.08f)
            FillRectCol(S(lx + 4), S(baseY), S(rx - 4), S(baseY + 3), t.accentHot);
        else
            FillRectCol(S(lx + 4), S(baseY), S(rx - 4), S(baseY + 1),
                        LerpCol(t.frame, t.panel, 0.35f));
        for (int i = 0; i < 8; ++i) {
            const int x = 34 + i * 52;
            FillRectCol(S(x), S(baseY + 3), S(x + 1), S(baseY + 8),
                        spark > 0.15f ? t.accentHot : LerpCol(t.frame, t.panel, 0.45f));
            const int len = (int)strlen(kFreqLabel[i]);
            DrawCellsTL(kFreqLabel[i], x - 4 * len, baseY + 10, 4);
        }
        DrawCellsTL("43 BAND", lx + 6, 40, 2);
        // vertical dB micro-scale on the window's left edge
        static const char* dbTag[3] = { "0", "-12", "-24" };
        for (int i = 0; i < 3; ++i) {
            const int y = baseY - 4 - i * 88;
            DrawCellsTL(dbTag[i], lx + 4, y - 10, 2);
            FillRectCol(S(lx + 2), S(y), S(lx + 6), S(y + 1),
                        LerpCol(CustomFrameCol(), CustomPanelCol(), 0.30f));
        }
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
        DrawCellsTL("XY PHASE", wx + 6, wy + 5, 2);
        if (spark > 0.2f)
            FillRectCol(S(wr - 10), S(wy + 6), S(wr - 6), S(wy + 10), t.accentHot);
    }

    // ---- bottom-left: stacked full-width L / R scopes ----------------------
    {
        GfxPoint pts[120];
        for (int side = 0; side < 2; ++side) {
            const float* wv = side ? g_state->spectrum.waveR : g_state->spectrum.waveL;
            const int midY = side ? 408 : 336;
            const int off = TriggerOffset(wv);
            FillRectCol(S(64), S(midY), S(404), S(midY + 1),
                        LerpCol(t.seam, t.outer, 0.30f));
            // time-grid ticks along the scope centre axis
            for (int x = 64; x <= 404; x += 34)
                FillRectCol(S(x), S(midY - 3), S(x + 1), S(midY + 4),
                            LerpCol(t.seam, t.outer, 0.15f));
            for (int i = 0; i < 120; ++i) {
                const float v = std::clamp(wv[(off + i * 3 / 2) % 240] / 64.0f, -1.f, 1.f);
                pts[i].x = (std::int16_t)S(64 + i * 340 / 119);
                pts[i].y = (std::int16_t)S(midY + int(v * 30));
            }
            gfx_polyline(pts, 120, side ? LerpCol(t.wave, t.bar, 0.40f) : t.wave, 2);
            DrawCellsTL(side ? "CH:R" : "CH:L", 18, midY - 5, 2);
        }
    }

    // ---- bottom-right: spacious transport bay ------------------------------
    {
        const int px = 432;
        DrawCellsTL(TransportWord(), px, 304, g_state->current >= 0 && !g_state->libPaused ? 0 : 2);
        {
            char b[24];
            snprintf(b, sizeof(b), "%s", g_state->current < 0 ? "--" : "LOCAL");
            if (g_state->current >= 0 && g_state->current < kTrackCount) snprintf(b, sizeof(b), "TR:%02d", g_state->current);
            DrawCellsTL(b, px, 322, g_state->current >= 0 ? 0 : 2);
            DrawCellsTL(PlaybackOrderWord(), px + 72, 322, 2);
        }
        DrawCellsTL("EXIT", px, 344, g_state->selected == kExitIndex ? 1 : 2);
        // stereo phase correlation meter (real telemetry, centre-out cursor)
        PaintCorrelationBar(px + 34, 378, 96);
        // horizontal L/R level bars with hard peak-hold ticks + CLIP lamps
        for (int side = 0; side < 2; ++side) {
            const int y = 396 + side * 20;
            const float drive = g_mem ? g_mem->meter[side] : 0.f;
            FillRectCol(S(px + 24), S(y), S(576), S(y + 6), LerpCol(t.seam, t.outer, 0.30f));
            const int w = int(drive * 120.f);
            if (w > 0) FillRectCol(S(px + 24), S(y), S(px + 24 + w), S(y + 6), CustomStripCol());
            if (g_mem && g_mem->meterPeak[side] > 0.02f) {
                const int pxx = px + 24 + int(g_mem->meterPeak[side] * 120.f);
                FillRectCol(S(pxx), S(y - 2), S(pxx + 2), S(y + 8), t.accentHot);
            }
            DrawCellsTL(side ? "R" : "L", px, y - 2, 2);
            PaintClipLed(584, y, side);
        }
    }

    // hub: small diamond badge where the axes cross
    {
        GfxPoint hub[] = { {(std::int16_t)S(cx), (std::int16_t)S(cy - 10)},
                           {(std::int16_t)S(cx + 12), (std::int16_t)S(cy)},
                           {(std::int16_t)S(cx), (std::int16_t)S(cy + 10)},
                           {(std::int16_t)S(cx - 12), (std::int16_t)S(cy)},
                           {(std::int16_t)S(cx), (std::int16_t)S(cy - 10)} };
        gfx_polyline(hub, 5, CustomFrameCol(), 1);
        const bool blink = (g_state->tick % 30) < 20;
        if (g_state->current >= 0 && (!g_state->libPaused || blink))
            FillRectCol(S(cx - 2), S(cy - 2), S(cx + 3), S(cy + 3),
                        imp > 0.08f ? t.accentHot : t.bar);
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

static GfxPoint ScenePoint(float x, float y) {
    return { (std::int16_t)S(int(std::round(x))), (std::int16_t)S(int(std::round(y))) };
}

// ---- composition: SINK descent shaft (HELIX slot) --------------------------
// Independent L/R coils descend on a common orbit inside the original shaft.
void PaintHelixLayout() {
    const Theme& t = Th();
    FillRectCol(0, 0, S(kLogicalW), S(kLogicalH), t.outer);


    // shaft geometry: wide well with regular guide-rail gutters
    const int wl = 176, wr = 464, wt = 34, wb = 446;
    const int cx = 320, pitch = 10;
    const COLORREF wc = CustomFrameCol();
    FillRectCol(S(wl - 14), S(wt), S(wr + 14), S(wb), CustomPanelCol());
    FillRectCol(S(wl - 14), S(wt), S(wl - 13), S(wb), wc);
    FillRectCol(S(wr + 13), S(wt), S(wr + 14), S(wb), wc);
    FillRectCol(S(wl - 14), S(wt), S(wr + 14), S(wt + 1), wc);
    FillRectCol(S(wl - 14), S(wb - 1), S(wr + 14), S(wb), wc);

    gfx_push_clip(S(wl - 14), S(wt + 1), S(wr + 14), S(wb - 1));

    // ---- two-layer parallax geology: the shaft walls scroll WITH the fall -
    // far layer at 0.45x (deep outer wall), near layer at 1.0x (same clock
    // as the DNA), so the descent reads against a moving reference frame.
    const double clock = g_mem ? g_mem->sinkClock : 0.0;
    {
        // far: structural beams every 48px + diagonal wall braces
        const int bgOff = int(std::fmod(clock * 0.45 * pitch, 48.0));
        const COLORREF beam = LerpCol(CustomPanelCol(), t.seam, 0.50f);
        for (int y = wt - 48; y < wb + 48; y += 48) {
            const int yy = y - bgOff + 48;
            if (yy < wt - 4 || yy > wb + 4) continue;
            FillRectCol(S(wl - 6), S(yy), S(wr + 6), S(yy + 1), beam);
            // 18px brace slots on both inner walls
            for (int k = 0; k < 3; ++k) {
                const int by = yy + 8 + k * 6;
                FillRectCol(S(wl - 12 + k * 2), S(by), S(wl - 4 + k * 2), S(by + 1), beam);
                FillRectCol(S(wr + 4 - k * 2), S(by), S(wr + 12 - k * 2), S(by + 1), beam);
            }
        }
        // near: wall ruler ticks + rolling depth tags, locked to the DNA.
        // World-indexed so the pattern scrolls continuously with no wrap jump.
        const double T = clock * pitch;
        const COLORREF nearC = LerpCol(CustomFrameCol(), CustomPanelCol(), 0.35f);
        const int off10 = int(std::fmod(T, 10.0));
        const int base10 = int(std::floor(T / 10.0));
        for (int k = 0; k <= (wb - wt) / 10 + 1; ++k) {
            const int yy = wb - k * 10 - off10;
            if (yy < wt || yy > wb) continue;
            const int len = (base10 + k) % 4 == 0 ? 8 : 4;
            FillRectCol(S(wl - 14), S(yy), S(wl - 14 + len), S(yy + 1), nearC);
            FillRectCol(S(wr + 14 - len), S(yy), S(wr + 14), S(yy + 1), nearC);
        }
        const int off80 = int(std::fmod(T, 80.0));
        const int base80 = int(std::floor(T / 80.0));
        for (int k = 0; k <= (wb - wt) / 80 + 1; ++k) {
            const int yy = wb - k * 80 - off80;
            if (yy < wt + 4 || yy > wb - 12) continue;
            char db[8];
            snprintf(db, sizeof(db), "D-%02d", (base80 + k) & 63);
            DrawCellsTL(db, wl - 8, yy, 2);
            FillRectCol(S(wl - 12), S(yy + 4), S(wl - 10), S(yy + 5), nearC);
        }
    }

    // Two vertical shafts orbit as a rigid pair around the centreline. Each
    // ladder still turns in place around its own shaft.
    struct CoilPoint { float x, y, z; };
    auto coil = [&](float y, int side, int strand) -> CoilPoint {
        const float hand = side ? 1.f : -1.f;
        // Depth-delayed envelope: a drum hit enters at the mouth of the shaft
        // and slides down the coil instead of swelling the whole strand at
        // once. 25% live meter keeps the head responsive.
        float waveEnv = g_mem ? g_mem->meter[side] : 0.f;
        if (g_mem) {
            const float uY = std::clamp((y - wt) / float(wb - wt), 0.f, 1.f);
            const int delay = std::clamp(int(uY * 36.f), 0, stage::Memory::kHist - 1);
            const int idx = (g_mem->histHead - delay + stage::Memory::kHist * 4)
                            % stage::Memory::kHist;
            const float histVal = side ? g_mem->histR[idx] : g_mem->histL[idx];
            waveEnv = 0.25f * g_mem->meter[side] + 0.75f * histVal;
        }
        const double turn = (y-wt)*.049 + side*.8 + clock*(side ? -.17 : .20) + strand*3.14159265;
        const float radius = 24.f + 15.f*waveEnv;
        const double yaw = std::fmod(clock*.16, 6.283185307179586);
        // Stereo width: a decorrelated mix opens the two shafts apart, a mono
        // mix keeps them tight.
        const float corr = g_mem ? g_mem->correlation : 0.f;
        const float orbitR = 46.f + 14.f * (1.f - std::clamp(corr, -1.f, 1.f)) * 0.5f;
        const float orbitX = orbitR*float(std::cos(yaw));
        const float orbitZ = orbitR*float(std::sin(yaw));
        const float x = hand*orbitX + radius*float(std::sin(turn));
        const float depth = hand*orbitZ + radius*float(std::cos(turn));
        return {cx + x, y + .12f*depth, depth/96.f};
    };
    struct Stroke { CoilPoint a,b; int side; bool bond; };
    constexpr int step = 3, margin = 32, stepsPerBond = 4;
    constexpr int maxSteps = (kLogicalH + 2 * margin) / step + 2;
    Stroke strokes[8 * maxSteps + 4 * (maxSteps / stepsPerBond + 1)];
    int used = 0;
    for (int side = 0; side < 2; ++side) {
        const float top = wt - margin;
        CoilPoint prev[] = {coil(top, side, 0), coil(top, side, 1)};
        for (int j = 1; top + j * step <= wb + margin; ++j) {
            const float y = top + j * step;
            for (int strand = 0; strand < 2; ++strand) {
                const CoilPoint end = coil(y, side, strand);
                strokes[used++] = {prev[strand], end, side, false};
                prev[strand] = end;
            }
            if (j % stepsPerBond == 0) {
                strokes[used++] = {prev[0], prev[1], side, true};
            }
        }
    }
    std::sort(strokes, strokes + used, [](const Stroke& a, const Stroke& b) {
        return a.a.z + a.b.z < b.a.z + b.b.z;
    });
    for (int i = 0; i < used; ++i) {
        const Stroke& v = strokes[i];
        const float light = std::clamp(.5f + .25f * (v.a.z + v.b.z), 0.f, 1.f);
        const COLORREF backbone = v.side ? t.bar : LerpCol(t.wave,t.ink,.23f);
        const COLORREF pigment = v.bond ? (v.side ? LerpCol(t.wave,t.ink,.23f) : t.bar) : backbone;
        const float fade = v.bond ? .28f + .40f * (1.f-light) - .22f*(g_mem ? g_mem->coilAttack[v.side] : 0.f) : .04f + .61f * (1.f-light);
        const COLORREF ink = LerpCol(pigment, CustomPanelCol(), fade);
        GfxPoint seg[] = {ScenePoint(v.a.x,v.a.y),ScenePoint(v.b.x,v.b.y)};
        gfx_polyline(seg,2,ink,S(v.bond ? 1 : light>.58f ? 3 : 2));
        if (v.bond) {
            const float dx = v.b.x - v.a.x, dy = v.b.y - v.a.y;
            const float len = std::max(1.f, std::sqrt(dx*dx + dy*dy));
            const float nx = -dy/len, ny = dx/len;
            const float* wave = v.side ? g_state->spectrum.waveR : g_state->spectrum.waveL;
            const int center = std::clamp(int((.5f*(v.a.y + v.b.y)-wt)*239.f/(wb-wt)), 0, 239);
            constexpr int samples = 13;
            GfxPoint trace[samples];
            for (int k = 0; k < samples; ++k) {
                const float u = float(k)/(samples-1);
                const int sample = (center + (k - samples/2)*2 + 240) % 240;
                const float signal = std::clamp(wave[sample]/72.f, -1.f, 1.f);
                const float excursion = 3.8f * signal * std::abs(signal);
                trace[k] = ScenePoint(v.a.x + dx*u + nx*excursion,
                                      v.a.y + dy*u + ny*excursion);
            }
            gfx_polyline(trace,samples,LerpCol(ink,pigment,.48f),S(1));
        }
    }
    gfx_pop_clip();

    // ---- left bay: depth gauge + compact L meter (x 24..156) --------------
    {
        const int gx = 96, gt = 76, gb = 400;
        DrawCellsTL("DEPTH", 62, 42, 2);
        FillRectCol(S(gx), S(gt), S(gx + 1), S(gb), LerpCol(CustomFrameCol(), t.outer, 0.25f));
        for (int i = 0; i <= 10; ++i) {
            const int y = gt + i * (gb - gt) / 10;
            FillRectCol(S(gx - (i % 5 == 0 ? 8 : 4)), S(y), S(gx), S(y + 1),
                        LerpCol(CustomFrameCol(), t.outer, 0.30f));
        }
        // stepped cursor block: 14x7 with a hot top edge
        const int my = gt + int(PlayProgress() * (gb - gt));
        FillRectCol(S(gx - 10), S(my - 3), S(gx + 4), S(my + 4), t.bar);
        FillRectCol(S(gx - 10), S(my - 3), S(gx + 4), S(my - 1), t.accentHot);
        char db[24];
        snprintf(db, sizeof(db), "-%04dM", int(PlayProgress() * 9999));
        DrawCellsTL(db, 54, 412, 3);
        // compact L channel meter beside the gauge
        const int mx = 126, mBot = 400;
        const float drive = g_mem ? g_mem->meter[0] : 0.f;
        const int n = int(drive * 40);
        for (int i = 0; i < n; ++i) {
            const int y = mBot - i * 8;
            FillRectCol(S(mx), S(y - 6), S(mx + 12), S(y), i > 30 ? t.accentHot : CustomStripCol());
        }
        if (g_mem && g_mem->meterPeak[0] > 0.02f) {
            const int py = mBot - int(g_mem->meterPeak[0] * 320.f);
            FillRectCol(S(mx), S(py - 2), S(mx + 12), S(py), t.accentHot);
        }
        DrawCellsTL("L", mx + 2, 408, 2);
    }

    // ---- right bay: R meter + LOW ladder + order (x 484..616) -------------
    {
        const int mx = 490, mBot = 400;
        const float drive = g_mem ? g_mem->meter[1] : 0.f;
        const int n = int(drive * 40);
        for (int i = 0; i < n; ++i) {
            const int y = mBot - i * 8;
            FillRectCol(S(mx), S(y - 6), S(mx + 12), S(y), i > 30 ? t.accentHot : CustomStripCol());
        }
        if (g_mem && g_mem->meterPeak[1] > 0.02f) {
            const int py = mBot - int(g_mem->meterPeak[1] * 320.f);
            FillRectCol(S(mx), S(py - 2), S(mx + 12), S(py), t.accentHot);
        }
        DrawCellsTL("R", mx + 2, 408, 2);
        const int lx = 514, ly = 96;
        DrawCellsTL("LOW", lx, 78, 2);
        for (int k = 0; k < 8; ++k) {
            const int y = ly + k * 10;
            const int w = int(Lv(k) * 100);
            if (w > 0) FillRectCol(S(lx), S(y), S(lx + w), S(y + 4), t.bar);
            const int pw = int(stage::bandLevel(g_state->spectrum.barsPrev[k]) * 100);
            if (pw > w) FillRectCol(S(lx + w), S(y), S(lx + pw), S(y + 4), t.barLit);
        }
        DrawCellsTL(PlaybackOrderWord(), lx, 196, 2);
    }

    PaintCustomHeader("SINK / 05");
    PaintCustomFooter(false);
}

void PaintDiveLayout() {
    const Theme& t = Th();
    const float kPi = 3.14159265f;
    FillRectCol(0, 0, S(kLogicalW), S(kLogicalH), t.outer);
    // the tunnel lives inside the left scope window only; the right bay is
    // the shared telemetry column
    InsetPanel(16, 34, 524, 446);
    gfx_push_clip(S(17), S(35), S(523), S(445));

    const bool active = g_state->current >= 0 && !g_state->libPaused;
    // Vanishing point is locked to the scope centre; the stereo image moves it
    // by at most +-6px instead of swinging the whole body.
    const float pan = g_mem ? g_mem->pan : 0.f;
    const int cx = 270 + int(pan * 6.f), cy = 240;
    const int K = 5;                       // layers in flight: full mid-field

    // fall phase: strictly uniform on the standby-aware dive clock - one
    // layer born every 1/0.32 s. Standby freezes the whole space; only the
    // beacons keep a slow 3 s breathing cycle.
    const double f = g_mem ? g_mem->diveClock : 0.35;
    const int base = int(std::floor(f));
    const float frac = float(f - base);

    struct Frame { float half; float ang; float u; };
    Frame fr[6];                           // K visible + 1 recycled anchor
    const float rot = float(f / 0.32) * 0.24f;   // uniform 0.24 rad/s, frozen in standby
    for (int k = 0; k <= K; ++k) {
        const float u = (k + frac) / K;              // 0 new/far .. >1 left
        const int age = base - k;
        fr[k].u = u;
        // exponential-equal spacing: 14 -> 27 -> 53 -> 104 -> 203 -> 392px,
        // every octave of depth gets one layer (no mid-field hole)
        fr[k].half = std::min(2200.f, 14.f * std::pow(28.f, u));
        fr[k].ang = age * 0.38f + rot;               // stronger inter-layer offset
    }

    const float imp = Impact();
    // nearest visible layer index (largest half still on screen)
    int nearK = -1;
    for (int k = 0; k < K; ++k)
        if (fr[k].half * 0.55f <= 430.f) nearK = k;

    // ---- outer void: 10 deterministic wireframe solids on golden-angle ----
    // slots, always OUTSIDE the octagon wall, approaching on the same
    // exponential perspective and sweeping past the screen edges.
    {
        for (int i = 0; i < 10; ++i) {
            const float uObj = float(std::fmod(f * 0.55 + i / 10.0, 1.0));
            const float theta = i * 2.39996f + rot * 0.4f;
            const float dist = 1.55f + 0.55f * (i % 3);
            const float size = 14.f * std::pow(24.f, uObj);   // same perspective law
            if (size > 900.f) continue;                        // fully passed
            const float ox = cx + dist * size * cosf(theta);
            const float oy = cy + dist * size * sinf(theta);
            if (ox < -260.f || ox > 900.f || oy < -260.f || oy > 740.f) continue;
            const float bandE = Lv((i * 4 + 2) % 43);
            const float s = std::min(130.f, size * (0.28f + 0.14f * bandE));  // solid half-size
            // Calibration targets, not floating debris: quiet solids sink into
            // the frame ink; only a hot band lights a solid vermilion.
            const COLORREF oc = bandE > 0.72f
                                    ? DepthInk(t.accentHot, 0.20f + 0.45f * (1.f - uObj))
                                    : DepthInk(CustomFrameCol(), 0.45f + 0.45f * (1.f - uObj));
            // back face shrinks toward the vanishing point (0.78x)
            auto P = [&](float dx, float dy, float shrink) -> GfxPoint {
                const float px = cx + (ox - cx + dx) * shrink;
                const float py = cy + (oy - cy + dy) * shrink;
                return { (std::int16_t)S(int(px)), (std::int16_t)S(int(py)) };
            };
            auto edge = [&](GfxPoint a, GfxPoint b) {
                GfxPoint seg[2] = { a, b };
                gfx_polyline(seg, 2, oc, S(1));
            };
            const int kind = i % 3;
            if (kind == 0) {
                // perspective cube: front square + shrunken back square + 4 legs
                const GfxPoint f0 = P(-s, -s, 1.f), f1 = P(s, -s, 1.f),
                               f2 = P(s, s, 1.f),  f3 = P(-s, s, 1.f);
                const GfxPoint b0 = P(-s, -s, .78f), b1 = P(s, -s, .78f),
                               b2 = P(s, s, .78f),  b3 = P(-s, s, .78f);
                edge(f0, f1); edge(f1, f2); edge(f2, f3); edge(f3, f0);
                edge(b0, b1); edge(b1, b2); edge(b2, b3); edge(b3, b0);
                edge(f0, b0); edge(f1, b1); edge(f2, b2); edge(f3, b3);
            } else if (kind == 1) {
                // tall narrow monolith
                const float w = s * 0.38f, h = s * 1.5f;
                const GfxPoint f0 = P(-w, -h, 1.f), f1 = P(w, -h, 1.f),
                               f2 = P(w, h, 1.f),  f3 = P(-w, h, 1.f);
                const GfxPoint b0 = P(-w, -h, .78f), b1 = P(w, -h, .78f),
                               b2 = P(w, h, .78f),  b3 = P(-w, h, .78f);
                edge(f0, f1); edge(f1, f2); edge(f2, f3); edge(f3, f0);
                edge(b0, b1); edge(b1, b2); edge(b2, b3); edge(b3, b0);
                edge(f0, b0); edge(f1, b1); edge(f2, b2); edge(f3, b3);
            } else {
                // triangular prism
                const GfxPoint f0 = P(0.f, -s, 1.f), f1 = P(s, s * .8f, 1.f),
                               f2 = P(-s, s * .8f, 1.f);
                const GfxPoint b0 = P(0.f, -s, .78f), b1 = P(s, s * .8f, .78f),
                               b2 = P(-s, s * .8f, .78f);
                edge(f0, f1); edge(f1, f2); edge(f2, f0);
                edge(b0, b1); edge(b1, b2); edge(b2, b0);
                edge(f0, b0); edge(f1, b1); edge(f2, b2);
            }
        }
    }

    // ---- live corridor edges: each layer's 8 vertices connect to the next
    // layer's corresponding vertex (incl. the recycled anchor) as clean
    // two-point struts. The wave work stays on the 240-point inner ring, so
    // the corridor reads as straight architecture instead of a squiggle.
    for (int k = 0; k + 1 <= K; ++k) {
        const Frame& A = fr[k];
        const Frame& B = fr[k + 1];
        const float fade = std::pow(1.f - std::min(1.f, A.u), 1.35f);
        const COLORREF ec = DepthInk(CustomFrameCol(), 0.25f + 0.55f * fade);
        for (int c = 0; c < 8; ++c) {
            const float aa = kPi / 8.f + c * kPi / 4.f;
            GfxPoint seg[2] = {
                ScenePoint(cx + A.half * cosf(aa + A.ang), cy + A.half * sinf(aa + A.ang)),
                ScenePoint(cx + B.half * cosf(aa + B.ang), cy + B.half * sinf(aa + B.ang)) };
            gfx_polyline(seg, 2, ec, S(1));
        }
    }

    // painter's order: far (small, transparent) first, near (large, solid)
    for (int k = 0; k < K; ++k) {
        const Frame& F = fr[k];
        if (F.half * 0.55f > 430.f) continue;
        // depth cue: far layers dissolve into the panel, near layers solid
        const float fade = std::pow(1.f - std::clamp(F.u, 0.f, 1.f), 1.35f);
        const float ca = cosf(F.ang), sa = sinf(F.ang);
        auto pt = [&](float lx, float ly) -> GfxPoint {
            return { (std::int16_t)S(cx + int(lx * ca - ly * sa)),
                     (std::int16_t)S(cy + int(lx * sa + ly * ca)) };
        };
        float ovx[8], ovy[8];
        GfxPoint oct[8];
        for (int c = 0; c < 8; ++c) {
            const float a = kPi / 8.f + c * kPi / 4.f;
            ovx[c] = F.half * cosf(a);
            ovy[c] = F.half * sinf(a);
            oct[c] = pt(ovx[c], ovy[c]);
        }
        // octagon rim: thin 1px cold frame; the nearest layer steps to 2px
        // accentHot on a beat (kept slim - no chunky 3px slab)
        const bool rimHot = imp > 0.08f && k == nearK;
        const int rimW = rimHot ? S(2) : S(1);
        const COLORREF rimC = rimHot ? t.accentHot : DepthInk(CustomFrameCol(), fade);
        for (int c = 0; c < 8; ++c) {
            GfxPoint rim[2] = { oct[c], oct[(c + 1) % 8] };
            gfx_polyline(rim, 2, rimC, rimW);
        }
        // A single luminous scope trace follows the eight faces. Each
        // vertex samples signed PCM so it travels to either side of its rail.
        if (F.half >= 34.f) {
            // Depth delay: only the nearest layer reads the live PCM; the
            // layers behind it replay the quantised snapshot of their own age,
            // so the waveform visibly recedes down the tunnel.
            int hIdx = -1;
            if (g_mem && k != nearK) {
                const int ageSlot = std::clamp(nearK - k, 1, stage::Memory::kDiveLayers - 1);
                hIdx = (g_mem->diveWaveHead - ageSlot + stage::Memory::kDiveLayers * 4)
                       % stage::Memory::kDiveLayers;
            }
            GfxPoint trace[241];
            for (int i=0; i<240; ++i) {
                const int c=i/30, next=(c+1)%8;
                const float u=(i%30)/30.f;
                const float* wave=c%2 ? g_state->spectrum.waveR : g_state->spectrum.waveL;
                float sample=std::clamp(wave[i]/56.f,-1.f,1.f);
                if (hIdx >= 0) sample=float(g_mem->diveWaves[hIdx][i])/127.f;
                const float offset=.135f*sample*std::abs(sample);
                const float radius= .90f-offset;
                trace[i]=pt((ovx[c]+(ovx[next]-ovx[c])*u)*radius,
                            (ovy[c]+(ovy[next]-ovy[c])*u)*radius);
            }
            trace[240]=trace[0];
            gfx_polyline(trace,241,DepthInk(t.bar,fade),S(1));
        }
        // laser beacons: warm accent cross + high-contrast core, fully
        // separated from the cold framework (light themes: ink hairline
        // cross; dark themes: near-white hot core). All offsets S()-scaled.
        {
            const float period = active ? 1.8f : 3.0f;
            const float phase = float(g_state->tick) / 60.f * 2.f * kPi / period + k * 1.3f;
            float pulse = 0.5f + 0.5f * sinf(phase);
            pulse = pulse * pulse * pulse;                 // sharp flash peak
            pulse = std::max(0.20f, pulse);                // resting ember floor
            const COLORREF rayC = LightTheme()
                ? DepthInk(t.ink, fade * 0.6f)
                : DepthInk(t.accentHot, fade);
            const COLORREF coreC = LightTheme()
                ? DepthInk(t.accentHot, fade * 0.5f)
                : DepthInk(LerpCol(t.accentHot, RGB(255, 255, 255), 0.55f), fade * 0.6f);
            const int len = S(int(2 + 8 * pulse * std::min(1.f, F.u)));
            const int core = pulse > 0.5f ? S(2) : S(1);
            const int rayW = S(1);
            for (int c = 0; c < 8; ++c) {
                const int bx = oct[c].x, by = oct[c].y;
                FillRectCol(bx - len, by, bx + len + 1, by + rayW, rayC);
                FillRectCol(bx, by - len, bx + rayW, by + len + 1, rayC);
                FillRectCol(bx - core + rayW, by - core + rayW, bx + core + 1, by + core + 1, coreC);
            }
        }
    }

    // fixed optical aiming ring: a hard reference circle that never moves
    {
        const COLORREF ac = LerpCol(CustomFrameCol(), CustomPanelCol(), 0.45f);
        GfxPoint aim[33];
        for (int i = 0; i <= 32; ++i)
            aim[i] = ScenePoint(270.f + 42.f * kCircle32.c[i],
                                240.f + 42.f * kCircle32.s[i]);
        gfx_polyline(aim, 33, ac, S(1));
    }
    PaintScopeGraticule(16, 34, 524, 446, true, true, true);
    PaintSpecPlate(24, 42, "OPTIC // 5-GATE", Impact() > 0.18f);

    gfx_pop_clip();
    PaintScopeTelemetryBay("DIVE", "OPT-06");
    PaintCustomHeader("DIVE / 06");
    PaintCustomFooter(false);
    PaintLayoutSwitchSweep();
}

// ---- composition: SONO, SDR-style spectrogram waterfall --------------------
// 43 bands x 88 rows of quantised history printed downward at a uniform
// 15 rows/s. Five discrete hard-edge colour steps (no gradients): chords,
// melody ridges and drum lines read directly off the scroll.
void PaintSonoLayout() {
    const Theme& t = Th();
    FillRectCol(0, 0, S(kLogicalW), S(kLogicalH), t.outer);

    // main waterfall window (the single protagonist)
    InsetPanel(16, 34, 536, 446);
    const int pitch = 12, barW = 10;
    const int x0 = 20;
    // live mini spectrum strip on top (y 40..70): clean, no ghost wall
    {
        const int baseY = 70;
        for (int k = 0; k < 43; ++k) {
            const int x1 = x0 + k * pitch, x2 = x1 + barW;
            const int up = int(Lv(k) * 28);
            const int prevUp = int(stage::bandLevel(g_state->spectrum.barsPrev[k]) * 28);
            for (int y = baseY - up; y < baseY; y += 3)
                FillRectCol(S(x1), S(y), S(x2), S(std::min(y + 2, baseY)), t.bar);
            for (int y = baseY - prevUp; y < baseY - up; y += 3)
                FillRectCol(S(x1), S(y), S(x2), S(std::min(y + 2, baseY - up)), t.barLit);
        }
        // print-head baseline: the live spectrum above prints INTO the
        // history waterfall below - accent line + > < feed markers
        FillRectCol(S(20), S(72), S(532), S(73), t.accentHot);
        DrawCellsTL(">", 22, 66, 0);
        DrawCellsTL("<", 522, 66, 0);
    }
    // waterfall body (y 76..428 = 88 rows x 4px): newest at top, uniform down
    if (g_mem) {
        const int H = stage::Memory::kSonoRows;
        // frac shares the ring-push phase: 3->0 happens on the exact frame
        // sonoHead advances, so the scroll never rebounds
        const int frac = int(std::clamp(g_mem->sonoClock - g_mem->sonoLast, 0.0, 0.999) * 4.0);
        const COLORREF c1 = LerpCol(t.panel, t.seam, 0.85f);
        gfx_push_clip(S(20), S(76), S(532), S(428));
        for (int r = 0; r < H; ++r) {
            const int y = 76 + r * 4 + frac;
            if (y >= 428) break;
            const int idx = (g_mem->sonoHead - 1 - r + H * 4) % H;
            for (int k = 0; k < 43; ++k) {
                const int v = g_mem->sono[idx][k];
                if (v < 20) continue;                     // panel shows through
                const COLORREF c = v <= 70 ? c1 : v <= 135 ? t.wave
                                 : v <= 205 ? t.bar : t.accentHot;
                const int x1 = x0 + k * pitch;
                FillRectCol(S(x1), S(y), S(x1 + barW), S(y + 3), c);
            }
        }
        gfx_pop_clip();
    }
    // frequency ruler under the waterfall (TL-anchored to the bottom edge)
    for (int i = 0; i < 8; ++i) {
        const int x = 34 + i * 68;
        FillRectCol(S(x), S(428), S(x + 1), S(431), LerpCol(t.frame, t.panel, 0.45f));
        const int len = (int)strlen(kFreqLabel[i]);
        DrawCellsTL(kFreqLabel[i], x - 4 * len, 432, 4);
    }
    // plain-language band tags above each axis region, on the window's top
    // margin (mini bars only reach y42 at full scale, tags sit at y36..47)
    {
        static const char* zone[4] = { "SUB", "LOW", "MID", "HIGH" };
        static const int zx[4] = { 60, 160, 296, 452 };
        for (int i = 0; i < 4; ++i) {
            FillRectCol(S(zx[i] - 2), S(36), S(zx[i] + 24), S(47), CustomPanelCol());
            DrawCellsTL(zone[i], zx[i], 36, 2);
        }
    }

    // right telemetry column: strict vertical zones, zero overlap
    InsetPanel(544, 34, 624, 446);
    // zone 1 (y 40..72): identity
    DrawCellsTL("SONO", 552, 42, 2);
    DrawCellsTL("15HZ", 552, 56, 3);
    // time-depth ticks: 4px marks on the panel's left edge only (the -NS
    // labels live inside the waterfall window's right margin, away from meters)
    {
        static const char* marks[3] = { "-2S", "-4S", "-6S" };
        for (int i = 0; i < 4; ++i) {
            const int y = 76 + i * 120;
            FillRectCol(S(544), S(y), S(548), S(y + 1), t.frame);
            if (i > 0) DrawCellsTL(marks[i - 1], 506, y - 5, 2);
        }
    }
    // zone 2 (y 78..300): clip LEDs + 200px L/R meters + labels
    for (int side = 0; side < 2; ++side) {
        const int x = 556 + side * 30;
        const bool clip = g_mem && g_mem->clipHold[side] > 0.f;
        FillRectCol(S(x + 4), S(78), S(x + 10), S(84), t.frame);
        FillRectCol(S(x + 5), S(79), S(x + 9), S(83), clip ? t.accentHot : t.seam);
        const float drive = g_mem ? g_mem->meter[side] : 0.f;
        const int n = int(drive * 33);
        for (int i = 0; i < n; ++i) {
            const int y = 288 - i * 6;
            FillRectCol(S(x), S(y - 4), S(x + 14), S(y), i > 25 ? t.accentHot : CustomStripCol());
        }
        if (g_mem && g_mem->meterPeak[side] > 0.02f) {
            const int py = 288 - int(g_mem->meterPeak[side] * 198.f);
            FillRectCol(S(x), S(py - 2), S(x + 14), S(py), t.accentHot);
        }
        DrawCellsTL(side ? "R" : "L", x + 3, 294, 2);
    }
    // zone 3 (y 320..440): legend / correlation / order, >=18px apart
    {
        const COLORREF steps[5] = { t.panel, LerpCol(t.panel, t.seam, 0.85f),
                                    t.wave, t.bar, t.accentHot };
        for (int i = 0; i < 5; ++i)
            FillRectCol(S(552 + i * 13), S(330), S(562 + i * 13), S(336), steps[i]);
        FillRectCol(S(552), S(329), S(617), S(330), t.frame);
        char b[16];
        const int corr = g_mem ? int(g_mem->correlation * 99.f) : 0;
        snprintf(b, sizeof(b), "COR%c%02d", corr < 0 ? '-' : '+', std::abs(corr));
        DrawCellsTL(b, 552, 356, 3);
        DrawCellsTL(PlaybackOrderWord(), 552, 376, 2);
    }

    PaintCustomHeader("SONO / 07");
    PaintCustomFooter(false);
}

// ---- composition: TRACK, PC-98 / FastTracker II hex pattern scroll ---------
// 43 bands aggregate into 8 acoustic channels. Top: 8 mini channel meters.
// Bottom: a hex level pattern scrolling uniformly down at 12 rows/s with the
// newest row locked in the 4th-row playhead slot - pure DOS tracker DNA.
void PaintTrackLayout() {
    const Theme& t = Th();
    FillRectCol(0, 0, S(kLogicalW), S(kLogicalH), t.outer);
    static const char* chName[8] = { "01:SUB", "02:BAS", "03:LOW", "04:MID",
                                     "05:VOC", "06:HIM", "07:BRL", "08:AIR" };
    static const int lo[8] = { 0, 3, 7, 12, 18, 24, 30, 37 };
    static const int hi[8] = { 2, 6, 11, 17, 23, 29, 36, 42 };
    auto chanLv = [&](int c) -> float {
        float m = 0.f;
        for (int k = lo[c]; k <= hi[c]; ++k) m = std::max(m, Lv(k));
        return m;
    };
    const int colX = 56, colW = 70;

    // ---- channel meter head ------------------------------------------------
    InsetPanel(16, 34, 624, 122);
    DrawCellsTL("ROW", 22, 40, 2);
    for (int c = 0; c < 8; ++c) {
        const int x = colX + c * colW;
        if (c > 0) FillRectCol(S(x - 1), S(38), S(x), S(118), t.seam);
        DrawCellsTL(chName[c], x + 4, 40, 2);
        const float lv = chanLv(c);
        if (lv * 255.f > 180.f)
            FillRectCol(S(x + 60), S(42), S(x + 64), S(46), t.accentHot);
        // clean 52px vertical segmented meter (no ghost wall)
        const int bx = x + 26, bw = 18, baseY = 116, maxH = 52;
        const int up = int(lv * maxH);
        for (int y = baseY - up; y < baseY; y += 3)
            FillRectCol(S(bx), S(y), S(bx + bw), S(std::min(y + 2, baseY)),
                        y < baseY - maxH + 12 ? t.barLit : t.bar);
    }

    // ---- hex pattern scroll -------------------------------------------------
    InsetPanel(16, 128, 624, 446);
    const int topY = 132, rowH = 14, headSlot = 3;
    const int playY = topY + headSlot * rowH;
    // frac shares the ring-push phase; every row uses ONE continuous formula
    // (no pinned-row special case) so the whole sheet slides uniformly and
    // the fixed playhead rules simply bracket whichever row passes through
    const int frac = g_mem
        ? int(std::clamp(g_mem->trackClock - g_mem->trackLast, 0.0, 0.999) * rowH) : 0;
    const int current = g_mem ? (g_mem->trackRowCounter - 1) & 0xFF : 0;
    const int H = stage::Memory::kTrackRows;
    const char* hexd = "0123456789ABCDEF";
    gfx_push_clip(S(18), S(130), S(622), S(444));
    for (int age = -1; age <= 24; ++age) {
        const int y = playY + age * rowH + frac;
        if (y + rowH < 130 || y > 444) continue;
        const int rowNum = (current - age) & 0xFF;
        const bool hasData = g_mem && age >= 0 && age < H;
        const int idx = hasData ? (g_mem->trackHead - 1 - age + H * 4) % H : 0;
        const bool beatRow = hasData && g_mem->trackBeat[idx];
        // beat row: 3px accent cursor + a 1px seam underline (no opaque fill
        // that the glyph blits would chew holes into)
        if (beatRow) {
            FillRectCol(S(18), S(y), S(21), S(y + rowH), t.accentHot);
            FillRectCol(S(21), S(y + rowH - 1), S(622), S(y + rowH), t.seam);
        }
        // row number: every 4th row highlighted like a tracker beat line
        char rb[4] = { hexd[rowNum >> 4], hexd[rowNum & 15], 0, 0 };
        DrawCellsTL(rb, 26, y + 1, rowNum % 4 == 0 ? 3 : 2);
        for (int c = 0; c < 8; ++c) {
            const int x = colX + c * colW;
            const int v = hasData ? g_mem->trackHex[idx][c] : 0;
            // small dim hex value on the left (silent rows stay blank - no
            // "--" noise); the wide energy block on the right is the hero
            if (v > 0) {
                char vb[4] = { hexd[v >> 4], hexd[v & 15], 0, 0 };
                DrawCellsTL(vb, x + 2, y + 2, 2);
            }
            // 44x8 solid note block: wave -> bar -> accentHot by energy,
            // whole block flashes accentHot on a beat row
            const int bx = x + 20, bw = 44;
            const int w = int(v / 255.f * bw);
            if (w > 0) {
                const COLORREF bc = beatRow ? t.accentHot
                                  : v >= 0xC0 ? t.accentHot
                                  : v >= 0x60 ? t.bar : t.wave;
                const float distance = std::clamp(float(age) / 18.f, 0.f, 1.f);
                const COLORREF trail = LerpCol(bc, CustomPanelCol(), .16f + .66f * distance);
                panelBox(bx, y + 5, bx + w, y + (age == 0 ? 10 : 8), trail);
                if (age == 0) panelBox(bx + w - 1, y + 4, bx + w, y + 11, bc);
            }
        }
    }
    gfx_pop_clip();
    // playhead slot: two 1px frame rules bracket the passing rows; any
    // channel currently sounding (v > 60) lights a 2px trigger bar in its
    // column - rhythm-game readable at a glance
    FillRectCol(S(18), S(playY - 1), S(622), S(playY), CustomFrameCol());
    FillRectCol(S(18), S(playY + rowH), S(622), S(playY + rowH + 1), CustomFrameCol());
    if (g_mem) {
        const int headIdx = (g_mem->trackHead - 1 + H * 4) % H;
        const int hy = playY + frac;
        for (int c = 0; c < 8; ++c) {
            if (g_mem->trackHex[headIdx][c] > 60) {
                const int x = colX + c * colW;
                FillRectCol(S(x + 20), S(hy), S(x + 64), S(hy + 2), t.accentHot);
            }
        }
    }

    PaintCustomHeader("TRACK / 08");
    PaintCustomFooter(false);
}

// ---- 7-segment VFD digit (hard rectangles, unlit ghost segments) ----------
static void Draw7Seg(int x, int y, int digit, COLORREF lit, COLORREF unlit) {
    // 18x34 digit, 3px segment thickness: a b c d e f g
    static const unsigned char map[10] = {
        0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F
    };
    const unsigned char m = digit >= 0 && digit <= 9 ? map[digit] : 0;
    auto seg = [&](int sx, int sy, int sw, int sh, int bit) {
        FillRectCol(S(sx), S(sy), S(sx + sw), S(sy + sh), (m >> bit) & 1 ? lit : unlit);
    };
    seg(x + 3, y,      12, 3, 0);   // a
    seg(x + 15, y + 3,  3, 13, 1);  // b
    seg(x + 15, y + 18, 3, 13, 2);  // c
    seg(x + 3, y + 31, 12, 3, 3);   // d
    seg(x,     y + 18, 3, 13, 4);   // e
    seg(x,     y + 3,  3, 13, 5);   // f
    seg(x + 3, y + 16, 12, 3, 6);   // g
}

// ---- composition: DECK, cassette / DAT hardware faceplate ------------------
// Linked tape reels (left shrinks / right grows with real progress), a VFD
// 7-segment timecode window, broadcast PPM meters with clip latches and a
// compact VFD spectrum. Reels spin on the standby-aware deck clock.
// Polar scan memory: bearing encodes time within the sweep and range
// encodes frequency. Echoes come from the existing 15 Hz spectrum history.
// Full-circle radar: a rotating sweep with a fading afterglow sector, range
// rings, and contacts painted as radial arcs around the whole compass.

void PaintDeckLayout() {
    const Theme& t = Th();
    const float kPi = 3.14159265f;
    FillRectCol(0, 0, S(kLogicalW), S(kLogicalH), t.outer);
    const COLORREF ghost = LerpCol(t.panel, t.seam, 0.45f);
    const float p = float(PlayProgress());
    const float imp = Impact();

    // ---- military data-recorder bay ------------------------------------------
    InsetPanel(16, 34, 624, 240);
    // registration corners on the bay
    PaintCornerBrackets(22, 40, 618, 234, 10);
    const int ly = 134, lx = 166, rx = 474;
    const int rL = 52 - int(20 * p), rR = 32 + int(20 * p);
    const COLORREF tape = LerpCol(CustomPanelCol(), t.ink, 0.22f);
    const COLORREF tapeLine = LerpCol(CustomPanelCol(), t.ink, 0.36f);
    // hard-edge octagon ring (industrial data platter, not a soft disc)
    auto octRing = [&](int cx, int cy, int r, COLORREF c, int w) {
        GfxPoint pts[9];
        for (int i = 0; i <= 8; ++i) {
            const float a = kPi / 8.f + i * kPi / 4.f;
            pts[i] = { (std::int16_t)S(cx + int(r * cosf(a))),
                       (std::int16_t)S(cy + int(r * sinf(a))) };
        }
        gfx_polyline(pts, 9, c, w);
    };
    auto octFill = [&](int cx, int cy, int r, COLORREF c) {
        GfxPoint pts[8];
        for (int i = 0; i < 8; ++i) {
            const float a = kPi / 8.f + i * kPi / 4.f;
            pts[i] = { (std::int16_t)S(cx + int(r * cosf(a))),
                       (std::int16_t)S(cy + int(r * sinf(a))) };
        }
        gfx_polygon(pts, 8, c);
    };
    const double spin = g_mem ? g_mem->deckClock : 0.0;
    for (int side = 0; side < 2; ++side) {
        const int cx = side ? rx : lx, r = side ? rR : rL;
        octFill(cx, ly, r, tape);
        octRing(cx, ly, r, CustomFrameCol(), S(1));
        for (int rr = r - 6; rr > 22; rr -= 6) octRing(cx, ly, rr, tapeLine, S(1));
        // 4 aiming stubs on the platter's cardinal corners (static chassis)
        for (int i = 0; i < 4; ++i) {
            const float a = i * kPi / 2.f;
            GfxPoint seg[2] = {
                { (std::int16_t)S(cx + int((r + 2) * cosf(a))), (std::int16_t)S(ly + int((r + 2) * sinf(a))) },
                { (std::int16_t)S(cx + int((r + 7) * cosf(a))), (std::int16_t)S(ly + int((r + 7) * sinf(a))) } };
            gfx_polyline(seg, 2, CustomFrameCol(), S(1));
        }
        // industrial lock hub: static crosshair scale ring + rotating
        // three-blade hard reticle rotor + vermilion lock pin
        octFill(cx, ly, 20, CustomPanelCol());
        octRing(cx, ly, 20, CustomFrameCol(), S(1));
        for (int i = 0; i < 4; ++i) {
            const float a = kPi / 4.f + i * kPi / 2.f;
            GfxPoint seg[2] = {
                { (std::int16_t)S(cx + int(16 * cosf(a))), (std::int16_t)S(ly + int(16 * sinf(a))) },
                { (std::int16_t)S(cx + int(19 * cosf(a))), (std::int16_t)S(ly + int(19 * sinf(a))) } };
            gfx_polyline(seg, 2, LerpCol(CustomFrameCol(), CustomPanelCol(), 0.3f), S(1));
        }
        for (int i = 0; i < 3; ++i) {
            const float a = float(spin) + i * 2.f * kPi / 3.f;
            // blade: a hard 3-point vane from the axle to the hub rim
            GfxPoint blade[4] = {
                { (std::int16_t)S(cx + int(4 * cosf(a - 0.35f))), (std::int16_t)S(ly + int(4 * sinf(a - 0.35f))) },
                { (std::int16_t)S(cx + int(15 * cosf(a))), (std::int16_t)S(ly + int(15 * sinf(a))) },
                { (std::int16_t)S(cx + int(4 * cosf(a + 0.35f))), (std::int16_t)S(ly + int(4 * sinf(a + 0.35f))) },
                { (std::int16_t)S(cx + int(4 * cosf(a - 0.35f))), (std::int16_t)S(ly + int(4 * sinf(a - 0.35f))) } };
            gfx_polyline(blade, 4, CustomFrameCol(), S(1));
        }
        FillRectCol(S(cx - 2), S(ly - 2), S(cx + 3), S(ly + 3), t.accentHot);
    }
    // tape path: taut 1px polyline via 5x5 hollow guide rollers; the centre
    // assembly is a stepped pixel head block that snaps 0..4px on impact
    {
        const int bounce = int(imp * 4.f);
        GfxPoint path[5] = {
            { (std::int16_t)S(lx), (std::int16_t)S(ly + rL) },
            { (std::int16_t)S(220), (std::int16_t)S(210) },
            { (std::int16_t)S(320), (std::int16_t)S(210 + bounce) },
            { (std::int16_t)S(420), (std::int16_t)S(210) },
            { (std::int16_t)S(rx), (std::int16_t)S(ly + rR) } };
        gfx_polyline(path, 5, LerpCol(t.ink, CustomFrameCol(), 0.35f), S(1));
        for (int gx : { 220, 420 }) {
            FillRectCol(S(gx - 2), S(208), S(gx + 3), S(213), CustomFrameCol());
            FillRectCol(S(gx - 1), S(209), S(gx + 2), S(212), CustomPanelCol());
        }
        // stepped head assembly: wide base + narrow pickup + hot gap line
        FillRectCol(S(310), S(212 + bounce), S(331), S(218 + bounce), CustomFrameCol());
        FillRectCol(S(312), S(213 + bounce), S(329), S(217 + bounce), CustomPanelCol());
        FillRectCol(S(316), S(206 + bounce), S(325), S(213 + bounce), CustomFrameCol());
        FillRectCol(S(318), S(208 + bounce), S(323), S(212 + bounce), CustomPanelCol());
        FillRectCol(S(318), S(212 + bounce), S(323), S(213 + bounce),
                    imp > 0.08f ? t.accentHot : LerpCol(t.accentHot, CustomPanelCol(), 0.5f));
    }
    // ---- VFD timecode window ------------------------------------------------
    InsetPanel(244, 66, 396, 206);
    DrawCellsTL("MEMOIRS // 09", 268, 74, 2);
    {
        const int secs = std::max(0, int(g_state->seconds));
        const int mm = (secs / 60) % 100, ss = secs % 60;
        const int dx[4] = { 262, 286, 322, 346 };
        const int dv[4] = { mm / 10, mm % 10, ss / 10, ss % 10 };
        for (int i = 0; i < 4; ++i) Draw7Seg(dx[i], 96, dv[i], t.bar, ghost);
        // colon blinks with playback; STOP holds it lit (standby lock)
        const bool colon = g_state->libPaused || g_state->current < 0 || (g_state->tick % 60) < 30;
        const COLORREF cc = colon ? t.bar : ghost;
        FillRectCol(S(310), S(106), S(313), S(109), cc);
        FillRectCol(S(310), S(117), S(313), S(120), cc);
    }
    // separator + REC-LOCK / SYNC status microtags under the digits
    FillRectCol(S(252), S(136), S(388), S(137), LerpCol(CustomFrameCol(), CustomPanelCol(), 0.4f));
    {
        const bool rec = g_state->current >= 0 && !g_state->libPaused;
        DrawCellsTL("LOCK", 252, 140, rec ? 0 : 2);
        FillRectCol(S(322), S(142), S(326), S(146), rec ? t.accentHot : t.seam);
        DrawCellsTL("SYNC", 344, 140, g_mem && g_mem->impact > 0.08f ? 0 : 2);
    }
    {
        char b[16];
        snprintf(b, sizeof(b), "%s", g_state->current < 0 ? "--" : "LOCAL");
        if (g_state->current >= 0 && g_state->current < kTrackCount) snprintf(b, sizeof(b), "TR:%02d", g_state->current);
        DrawCellsTL(b, 252, 158, g_state->current >= 0 ? 0 : 2);
        const char* ow = PlaybackOrderWord();
        DrawCellsTL(ow, 388 - (int)strlen(ow) * 8, 158, 2);
        DrawCellsTL(TransportWord(), 252, 178, g_state->current >= 0 && !g_state->libPaused ? 0 : 2);
    }

    // ---- broadcast PPM meters (taller bay: dB row fully independent) --------
    InsetPanel(16, 248, 624, 342);
    for (int side = 0; side < 2; ++side) {
        const int y = side ? 286 : 258;
        const float drive = g_mem ? g_mem->meter[side] : 0.f;
        const int n = int(drive * 48.f);
        for (int i = 0; i < 48; ++i) {
            const int x = 56 + i * 10;
            const COLORREF c = i < n ? (i < 36 ? CustomStripCol() : t.accentHot) : ghost;
            FillRectCol(S(x), S(y), S(x + 8), S(y + 8), c);
        }
        if (g_mem && g_mem->meterPeak[side] > 0.02f) {
            const int px = 56 + std::min(47, int(g_mem->meterPeak[side] * 48.f)) * 10;
            FillRectCol(S(px), S(y - 2), S(px + 8), S(y), t.accentHot);
        }
        DrawCellsTL(side ? "R" : "L", 28, y - 2, 2);
        PaintClipLed(568, y + 1, side);
    }
    // dB scale: its own row (271..282), 4px clear of both meter bars
    {
        static const char* db[7] = { "-30", "-20", "-12", "-6", "-3", "0", "+3" };
        static const int dbCell[7] = { 0, 15, 26, 35, 39, 44, 47 };
        for (int i = 0; i < 7; ++i) {
            const int x = 56 + dbCell[i] * 10;
            DrawCellsTL(db[i], x, 271, 2);
        }
    }
    PaintCorrelationBar(272, 316, 96);

    // ---- compact VFD spectrum -------------------------------------------------
    InsetPanel(16, 350, 624, 446);
    {
        const int baseY = 438, maxH = 78;
        const int pitch = 14, barW = 12;
        const int x0 = (kLogicalW - 43 * pitch) / 2;
        for (int k = 0; k < 43; ++k) {
            const int x1 = x0 + k * pitch, x2 = x1 + barW;
            const int up = int(Lv(k) * maxH);
            const int prevUp = int(stage::bandLevel(g_state->spectrum.barsPrev[k]) * maxH);
            for (int y = baseY - up; y < baseY; y += 3)
                FillRectCol(S(x1), S(y), S(x2), S(std::min(y + 2, baseY)), t.bar);
            for (int y = baseY - prevUp; y < baseY - up; y += 3)
                FillRectCol(S(x1), S(y), S(x2), S(std::min(y + 2, baseY - up)), t.barLit);
        }
        if (imp > 0.08f)
            FillRectCol(S(x0), S(baseY), S(x0 + 43 * pitch), S(baseY + 3), t.accentHot);
        else
            FillRectCol(S(x0), S(baseY), S(x0 + 43 * pitch), S(baseY + 1),
                        LerpCol(t.frame, t.panel, 0.35f));
    }

    PaintCustomHeader("DECK / 09");
    PaintCustomFooter(false);
}

// ---- composition: POLY, 3D wireframe icosahedron core ----------------------
// A 12-vertex icosahedron (30 edges) wraps a counter-rotating octahedron
// (6 vertices, 12 edges) above a minimal perspective floor. All 42 edges are
// depth-sorted with the painter's algorithm, so near struts occlude far ones.
// Two previous frames of projected vertices are kept as a phosphor ghost: the
// buffer lives here (not in stage::Memory) because the painter only ever sees
// a const Memory snapshot.
namespace {
struct PolyGhostBuf {
    GfxPoint pts[2][18]{};
    int count = 0;
    int lastTrack = -2;
};
PolyGhostBuf g_polyGhost;
} // namespace

void PaintPolyLayout() {
    const Theme& t = Th();
    FillRectCol(0, 0, S(kLogicalW), S(kLogicalH), t.outer);

    if (g_state->current != g_polyGhost.lastTrack) {
        g_polyGhost.count = 0;
        g_polyGhost.lastTrack = g_state->current;
    }

    const float imp = Impact();
    const float lowS = g_mem ? g_mem->lowSustain : 0.f;
    const float midS = g_mem ? g_mem->midSustain : 0.f;
    const float spark = g_mem ? g_mem->highSpark : 0.f;
    const double clk = g_mem ? g_mem->polyClock : 0.0;

    // ---- left main window: 3D perspective core ---------------------------
    InsetPanel(16, 34, 412, 446);
    gfx_push_clip(S(17), S(35), S(411), S(445));

    // perspective floor: six rows roll toward the camera on the left channel
    {
        const COLORREF gridC = LerpCol(CustomPanelCol(), t.seam, 0.55f);
        const float floorPhase = float(std::fmod(clk * 0.55, 1.0));
        for (int i = 0; i <= 5; ++i) {
            const float u = std::clamp((i + floorPhase) / 6.f, 0.f, 1.f);
            const int y = 352 + int(90.f * std::pow(u, 1.7f));
            const int xl = 104 - int(74.f * u), xr = 324 + int(74.f * u);
            GfxPoint fpts[17];
            for (int k = 0; k <= 16; ++k) {
                const float tx = k / 16.f;
                const float disp = std::clamp(g_state->spectrum.waveL[k * 14] / 64.f,
                                              -1.f, 1.f);
                fpts[k] = ScenePoint(xl + (xr - xl) * tx, y + disp * (1.0f + 2.2f * u));
            }
            gfx_polyline(fpts, 17, gridC, S(1));
        }
        for (int k = 0; k <= 8; ++k) {
            const float u = k / 8.f;
            GfxPoint seg[2] = { ScenePoint(104.f + 220.f * u, 352.f),
                                ScenePoint(40.f + 344.f * u, 442.f) };
            gfx_polyline(seg, 2, gridC, S(1));
        }
    }

    // ---- locked geometry: 12-vertex icosahedron, 6-vertex octahedron ------
    constexpr float phi = 1.61803398875f;
    constexpr float inv = 0.525731112f;           // 1 / sqrt(1 + phi^2)
    constexpr float piv = phi * inv;              // 0.850650808
    static constexpr float V[12][3] = {
        { -inv, piv, 0.f },   { inv, piv, 0.f },
        { -inv, -piv, 0.f },  { inv, -piv, 0.f },
        { 0.f, -inv, piv },   { 0.f, inv, piv },
        { 0.f, -inv, -piv },  { 0.f, inv, -piv },
        { piv, 0.f, -inv },   { piv, 0.f, inv },
        { -piv, 0.f, -inv },  { -piv, 0.f, inv } };
    static constexpr int E[30][2] = {
        { 0, 11 }, { 0, 5 }, { 0, 1 }, { 0, 7 }, { 0, 10 },
        { 1, 5 }, { 5, 11 }, { 11, 10 }, { 10, 7 }, { 7, 1 },
        { 3, 9 }, { 3, 4 }, { 3, 2 }, { 3, 6 }, { 3, 8 },
        { 4, 9 }, { 2, 4 }, { 6, 2 }, { 8, 6 }, { 9, 8 },
        { 4, 5 }, { 9, 5 }, { 9, 1 }, { 8, 1 }, { 8, 7 },
        { 6, 7 }, { 6, 10 }, { 2, 10 }, { 2, 11 }, { 4, 11 } };
    static constexpr float IV[6][3] = {
        { 1.f, 0.f, 0.f }, { -1.f, 0.f, 0.f },
        { 0.f, 1.f, 0.f }, { 0.f, -1.f, 0.f },
        { 0.f, 0.f, 1.f }, { 0.f, 0.f, -1.f } };
    static constexpr int IE[12][2] = {
        { 0, 2 }, { 0, 3 }, { 0, 4 }, { 0, 5 },
        { 1, 2 }, { 1, 3 }, { 1, 4 }, { 1, 5 },
        { 2, 4 }, { 2, 5 }, { 3, 4 }, { 3, 5 } };

    const float fcx = 214.f, fcy = 222.f, fov = 330.f;
    const float camZ = 3.85f - 0.14f * imp;
    const float ax = float(clk * 0.62), ay = float(clk * 0.91), az = float(clk * 0.27);
    const float inScale = 0.42f + 0.16f * lowS + 0.12f * imp;

    float ox[18], oy[18], oz[18];
    float rx3[18], rz2[18];   // rotated world coords, reused by the top view
    auto put = [&](int idx, float vx, float vy, float vz,
                   float rx, float ry, float rz) {
        const float c1 = cosf(rx), s1 = sinf(rx);
        const float y1 = vy * c1 - vz * s1;
        const float z1 = vy * s1 + vz * c1;
        const float c2 = cosf(ry), s2 = sinf(ry);
        const float x2 = vx * c2 + z1 * s2;
        const float z2 = -vx * s2 + z1 * c2;
        const float c3 = cosf(rz), s3 = sinf(rz);
        const float x3 = x2 * c3 - y1 * s3;
        const float y3 = x2 * s3 + y1 * c3;
        const float d = fov / (camZ - z2);
        ox[idx] = fcx + x3 * d;
        oy[idx] = fcy - y3 * d;
        oz[idx] = z2;
        rx3[idx] = x3;
        rz2[idx] = z2;
    };
    for (int i = 0; i < 12; ++i) {
        const float rad = 1.f + 0.52f * Lv(i * 3 + 2);
        put(i, V[i][0] * rad, V[i][1] * rad, V[i][2] * rad, ax, ay, az);
    }
    for (int i = 0; i < 6; ++i) {
        put(12 + i, IV[i][0] * inScale, IV[i][1] * inScale, IV[i][2] * inScale,
            -ax * 1.25f, -ay * 1.15f, -az);
    }

    int hotIdx = -1;
    float hotE = 0.f;
    for (int i = 0; i < 12; ++i) {
        const float e = Lv(i * 3 + 2);
        if (e > hotE) { hotE = e; hotIdx = i; }
    }

    struct Edge { float za, zb; int a, b; bool inner; };
    Edge edges[42];
    int ne = 0;
    for (int i = 0; i < 30; ++i)
        edges[ne++] = { oz[E[i][0]], oz[E[i][1]], E[i][0], E[i][1], false };
    for (int i = 0; i < 12; ++i)
        edges[ne++] = { oz[12 + IE[i][0]], oz[12 + IE[i][1]],
                        12 + IE[i][0], 12 + IE[i][1], true };
    std::sort(edges, edges + ne, [](const Edge& a, const Edge& b) {
        return a.za + a.zb < b.za + b.zb;
    });

    GfxPoint gp[18];
    for (int i = 0; i < 18; ++i) gp[i] = ScenePoint(ox[i], oy[i]);

    if (g_polyGhost.count > 0) {
        const int g = g_polyGhost.count - 1;
        const COLORREF gc = DepthInk(t.wave, 0.72f);
        for (int i = 0; i < 30; ++i) {
            GfxPoint seg[2] = { g_polyGhost.pts[g][E[i][0]],
                                g_polyGhost.pts[g][E[i][1]] };
            gfx_polyline(seg, 2, gc, S(1));
        }
    }

    for (int i = 0; i < ne; ++i) {
        const Edge& e = edges[i];
        const float zNorm = std::clamp((0.5f * (e.za + e.zb) + 1.4f) / 2.8f, 0.f, 1.f);
        COLORREF ec;
        int w;
        if (!e.inner) {
            if (zNorm < 0.48f) {
                ec = DepthInk(t.wave, 0.55f);
                w = S(1);
            } else {
                ec = DepthInk(t.bar, 0.35f * (1.f - zNorm));
                w = zNorm > 0.72f ? S(2) : S(1);
            }
        } else {
            ec = DepthInk(CustomFrameCol(), 0.55f * (1.f - zNorm));
            w = S(1);
        }
        GfxPoint seg[2] = { gp[e.a], gp[e.b] };
        gfx_polyline(seg, 2, ec, w);
    }

    // hard-edge vertex nodes
    {
        int near3[3] = { -1, -1, -1 };
        for (int i = 0; i < 12; ++i) {
            for (int s = 0; s < 3; ++s) {
                if (near3[s] < 0 || oz[i] > oz[near3[s]]) {
                    for (int q = 2; q > s; --q) near3[q] = near3[q - 1];
                    near3[s] = i;
                    break;
                }
            }
        }
        for (int i = 0; i < 12; ++i) {
            if (Lv(i * 3 + 2) <= 0.62f && spark <= 0.25f) continue;
            bool isNear = false;
            for (int s = 0; s < 3; ++s) isNear = isNear || near3[s] == i;
            const int size = (imp > 0.08f && isNear) ? 4 : 3;
            const int half = size / 2;
            FillRectCol(gp[i].x - S(half), gp[i].y - S(half),
                        gp[i].x - S(half) + S(size), gp[i].y - S(half) + S(size),
                        t.accentHot);
        }
    }

    if (spark > 0.22f) {
        int t0 = -1, t1 = -1;
        float e0 = -1.f, e1 = -1.f;
        for (int i = 0; i < 12; ++i) {
            const float e = Lv(i * 3 + 2);
            if (e > e0) { e1 = e0; t1 = t0; e0 = e; t0 = i; }
            else if (e > e1) { e1 = e; t1 = i; }
        }
        for (int s = 0; s < 2; ++s) {
            const int v = s ? t1 : t0;
            if (v < 0) continue;
            GfxPoint seg[2] = { gp[v], gp[12 + (v % 6)] };
            gfx_polyline(seg, 2, t.accentHot, S(1));
        }
    }

    // ---- dynamic peak target lock + 45-degree engineering leader ----------
    if (hotIdx >= 0 && hotE > 0.25f) {
        const int r = S(4);
        const int hx = gp[hotIdx].x, hy = gp[hotIdx].y;
        FillRectCol(hx - r, hy - r, hx + r + S(1), hy - r + S(1), t.accentHot);
        FillRectCol(hx - r, hy + r, hx + r + S(1), hy + r + S(1), t.accentHot);
        FillRectCol(hx - r, hy - r, hx - r + S(1), hy + r + S(1), t.accentHot);
        FillRectCol(hx + r, hy - r, hx + r + S(1), hy + r + S(1), t.accentHot);
        const int dir = (ox[hotIdx] < fcx) ? -1 : 1;
        GfxPoint lead[3] = {
            gp[hotIdx],
            ScenePoint(ox[hotIdx] + dir * 22.f, oy[hotIdx] - 22.f),
            ScenePoint(ox[hotIdx] + dir * 82.f, oy[hotIdx] - 22.f) };
        gfx_polyline(lead, 3, t.accentHot, S(1));
        char tag[16];
        snprintf(tag, sizeof(tag), "B%02d/%02d", hotIdx * 3 + 2, int(hotE * 99.f));
        int tx = int(ox[hotIdx] + dir * 82.f) - (dir < 0 ? 56 : 0);
        tx = std::clamp(tx, 20, 340);
        DrawCellsTL(tag, tx, int(oy[hotIdx]) - 34, 3);
    }

    if (g_state->current >= 0 && !g_state->libPaused) {
        if (g_polyGhost.count >= 1)
            for (int i = 0; i < 18; ++i) g_polyGhost.pts[0][i] = g_polyGhost.pts[1][i];
        for (int i = 0; i < 18; ++i) g_polyGhost.pts[1][i] = gp[i];
        if (g_polyGhost.count == 0)
            for (int i = 0; i < 18; ++i) g_polyGhost.pts[0][i] = gp[i];
        g_polyGhost.count = std::min(2, g_polyGhost.count + 1);
    }

    PaintScopeGraticule(16, 34, 412, 446, true, true, false);
    PaintSpecPlate(24, 42, "CAD // 12V-30E");
    gfx_pop_clip();

    // ---- top-right window: 2D orthographic XZ projection ------------------
    InsetPanel(420, 34, 624, 242);
    gfx_push_clip(S(421), S(35), S(623), S(241));
    {
        const int mx = 522, my = 144;
        const COLORREF gc = LerpCol(CustomFrameCol(), CustomPanelCol(), 0.55f);
        FillRectCol(S(mx - 64), S(my), S(mx + 64), S(my + 1), gc);
        FillRectCol(S(mx), S(my - 64), S(mx + 1), S(my + 64), gc);
        GfxPoint circ[33];
        for (int i = 0; i <= 32; ++i)
            circ[i] = ScenePoint(mx + 64.f * kCircle32.c[i],
                                 my - 64.f * kCircle32.s[i]);
        gfx_polyline(circ, 33, DepthInk(t.seam, 0.35f), S(1));

        GfxPoint top[12];
        for (int i = 0; i < 12; ++i)
            top[i] = ScenePoint(mx + rx3[i] * 42.f, my - rz2[i] * 42.f);
        for (int i = 0; i < 30; ++i) {
            GfxPoint seg[2] = { top[E[i][0]], top[E[i][1]] };
            gfx_polyline(seg, 2, DepthInk(t.wave, 0.28f), S(1));
        }
        if (hotIdx >= 0 && hotE > 0.25f) {
            const int hx = top[hotIdx].x, hy = top[hotIdx].y;
            FillRectCol(hx - S(2), hy - S(2), hx + S(3), hy + S(3), t.accentHot);
        }
        DrawCellsTL("TOP // XZ PROJ", 428, 42, 2);
        char b[32];
        snprintf(b, sizeof(b), "YAW:%03d PCH:%03d",
                 int(std::fmod(double(ay) * 57.2958, 360.0)),
                 int(std::fmod(double(ax) * 57.2958, 360.0)));
        DrawCellsTL(b, 428, 222, 2);
    }
    gfx_pop_clip();

    // ---- bottom-right window: three-band decision bay ---------------------
    InsetPanel(420, 250, 624, 446);
    {
        const float bands[3] = { lowS, midS, spark };
        static const char* nm[3] = { "01:SUB", "02:MID", "03:AIR" };
        for (int i = 0; i < 3; ++i) {
            const int y = 260 + i * 44;
            const bool act = bands[i] > 0.62f || (i == 0 && imp > 0.16f);
            const COLORREF bg = act ? (i == 1 ? t.bar : t.accentHot) : CustomPanelCol();
            FillRectCol(S(428), S(y), S(616), S(y + 36), bg);
            if (!act) {
                FillRectCol(S(428), S(y), S(616), S(y + 1), CustomFrameCol());
                FillRectCol(S(428), S(y + 35), S(616), S(y + 36), CustomFrameCol());
                FillRectCol(S(428), S(y), S(429), S(y + 36), CustomFrameCol());
                FillRectCol(S(615), S(y), S(616), S(y + 36), CustomFrameCol());
            }
            DrawCellsTL(nm[i], 436, y + 5, act ? 0 : 2);
            DrawCellsTL(act ? "SYNC" : "NORM", 576, y + 5, act ? 0 : 2);
            const int w = int(std::clamp(bands[i], 0.f, 1.f) * 172.f);
            if (w > 0)
                FillRectCol(S(436), S(y + 30), S(436 + w), S(y + 33),
                            act ? t.ink : t.bar);
        }
        for (int side = 0; side < 2; ++side) {
            const int y = 402 + side * 12;
            const float drive = g_mem ? g_mem->meter[side] : 0.f;
            DrawCellsTL(side ? "R" : "L", 424, y - 2, 2);
            FillRectCol(S(436), S(y), S(564), S(y + 7),
                        LerpCol(t.seam, CustomPanelCol(), 0.30f));
            const int w = int(std::clamp(drive, 0.f, 1.f) * 128.f);
            if (w > 0) FillRectCol(S(436), S(y), S(436 + w), S(y + 7), CustomStripCol());
            if (g_mem && g_mem->meterPeak[side] > 0.02f) {
                const int px = 436 + int(g_mem->meterPeak[side] * 128.f);
                FillRectCol(S(px), S(y - 2), S(px + 2), S(y + 9), t.accentHot);
            }
        }
        char b[16];
        const int corr = g_mem ? int(g_mem->correlation * 99.f) : 0;
        snprintf(b, sizeof(b), "COR%c%02d", corr < 0 ? '-' : '+', std::abs(corr));
        DrawCellsTL(b, 572, 408, 3);
    }

    PaintCustomHeader("POLY / 10");
    PaintCustomFooter(false);
    PaintLayoutSwitchSweep();
}

// ---- composition: WARP, wide panorama terrain radar ------------------------
// 82% panorama above, flat 18% console below: no side bay, the horizon runs
// the full chassis width and the tallest near peak is auto-tracked.
void PaintWarpLayout() {
    const Theme& t = Th();
    FillRectCol(0, 0, S(kLogicalW), S(kLogicalH), t.outer);
    InsetPanel(16, 34, 624, 376);
    gfx_push_clip(S(17), S(35), S(623), S(375));

    const float pan = g_mem ? g_mem->pan : 0.f;
    const int cx = 320 + int(pan * 8.f), horizonY = 168;
    const double wClk = g_mem ? g_mem->warpClock : 0.0;
    const float lvl = g_mem ? (g_mem->meter[0] + g_mem->meter[1]) * 0.5f : 0.f;
    const float spark = g_mem ? g_mem->highSpark : 0.f;
    const float imp = Impact();

    // ---- upper half: 48 radar stars above the horizon ---------------------
    for (int i = 0; i < 48; ++i) {
        const float u = float(std::fmod(wClk * 0.14 + i / 48.0, 1.0));
        const float ang = -3.14159265f * float((i * 37 % 100) / 100.f);
        const float r1 = 6.f * std::pow(52.f, u);
        const float tail = 0.05f + 0.14f * lvl;
        const float r0 = r1 * (1.f - tail);
        const GfxPoint p0 = ScenePoint(cx + r0 * cosf(ang), horizonY + r0 * sinf(ang));
        const GfxPoint p1 = ScenePoint(cx + r1 * cosf(ang), horizonY + r1 * sinf(ang));
        const COLORREF sc = (u > 0.78f && spark > 0.20f)
                                ? t.accentHot : DepthInk(t.wave, 1.f - u);
        GfxPoint seg[2] = { p0, p1 };
        gfx_polyline(seg, 2, sc, S(1));
    }

    // azimuth scale across the full panorama
    {
        static const int ax5[5] = { 80, 200, 320, 440, 560 };
        static const char* az5[5] = { "-60", "-30", "0", "+30", "+60" };
        for (int i = 0; i < 5; ++i) {
            FillRectCol(S(ax5[i]), S(horizonY - 5), S(ax5[i] + 1), S(horizonY), t.bar);
            DrawCellsTL(az5[i], ax5[i] - 8, horizonY - 16, 2);
        }
    }

    if (imp > 0.10f)
        FillRectCol(S(18), S(horizonY), S(622), S(horizonY + 2), t.accentHot);
    else
        FillRectCol(S(18), S(horizonY), S(622), S(horizonY + 1), CustomFrameCol());

    // ---- lower half: 16 x 24 terrain, wide and shallow --------------------
    const float sub = g_mem && g_mem->warpLast >= 0.0
                          ? float(std::clamp(g_mem->warpClock - g_mem->warpLast, 0.0, 0.999))
                          : 0.f;
    GfxPoint prev[24];
    GfxPoint bestPt{ 0, 0 };
    float bestVal = 0.f, bestX = 0.f, bestY = 0.f;
    int bestCol = -1;
    for (int r = 0; r < 16; ++r) {
        const float u = (r + sub) / 16.f;
        const float baseY = horizonY + 206.f * std::pow(u, 1.85f);
        const float halfW = 72.f + 276.f * std::pow(u, 1.45f);
        const float elev = 10.f + 74.f * std::pow(u, 1.35f);
        const int rowIdx = g_mem ? (g_mem->warpHead - 1 - r + 16 * 4) % 16 : 0;
        GfxPoint cur[24];
        float vv[24];
        for (int c = 0; c < 24; ++c) {
            const float tx = -1.f + 2.f * c / 23.f;
            const float v = g_mem ? g_mem->warpRows[rowIdx][c] / 255.f : 0.f;
            vv[c] = v;
            cur[c] = ScenePoint(cx + tx * halfW, baseY - elev * v);
            if (r >= 13 && v > bestVal) {
                bestVal = v;
                bestCol = c;
                bestPt = cur[c];
                bestX = cx + tx * halfW;
                bestY = baseY - elev * v;
            }
        }
        GfxPoint poly[26];
        for (int c = 0; c < 24; ++c) poly[c] = cur[c];
        poly[24] = ScenePoint(cx + halfW, 376.f);
        poly[25] = ScenePoint(cx - halfW, 376.f);
        gfx_polygon(poly, 26, CustomPanelCol());
        if (r > 0) {
            for (int c = 0; c < 24; c += 2) {
                GfxPoint seg[2] = { prev[c], cur[c] };
                gfx_polyline(seg, 2, DepthInk(CustomFrameCol(), 1.f - u), S(1));
            }
        }
        const COLORREF ridgeC = r >= 14 ? t.bar : DepthInk(t.wave, 1.f - u);
        gfx_polyline(cur, 24, ridgeC, S(r == 15 ? 2 : 1));
        if (r >= 10) {
            for (int c = 0; c < 24; ++c) {
                if (vv[c] <= 0.72f) continue;
                FillRectCol(cur[c].x - S(1), cur[c].y - S(1),
                            cur[c].x + S(2), cur[c].y + S(2), t.accentHot);
            }
        }
        for (int c = 0; c < 24; ++c) prev[c] = cur[c];
    }

    // ---- tallest near peak: hollow lock + mast up to the azimuth scale ----
    if (bestVal > 0.35f && bestCol >= 0) {
        const int r = S(4);
        FillRectCol(bestPt.x - r, bestPt.y - r, bestPt.x + r + S(1), bestPt.y - r + S(1), t.accentHot);
        FillRectCol(bestPt.x - r, bestPt.y + r, bestPt.x + r + S(1), bestPt.y + r + S(1), t.accentHot);
        FillRectCol(bestPt.x - r, bestPt.y - r, bestPt.x - r + S(1), bestPt.y + r + S(1), t.accentHot);
        FillRectCol(bestPt.x + r, bestPt.y - r, bestPt.x + r + S(1), bestPt.y + r + S(1), t.accentHot);
        FillRectCol(bestPt.x, S(horizonY - 26), bestPt.x + S(1), bestPt.y, t.accentHot);
        char tag[16];
        snprintf(tag, sizeof(tag), "PK:%s", kFreqLabel[bestCol * 8 / 24]);
        DrawCellsTL(tag, std::clamp(int(bestX) - 16, 24, 556), horizonY - 38, 3);
    }

    PaintScopeGraticule(16, 34, 624, 376, false, true, false);
    PaintSpecPlate(24, 42, "TERRAIN // WIDE");
    gfx_pop_clip();

    // ---- bottom console ---------------------------------------------------
    InsetPanel(16, 384, 624, 446);
    {
        const bool trig = imp > 0.08f;
        FillRectCol(S(26), S(392), S(32), S(398), CustomFrameCol());
        FillRectCol(S(27), S(393), S(31), S(397), trig ? t.accentHot : t.seam);
        DrawCellsTL("WARP // TER-11", 38, 390, 2);
        char b[24];
        const int corr = g_mem ? int(g_mem->correlation * 99.f) : 0;
        const int panV = g_mem ? int(g_mem->pan * 99.f) : 0;
        snprintf(b, sizeof(b), "COR%c%02d PAN%c%02d",
                 corr < 0 ? '-' : '+', std::abs(corr),
                 panV < 0 ? '-' : '+', std::abs(panV));
        DrawCellsTL(b, 24, 412, 2);
    }
    for (int side = 0; side < 2; ++side) {
        const int y = 392 + side * 18;
        const float drive = g_mem ? g_mem->meter[side] : 0.f;
        DrawCellsTL(side ? "R" : "L", 156, y, 2);
        for (int i = 0; i < 36; ++i) {
            const int x = 168 + i * 276 / 36;
            const int x1 = 168 + (i + 1) * 276 / 36 - 1;
            const bool lit = float(i) / 36.f < drive;
            FillRectCol(S(x), S(y), S(x1), S(y + 6),
                        lit ? (i > 31 ? t.accentHot : CustomStripCol())
                            : LerpCol(t.seam, CustomPanelCol(), 0.35f));
        }
        if (g_mem && g_mem->meterPeak[side] > 0.02f) {
            const int px = 168 + int(g_mem->meterPeak[side] * 276.f);
            FillRectCol(S(px), S(y - 2), S(px + 2), S(y + 8), t.accentHot);
        }
        if (g_mem && g_mem->clipHold[side] > 0.f)
            DrawCellsTL("CLIP", 452, y - 2, 3);
    }
    {
        static const char* tag[3] = { "LO", "MD", "HI" };
        const float e3[3] = { g_mem ? g_mem->lowSustain : 0.f,
                              g_mem ? g_mem->midSustain : 0.f,
                              g_mem ? g_mem->highSpark : 0.f };
        for (int i = 0; i < 3; ++i) {
            const int y = 392 + i * 14;
            DrawCellsTL(tag[i], 486, y, 2);
            FillRectCol(S(506), S(y + 2), S(602), S(y + 8), t.seam);
            const int w = int(std::clamp(e3[i], 0.f, 1.f) * 96.f);
            if (w > 0)
                FillRectCol(S(506), S(y + 2), S(506 + w), S(y + 8),
                            i == 0 ? t.bar : i == 1 ? t.wave : t.accentHot);
        }
        DrawCellsTL(PlaybackOrderWord(), 486, 432, 2);
    }

    PaintCustomHeader("WARP / 11");
    PaintCustomFooter(false);
    PaintLayoutSwitchSweep();
}

// ---- composition: MOIRE, blueprint interferometer + physical section ------
// An unframed optical coordinate sheet fills the top; a drafting title block
// sits in its lower-right corner; a 42px physical cross-section slot below
// plots the real interference sum along the horizontal axis.
void PaintMoireLayout() {
    const Theme& t = Th();
    FillRectCol(0, 0, S(kLogicalW), S(kLogicalH), t.outer);
    InsetPanel(16, 34, 624, 398);
    gfx_push_clip(S(17), S(35), S(623), S(397));

    const float cx = 320.f, cy = 216.f;
    const float m = g_mem ? float(g_mem->moireClock) : 0.f;
    const float mL = g_mem ? g_mem->meter[0] : 0.f;
    const float mR = g_mem ? g_mem->meter[1] : 0.f;
    const float lowS = g_mem ? g_mem->lowSustain : 0.f;
    const float midS = g_mem ? g_mem->midSustain : 0.f;
    const float spark = g_mem ? g_mem->highSpark : 0.f;

    // blueprint grid: a faint cross every 32px
    {
        const COLORREF gc = LerpCol(CustomPanelCol(), t.seam, 0.45f);
        for (int gx = 24; gx < 616; gx += 32)
            for (int gy = 42; gy < 392; gy += 32) {
                FillRectCol(S(gx - 1), S(gy), S(gx + 2), S(gy + 1), gc);
                FillRectCol(S(gx), S(gy - 1), S(gx + 1), S(gy + 2), gc);
            }
    }

    const float rx = 72.f + 22.f * midS;
    const float ry = 42.f + 14.f * midS;
    float ox[3], oy[3], pitch[3];
    COLORREF src[3];
    ox[0] = cx + rx * cosf(m);
    oy[0] = cy + ry * sinf(m * 1.3f);
    pitch[0] = 9.0f + 5.5f * (1.f - mL);
    src[0] = t.wave;
    ox[1] = cx + rx * cosf(m + 2.0944f);
    oy[1] = cy + ry * sinf(m * 1.3f + 1.5708f);
    pitch[1] = 9.0f + 5.5f * (1.f - mR);
    src[1] = t.bar;
    ox[2] = cx + 34.f * sinf(m * 0.7f);
    oy[2] = cy + 26.f * cosf(m * 0.5f);
    pitch[2] = 14.0f + 6.0f * (1.f - lowS);
    src[2] = LerpCol(CustomFrameCol(), CustomPanelCol(), 0.35f);

    // hard 1px rings, faintest source first; LUT + radius-dependent segments
    static const int order[3] = { 2, 0, 1 };
    for (int oi = 0; oi < 3; ++oi) {
        const int e = order[oi];
        for (int k = 1; k <= 26; ++k) {
            const float r = k * pitch[e];
            if (r > 320.f) break;
            COLORREF rc = src[e];
            if (r > 220.f) rc = LerpCol(rc, CustomPanelCol(), 0.25f);
            const int segs = r < 54.f ? 32 : 64;
            const float* lc = r < 54.f ? kCircle32.c : kCircle64.c;
            const float* ls = r < 54.f ? kCircle32.s : kCircle64.s;
            GfxPoint ring[65];
            for (int i = 0; i <= segs; ++i)
                ring[i] = ScenePoint(ox[e] + r * lc[i], oy[e] + r * ls[i]);
            gfx_polyline(ring, segs + 1, rc, S(1));
        }
    }

    // a hit retriggers one expanding hard wavefront
    if (g_mem && g_mem->moirePulse < 1.15f) {
        const float pr = 14.f + g_mem->moirePulse * 270.f;
        GfxPoint wave[65];
        for (int i = 0; i <= 64; ++i)
            wave[i] = ScenePoint(ox[2] + pr * kCircle64.c[i],
                                 oy[2] + pr * kCircle64.s[i]);
        gfx_polyline(wave, 65, DepthInk(t.accentHot, g_mem->moirePulse * 0.55f), S(1));
    }

    for (int e = 0; e < 3; ++e) {
        const COLORREF cc = spark > 0.22f ? t.accentHot : CustomFrameCol();
        GfxPoint h[2] = { ScenePoint(ox[e] - 5.f, oy[e]), ScenePoint(ox[e] + 6.f, oy[e]) };
        GfxPoint v[2] = { ScenePoint(ox[e], oy[e] - 5.f), ScenePoint(ox[e], oy[e] + 6.f) };
        gfx_polyline(h, 2, cc, S(1));
        gfx_polyline(v, 2, cc, S(1));
        FillRectCol(S(int(ox[e])), S(int(oy[e])), S(int(ox[e]) + 1), S(int(oy[e]) + 1), cc);
    }

    PaintScopeGraticule(16, 34, 624, 398, true, false, false);

    // micrometer cursors: each source projected onto the four sheet edges
    for (int e = 0; e < 3; ++e) {
        const int sx = std::clamp(int(ox[e]), 26, 614);
        FillRectCol(S(sx - 1), S(35), S(sx + 2), S(40), src[e]);
        FillRectCol(S(sx - 1), S(391), S(sx + 2), S(396), src[e]);
        const int sy = std::clamp(int(oy[e]), 44, 390);
        FillRectCol(S(17), S(sy - 1), S(22), S(sy + 2), src[e]);
        FillRectCol(S(618), S(sy - 1), S(623), S(sy + 2), src[e]);
    }

    // drafting title block
    {
        const int bl = 458, bt = 328, br = 618, bb = 392;
        FillRectCol(S(bl), S(bt), S(br), S(bb), CustomPanelCol());
        FillRectCol(S(bl), S(bt), S(br), S(bt + 1), CustomFrameCol());
        FillRectCol(S(bl), S(bb - 1), S(br), S(bb), CustomFrameCol());
        FillRectCol(S(bl), S(bt), S(bl + 1), S(bb), CustomFrameCol());
        FillRectCol(S(br - 1), S(bt), S(br), S(bb), CustomFrameCol());
        FillRectCol(S(bl + 2), S(bb - 26), S(br - 2), S(bb - 25), CustomFrameCol());
        PaintSpecPlate(462, 331, "FIG.12 // INTERFERO");
        char b[24];
        snprintf(b, sizeof(b), "P-L:%02d P-R:%02d",
                 int(pitch[0] * 10.f), int(pitch[1] * 10.f));
        DrawCellsTL(b, 464, 352, 2);
        const int corr = g_mem ? int(g_mem->correlation * 99.f) : 0;
        snprintf(b, sizeof(b), "COR%c%02d", corr < 0 ? '-' : '+', std::abs(corr));
        DrawCellsTL(b, 464, 370, 3);
        const bool trig = Impact() > 0.08f;
        FillRectCol(S(548), S(371), S(552), S(375), trig ? t.accentHot : t.seam);
        DrawCellsTL("TRIG", 558, 370, trig ? 0 : 2);
    }
    gfx_pop_clip();

    // ---- bottom: 42px physical cross-section slot -------------------------
    InsetPanel(16, 404, 624, 446);
    gfx_push_clip(S(17), S(405), S(623), S(445));
    {
        DrawCellsTL("CUT:Y0", 22, 420, 2);
        FillRectCol(S(80), S(425), S(612), S(426),
                    LerpCol(CustomFrameCol(), CustomPanelCol(), 0.55f));
        GfxPoint sec[134];
        float vv[134];
        for (int i = 0; i < 134; ++i) {
            const float px = 80.f + i * (532.f / 133.f);
            const float d0 = std::hypot(px - ox[0], cy - oy[0]) / pitch[0];
            const float d1 = std::hypot(px - ox[1], cy - oy[1]) / pitch[1];
            const float d2 = std::hypot(px - ox[2], cy - oy[2]) / pitch[2];
            const float val = (std::cos(6.283185307f * d0) +
                               std::cos(6.283185307f * d1) +
                               0.6f * std::cos(6.283185307f * d2)) / 2.6f;
            vv[i] = val;
            sec[i] = ScenePoint(px, 425.f - val * 14.f);
        }
        gfx_polyline(sec, 134, t.bar, S(1));
        for (int i = 0; i < 134; ++i) {
            if (vv[i] <= 0.78f) continue;
            FillRectCol(sec[i].x - S(1), sec[i].y - S(1),
                        sec[i].x + S(1), sec[i].y + S(1), t.accentHot);
        }
    }
    gfx_pop_clip();

    PaintCustomHeader("MOIRE / 12");
    PaintCustomFooter(false);
    PaintLayoutSwitchSweep();
}

// ---- composition: RASTER, data-brutalist test-pattern barcode -------------
// Full width, no telemetry bay: three unequal horizontal bands (16-slot
// barcode, symmetric 43-band knife matrix, 1px raster hairs) split by a
// micron-thin telemetry gap. Nothing decorative survives.
void PaintRasterLayout() {
    const Theme& t = Th();
    FillRectCol(0, 0, S(kLogicalW), S(kLogicalH), t.outer);
    InsetPanel(16, 34, 624, 446);
    gfx_push_clip(S(17), S(35), S(623), S(445));

    const float imp = Impact();
    const float spark = g_mem ? g_mem->highSpark : 0.f;
    const float clk = g_mem ? float(g_mem->rasterClock) : 0.f;
    const bool invFlash = imp > 0.24f;

    // ---- upper band (y 40..144): 16 low/mid barcode slots -----------------
    for (int k = 0; k < 16; ++k) {
        const float lv = Lv(k);
        const int sx = 24 + k * 37;
        if (lv < 0.08f) {
            FillRectCol(S(sx + 18), S(40), S(sx + 19), S(144), t.seam);
            continue;
        }
        const int bw = std::max(1, int(lv * 34.f));
        const int x0 = sx + 18 - bw / 2;
        FillRectCol(S(x0), S(40), S(x0 + bw), S(144),
                    lv > 0.72f ? t.accentHot : (k % 2 == 0 ? t.bar : t.ink));
    }

    // ---- data gap (y 150..166) -------------------------------------------
    FillRectCol(S(24), S(150), S(616), S(151), CustomFrameCol());
    FillRectCol(S(24), S(165), S(616), S(166), CustomFrameCol());
    {
        char b[32];
        DrawCellsTL("TEST.PAT // 43B", 28, 154, 2);
        const int lo = g_mem ? int(g_mem->lowSustain * 99.f) : 0;
        const int md = g_mem ? int(g_mem->midSustain * 99.f) : 0;
        const int hi = g_mem ? int(g_mem->highSpark * 99.f) : 0;
        snprintf(b, sizeof(b), "LO:%02d MD:%02d HI:%02d", lo, md, hi);
        DrawCellsTL(b, 168, 154, 3);
        const int corr = g_mem ? int(g_mem->correlation * 99.f) : 0;
        snprintf(b, sizeof(b), "COR%c%02d", corr < 0 ? '-' : '+', std::abs(corr));
        DrawCellsTL(b, 320, 154, 2);
        const int panV = g_mem ? int(g_mem->pan * 99.f) : 0;
        snprintf(b, sizeof(b), "PAN%c%02d", panV < 0 ? '-' : '+', std::abs(panV));
        DrawCellsTL(b, 384, 154, 2);
        const int cenV = g_mem ? int(g_mem->centroid * 99.f) : 0;
        snprintf(b, sizeof(b), "CEN:%02d", cenV);
        DrawCellsTL(b, 448, 154, 2);
        const bool trig = imp > 0.08f;
        FillRectCol(S(536), S(155), S(540), S(159), trig ? t.accentHot : t.seam);
        DrawCellsTL("TRIG", 546, 154, trig ? 0 : 2);
    }

    // ---- middle band (y 172..342): symmetric 43-band knife matrix ---------
    if (invFlash)
        FillRectCol(S(24), S(172), S(616), S(342),
                    LerpCol(t.ink, CustomPanelCol(), 0.15f));
    {
        const int cy = 257;
        for (int k = 0; k < 43; ++k) {
            const float lv = Lv(k);
            if (lv <= 0.12f) continue;
            const int bw = std::max(1, int(lv * 12.f));
            const int halfH = int(lv * 82.f);
            const int x0 = 40 + k * 13;
            const COLORREF c = invFlash
                                   ? CustomPanelCol()
                                   : (lv > 0.76f ? t.accentHot
                                                 : (k % 3 == 0 ? t.wave : t.bar));
            FillRectCol(S(x0), S(cy - halfH), S(x0 + bw), S(cy + halfH), c);
        }
        FillRectCol(S(24), S(cy), S(616), S(cy + 1),
                    LerpCol(CustomFrameCol(), CustomPanelCol(), 0.30f));
        const int needle = 40 + int(std::fmod(clk * 0.30f, 1.f) * 560.f);
        FillRectCol(S(needle), S(172), S(needle + 1), S(342), t.accentHot);
    }

    // ---- lower band (y 348..438): 1px raster hairs ------------------------
    {
        for (int i = 0; i < 197; ++i) {
            const int x = 24 + i * 3;
            if (x > 616) break;
            const int s = (i * 7) % 240;
            const float w = (std::abs(g_state->spectrum.waveL[s]) +
                             std::abs(g_state->spectrum.waveR[s])) * 0.5f / 64.f;
            const float hiE = Lv(20 + (i % 23));
            const int h = int(std::clamp(hiE * w, 0.f, 1.f) * 90.f);
            if (h <= 0) continue;
            const COLORREF c = (spark > 0.22f && i % 7 == 0)
                                   ? t.accentHot
                                   : ((i % 5 < 3) ? DepthInk(t.wave, 0.35f) : t.bar);
            FillRectCol(S(x), S(438 - h), S(x + 1), S(438), c);
        }
    }

    gfx_pop_clip();
    PaintCustomHeader("RASTER / 13");
    PaintCustomFooter(false);
    PaintLayoutSwitchSweep();
}

// ---- composition: TORUS, classic round CRT vector terminal ----------------
// The only non-rectangular chassis: one round green-screen scope, four thin
// corner readouts floating in the surrounding void.
void PaintTorusLayout() {
    const Theme& t = Th();
    FillRectCol(0, 0, S(kLogicalW), S(kLogicalH), t.outer);

    const float cx = 320.f, cy = 240.f;
    gfx_ellipse(S(int(cx - 186.f)), S(int(cy - 186.f)),
                S(int(cx + 186.f) + 1), S(int(cy + 186.f) + 1), CustomPanelCol());
    {
        GfxPoint ring[65];
        for (int i = 0; i <= 64; ++i)
            ring[i] = ScenePoint(cx + 186.f * kCircle64.c[i], cy + 186.f * kCircle64.s[i]);
        gfx_polyline(ring, 65, CustomFrameCol(), S(2));
        for (int i = 0; i <= 64; ++i)
            ring[i] = ScenePoint(cx + 180.f * kCircle64.c[i], cy + 180.f * kCircle64.s[i]);
        gfx_polyline(ring, 65, DepthInk(CustomFrameCol(), 0.45f), S(1));
    }

    // 360-degree bezel scale, crosshair and one faint inner reference circle
    {
        const COLORREF tickC = LerpCol(CustomFrameCol(), CustomPanelCol(), 0.45f);
        for (int deg = 0; deg < 360; deg += 10) {
            const float a = 6.283185307f * deg / 360.f;
            const float r0 = (deg % 30 == 0) ? 168.f : 174.f;
            GfxPoint seg[2] = { ScenePoint(cx + r0 * cosf(a), cy + r0 * sinf(a)),
                                ScenePoint(cx + 180.f * cosf(a), cy + 180.f * sinf(a)) };
            gfx_polyline(seg, 2, tickC, S(1));
        }
        const COLORREF axc = LerpCol(CustomFrameCol(), CustomPanelCol(), 0.62f);
        FillRectCol(S(int(cx) - 166), S(int(cy)), S(int(cx) - 8), S(int(cy) + 1), axc);
        FillRectCol(S(int(cx) + 8), S(int(cy)), S(int(cx) + 166), S(int(cy) + 1), axc);
        FillRectCol(S(int(cx)), S(int(cy) - 166), S(int(cx) + 1), S(int(cy) - 8), axc);
        FillRectCol(S(int(cx)), S(int(cy) + 8), S(int(cx) + 1), S(int(cy) + 166), axc);
        GfxPoint ic[65];
        for (int i = 0; i <= 64; ++i)
            ic[i] = ScenePoint(cx + 92.f * kCircle64.c[i], cy + 92.f * kCircle64.s[i]);
        gfx_polyline(ic, 65, DepthInk(t.seam, 0.45f), S(1));
    }

    const float clk = g_mem ? float(g_mem->torusClock) : 0.f;
    const float lowS = g_mem ? g_mem->lowSustain : 0.f;
    const float midS = g_mem ? g_mem->midSustain : 0.f;
    const float pan = g_mem ? g_mem->pan : 0.f;
    const float imp = Impact();

    const float ax = clk * 0.72f, ay = clk * 1.05f, az = clk * 0.31f;
    const float R = 1.25f + 0.25f * lowS + 0.18f * imp;
    const float rBase = 0.48f + 0.18f * midS;
    const float fov = 345.f, camZ = 4.25f - 0.14f * imp;
    const float cax = cosf(ax), sax = sinf(ax);
    const float cay = cosf(ay), say = sinf(ay);
    const float caz = cosf(az), saz = sinf(az);

    auto project = [&](float x0, float y0, float z0,
                       float& ox, float& oy, float& oz) {
        const float y1 = y0 * cax - z0 * sax;
        const float z1 = y0 * sax + z0 * cax;
        const float x2 = x0 * cay + z1 * say;
        const float z2 = -x0 * say + z1 * cay;
        const float x3 = x2 * caz - y1 * saz;
        const float y3 = x2 * saz + y1 * caz;
        const float d = fov / (camZ - z2);
        ox = cx + x3 * d;
        oy = cy - y3 * d;
        oz = z2;
    };
    // nothing may leave the round glass: pull stray points back to r=172
    auto onGlass = [&](float& x, float& y) {
        const float dx = x - cx, dy = y - cy;
        const float d = std::sqrt(dx * dx + dy * dy);
        if (d > 172.f) {
            const float k = 172.f / d;
            x = cx + dx * k;
            y = cy + dy * k;
        }
    };

    struct Node { float x, y, z, lv; };
    Node nodes[96], rings[12][8];
    int nn = 0;
    float hotX = 0.f, hotY = 0.f, hotLv = 0.f;
    for (int uIdx = 0; uIdx < 12; ++uIdx) {
        const float u = 6.283185307f * uIdx / 12.f;
        for (int vIdx = 0; vIdx < 8; ++vIdx) {
            const float v = 6.283185307f * vIdx / 8.f + u * pan * 0.5f;
            const float lv = Lv((uIdx * 3 + vIdx) % 43);
            const float r = rBase * (1.f + 0.55f * lv);
            const float rr = R + r * cosf(v);
            const float x0 = rr * cosf(u), y0 = rr * sinf(u), z0 = r * sinf(v);
            Node n{};
            project(x0, y0, z0, n.x, n.y, n.z);
            n.lv = lv;
            onGlass(n.x, n.y);
            nodes[nn++] = n;
            rings[uIdx][vIdx] = n;
            if (lv > hotLv) { hotLv = lv; hotX = n.x; hotY = n.y; }
        }
    }

    // gimbal axes
    {
        const float A = 1.75f;
        const float ends[6][3] = { { A, 0, 0 }, { -A, 0, 0 },
                                   { 0, A, 0 }, { 0, -A, 0 },
                                   { 0, 0, A }, { 0, 0, -A } };
        GfxPoint gp2[6];
        for (int i = 0; i < 6; ++i) {
            float px = 0.f, py = 0.f, pz = 0.f;
            project(ends[i][0], ends[i][1], ends[i][2], px, py, pz);
            onGlass(px, py);
            gp2[i] = ScenePoint(px, py);
        }
        for (int a = 0; a < 3; ++a) {
            GfxPoint seg[2] = { gp2[a * 2], gp2[a * 2 + 1] };
            gfx_polyline(seg, 2, DepthInk(CustomFrameCol(), 0.42f), S(1));
        }
    }

    // meridian skeleton, then the depth-sorted hard nodes
    for (int uIdx = 0; uIdx < 12; ++uIdx) {
        GfxPoint ring[9];
        for (int vIdx = 0; vIdx < 8; ++vIdx)
            ring[vIdx] = ScenePoint(rings[uIdx][vIdx].x, rings[uIdx][vIdx].y);
        ring[8] = ring[0];
        gfx_polyline(ring, 9, DepthInk(t.seam, 0.45f), S(1));
    }

    std::sort(nodes, nodes + nn, [](const Node& a, const Node& b) { return a.z < b.z; });
    for (int i = 0; i < nn; ++i) {
        const Node& n = nodes[i];
        const float zNorm = std::clamp((n.z + 2.0f) / 4.0f, 0.f, 1.f);
        const int sz = 2 + int(zNorm * 2.5f) + (n.lv > 0.72f ? 1 : 0);
        const COLORREF c = n.lv > 0.75f
                               ? t.accentHot
                               : (zNorm > 0.55f
                                      ? DepthInk(t.bar, (1.f - zNorm) * 0.4f)
                                      : DepthInk(t.wave, (1.f - zNorm) * 0.7f));
        const int px = int(n.x), py = int(n.y), h = sz / 2;
        FillRectCol(S(px - h), S(py - h), S(px - h + sz), S(py - h + sz), c);
        if (zNorm > 0.75f && sz >= 4)
            FillRectCol(S(px), S(py), S(px + 1), S(py + 1), t.accentHot);
    }

    // hottest node locked in a 7x7 frame
    if (hotLv > 0.25f) {
        const int hx = S(int(hotX)), hy = S(int(hotY)), r = S(3);
        FillRectCol(hx - r, hy - r, hx + r + S(1), hy - r + S(1), t.accentHot);
        FillRectCol(hx - r, hy + r, hx + r + S(1), hy + r + S(1), t.accentHot);
        FillRectCol(hx - r, hy - r, hx - r + S(1), hy + r + S(1), t.accentHot);
        FillRectCol(hx + r, hy - r, hx + r + S(1), hy + r + S(1), t.accentHot);
    }

    // ---- four corner readouts in the surrounding void ---------------------
    PaintSpecPlate(18, 40, "CRT-14 // TORUS", imp > 0.18f);
    {
        const bool trig = imp > 0.08f;
        FillRectCol(S(22), S(60), S(28), S(66), CustomFrameCol());
        FillRectCol(S(23), S(61), S(27), S(65), trig ? t.accentHot : t.seam);
        DrawCellsTL("TRIG", 34, 58, trig ? 0 : 2);
    }
    {
        char b[16];
        snprintf(b, sizeof(b), "AX:%03d", int(std::fmod(double(ax) * 57.2958, 360.0)));
        DrawCellsTL(b, 512, 42, 2);
        snprintf(b, sizeof(b), "AY:%03d", int(std::fmod(double(ay) * 57.2958, 360.0)));
        DrawCellsTL(b, 512, 56, 2);
    }
    for (int side = 0; side < 2; ++side) {
        const int x = 22 + side * 12;
        const float drive = g_mem ? g_mem->meter[side] : 0.f;
        const int n = int(std::clamp(drive, 0.f, 1.f) * 96.f);
        FillRectCol(S(x), S(436 - 96), S(x + 7), S(436),
                    LerpCol(CustomPanelCol(), t.seam, 0.35f));
        if (n > 0) FillRectCol(S(x), S(436 - n), S(x + 7), S(436),
                               side ? t.wave : t.bar);
        if (g_mem && g_mem->meterPeak[side] > 0.02f) {
            const int py = 436 - int(g_mem->meterPeak[side] * 96.f);
            FillRectCol(S(x), S(py - 1), S(x + 7), S(py), t.accentHot);
        }
        DrawCellsTL(side ? "R" : "L", x, 326, 2);
    }
    {
        char b[16];
        const int corr = g_mem ? int(g_mem->correlation * 99.f) : 0;
        snprintf(b, sizeof(b), "COR%c%02d", corr < 0 ? '-' : '+', std::abs(corr));
        DrawCellsTL(b, 524, 376, 3);
        const int panV = g_mem ? int(g_mem->pan * 99.f) : 0;
        snprintf(b, sizeof(b), "PAN%c%02d", panV < 0 ? '-' : '+', std::abs(panV));
        DrawCellsTL(b, 524, 392, 3);
        const int cenV = g_mem ? int(g_mem->centroid * 99.f) : 0;
        snprintf(b, sizeof(b), "CEN:%02d", cenV);
        DrawCellsTL(b, 524, 408, 2);
        DrawCellsTL(PlaybackOrderWord(), 524, 424, 2);
    }

    PaintCustomHeader("TORUS / 14");
    PaintCustomFooter(false);
    PaintLayoutSwitchSweep();
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
        if (layoutMode == layout::Sono) { PaintSonoLayout(); return; }
        if (layoutMode == layout::Track) { PaintTrackLayout(); return; }
        if (layoutMode == layout::Deck) { PaintDeckLayout(); return; }
        if (layoutMode == layout::Poly) { PaintPolyLayout(); return; }
        if (layoutMode == layout::Warp) { PaintWarpLayout(); return; }
        if (layoutMode == layout::Moire) { PaintMoireLayout(); return; }
        if (layoutMode == layout::Raster) { PaintRasterLayout(); return; }
        if (layoutMode == layout::Torus) { PaintTorusLayout(); return; }
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
