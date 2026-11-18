#include "core/spectrum.h"
#include "stage.h"
// ui.cpp - HellSinker "SOUND MEMOIRS" screen, reimplemented to match the
// original at 640x480 logical resolution, rendered at 2x (1280x960).
//
// Layout, palette indices, glyph metrics and the per-frame animation timings
// were recovered by measuring the original screen; the tuning values sit
// inline next to the drawing code that consumes them.
#include "ui.h"
#include "layout.h"
#include "library.h"
#include "tag_reader.h"
#include "panel_text.h"
#include "config_panel.h"
#include <random>
#include <shlwapi.h>

#include <mmsystem.h>
#include <commdlg.h>
#include <shellapi.h>
#include <math.h>
#include <stdio.h>
#include <time.h>
#include <algorithm>
using std::max;

namespace {

// ---- logical screen geometry ------------------------------------------
const int kLogicalW = 640;
const int kLogicalH = 480;

// panel frame: the original draws four lines at 320+-t1-1 / 320+-t1 and
// 240+-t2 with t1 max 177, t2 max 236  ->  x 142..497, y 4..476.
const int kPanelL = 142, kPanelR = 497, kPanelT = 4, kPanelB = 476;

// spectrum window (beige field) and bars
const int kSpecL = 148, kSpecT = 102, kSpecR = 492;
const int kSpecAxisY = 236;             // cyan frequency axis line
const int kBars = memoirs::kSpectrumBands;                   // 43 columns, 7px wide, pitch 8
const int kBarW = 7, kBarPitch = 8;
const int kBarBaseY = 232;              // y2 of segment 0 (=408-176)
const int kSegH = 2;                    // y1 = y2-2, step 3

// title: static strip (reference frames a/b are pixel-identical here, so the
// original has no per-frame title animation; the glitch look is in the atlas)
const int kTitleX = 160, kTitleY = 32;  // sub_518BA8(32,160): a3=Y, a4=X

// EXIT: sub_5193E8(1,1, 280, 168) -> a4=Y, a5=X
const int kExitX = 168, kExitY = 280;

// TR grid: x = 168+56*col, y = 296+16*row (col-major, index = col*10+row)
const int kGridX = 168, kGridY = 296, kGridCol = 56, kGridRow = 16;
const int kGridCols = 5, kGridRows = 10;

// info lines: (220,256) and (220,264); hint at (144,472)
const int kInfoX = 220, kInfoY1 = 256, kInfoY2 = 264;
const int kHintX = 144, kHintY = 472;

// ---- themes -------------------------------------------------------------
// Screen RGB sampled from game frames for the "Original" preset; the packed
// draw colors are texture-modulated on top of it.
// Additional presets recolor the whole vector scene; text is drawn with the
// private Hellsinker TTF so every element stays crisp at any scale.
#include "themes.generated.h"
int g_themeIdx = 0;
static inline const Theme& Th() { return kThemes[g_themeIdx]; }

// Legacy names kept so the painting code reads unchanged; they now resolve
// through the active theme.
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

// run-length vector tables for title / axis (drawn as crisp scaled rects)
#include "themed_art_runs.h"

// spectrum band-edge seam columns (logical x), tinted toward Th().seam
const int kSeamX[8] = { 171, 195, 243, 291, 339, 387, 435, 475 };
const char* kFreqLabel[8] = { "52", "110", "540", "1060", "2080", "4160", "12K", "22K" };

const char* kTitle = "SOUND MEMOIRS";

// ---- sweep / FFT -------------------------------------------------------

struct TrackState {
    ResolvedSource src;
    bool ok;
    TrackState() : ok(false) {}
};

struct PlayerApp {
    Resolver* resolver;
    AudioEngine audio;
    PlayerSettings* settings;

    TrackState tracks[kTrackCount];
    int selected;      // 0..kTrackCount-1, or kExitIndex
    int current;
    bool audioReady;
    bool loading;
    std::string errorText;
    std::string audioError;

    // external-music playlist (local files added via Ctrl+O / drop / --file)
    std::vector<std::wstring> playlist;
    int localIndex;

    HWND hwnd;
    HDC backDC;
    HBITMAP backBmp;
    int backW, backH;

    // visualisation source
    std::vector<short> visPcm;
    int visRate;
    memoirs::Spectrum spectrum;
    unsigned int tick;       // frame counter since screen entry (60 fps)

    PlayerApp()
        : resolver(NULL), settings(NULL), selected(0), current(-1),
          audioReady(false), loading(false), localIndex(-1), hwnd(NULL),
          backDC(NULL), backBmp(NULL), backW(0), backH(0),
          visRate(44100), tick(0) {}
};

const int kExitIndex = -1;
PlayerApp g_app;
stage::Memory stageMemory;

// ---- dynamic scale ------------------------------------------------------
// The scene is authored at 640x480 logical pixels and rendered into a
// backbuffer of 640*scale x 480*scale, then letterboxed into the (resizable)
// client area.  scale follows the client height so everything grows
// proportionally with resolution; all geometry is vector / TTF, so it stays
// crisp at any scale.
float g_scale = 2.0f;
int g_offX = 0, g_offY = 0;   // letterbox offset of the backbuffer in the client
int g_bbW = 1280, g_bbH = 960;

// ---- small helpers -----------------------------------------------------
static inline int S(int v) { return (int)(v * g_scale + 0.5f); }
static inline int Sf(float v) { return (int)(v * g_scale + 0.5f); }
void RefreshTitle();
enum class Page { Tracks, Library, System };
Page page = Page::Tracks, returnPage = Page::Tracks;
int settingRow = 0, hoverChannel = -1;
bool fieldGrid = false;   // TAB overlay for the Field / Cross compositions
constexpr unsigned kRailIdleDelayFrames = 150;
constexpr unsigned kRailFadeFrames = 60;
unsigned lastRailActivityTick = 0;
unsigned int switchedAt = 0;
constexpr int railX=366, channelWidth=40;
HWND searchEdit = nullptr;
HFONT panelFont = nullptr;
int fontPixels = 0, cursor = 0;
constexpr int rows = 7;
std::wstring query;
std::vector<int> matches;
TagReader tagReader;
enum LocalOrder { Sequential, RepeatOne, RepeatAll, Shuffle };
int localOrder=Sequential;
std::vector<std::wstring> loadedPaths;
int loadedSelection=-1, loadedOrder=Sequential;
bool draggingSeek=false;
double dragFraction=0;
constexpr int seekLeft=154, seekRight=486, seekY=266;
double seekFraction(double logicalX) {
    return std::clamp((logicalX-seekLeft)/(seekRight-seekLeft),0.,1.);
}
std::wstring basenameW(const std::wstring&);
std::wstring titleOf(const std::wstring& p) {
    auto t=tagReader.get(p);return t.title.empty()?basenameW(p):t.title;
}
void refreshIndex() {
    int old = cursor >= 0 && cursor < (int)matches.size() ? matches[cursor] : -1;
    matches.clear();
    for(int i=0;i<(int)g_app.playlist.size();++i) {
        const auto& p=g_app.playlist[i];
        if(!query.empty() && !StrStrIW(basenameW(p).c_str(),query.c_str())) {
            auto t=tagReader.get(p);
            if(!StrStrIW(t.title.c_str(),query.c_str()) && !StrStrIW(t.artist.c_str(),query.c_str()) &&
               !StrStrIW(t.album.c_str(),query.c_str())) continue;
        }
        matches.push_back(i);
    }
    auto it = std::find(matches.begin(), matches.end(), old);
    cursor = it == matches.end() ? 0 : int(it - matches.begin());
}
HDC PanelAtlas();
void lineText(HDC dc,int x,int y,int right,int bottom,const std::wstring& value,
              COLORREF color,UINT align=DT_LEFT,
              PanelTextMode mode=PanelTextMode::AtlasFirst) {
    DrawPanelText(dc,{S(x),S(y),S(right),S(bottom)},value,color,align,g_scale,
                  PanelAtlas(),mode);
}
std::wstring basenameW(const std::wstring& p) {return p.substr(p.find_last_of(L"/\\")+1);}


const layout::TracksGeom& TG() {
    return layout::tracks(g_app.settings ? g_app.settings->layoutMode : 0);
}
const layout::SpecGeom& SG() {
    return layout::spec(g_app.settings ? g_app.settings->layoutMode : 0);
}

RECT TrackRect(int i) {
    const layout::TracksGeom& g = TG();
    int col = i / g.rows;   // column-major, like the game
    int row = i % g.rows;
    RECT r;
    r.left = g.gridX + col * g.colW - 4;
    r.top = g.gridY + row * g.rowH - 8;
    r.right = r.left + g.colW;
    r.bottom = r.top + g.rowH;
    return r;
}

bool PointInRect(const RECT& r, int x, int y) {
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

void FillRectCol(HDC dc, int l, int t, int r, int b, COLORREF c) {
    RECT rc;
    rc.left = l; rc.top = t; rc.right = r; rc.bottom = b;
    HBRUSH br = CreateSolidBrush(c);
    FillRect(dc, &rc, br);
    DeleteObject(br);
}

// Draw a run-length vector table (2x source coords) as crisp scaled rects.
// ox,oy = logical origin of the artwork; col0/col1 = theme colors for c=0/1.
void DrawRuns(HDC dc, const ArtRun* runs, int n, float ox, float oy,
              COLORREF col0, COLORREF col1) {
    for (int c = 0; c < 2; ++c) {
        HBRUSH br = CreateSolidBrush(c == 0 ? col0 : col1);
        for (int i = 0; i < n; ++i) {
            if (runs[i].c != c) continue;
            RECT rc;
            rc.left = Sf(ox + runs[i].x * 0.5f);
            rc.top = Sf(oy + runs[i].y * 0.5f);
            rc.right = Sf(ox + (runs[i].x + runs[i].w) * 0.5f);
            rc.bottom = Sf(oy + (runs[i].y + 1) * 0.5f);
            // never let rounding collapse a thin stroke to zero size
            if (rc.right <= rc.left) rc.right = rc.left + 1;
            if (rc.bottom <= rc.top) rc.bottom = rc.top + 1;
            FillRect(dc, &rc, br);
        }
        DeleteObject(br);
    }
}

COLORREF LerpCol(COLORREF a, COLORREF b, float t) {
    if (t <= 0.0f) return a;
    if (t >= 1.0f) return b;
    return RGB((int)(GetRValue(a) + (GetRValue(b) - GetRValue(a)) * t),
               (int)(GetGValue(a) + (GetGValue(b) - GetGValue(a)) * t),
               (int)(GetBValue(a) + (GetBValue(b) - GetBValue(a)) * t));
}

int ClampI(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Constant-alpha blit of a solid color over a rect (GDI has no alpha fill).
void AlphaFill(HDC dc, int l, int t, int r, int b, COLORREF c, int alpha) {
    if (alpha <= 0) return;
    static HDC srcDC = NULL;
    static HBITMAP srcBmp = NULL;
    if (!srcDC) {
        srcDC = CreateCompatibleDC(NULL);
        srcBmp = CreateBitmap(1, 1, 1, 32, NULL);
        SelectObject(srcDC, srcBmp);
    }
    SetPixel(srcDC, 0, 0, c);
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, (BYTE)(alpha > 255 ? 255 : alpha), 0 };
    AlphaBlend(dc, l, t, r - l, b - t, srcDC, 0, 0, 1, 1, bf);
}

void HLine(HDC dc, int x0, int x1, int y, COLORREF c, int w = 1) {
    HPEN p = CreatePen(PS_SOLID, w, c);
    HGDIOBJ old = SelectObject(dc, p);
    MoveToEx(dc, x0, y, NULL);
    LineTo(dc, x1, y);
    SelectObject(dc, old);
    DeleteObject(p);
}

void VLine(HDC dc, int y0, int y1, int x, COLORREF c, int w = 1) {
    HPEN p = CreatePen(PS_SOLID, w, c);
    HGDIOBJ old = SelectObject(dc, p);
    MoveToEx(dc, x, y0, NULL);
    LineTo(dc, x, y1);
    SelectObject(dc, old);
    DeleteObject(p);
}

// ---- themed bitmap art --------------------------------------------------
// Glyph shapes / proportions come from the original 8x11 cell atlas (and the
// title / axis artwork) so layout stays exactly as tuned.  Crispness at any
// resolution comes from NEAREST-NEIGHBOR stretching (no blur), and themes
// recolor the art by remapping its source palette to the active theme.
// Per-theme art is pre-rendered offline (tools/make_themed_art.py) into BMP
// resources, so the runtime never touches DIB byte order: it just loads the
// themed bitmap and nearest-neighbor stretches it.
struct ArtSet {
    HDC dc[kThemeCount];
    HBITMAP bmp[kThemeCount];
    bool built;
    ArtSet() : built(false) {
        for (int i = 0; i < kThemeCount; ++i) { dc[i] = 0; bmp[i] = 0; }
    }
    void Load(int baseId) {
        if (built) return;
        for (int t = 0; t < kThemeCount; ++t) {
            bmp[t] = LoadBitmapA(GetModuleHandleA(NULL), MAKEINTRESOURCEA(baseId + t));
            dc[t] = CreateCompatibleDC(NULL);
            SelectObject(dc[t], bmp[t]);
        }
        built = true;
    }
};

ArtSet g_atlas;
HDC PanelAtlas() {return g_atlas.dc[g_themeIdx];}

void EnsureArt() {
    g_atlas.Load(110);      // cells_<theme> (grid/info/hints pixel font)
}

// ---- fixed-pitch cell text --------------------------------------------
// palette: 0 normal, 1 selection band, 2 dim, 3 no-underline (info lines).
// The selection cream band is drawn full-cell-width by PaintSelection.
void DrawCells(HDC dc, const std::string& text, int x, int y, int palette = 0,
               bool /*underline*/ = false) {
    static const std::string cells = "ABCDEFGHIJKLNMOPQRSTUVWXYZ 0123456789_-.:;&<>*+";
    SetStretchBltMode(dc, COLORONCOLOR);
    for (char ch : text) {
        size_t index = cells.find(ch);
        if (index == std::string::npos) index = cells.find(' ');
        StretchBlt(dc, S(x - 4), S(y - 5), S(8), S(11),
                   g_atlas.dc[g_themeIdx], (int)index * 8, palette * 11, 8, 11, SRCCOPY);
        x += 8;
    }
}

// ---- audio / tracks (unchanged behaviour) ------------------------------
void EnsureAudio() {
    PlayerApp& app = g_app;
    if (app.audioReady) return;
    std::string err;
    if (app.audio.init(err)) {
        app.audioReady = true;
        app.audio.setVolume(app.settings->volume);
        app.audioError.clear();
    } else {
        app.audioError = err;
    }
}

void StopPlayback() {
    PlayerApp& app = g_app;
    app.audio.stop();
    app.current = -1;
    app.loading = false;
    RefreshTitle();
    InvalidateRect(app.hwnd, NULL, FALSE);
}

bool PlayDecoded(PcmBuffer& pcm, std::string& err) {
    PlayerApp& app = g_app;
    app.visPcm = pcm.samples;
    app.visRate = pcm.sampleRate;
    return app.audio.playPcm(pcm, err);
}

int AddLocal(const std::wstring& path) {
    PlayerApp& app = g_app;
    for (int i = 0; i < (int)app.playlist.size(); ++i)
        if (_wcsicmp(app.playlist[i].c_str(),path.c_str())==0) return i;
    tagReader.enqueue(path);
    app.playlist.push_back(path);
    return (int)app.playlist.size() - 1;
}

void OpenMusic(const std::wstring& path) {
    PlayerApp& app = g_app;
    int nextIndex=AddLocal(path);
    EnsureAudio();
    PcmBuffer pcm;
    std::string err = app.audioError;
    app.loading = true;
    InvalidateRect(app.hwnd, NULL, FALSE);
    UpdateWindow(app.hwnd);
    bool ok = false;
    try {
        if (app.audioReady && DecodeLocalMusic(path, pcm, err)) {
            app.audio.stop();
            app.current = -1;
            app.audio.setLoop(localOrder==RepeatOne);
            ok = PlayDecoded(pcm, err);
        }
    } catch (const std::bad_alloc&) {
        err = "Insufficient memory to load this track.";
    }
    app.loading = false;
    if (ok) {
        app.localIndex=nextIndex;
        app.current = kTrackCount;
        app.errorText.clear();
        RefreshTitle();
        DbgLog("local music playing: frames=%u", (unsigned)app.visPcm.size() / 2);
    } else {
        MessageBoxA(app.hwnd, err.c_str(), "SOUND MEMOIRS", MB_ICONERROR | MB_OK);
    }
    InvalidateRect(app.hwnd, NULL, FALSE);
}

void StepLocal(int delta) {
    PlayerApp& app = g_app;
    if (app.playlist.empty()) return;
    int n = (int)app.playlist.size();
    int i = app.localIndex < 0 ? 0 : (app.localIndex + delta) % n;
    if (i < 0) i += n;
    OpenMusic(app.playlist[i]);
}

void ChooseMusic(HWND hwnd, bool autoplay = true) {
    wchar_t buf[32768] = {};
    OPENFILENAMEW dialog = {};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = hwnd;
    dialog.lpstrFile = buf;
    dialog.nMaxFile = sizeof(buf) / sizeof(*buf);
    dialog.lpstrFilter = L"Audio files\0*.wav;*.ogg;*.mp3;*.flac;*.m4a;*.aac;*.wma;*.mp4;*.opus\0All files\0*.*\0";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR |
                   OFN_ALLOWMULTISELECT | OFN_EXPLORER;
    if (!GetOpenFileNameW(&dialog)) return;
    // Explorer multi-select: first string is the directory, then file names,
    // double-null terminated.  Single select: the buffer is one full path.
    std::wstring first = buf;
    size_t off = first.size() + 1;
    std::vector<std::wstring> added;
    if (off < sizeof(buf) / sizeof(*buf) && buf[off] != 0) {
        std::wstring dir = first;
        if (!dir.empty() && dir.back() != L'\\' && dir.back() != L'/') dir += L'\\';
        while (off < sizeof(buf) / sizeof(*buf) && buf[off] != 0) {
            std::wstring name = buf + off;
            added.push_back(dir + name);
            off += name.size() + 1;
        }
    } else {
        added.push_back(first);
    }
    if (added.empty()) return;
    int playIdx = -1;
    for (const std::wstring& p : added) {
        int i = AddLocal(p);
        if (playIdx < 0) playIdx = i;
    }
    refreshIndex();
    if (autoplay) OpenMusic(g_app.playlist[playIdx]);
}

void StartTrack(int idx) {
    PlayerApp& app = g_app;
    if (idx < 0 || idx >= kTrackCount) return;
    if (app.current == idx && app.audio.isPlaying()) {
        StopPlayback();
        return;
    }
    if (!app.tracks[idx].ok) {
        app.errorText = "MISSING: " + std::string(kTrackNames[idx]);
        app.current = -1;
        app.audio.stop();
        InvalidateRect(app.hwnd, NULL, FALSE);
        return;
    }
    EnsureAudio();
    if (!app.audioReady) {
        app.errorText = app.audioError.empty() ? "audio device unavailable" : app.audioError;
        InvalidateRect(app.hwnd, NULL, FALSE);
        return;
    }
    SetWindowTextA(app.hwnd, kTitle);
    const ResolvedSource& src = app.tracks[idx].src;
    app.loading = true;
    app.errorText.clear();
    app.current = -1;
    app.audio.stop();
    app.audio.setLoop(true);
    app.visPcm.clear();
    InvalidateRect(app.hwnd, NULL, FALSE);
    UpdateWindow(app.hwnd);

    std::vector<unsigned char> bytes;
    std::string err;
    if (src.kind != SRC_LOOSE_MP3 && !app.resolver->loadBytes(src, bytes, err)) {
        app.loading = false;
        app.errorText = err;
        InvalidateRect(app.hwnd, NULL, FALSE);
        return;
    }
    bool ok = false;
    if (src.kind == SRC_LOOSE_MP3) {
        int n = MultiByteToWideChar(CP_ACP, 0, src.loosePath.c_str(), -1, NULL, 0);
        std::vector<wchar_t> path(n);
        PcmBuffer pcm;
        if (n && MultiByteToWideChar(CP_ACP, 0, src.loosePath.c_str(), -1, path.data(), n))
            ok = DecodeLocalMusic(path.data(), pcm, err) && PlayDecoded(pcm, err);
    } else {
        PcmBuffer pcm;
        if (src.kind == SRC_LOOSE_WAV)
            ok = DecodeWav(bytes.empty() ? NULL : &bytes[0], (unsigned long)bytes.size(), pcm, err, 0);
        else
            ok = DecodeOgg(bytes.empty() ? NULL : &bytes[0], (unsigned long)bytes.size(), pcm, err, 0);
        if (ok) ok = PlayDecoded(pcm, err);
    }
    app.loading = false;
    if (ok) {
        app.current = idx;
        RefreshTitle();
    } else {
        app.errorText = err.empty() ? "decode failed" : err;
        app.current = -1;
    }
    InvalidateRect(app.hwnd, NULL, FALSE);
}

void RefreshTitle();

void SetVolume(int v) {
    PlayerApp& app = g_app;
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    app.settings->volume = v;
    if (app.audioReady) app.audio.setVolume(v);
    RefreshTitle();
    InvalidateRect(app.hwnd, NULL, FALSE);
}

// VOL + theme live in the window title bar, not on the 640x480 canvas (they
// used to overflow the panel's right edge and read as mis-set type).
void RefreshTitle() {
    PlayerApp& app = g_app;
    std::wstring t = L"SOUND MEMOIRS";
    if (app.current == kTrackCount && app.localIndex >= 0 &&
        app.localIndex < (int)app.playlist.size()) {
        const std::wstring& p = app.playlist[app.localIndex];
        t += L" - " + p.substr(p.find_last_of(L"/\\") + 1);
    }
    wchar_t buf[600];
    swprintf(buf, 600, L"%s  [VOL %d] [%hs]", t.c_str(),
             app.settings ? app.settings->volume : 0, Th().name);
    SetWindowTextW(app.hwnd, buf);
}

// ---- selection ---------------------------------------------------------
void MoveSelection(int dcol, int drow) {
    PlayerApp& app = g_app;
    if (app.selected == kExitIndex) {
        if (drow > 0) app.selected = 0;
        InvalidateRect(app.hwnd, NULL, FALSE);
        return;
    }
    int col = app.selected / kGridRows;
    int row = app.selected % kGridRows;
    col += dcol;
    row += drow;
    if (row < 0) {
        if (col > 0) { col--; row = kGridRows - 1; }
        else { app.selected = kExitIndex; InvalidateRect(app.hwnd, NULL, FALSE); return; }
    }
    if (row >= kGridRows) {
        if (col < kGridCols - 1) { col++; row = 0; }
        else row = kGridRows - 1;
    }
    if (col < 0) col = 0;
    if (col > kGridCols - 1) col = kGridCols - 1;
    int idx = col * kGridRows + row;
    if (idx < 0) idx = 0;
    if (idx >= kTrackCount) idx = kTrackCount - 1;
    app.selected = idx;
    InvalidateRect(app.hwnd, NULL, FALSE);
}

void UpdateVisualization() {
    auto& app = g_app;
    const size_t frames = app.audio.isPlaying() ? app.visPcm.size()/2 : 0;
    const size_t first = frames ? memoirs::visualCursor(app.audio.positionSeconds(),app.visRate,frames) : 0;
    memoirs::analyze(app.spectrum,app.visPcm.data(),frames,first);
}

// ---- painting ----------------------------------------------------------
void PaintBackdrop(HDC dc) {
    FillRectCol(dc, 0, 0, S(kLogicalW), S(kLogicalH), kColOuter);
}

// Level meter (asm 0x7F1E43..0x7F20DA): count = abs(int(env/32767*90));
// env here is already normalized, so count = int(env*90).  When count > 0
// the loop draws i = 0..count INCLUSIVE (count+1 stripes); the `cmp 2Dh` at
// 0x7F1ED0 is a dead compare, not a 45-cell cap — i > 44 only switches the
// packed color 0xFF2B1022 -> 0xFF44161E.  Stripe i: y 473-8i .. 479-8i,
// left x 4..138, right x 502..636.  Left meter reads the RIGHT channel
// (same array as the left waveform), right meter reads the LEFT channel.
void PaintLevelMeter(HDC dc) {
    PlayerApp& app = g_app;
    int nL = (int)(app.spectrum.envL * 90.0f);
    int nR = (int)(app.spectrum.envR * 90.0f);
    for (int i = 0; i <= nL && nL > 0; ++i) {
        int y2 = 479 - 8 * i, y1 = 473 - 8 * i;
        FillRectCol(dc, S(4), S(y1), S(138), S(y2), i > 44 ? kColStripHot : kColStrip);
    }
    for (int i = 0; i <= nR && nR > 0; ++i) {
        int y2 = 479 - 8 * i, y1 = 473 - 8 * i;
        FillRectCol(dc, S(502), S(y1), S(636), S(y2), i > 44 ? kColStripHot : kColStrip);
    }
}

// 240-point audio waveform polyline at x=70 (left) / x=570 (right),
// y = 2*i, i = 0..239 (sub_497ECC line calls inside sub_7EEAD8).
void PaintSideWave(HDC dc) {
    PlayerApp& app = g_app;
    if (app.current < 0) return;
    HPEN pen = CreatePen(PS_SOLID, 2, kColWave);
    HGDIOBJ old = SelectObject(dc, pen);
    POINT pts[240];
    for (int side = 0; side < 2; ++side) {
        float base = side == 0 ? 70.0f : 570.0f;
        for (int i = 0; i < 240; ++i) {
            float dx = side == 0 ? app.spectrum.waveL[i] : app.spectrum.waveR[i];
            pts[i].x = S((int)(base + dx));
            pts[i].y = S(2 * i);
        }
        Polyline(dc, pts, 240);
    }
    SelectObject(dc, old);
    DeleteObject(pen);
}

// Panel frame: four lines at x=320-t1-1 / 320+t1, y=240-t2 / 240+t2 where
// t1 = clamp(4*n32,0,128)/128*177 and t2 = clamp(4*n32-128,0,128)/128*236.
// n32 advances once per 60 fps frame; fully expanded after 64 frames (~1.07 s).
void PaintFrame(HDC dc) {
    PlayerApp& app = g_app;
    int c = (int)app.tick;
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
    VLine(dc, S(yt), S(yb), S(xl), kColFrame, 2);
    VLine(dc, S(yt), S(yb), S(xr), kColFrame, 2);
    HLine(dc, S(xl), S(xr), S(yt), kColFrame, 2);
    HLine(dc, S(xl), S(xr), S(yb), kColFrame, 2);
}

void PaintSpectrum(HDC dc, const layout::SpecGeom& g) {
    PlayerApp& app = g_app;
    const int t = (int)app.tick;
    // entrance fades (n32-keyed alphas in the original):
    //   bg quad   clamp(3n32-128,0,128)   grid bands clamp(12n32-512,0,192)
    //   axis      clamp(4n32-256,0,255)   labels     clamp(20n32-800,0,255)
    float bgA = ClampI(3 * t - 128, 0, 128) / 128.0f;
    float seamA = ClampI(12 * t - 512, 0, 192) / 192.0f;
    int axisA = ClampI(4 * t - 256, 0, 255);
    int labelA = ClampI(20 * t - 800, 0, 255);

    COLORREF panel = LerpCol(kColOuter, kColPanel, bgA);
    FillRectCol(dc, S(g.l), S(g.t), S(g.r), S(g.axisY), panel);

    // band seams: 1px columns tinted toward the theme seam color
    for (int i = 0; i < 8; ++i) {
        COLORREF c = LerpCol(panel, Th().seam, seamA);
        VLine(dc, S(g.t), S(g.axisY), S(kSeamX[i]), c, max(1, S(1)));
    }

    // 43 columns: current level (tex 88) from the compressed level, decay
    // tail (tex 87) from the OLD smoothed state down to level/4.
    const int kBars = g.bars;
    for (int k = 0; k < kBars; ++k) {
        int level = (int)app.spectrum.barsLevel[k];
        int old = (int)app.spectrum.barsPrev[k];
        int x1 = S(g.l + g.pitch * k);
        int x2 = S(g.l + g.pitch * k + g.barW);
        if (level > 3) {
            int segs = (level >> 2) + 1;
            for (int m = 0; m < segs; ++m) {
                int y2 = g.barBase - 3 * m;
                int y1 = y2 - kSegH;
                FillRectCol(dc, x1, S(y1), x2, S(y2), kColBar);
            }
        }
        if (old > 3 && (old >> 2) > level / 4) {
            int segs = (old >> 2) - level / 4 + 1;
            for (int m = 0; m < segs; ++m) {
                int mm = level / 4 + m;
                int y2 = g.barBase - 3 * mm;
                int y1 = y2 - kSegH;
                FillRectCol(dc, x1, S(y1), x2, S(y2), kColBarLit);
            }
        }
    }

    // frequency axis: solid vector cyan line + chunky pixel-font labels drawn
    // from the atlas orange row (palette 4), matching the original weight.
    if (axisA > 0) {
        COLORREF fc = LerpCol(panel, Th().frame, axisA / 255.0f);
        FillRectCol(dc, S(g.l), S(g.axisY), S(g.r), S(g.axisY + 2), fc);
    }
    if (labelA > 0) {
        for (int i = 0; i < 8; ++i) {
            int len = (int)strlen(kFreqLabel[i]);
            DrawCells(dc, kFreqLabel[i], kSeamX[i] - 4 * len + 4, g.axisY + 4, 4);
        }
    }
}

void PaintTitle(HDC dc) {
    // drawn as vector runs (crisp at any resolution), not a bitmap blit
    DrawRuns(dc, kTitleRuns, kTitleRunsN, 152, 18,
             Th().title, Th().titleBlock);
}

// Selection cursor: 56x16 cell at x = 156+56*col, y = 288+16*row (EXIT row
// y = 272..288).  Amber margins around the glyph band (band drawn by the
// caller via DrawCells palette 1).  Position jumps instantly.
void PaintSelection(HDC dc, const layout::TracksGeom& g, int col, int row) {
    int x = g.gridX - 12 + g.colW * col;
    int y = g.gridY - 8 + g.rowH * row;
    // original 5-frame pulse: margins brighten on phases 2..4
    bool pulse = (g_app.tick % 5) >= 2;
    COLORREF base = pulse ? LerpCol(kColSelBase, RGB(255, 255, 255), 0.12f) : kColSelBase;
    COLORREF edge = pulse ? LerpCol(kColSelEdge, RGB(255, 255, 255), 0.12f) : kColSelEdge;
    COLORREF foot = pulse ? LerpCol(kColSelFoot, RGB(255, 255, 255), 0.12f) : kColSelFoot;
    FillRectCol(dc, S(x), S(y), S(x + g.colW), S(y + g.rowH), base);
    // cream glyph band: ONE full-width horizontal strip across the cell
    FillRectCol(dc, S(x), S(y + 3), S(x + g.colW), S(y + 14), Th().selBand);
    FillRectCol(dc, S(x), S(y), S(x + g.colW), S(y + 1), edge);
    FillRectCol(dc, S(x), S(y + 14), S(x + g.colW), S(y + 15), foot);
}

// The flashing white light is a horizontal STRIPE inside the orange block
// (not a filled sub-block): a centered translucent white bar whose height
// pulses 0,2,4,6,8 over the original 5-frame cycle = the selection blink.
void PaintSelectionStripe(HDC dc, const layout::TracksGeom& g) {
    PlayerApp& app = g_app;
    int col, row;
    if (app.selected == kExitIndex) { col = 0; row = -1; }
    else { col = app.selected / g.rows; row = app.selected % g.rows; }
    int x = g.gridX - 12 + g.colW * col;
    int y = g.gridY - 8 + g.rowH * row;
    int h = 2 * (int)(app.tick % 5);
    if (h <= 0) return;
    int cy = y + 8;
    AlphaFill(dc, S(x + 1), S(cy - h / 2), S(x + g.colW - 1), S(cy - h / 2 + h),
              Th().selStripe, 0x60);
}

void PaintExit(HDC dc, const layout::TracksGeom& g) {
    bool selected = g_app.selected == kExitIndex;
    if (selected) PaintSelection(dc, g, 0, -1);
    DrawCells(dc, "EXIT", g.exitX, g.exitY, selected ? 1 : 0, true);
}

void PaintGridLabel(HDC dc, int idx, int x, int y, bool selected) {
    char label[8];
    snprintf(label, sizeof(label), "TR:%02d", idx);
    DrawCells(dc, label, x, y, selected ? 1 : 0, true);
}

void PaintGrid(HDC dc, const layout::TracksGeom& g) {
    PlayerApp& app = g_app;
    for (int col = 0; col < g.cols; ++col) {
        for (int row = 0; row < g.rows; ++row) {
            int idx = col * g.rows + row;
            if (idx >= kTrackCount) continue;
            if (app.selected == idx) PaintSelection(dc, g, col, row);
            int x = g.gridX + g.colW * col;
            int y = g.gridY + g.rowH * row;
            PaintGridLabel(dc, idx, x, y, app.selected == idx);
        }
    }
}

void PaintInfo(HDC dc, const layout::TracksGeom& g) {
    PlayerApp& app = g_app;
    const int x = g.infoX, y1 = g.infoY1, y2 = g.infoY2;

    std::string line1;
    if (app.loading) {
        line1 = "<LOADING>";
    } else if (app.current == kTrackCount) {
        line1 = "NOW PLAYING : ";
        if (app.localIndex >= 0 && app.localIndex < (int)app.playlist.size()) {
            const std::wstring& p = app.playlist[app.localIndex];
            size_t pos = p.find_last_of(L"/\\");
            std::wstring name = p.substr(pos + 1);
            std::string narrow;
            for (wchar_t c : name) {
                if (c >= L'a' && c <= L'z') c -= 32;
                narrow += (c >= 32 && c < 127) ? (char)c : '?';
            }
            if (narrow.size() > 24) narrow = narrow.substr(0, 24);
            line1 += narrow;
        } else {
            line1 += "LOCAL FILE";
        }
    } else if (app.current < 0) {
        line1 = "<SILENCE>";
    } else {
        char buf[32];
        snprintf(buf, sizeof(buf), "NOW PLAYING : TRACK %02d", app.current);
        line1 = buf;
    }

    char timeBuf[64];
    int secs = (int)app.audio.positionSeconds();
    if (secs < 0) secs = 0;
    snprintf(timeBuf, sizeof(timeBuf), "PLAY TIME   : %02dSEC", secs);
    std::string line2 = timeBuf;

    // Line 1 includes the "NOW PLAYING" prefix, so a 24-character local title
    // used to make the underline 34 logical pixels wider than the content box.
    const int lineLeft=x-4, lineRight=486;
    const int line1Capacity=max(0,(lineRight-lineLeft)/8);
    if((int)line1.size()>line1Capacity) {
        if(line1Capacity>=3) line1.replace(line1Capacity-3,std::string::npos,"...");
        else line1.resize(line1Capacity);
    }

    // no-underline cells; the underline is ONE continuous horizontal strip
    // per line drawn AFTER both lines (line 2's opaque cells would otherwise
    // overwrite line 1's baked underline).  VOL/theme sit past the strip.
    if (app.current == kTrackCount && app.localIndex >= 0 && app.localIndex < (int)app.playlist.size() &&
        basenameW(app.playlist[app.localIndex]).find_first_not_of(L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 ._-()") != std::wstring::npos)
        lineText(dc,lineLeft,y1-7,lineRight,y1+4,basenameW(app.playlist[app.localIndex]),Th().digit);
    else DrawCells(dc, line1, x, y1, 3);
    DrawCells(dc, line2, x, y2, 3);
    int w1 = (int)line1.size() * 8;
    HLine(dc, S(lineLeft), S(lineLeft) + S(w1), S(y1 + 4), Th().underline, max(1, S(1)));
    HLine(dc, S(lineLeft), S(lineLeft) + S(19 * 8), S(y2 + 4), Th().underline, max(1, S(1)));
}

void PaintHints(HDC dc, const layout::TracksGeom& g) {
    PlayerApp& app = g_app;
    // 0x7EF2DE: (dword_86690C/24)%2 picks the half-alpha blink variant.
    int palette = ((app.tick / 24) % 2) ? 2 : 0;
    DrawCells(dc, "<MAINSHOT: LOOPPLAY> <SUBWEAPON: STOP>", g.hintX, g.hintY, palette);
}

void EnsureBackBuffer(PlayerApp& app, HDC refDC) {
    RECT cr;
    GetClientRect(app.hwnd, &cr);
    int cw = cr.right - cr.left, ch = cr.bottom - cr.top;
    if (cw < 16 || ch < 16) { cw = 640; ch = 480; }
    float sc = (float)ch / (float)kLogicalH;
    float scw = (float)cw / (float)kLogicalW;
    if (scw < sc) sc = scw;
    if (app.settings && app.settings->scaleMode == 1 && sc >= 1.0f) {
        sc = floor(sc + 0.0001f);
    }
    int bw = (int)(kLogicalW * sc + 0.5f), bh = (int)(kLogicalH * sc + 0.5f);

    bool geometryChanged = app.backW != bw || app.backH != bh || g_offX != (cw-bw)/2 || g_offY != (ch-bh)/2;
    g_scale = sc;
    g_bbW = bw; g_bbH = bh;
    g_offX = (cw - bw) / 2; g_offY = (ch - bh) / 2;
    if(geometryChanged && searchEdit && IsWindowVisible(searchEdit)) MoveWindow(searchEdit,g_offX+S(154),g_offY+S(293),S(251),S(16),FALSE);
    if (app.backDC && app.backW == bw && app.backH == bh) return;
    if (app.backBmp) { DeleteObject(app.backBmp); app.backBmp = 0; }
    if (app.backDC) { DeleteDC(app.backDC); app.backDC = 0; }
    app.backDC = CreateCompatibleDC(refDC);
    app.backBmp = CreateCompatibleBitmap(refDC, bw, bh);
    SelectObject(app.backDC, app.backBmp);
    app.backW = bw;
    app.backH = bh;
    SetBkMode(app.backDC, TRANSPARENT);
    SetStretchBltMode(app.backDC, COLORONCOLOR);
    EnsureArt();
}

// --shot: dump the finished back buffer as a 24bpp bottom-up BMP so layout
// changes can be diffed against the pre-change baseline.
bool SaveBackBufferBmp(PlayerApp& app, const std::string& path) {
    if (!app.backDC || !app.backBmp || app.backW <= 0 || app.backH <= 0) return false;
    const int w = app.backW, h = app.backH;
    const int stride = ((w * 3) + 3) & ~3;
    const DWORD imageSize = (DWORD)stride * (DWORD)h;
    std::vector<unsigned char> pixels(imageSize, 0);
    BITMAPINFO bmi;
    memset(&bmi, 0, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;    // top-down rows; BMP readers accept this
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 24;
    bmi.bmiHeader.biCompression = BI_RGB;
    // GetDIBits refuses a bitmap that is still selected into the DC, so ask a
    // throwaway DC that never had app.backBmp in it.
    HDC tmp = CreateCompatibleDC(NULL);
    const bool got = tmp && GetDIBits(tmp, app.backBmp, 0, (UINT)h, pixels.data(),
                                      &bmi, DIB_RGB_COLORS);
    if (tmp) DeleteDC(tmp);
    if (!got) return false;

    BITMAPFILEHEADER fh;
    memset(&fh, 0, sizeof(fh));
    fh.bfType = 0x4D42;
    fh.bfOffBits = (DWORD)(sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER));
    fh.bfSize = fh.bfOffBits + imageSize;
    FILE* f = NULL;
    if (fopen_s(&f, path.c_str(), "wb") != 0 || !f) return false;
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&bmi.bmiHeader, sizeof(BITMAPINFOHEADER), 1, f);
    fwrite(pixels.data(), 1, imageSize, f);
    fclose(f);
    return true;
}

// --shot runs without an audio device; fill the visualiser with a
// deterministic synthetic spectrum so layout screenshots exercise every
// element instead of showing an empty field.
void FillShotSpectrum(PlayerApp& app) {
    for (int k = 0; k < memoirs::kSpectrumBands; ++k) {
        const float t = k / float(memoirs::kSpectrumBands);
        float v = 30.f + 55.f * exp(-pow((t - 0.22f) * 3.4f, 2.f))
                       + 34.f * exp(-pow((t - 0.55f) * 5.0f, 2.f));
        v += 12.f * sinf(t * 21.f);
        app.spectrum.barsLevel[k] = std::clamp(v, 0.f, 96.f);
        app.spectrum.barsPrev[k] = std::clamp(v * 0.72f, 0.f, 96.f);
    }
    for (int i = 0; i < 240; ++i) {
        const float p = i / 240.f;
        app.spectrum.waveL[i] = 26.f * sinf(p * 47.f) *
                                (0.4f + 0.6f * sinf(p * 3.1f));
        app.spectrum.waveR[i] = 20.f * sinf(p * 61.f + 1.7f) *
                                (0.5f + 0.5f * sinf(p * 2.3f));
    }
    app.spectrum.envL = 0.62f;
    app.spectrum.envR = 0.48f;
    for (int i = 0; i < 30; ++i)
        stageMemory.advance(app.spectrum, 0, i * 0.05, true, false);
}

// The archive replaces the lower instrument panel; the visualizer keeps running.
using lib::tr;
void panelBox(HDC dc,int x,int y,int r,int b,COLORREF c) { FillRectCol(dc,S(x),S(y),S(r),S(b),c); }
void noteRailActivity() { lastRailActivityTick = g_app.tick; }
float channelRailOpacity() {
    if (hoverChannel >= 0) return 1.0f;
    const unsigned idle = g_app.tick - lastRailActivityTick;
    if (idle <= kRailIdleDelayFrames) return 1.0f;
    if (idle >= kRailIdleDelayFrames + kRailFadeFrames) return 0.0f;
    return 1.0f - float(idle - kRailIdleDelayFrames) / kRailFadeFrames;
}
void channelRail(HDC dc) {
    const int left=S(railX-4), top=S(272);
    const int right=S(railX+3*channelWidth+4), bottom=S(288);
    const int width=right-left, height=bottom-top;
    const float opacity=channelRailOpacity();
    const bool fading=opacity<0.999f;
    static HDC savedDC=nullptr;
    static HBITMAP savedBmp=nullptr;
    static int savedW=0,savedH=0;
    if(fading) {
        if(savedW!=width || savedH!=height) {
            if(savedBmp) DeleteObject(savedBmp);
            if(savedDC) DeleteDC(savedDC);
            savedDC=CreateCompatibleDC(dc);
            savedBmp=CreateCompatibleBitmap(dc,width,height);
            SelectObject(savedDC,savedBmp);
            savedW=width;savedH=height;
        }
        BitBlt(savedDC,0,0,width,height,dc,left,top,SRCCOPY);
    }
    static const char* labels[]={"TR","LIB","SYS"};
    for(int i=0;i<3;++i) {
        int x=railX+i*channelWidth;
        bool active=i==int(page);
        if(page==Page::System) {
            const wchar_t* configChannels[]={L"TR",L"LIB",L"SYS"};
            lineText(dc,x+5,273,x+38,287,configChannels[i],active?Th().digit:Th().ink);
        } else DrawCells(dc,labels[i],x+8,280,active?0:2);
        if(i==hoverChannel && !active) panelBox(dc,x+4,286,x+30,287,Th().underline);
        if(active) {
            panelBox(dc,x,276,x+2,284,Th().bar);
            panelBox(dc,x,285,x+5,286,Th().bar);
        }
    }
    if(fading) {
        const int cover=ClampI((int)((1.0f-opacity)*255.0f+0.5f),0,255);
        BLENDFUNCTION blend={AC_SRC_OVER,0,(BYTE)cover,0};
        AlphaBlend(dc,left,top,width,height,savedDC,0,0,width,height,blend);
    }
}
std::wstring stamp(double seconds) {
    int s=max(0,(int)seconds);wchar_t t[32];swprintf_s(t,L"%02d:%02d",s/60,s%60);return t;
}
const wchar_t* orderLabel() {
    switch(localOrder) {
        case RepeatOne:return tr(L"ONE",L"单曲",L"一曲");
        case RepeatAll:return tr(L"LOOP",L"列表循环",L"全曲");
        case Shuffle:return tr(L"SHUFFLE",L"随机",L"ランダム");
        default:return tr(L"ORDER",L"顺序",L"順番");
    }
}
void changeOrder() {localOrder=(localOrder+1)%4;if(g_app.current==kTrackCount)g_app.audio.setLoop(localOrder==RepeatOne);}
void paintSeekRail(HDC dc, double fraction) {
    panelBox(dc,seekLeft,seekY,seekRight,seekY+2,Th().seam);
    int end=seekLeft+int((seekRight-seekLeft)*std::clamp(fraction,0.,1.)+.5);
    if(end>seekLeft) panelBox(dc,seekLeft,seekY,end,seekY+2,Th().bar);
}
// Give the pointer immediate feedback without rendering the spectrum or
// resetting the audio device for every move. WM_PAINT uses the same rail.
void previewSeek(HWND hwnd,double logicalX) {
    dragFraction=seekFraction(logicalX);
    if (!g_app.backDC) return;
    paintSeekRail(g_app.backDC,dragFraction);
    HDC dc=GetDC(hwnd);
    if(dc) {
        BitBlt(dc,g_offX+S(seekLeft),g_offY+S(seekY),S(seekRight)-S(seekLeft),
               S(seekY+2)-S(seekY),g_app.backDC,S(seekLeft),S(seekY),SRCCOPY);
        ReleaseDC(hwnd,dc);
    }
}
void archivePaint(HDC dc) {
    auto& a=g_app;
    const PanelTextMode libraryTextMode = a.settings->libFont == 1
        ? PanelTextMode::PixelOnly : PanelTextMode::AtlasFirst;
    auto now=a.current==kTrackCount && a.localIndex>=0 ? titleOf(a.playlist[a.localIndex]):
        a.current>=0 ? std::wstring(L"TR / ")+std::to_wstring(a.current):std::wstring(tr(L"Standby",L"待机",L"待機"));
    bool local=a.current==kTrackCount;
    lineText(dc,154,249,201,264,a.audio.isPaused()?L"HOLD":L"ON AIR",Th().digit);
    lineText(dc,204,249,383,264,now,Th().ink);
    double duration=local?a.audio.durationSeconds():0, pos=draggingSeek?duration*dragFraction:a.audio.positionSeconds();
    lineText(dc,384,249,486,264,stamp(pos)+L"/"+stamp(duration),Th().digit,DT_RIGHT);
    paintSeekRail(dc,duration>0?pos/duration:0);
    lineText(dc,154,271,330,289,tr(L"LOCAL ARCHIVE",L"本地曲库",L"音楽庫"),Th().ink);
    panelBox(dc,154,293,405,309,Th().strip);
    lineText(dc,159,293,400,309,query.empty()?tr(L"Search tracks",L"搜索曲名、艺人或专辑",L"曲・アーティスト検索"):query,Th().ink);
    lineText(dc,406,293,486,309,tr(L"Add files",L"添加音乐",L"曲を追加"),Th().digit,DT_CENTER);
    int page=cursor/rows;
    // Thin ruled slots echo the spectrum divisions even when the catalog is sparse.
    for(int row=0;row<rows;++row) {
        int y=330+row*18;
        panelBox(dc,154,y,158,y+1,Th().frame);
        panelBox(dc,160,y,486,y+1,Th().strip);
    }
    if(matches.empty()) {
        lineText(dc,154,349,486,375,query.empty()?tr(L"Drop music here or choose + Add files",L"拖入音乐，或点击「添加音乐」",L"音楽をドロップ、または曲を追加"):
            tr(L"No matching tracks",L"没有匹配的歌曲",L"一致する曲がありません"),Th().ink,DT_CENTER);
    }
    for(int row=0;row<rows && page*rows+row<(int)matches.size();++row) {
        int pos=page*rows+row,idx=matches[pos],y=313+row*18;
        if(cursor==pos) panelBox(dc,154,y+2,157,y+15,Th().selEdge);
        wchar_t n[24];swprintf_s(n,L"%03d",idx+1);
        lineText(dc,161,y,188,y+17,n,Th().digit);
        lineText(dc,192,y,464,y+17,titleOf(a.playlist[idx]),Th().ink,DT_LEFT,
                 libraryTextMode);
        if(local && a.localIndex==idx) for(int j=0;j<3;++j) {
            int height=a.audio.isPaused()?2:2+std::clamp((int)a.spectrum.barsLevel[3+j*4],0,10);
            panelBox(dc,471+j*4,y+14-height,473+j*4,y+14,Th().bar);
        }
    }
    panelBox(dc,154,440,486,441,Th().frame);
    lineText(dc,154,443,212,460,orderLabel(),Th().digit);
    lineText(dc,213,443,277,460,local && !a.audio.isPaused()?tr(L"PAUSE",L"暂停",L"一時停止"):tr(L"PLAY",L"播放",L"再生"),Th().digit);
    lineText(dc,278,443,331,460,tr(L"Stop",L"停止",L"停止"),Th().digit);
    lineText(dc,341,443,375,460,L"◀",Th().digit,DT_CENTER);
    lineText(dc,375,443,453,460,std::to_wstring(matches.empty()?0:page+1)+L" / "+std::to_wstring((matches.size()+rows-1)/rows),Th().ink,DT_CENTER);
    lineText(dc,453,443,486,460,L"▶",Th().digit,DT_CENTER);
    if(!matches.empty()) {
        auto& path=a.playlist[matches[cursor]];auto t=tagReader.get(path);
        auto info=t.artist+(t.artist.empty()||t.album.empty()?L"":L" / ")+t.album;
        lineText(dc,154,461,486,476,info.empty()?path:info,Th().ink,DT_LEFT,
                 libraryTextMode);
    }
}
void focusSearch(bool show) {
    if(!searchEdit) return;
    if(show) {
        MoveWindow(searchEdit,g_offX+S(154),g_offY+S(293),S(251),S(16),TRUE);
        int pixels=max(8,S(12));
        if(pixels!=fontPixels) {
            HFONT next=CreateFontW(-pixels,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,0,0,L"Microsoft YaHei UI");
            SendMessageW(searchEdit,WM_SETFONT,(WPARAM)next,TRUE);
            if(panelFont) DeleteObject(panelFont);
            panelFont=next;fontPixels=pixels;
        }
        ShowWindow(searchEdit,SW_SHOW);SetFocus(searchEdit);
        SendMessageW(searchEdit,EM_SETSEL,0,-1);
    } else {ShowWindow(searchEdit,SW_HIDE);SetFocus(g_app.hwnd);}
}
void moveCursor(int delta) {
    if(!matches.empty()) cursor=std::clamp(cursor+delta,0,(int)matches.size()-1);
}
void playCursor() { if(!matches.empty()) OpenMusic(g_app.playlist[matches[cursor]]); }
void PaintWings(HDC dc,int style,const stage::Memory& motion){
    if(style==1){ PaintLevelMeter(dc); PaintSideWave(dc); return; }
    const auto& t=Th();
    for(int side=0;side<2;++side){
        int base=side?570:70;
        const float* wave=side?g_app.spectrum.waveR:g_app.spectrum.waveL;
        if(style==3){
            POINT upper[220],lower[220];
            for(int j=0;j<220;j++){
                float v=wave[j], lag=wave[(j+12)%220];
                int center=base+int(.58f*v), w=3+int(.37f*std::abs(v-lag));
                int y=26+2*j;
                upper[j]={S(center-w),S(y)};lower[j]={S(center+w),S(y)};
                if(j%5==0)FillRectCol(dc,S(center-w),S(y),S(center+w+1),S(y+1),t.strip);
            }
            HGDIOBJ old=SelectObject(dc,GetStockObject(DC_PEN));
            SetDCPenColor(dc,t.wave);Polyline(dc,upper,220);
            SetDCPenColor(dc,t.bar);Polyline(dc,lower,220);SelectObject(dc,old);
        }else{
            const int span=2+int(61*std::clamp(motion.wing[side],0.f,1.f));
            for(int row=0;row<44;row++){
                float peak=0;
                for(int j=0;j<5;j++)peak=std::max(peak,std::abs(wave[row*5+j]));
                const float contour=.34f+.66f*sqrt(std::min(1.f,peak/64.f));
                int width=2+int((span-2)*contour), y=27+row*10;
                if(style==2){
                    const float heat=std::clamp(contour*.62f+motion.wing[side]*.38f,0.f,1.f);
                    const COLORREF ink=row&1
                        ?LerpCol(t.wave,t.underline,.62f+.16f*heat)
                        :LerpCol(t.wave,t.bar,.08f+.34f*heat);
                    FillRectCol(dc,S(base-width),S(y),S(base+width),S(y+2),ink);
                    const int length=row%5==0?5:2;
                    const COLORREF tick=row%5==0
                        ?LerpCol(t.hint,t.bar,.34f+.34f*heat)
                        :LerpCol(t.hint,t.underline,.42f+.22f*heat);
                    for(int edge:{-64,63}){
                        const int x=base+edge;
                        FillRectCol(dc,S(x),S(y),S(x+(edge<0?length:-length)),S(y+1),tick);
                    }
                }else{
                    for(int col=0;col<12;col++){
                        int x=base+(side?-1:1)*(col*10-55);
                        bool lit=col*5+2<width;
                        FillRectCol(dc,S(x-2),S(y-1),S(x+(lit?3:0)),S(y+(lit?3:1)),
                            lit?(col>8?t.bar:t.wave):LerpCol(t.outer,t.wave,.30f));
                    }
                }
            }
        }
        DrawCells(dc,side?"CH : L":"CH : R",base-20,13,3);
    }
}
// ---- composition: full-frame field -------------------------------------
// No panel frame and no side wings: the spectrum becomes a symmetric
// full-width field with the title floating in a corner.  TAB raises the
// original TR grid on top so selection keeps the original coordinates.
void PaintFieldLayout(HDC dc) {
    PlayerApp& app = g_app;
    const auto& t = Th();
    FillRectCol(dc, 0, 0, S(kLogicalW), S(kLogicalH),
                LerpCol(t.outer, t.panel, 0.55f));

    const int baseY = 340;
    FillRectCol(dc, 0, S(baseY), S(kLogicalW), S(baseY + 2),
                LerpCol(t.frame, t.outer, 0.42f));

    const int cols = memoirs::kSpectrumBands;
    const int pitch = 14, barW = 12;
    const int x0 = (kLogicalW - cols * pitch) / 2;
    for (int k = 0; k < cols; ++k) {
        const float lv = std::clamp(app.spectrum.barsLevel[k] / 96.0f, 0.f, 1.f);
        const int x1 = x0 + k * pitch, x2 = x1 + barW;
        const int up = int(lv * 258);
        if (up > 1) {
            FillRectCol(dc, S(x1), S(baseY - up), S(x2), S(baseY),
                        LerpCol(t.bar, t.wave, 0.42f));
            FillRectCol(dc, S(x1), S(baseY - up), S(x2), S(baseY - up + 3), t.barLit);
        }
        const int down = int(lv * 46);
        if (down > 1) {
            FillRectCol(dc, S(x1), S(baseY + 2), S(x2), S(baseY + 2 + down),
                        LerpCol(t.bar, t.panel, 0.55f));
        }
    }

    // frequency ticks and labels sit below the mirrored tail
    for (int i = 0; i < 8; ++i) {
        const int x = 48 + i * 78;
        FillRectCol(dc, S(x), S(baseY + 4), S(x + 1), S(baseY + 50),
                    LerpCol(t.frame, t.outer, 0.55f));
        const int len = (int)strlen(kFreqLabel[i]);
        DrawCells(dc, kFreqLabel[i], x - 4 * len + 4, baseY + 52, 4);
    }

    // Channel traces: left channel above the baseline, right below, so the
    // two never paint over each other.
    POINT pts[120];
    for (int side = 0; side < 2; ++side) {
        const float* wv = side ? app.spectrum.waveR : app.spectrum.waveL;
        for (int i = 0; i < 120; ++i) {
            const float v = std::clamp(wv[i * 2] / 64.0f, -1.f, 1.f);
            pts[i].x = S(i * kLogicalW / 119);
            pts[i].y = S(baseY + (side ? 2 + int(v * 62) : -2 - int(v * 62)));
        }
        HGDIOBJ old = SelectObject(dc, GetStockObject(DC_PEN));
        SetDCPenColor(dc, side ? LerpCol(t.wave, t.outer, 0.38f) : t.wave);
        Polyline(dc, pts, 120);
        SelectObject(dc, old);
    }

    DrawCells(dc, "SOUND MEMOIRS", 24, 24, 0);
    {
        const char* themeName = t.name;
        const int w = (int)strlen(themeName) * 8;
        DrawCells(dc, themeName, 616 - w, 24, 2);
    }

    std::string now;
    if (app.current == kTrackCount) now = "NOW PLAYING : LOCAL FILE";
    else if (app.current >= 0) {
        char b[48];
        snprintf(b, sizeof(b), "NOW PLAYING : TRACK %02d", app.current);
        now = b;
    } else now = "<SILENCE>";
    DrawCells(dc, now, 24, 452, 3);
    {
        char tb[32];
        int secs = max(0, (int)app.audio.positionSeconds());
        snprintf(tb, sizeof(tb), "TIME : %02dSEC", secs);
        DrawCells(dc, tb, 616 - (int)strlen(tb) * 8, 452, 3);
    }
    DrawCells(dc, "TAB GRID / F9 LAYOUT", 24, 466, 2);
    {
        char sb[16];
        snprintf(sb, sizeof(sb), "SEL:TR:%02d", std::max(0, app.selected));
        DrawCells(dc, sb, 616 - (int)strlen(sb) * 8, 466, 2);
    }

    if (fieldGrid) {
        AlphaFill(dc, 0, 0, S(kLogicalW), S(kLogicalH), RGB(10, 12, 16), 0x8C);
        FillRectCol(dc, S(148), S(266), S(492), S(kLogicalH), t.outer);
        const layout::TracksGeom& g = TG();
        PaintExit(dc, g);
        PaintGrid(dc, g);
        PaintSelectionStripe(dc, g);
        DrawCells(dc, "TAB CLOSE", 24, 466, 2);
    }
}

// ---- composition: vertical three-column rack ---------------------------
// A 24px title bar over three columns: channel meters, the main display and
// the full 46-track list.  Nothing is borrowed from the original panel frame.
void PaintStackLayout(HDC dc) {
    PlayerApp& app = g_app;
    const auto& t = Th();
    FillRectCol(dc, 0, 0, S(kLogicalW), S(kLogicalH), LerpCol(t.outer, t.panel, 0.35f));

    FillRectCol(dc, 0, 0, S(kLogicalW), S(24), t.panel);
    FillRectCol(dc, 0, S(23), S(kLogicalW), S(24), t.frame);
    DrawCells(dc, "SOUND MEMOIRS", 16, 8, 0);
    { const char* nm = t.name; DrawCells(dc, nm, 616 - (int)strlen(nm) * 8, 8, 2); }

    // left: two channel meters, bottom-up
    for (int side = 0; side < 2; ++side) {
        const int x1 = side ? 44 : 18, x2 = side ? 56 : 30;
        const int n = std::clamp(int((side ? app.spectrum.envR : app.spectrum.envL) * 60.0f), 0, 60);
        for (int i = 0; i < n; ++i) {
            const int y2 = 440 - i * 6;
            FillRectCol(dc, S(x1), S(y2 - 5), S(x2), S(y2),
                        i > 44 ? t.stripHot : t.strip);
        }
        DrawCells(dc, side ? "R" : "L", x1 + 1, 446, 2);
    }

    // centre display
    const int cl = 96, ct = 32, cr = 400, cb = 440;
    FillRectCol(dc, S(cl), S(ct), S(cr), S(cb), LerpCol(t.panel, t.outer, 0.30f));
    FillRectCol(dc, S(cl), S(ct), S(cr), S(ct + 1), t.frame);
    FillRectCol(dc, S(cl), S(cb - 1), S(cr), S(cb), t.frame);
    FillRectCol(dc, S(cl), S(ct), S(cl + 1), S(cb), t.frame);
    FillRectCol(dc, S(cr - 1), S(ct), S(cr), S(cb), t.frame);

    const int baseY = 330;
    const int cols = memoirs::kSpectrumBands;
    const int pitch = 6, barW = 5;
    const int x0 = cl + 1 + ((cr - cl - 2) - cols * pitch) / 2;
    for (int k = 0; k < cols; ++k) {
        const float lv = std::clamp(app.spectrum.barsLevel[k] / 96.0f, 0.f, 1.f);
        const int x1 = x0 + k * pitch, x2 = x1 + barW;
        const int up = int(lv * 238);
        if (up > 1) {
            FillRectCol(dc, S(x1), S(baseY - up), S(x2), S(baseY),
                        LerpCol(t.wave, t.bar, 0.40f));
            FillRectCol(dc, S(x1), S(baseY - up), S(x2), S(baseY - up + 2), t.barLit);
        }
        const int down = int(lv * 22);
        if (down > 1)
            FillRectCol(dc, S(x1), S(baseY + 1), S(x2), S(baseY + 1 + down),
                        LerpCol(t.bar, t.panel, 0.58f));
    }

    // lower half: the two channel traces
    POINT pts[120];
    for (int side = 0; side < 2; ++side) {
        const float* wv = side ? app.spectrum.waveR : app.spectrum.waveL;
        const int cy = side ? 416 : 392;
        for (int i = 0; i < 120; ++i) {
            const float v = std::clamp(wv[i * 2] / 64.0f, -1.f, 1.f);
            pts[i].x = S(cl + 4 + i * (cr - cl - 8) / 119);
            pts[i].y = S(cy + int(v * 18));
        }
        HGDIOBJ old = SelectObject(dc, GetStockObject(DC_PEN));
        SetDCPenColor(dc, side ? LerpCol(t.wave, t.bar, 0.45f) : t.wave);
        Polyline(dc, pts, 120);
        SelectObject(dc, old);
    }
    DrawCells(dc, "FIELD / 43 BAND", cl + 4, ct + 4, 2);

    // right: full track list, two columns of 23
    const int listX = 408, listY = 40, colW = 108, rowH = 17;
    for (int i = 0; i < kTrackCount; ++i) {
        const int col = i / 23, row = i % 23;
        const int x = listX + col * colW, y = listY + row * rowH;
        const bool sel = app.selected == i;
        if (sel) {
            FillRectCol(dc, S(x), S(y), S(x + colW - 6), S(y + rowH - 2),
                        LerpCol(t.selBand, t.panel, 0.30f));
            FillRectCol(dc, S(x), S(y), S(x + 3), S(y + rowH - 2), t.bar);
        }
        char b[8];
        snprintf(b, sizeof(b), "TR:%02d", i);
        DrawCells(dc, b, x + 8, y + 3, sel ? 0 : 2);
    }

    // bottom rail: seek + time + hint
    FillRectCol(dc, S(cl), S(452), S(cr), S(454), t.seam);
    const double dur = app.current == kTrackCount ? app.audio.durationSeconds() : 0.0;
    const double pos = dur > 0 ? app.audio.positionSeconds() : 0.0;
    const int end = cl + int((cr - cl) * std::clamp(pos / (dur > 0 ? dur : 1.0), 0.0, 1.0));
    if (end > cl) FillRectCol(dc, S(cl), S(452), S(end), S(454), t.bar);
    {
        char tb[32];
        snprintf(tb, sizeof(tb), "TIME : %02dSEC", std::max(0, int(pos)));
        DrawCells(dc, tb, 632 - (int)strlen(tb) * 8, 460, 3);
    }
    DrawCells(dc, "F7 VISUAL / F8 WING / F9 LAYOUT", cl, 460, 2);
}

// ---- composition: circular instrument dial -----------------------------
// The 43 bands radiate from a ring instead of a horizontal window; the 46
// tracks become tick marks around the outside so the whole screen reads as
// one dial.  TAB is not needed here.
void PaintRingLayout(HDC dc) {
    PlayerApp& app = g_app;
    const auto& t = Th();
    const float kPi = 3.14159265f;
    const int cx = 320, cy = 236;
    FillRectCol(dc, 0, 0, S(kLogicalW), S(kLogicalH), LerpCol(t.outer, t.panel, 0.42f));

    // concentric guide rings
    auto ring = [&](int radius, COLORREF c, int thick) {
        const int segs = 120;
        for (int i = 0; i < segs; ++i) {
            const float a = 2.f * kPi * i / segs;
            const int x = cx + int(radius * cosf(a));
            const int y = cy + int(radius * sinf(a));
            FillRectCol(dc, S(x), S(y), S(x + thick), S(y + thick), c);
        }
    };
    ring(96, LerpCol(t.frame, t.outer, 0.35f), 1);
    ring(126, LerpCol(t.frame, t.outer, 0.20f), 1);
    ring(196, LerpCol(t.seam, t.outer, 0.30f), 1);

    // 43 radial band blocks, starting at 12 o'clock
    const int bands = memoirs::kSpectrumBands;
    for (int k = 0; k < bands; ++k) {
        const float lv = std::clamp(app.spectrum.barsLevel[k] / 96.0f, 0.f, 1.f);
        const int blocks = 2 + int(lv * 10.f);
        const float a = -kPi / 2.f + 2.f * kPi * k / bands;
        for (int b = 0; b < blocks; ++b) {
            const int r = 132 + b * 6;
            const int x = cx + int(r * cosf(a)) - 1;
            const int y = cy + int(r * sinf(a)) - 1;
            const COLORREF c = (b == blocks - 1) ? t.bar
                              : LerpCol(t.wave, t.bar, 0.28f);
            FillRectCol(dc, S(x), S(y), S(x + 3), S(y + 3), c);
        }
    }

    // centre: phase diamond with up to three decayed traces
    {
        POINT frame[] = {{S(cx), S(cy - 38)}, {S(cx + 46), S(cy)}, {S(cx), S(cy + 38)},
                         {S(cx - 46), S(cy)}, {S(cx), S(cy - 38)}};
        HGDIOBJ old = SelectObject(dc, GetStockObject(DC_PEN));
        SetDCPenColor(dc, t.frame);
        Polyline(dc, frame, 5);
        // XY plot of the two channels: the classic vector-display figure
        {
            POINT pts2[120];
            for (int i = 0; i < 120; ++i) {
                const float l = std::clamp(app.spectrum.waveL[i * 2] / 64.f, -1.f, 1.f);
                const float r = std::clamp(app.spectrum.waveR[i * 2] / 64.f, -1.f, 1.f);
                pts2[i] = {S(cx + int(l * 34)), S(cy - int(r * 34))};
            }
            SetDCPenColor(dc, t.bar);
            Polyline(dc, pts2, 120);
        }
        SetDCPenColor(dc, t.bar);
        FillRectCol(dc, S(cx - 1), S(cy - 1), S(cx + 2), S(cy + 2), t.bar);
        SelectObject(dc, old);
    }

    // 46 track ticks on the outside
    for (int i = 0; i < kTrackCount; ++i) {
        const float a = -kPi / 2.f + 2.f * kPi * i / kTrackCount;
        const int x = cx + int(206 * cosf(a));
        const int y = cy + int(206 * sinf(a));
        const bool sel = app.selected == i;
        const int halfW = sel ? 3 : 2, halfH = sel ? 6 : 3;
        FillRectCol(dc, S(x - halfW), S(y - halfH), S(x + halfW), S(y + halfH),
                    sel ? t.bar : LerpCol(t.seam, t.ink, 0.55f));
    }

    DrawCells(dc, "SOUND MEMOIRS", 152, 14, 0);
    { const char* nm = t.name; DrawCells(dc, nm, 632 - (int)strlen(nm) * 8, 14, 2); }

    {
        char b[16];
        snprintf(b, sizeof(b), "TR:%02d", std::max(0, app.selected));
        DrawCells(dc, b, cx - (int)strlen(b) * 4, 446, app.selected < kTrackCount ? 0 : 2);
    }
    DrawCells(dc, "F7 VISUAL / F9 LAYOUT", 196, 462, 2);
}

// ---- composition: crosshair / vector display ---------------------------
// Four axes leave a centre window: title above, progress below, one channel
// trace left and right.  TAB raises the original TR grid like Field does.
void PaintCrossLayout(HDC dc) {
    PlayerApp& app = g_app;
    const auto& t = Th();
    FillRectCol(dc, 0, 0, S(kLogicalW), S(kLogicalH), LerpCol(t.outer, t.panel, 0.48f));

    const int cx = 320, cy = 240;
    const COLORREF axis = LerpCol(t.frame, t.outer, 0.66f);
    FillRectCol(dc, S(cx - 1), S(24), S(cx + 1), S(456), axis);
    FillRectCol(dc, S(24), S(cy - 1), S(616), S(cy + 1), axis);

    for (int y = 40; y < 456; y += 40) {
        FillRectCol(dc, S(cx - 6), S(y), S(cx - 1), S(y + 1), t.seam);
        FillRectCol(dc, S(cx + 1), S(y), S(cx + 6), S(y + 1), t.seam);
    }
    for (int x = 40; x < 616; x += 40) {
        FillRectCol(dc, S(x), S(cy - 6), S(x + 1), S(cy - 1), t.seam);
        FillRectCol(dc, S(x), S(cy + 1), S(x + 1), S(cy + 6), t.seam);
    }

    // channel traces ride above (left) and below (right) the horizontal axis
    POINT pts[120];
    for (int side = 0; side < 2; ++side) {
        const float* wv = side ? app.spectrum.waveR : app.spectrum.waveL;
        const int sway = side ? 26 : -26;
        const int x0 = side ? 392 : 24;
        for (int i = 0; i < 120; ++i) {
            const float v = std::clamp(wv[i * 2] / 64.0f, -1.f, 1.f);
            pts[i].x = S(x0 + i * 224 / 119);
            pts[i].y = S(cy + sway + int(v * 16));
        }
        HGDIOBJ old = SelectObject(dc, GetStockObject(DC_PEN));
        SetDCPenColor(dc, side ? LerpCol(t.wave, t.bar, 0.45f) : t.wave);
        Polyline(dc, pts, 120);
        SelectObject(dc, old);
    }

    // centre window: phase diamond over up to three decayed traces
    {
        POINT frame[] = {{S(cx), S(cy - 40)}, {S(cx + 48), S(cy)}, {S(cx), S(cy + 40)},
                         {S(cx - 48), S(cy)}, {S(cx), S(cy - 40)}};
        HGDIOBJ old = SelectObject(dc, GetStockObject(DC_PEN));
        SetDCPenColor(dc, t.frame);
        Polyline(dc, frame, 5);
        // XY plot of the two channels inside the diamond
        {
            POINT pts2[120];
            for (int i = 0; i < 120; ++i) {
                const float l = std::clamp(app.spectrum.waveL[i * 2] / 64.f, -1.f, 1.f);
                const float r = std::clamp(app.spectrum.waveR[i * 2] / 64.f, -1.f, 1.f);
                pts2[i] = {S(cx + int(l * 40)), S(cy - int(r * 40))};
            }
            SetDCPenColor(dc, t.bar);
            Polyline(dc, pts2, 120);
        }
        SelectObject(dc, old);
    }

    // progress runs down the lower axis
    {
        const double dur = app.current == kTrackCount ? app.audio.durationSeconds() : 0.0;
        const double pos = dur > 0 ? app.audio.positionSeconds() : 0.0;
        const int len = int(192.0 * std::clamp(pos / (dur > 0 ? dur : 1.0), 0.0, 1.0));
        FillRectCol(dc, S(cx - 3), S(264), S(cx + 3), S(264 + len), t.bar);
    }

    DrawCells(dc, "SOUND MEMOIRS", 24, 20, 0);
    { const char* nm = t.name; DrawCells(dc, nm, 616 - (int)strlen(nm) * 8, 20, 2); }
    DrawCells(dc, "EXIT", 24, 300, app.selected == kExitIndex ? 1 : 2);

    {
        char b[24];
        snprintf(b, sizeof(b), "TR:%02d", std::max(0, app.selected));
        DrawCells(dc, b, cx - (int)strlen(b) * 4, 466, 0);
    }
    DrawCells(dc, "TAB GRID / F9 LAYOUT", 24, 466, 2);

    if (fieldGrid) {
        AlphaFill(dc, 0, 0, S(kLogicalW), S(kLogicalH), RGB(10, 12, 16), 0x8C);
        FillRectCol(dc, S(148), S(266), S(492), S(kLogicalH), t.outer);
        const layout::TracksGeom& g = TG();
        PaintExit(dc, g);
        PaintGrid(dc, g);
        PaintSelectionStripe(dc, g);
        DrawCells(dc, "TAB CLOSE", 24, 466, 2);
    }
}

void PaintScene(HDC dc) {
    SetBkMode(dc, TRANSPARENT);
    const int layoutMode = g_app.settings->layoutMode;
    if (page == Page::Tracks) {
        if (layoutMode == layout::Field) { PaintFieldLayout(dc); return; }
        if (layoutMode == layout::Stack) { PaintStackLayout(dc); return; }
        if (layoutMode == layout::Ring) { PaintRingLayout(dc); return; }
        if (layoutMode == layout::Cross) { PaintCrossLayout(dc); return; }
    }
    PaintBackdrop(dc);
    const int mode=g_app.settings->stageMode;
    PaintWings(dc,g_app.settings->wingMode?g_app.settings->wingMode:mode+1,stageMemory);
    PaintFrame(dc);
    if(!mode) PaintSpectrum(dc, SG());
    else {
        const auto& t=Th();
        stage::paint({t.panel,LerpCol(t.seam,t.wave,.48f),t.wave,t.bar,t.frame},dc,g_scale,mode,g_app.spectrum,stageMemory);
        const char* marks[]={"","ORBIT : 01","STRATA : 02","PHASE : 03"};
        DrawCells(dc,marks[mode],152,74,3);
    }
    PaintTitle(dc);
    if(page==Page::System) {
        const config_panel::State state = {
            settingRow, g_app.settings->volume, Th().name,
            lib::language(), g_app.settings->libFont,
            layout::name(g_app.settings->layoutMode, lib::language())
        };
        const config_panel::DrawContext drawing = {
            dc, g_scale, PanelAtlas(),
            {Th().panel, Th().frame, Th().bar, Th().ink, Th().strip,
             Th().stripHot, Th().seam, Th().outer, Th().digit, Th().selEdge}
        };
        config_panel::Paint(drawing, state);
    }
    else if(page==Page::Library) archivePaint(dc);
    else {
        const layout::TracksGeom& g = TG();
        PaintInfo(dc, g);
        PaintExit(dc, g);
        PaintGrid(dc, g);
        PaintSelectionStripe(dc, g);
        PaintHints(dc, g);
    }
    channelRail(dc);
    if(hoverChannel>=0) {
        const wchar_t* hints[]={tr(L"F1   Original jukebox",L"F1   原版点唱机",L"F1   原版ジュークボックス"),
            tr(L"F2   Local archive",L"F2   本地曲库",L"F2   ローカル音楽庫"),
            tr(L"F3   System settings",L"F3   系统设置",L"F3   システム設定")};
        panelBox(dc,144,462,495,478,Th().outer);
        lineText(dc,154,462,486,478,hints[hoverChannel],Th().digit,DT_RIGHT);
    }
    unsigned age=g_app.tick-switchedAt;
    if(switchedAt && age<12) {
        int y=290+int(age)*14;
        panelBox(dc,150,y,490,y+1,Th().frame);
    }
    // entrance: near-black tint (packed 0x100408) fading out over the first
    // 32 frames via fullscreen quad alpha clamp(8*n32,0,255).  The original
    // also has a late white flash (tex 290, mode 14) not reproduced here.
    int t = (int)g_app.tick;
    if (t < 32)
        AlphaFill(dc, 0, 0, S(kLogicalW), S(kLogicalH), RGB(8, 4, 16), 255 - ClampI(8 * t, 0, 255));
}

void OnPaint(HWND hwnd) {
    PlayerApp& app = g_app;
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    EnsureBackBuffer(app, dc);
    PaintScene(app.backDC);
    RECT cr;
    GetClientRect(hwnd, &cr);
    // Fill ONLY the letterbox margins; wiping the whole client each frame
    // before the blit made the entire window strobe at 60 fps.
    if (g_offX > 0) {
        FillRectCol(dc, 0, 0, g_offX, cr.bottom, kColOuter);
        FillRectCol(dc, g_offX + g_bbW, 0, cr.right, cr.bottom, kColOuter);
    }
    if (g_offY > 0) {
        FillRectCol(dc, 0, 0, cr.right, g_offY, kColOuter);
        FillRectCol(dc, 0, g_offY + g_bbH, cr.right, cr.bottom, kColOuter);
    }
    BitBlt(dc, g_offX, g_offY, g_bbW, g_bbH, app.backDC, 0, 0, SRCCOPY);
    EndPaint(hwnd, &ps);
}

void OnTimer(HWND hwnd) {
    PlayerApp& app = g_app;
    if (app.audio.isMci()) app.audio.poll();
    if(tagReader.changed() && !query.empty())refreshIndex();
    if(app.current==kTrackCount && !app.loading && !draggingSeek && app.audio.finished()) {
        int count=(int)app.playlist.size(), next=app.localIndex+1;
        if(localOrder==RepeatAll && count) next%=count;
        if(localOrder==Shuffle && count) {
            static std::mt19937 rng{std::random_device{}()};
            next=count<2?0:(app.localIndex+std::uniform_int_distribution<int>(1,count-1)(rng))%count;
        }
        StopPlayback();
        if(next>=0 && next<count) OpenMusic(app.playlist[next]);
    }
    UpdateVisualization();
    const bool playing=app.audio.isPlaying();
    stageMemory.advance(app.spectrum,app.current==kTrackCount?kTrackCount+app.localIndex:app.current,
        app.audio.positionSeconds(),playing,playing&&app.audio.isPaused());
    ++app.tick;
    InvalidateRect(hwnd, NULL, FALSE);
}

constexpr UINT kLibraryCommand = 0x120;
constexpr UINT kIntegerScaleCommand = 20;
void selectPage(HWND hwnd, Page next) {
    if(page==next) return;
    if(draggingSeek){ReleaseCapture();draggingSeek=false;}
    if(next==Page::System) returnPage=page;
    page=next; switchedAt=g_app.tick;
    focusSearch(false);
    if(page==Page::Library) refreshIndex();
    InvalidateRect(hwnd,nullptr,FALSE);
}
void OpenLibrary(HWND hwnd) {selectPage(hwnd,page==Page::Library?Page::Tracks:Page::Library);}
void toggleIntegerScale(HWND hwnd) {
    g_app.settings->scaleMode = g_app.settings->scaleMode ? 0 : 1;
    InvalidateRect(hwnd,nullptr,FALSE);
}
void adjustSetting(int direction) {
    if(settingRow==0) SetVolume(g_app.settings->volume+direction*5);
    else if(settingRow==1) {
        g_themeIdx=(g_themeIdx+direction+kThemeCount)%kThemeCount;
        g_app.settings->theme=g_themeIdx;
        RefreshTitle();
    } else if(settingRow==2) lib::cycleLanguage(direction);
    else if(settingRow==3) g_app.settings->libFont=(g_app.settings->libFont+direction+2)%2;
    else {
        g_app.settings->layoutMode=(g_app.settings->layoutMode+direction+layout::Count)%layout::Count;
        fieldGrid=false;
        InvalidateRect(g_app.hwnd,nullptr,FALSE);
    }
}

void OnKeyDown(HWND hwnd, WPARAM key) {
    PlayerApp& app = g_app;
    if(key==VK_F8){app.settings->wingMode=(app.settings->wingMode+1)%stage::wingCount;InvalidateRect(hwnd,nullptr,FALSE);return;}
    if(key==VK_F7){app.settings->stageMode=(app.settings->stageMode+1)%stage::count;InvalidateRect(hwnd,nullptr,FALSE);return;}
    if(key==VK_F9) {
        app.settings->layoutMode=(app.settings->layoutMode+1)%layout::Count;
        fieldGrid=false;
        InvalidateRect(hwnd,nullptr,FALSE);
        return;
    }
    if(key==VK_TAB && (app.settings->layoutMode==layout::Field ||
                       app.settings->layoutMode==layout::Cross)) {
        fieldGrid=!fieldGrid;
        InvalidateRect(hwnd,nullptr,FALSE);
        return;
    }
    if(key==VK_F6) {toggleIntegerScale(hwnd);return;}
    if(key>=VK_F1 && key<=VK_F3) {selectPage(hwnd,Page(key-VK_F1));return;}
    if(page==Page::System) {
        switch(key) {
            case VK_UP: settingRow=(settingRow+config_panel::kSettingCount-1)%config_panel::kSettingCount;return;
            case VK_DOWN: settingRow=(settingRow+1)%config_panel::kSettingCount;return;
            case VK_LEFT: adjustSetting(-1);return;
            case VK_RETURN: case VK_RIGHT: adjustSetting(1);return;
            case VK_ESCAPE: selectPage(hwnd,returnPage);return;
            case VK_SPACE: return;
        }
    }
    if(page==Page::Library) {
        switch(key) {
            case VK_UP: moveCursor(-1); return;
            case VK_DOWN: moveCursor(1); return;
            case VK_PRIOR: moveCursor(-rows); return;
            case VK_NEXT: moveCursor(rows); return;
            case VK_RETURN: playCursor(); return;
            case VK_SPACE: if(app.current==kTrackCount)app.audio.togglePause();else playCursor();return;
            case VK_LEFT: case VK_RIGHT:
                if(app.current==kTrackCount) app.audio.seek(app.audio.positionSeconds()+(key==VK_LEFT?-5:5));return;
            case VK_OEM_2: focusSearch(true); return;
            case VK_ESCAPE: OpenLibrary(hwnd); return;
        }
    }
    switch (key) {
        case VK_LEFT:  MoveSelection(-1, 0); break;
        case VK_RIGHT: MoveSelection(1, 0); break;
        case VK_UP:    MoveSelection(0, -1); break;
        case VK_DOWN:  MoveSelection(0, 1); break;
        case VK_RETURN:
        case VK_SPACE:
            if (app.selected == kExitIndex) PostMessageA(hwnd, WM_CLOSE, 0, 0);
            else StartTrack(app.selected);
            break;
        case 'L':
            if (GetKeyState(VK_CONTROL) & 0x8000)
                OpenLibrary(hwnd);
            break;
        case 'O':
            if (GetKeyState(VK_CONTROL) & 0x8000) ChooseMusic(hwnd);
            break;
        case 'M': ChooseMusic(hwnd); break;
        case 'N': case VK_NEXT:  StepLocal(+1); break;
        case 'P': case VK_PRIOR: StepLocal(-1); break;
        case 'C': case 'T':
            g_themeIdx = app.settings->theme = (g_themeIdx + 1) % kThemeCount;
            RefreshTitle();
            InvalidateRect(hwnd, NULL, FALSE);
            break;
        case 'S': StopPlayback(); break;
        case VK_OEM_PLUS: case VK_ADD: SetVolume(app.settings->volume + 5); break;
        case VK_OEM_MINUS: case VK_SUBTRACT: SetVolume(app.settings->volume - 5); break;
        case VK_ESCAPE:
            if (app.audio.isPlaying()) StopPlayback();
            else PostMessageA(hwnd, WM_CLOSE, 0, 0);
            break;
        case 'Q':
            if (GetKeyState(VK_CONTROL) & 0x8000) PostMessageA(hwnd, WM_CLOSE, 0, 0);
            break;
        default: break;
    }
}

// Mouse routing for the compositions that do not use the original panel.
void HandleLayoutMouse(HWND hwnd, int lx, int ly, bool dbl) {
    PlayerApp& app = g_app;
    const int mode = app.settings->layoutMode;
    if (mode == layout::Stack) {
        if (lx >= 408 && lx < 624 && ly >= 40 && ly < 431) {
            const int col = (lx - 408) / 108, row = (ly - 40) / 17;
            const int idx = col * 23 + row;
            if (idx >= 0 && idx < kTrackCount) {
                app.selected = idx;
                if (dbl) StartTrack(idx);
                else InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        return;
    }
    if (mode == layout::Ring) {
        const float dx = float(lx - 320), dy = float(ly - 236);
        const float r2 = dx * dx + dy * dy;
        if (r2 >= 170.f * 170.f && r2 <= 230.f * 230.f) {
            const float kPi = 3.14159265f;
            float a = atan2f(dy, dx) + kPi / 2.f;
            if (a < 0) a += 2.f * kPi;
            int idx = int(a / (2.f * kPi) * kTrackCount + 0.5f) % kTrackCount;
            app.selected = idx;
            if (dbl) StartTrack(idx);
            else InvalidateRect(hwnd, nullptr, FALSE);
        }
        return;
    }
    // Field / Cross: the crosshair EXIT label is always live; the TAB grid
    // keeps the original coordinates so its hit test is the original one.
    if (mode == layout::Cross && lx >= 24 && lx < 72 && ly >= 296 && ly < 314) {
        if (dbl) PostMessageA(hwnd, WM_CLOSE, 0, 0);
        else { app.selected = kExitIndex; InvalidateRect(hwnd, NULL, FALSE); }
        return;
    }
    if (!fieldGrid) return;
    RECT ex;
    ex.left = S(156); ex.top = S(272);
    ex.right = S(212); ex.bottom = S(288);
    if (PointInRect(ex, S(lx), S(ly))) {
        if (dbl) PostMessageA(hwnd, WM_CLOSE, 0, 0);
        else { app.selected = kExitIndex; InvalidateRect(hwnd, NULL, FALSE); }
        return;
    }
    for (int i = 0; i < kTrackCount; ++i) {
        RECT r = TrackRect(i);
        r.left = S(r.left); r.top = S(r.top); r.right = S(r.right); r.bottom = S(r.bottom);
        if (PointInRect(r, S(lx), S(ly))) {
            app.selected = i;
            if (dbl) StartTrack(i);
            else InvalidateRect(hwnd, NULL, FALSE);
            return;
        }
    }
}

void OnMouseDown(HWND hwnd, int x, int y, bool dbl) {
    PlayerApp& app = g_app;
    x -= g_offX; y -= g_offY;   // client -> backbuffer (letterbox) coords
    int lx=(int)floor(x/g_scale),ly=(int)floor(y/g_scale);
    if (page == Page::Tracks && app.settings->layoutMode != layout::Original) {
        HandleLayoutMouse(hwnd, lx, ly, dbl);
        return;
    }
    if(ly>=272 && ly<288 && lx>=railX && lx<railX+3*channelWidth) {
        selectPage(hwnd,Page((lx-railX)/channelWidth));
        return;
    }
    if(page==Page::System) {
        const config_panel::HitResult hit =
            config_panel::HitTest(lx, ly);
        if (hit) {
            settingRow = hit.row;
            if (hit.action == config_panel::HitAction::Decrease) {
                adjustSetting(-1);
            } else if (hit.action == config_panel::HitAction::Increase) {
                adjustSetting(1);
            }
        }
        return;
    }
    if(page==Page::Library) {
        if(ly>=293 && ly<309 && lx>=154 && lx<405) { focusSearch(true); return; }
        focusSearch(false);
        if(ly>=263 && ly<272 && lx>=154 && lx<486 && app.current==kTrackCount) {
            if(app.audio.durationSeconds()>0) {
                draggingSeek=true; SetCapture(hwnd); previewSeek(hwnd,x/g_scale);
            }
            return;
        }
        if(ly>=293 && ly<309 && lx>=406 && lx<486) ChooseMusic(hwnd,false);
        if(ly>=313 && ly<439 && lx>=154 && lx<486) {
            int i=cursor/rows*rows+(ly-313)/18;
            if(i<(int)matches.size()) {cursor=i;if(dbl) playCursor();}
        }
        if(ly>=443 && ly<460) {
            if(lx>=154 && lx<212) changeOrder();
            if(lx>=213 && lx<277) {if(app.current==kTrackCount)app.audio.togglePause();else playCursor();}
            if(lx>=278 && lx<331) StopPlayback();
            if(lx>=341 && lx<375) moveCursor(-rows);
            if(lx>=453 && lx<486) moveCursor(rows);
        }
        return;
    }
    RECT ex;
    ex.left = S(156); ex.top = S(272);
    ex.right = S(212); ex.bottom = S(288);
    if (PointInRect(ex, x, y)) {
        if (dbl) PostMessageA(hwnd, WM_CLOSE, 0, 0);
        else { app.selected = kExitIndex; InvalidateRect(hwnd, NULL, FALSE); }
        return;
    }
    for (int i = 0; i < kTrackCount; ++i) {
        RECT r = TrackRect(i);
        r.left = S(r.left); r.top = S(r.top); r.right = S(r.right); r.bottom = S(r.bottom);
        if (PointInRect(r, x, y)) {
            app.selected = i;
            if (dbl) StartTrack(i);
            else InvalidateRect(hwnd, NULL, FALSE);
            return;
        }
    }
}

void catalogAction(HWND hwnd,int action) {
    auto& a=g_app;auto& list=a.playlist;
    if(action==1) {for(auto& path:catalog::folder(hwnd))AddLocal(path);refreshIndex();return;}
    if(matches.empty())return;
    int idx=matches[cursor],prev=cursor;
    std::wstring picked=list[idx],playing=a.localIndex>=0 && a.localIndex<(int)list.size()?list[a.localIndex]:L"";
    switch(action) {
        case 2: // Remove only the entry, never the underlying audio file.
            if(a.current==kTrackCount && idx==a.localIndex)StopPlayback();
            list.erase(list.begin()+idx);break;
        case 3: case 4: {
            int dest=idx+(action==3?-1:1);
            if(dest<0 || dest>=(int)list.size())return;
            std::swap(list[idx],list[dest]);break;
        }
        case 5:
            std::stable_sort(list.begin(),list.end(),[](const auto& x,const auto& y){
                return StrCmpLogicalW(basenameW(x).c_str(),basenameW(y).c_str())<0;
            });break;
    }
    auto active=std::find(list.begin(),list.end(),playing);
    a.localIndex=active==list.end()?-1:int(active-list.begin());
    refreshIndex();
    cursor=matches.empty()?0:std::min(prev,(int)matches.size()-1);
    for(int i=0;i<(int)matches.size();++i)
        if(list[matches[i]]==picked){cursor=i;break;}
}
void popup(HWND hwnd,LPARAM coords) {
    POINT pt={(short)LOWORD(coords),(short)HIWORD(coords)};
    if(pt.x==-1 && pt.y==-1) {pt={g_offX+S(360),g_offY+S(280)};ClientToScreen(hwnd,&pt);}
    else if(page==Page::Library) {
        POINT client=pt;ScreenToClient(hwnd,&client);
        int x=(int)floor((client.x-g_offX)/g_scale),y=(int)floor((client.y-g_offY)/g_scale);
        int item=(cursor/rows)*rows+(y-313)/18;
        if(x>=154 && x<486 && y>=313 && y<439 && item<(int)matches.size())cursor=item;
    }
    HMENU m=CreatePopupMenu();if(!m)return;
    const wchar_t* name[]={tr(L"TR   Memoirs\tF1",L"TR   原版点唱机\tF1",L"TR   ジュークボックス\tF1"),
        tr(L"LIB   Library\tF2",L"LIB   本地曲库\tF2",L"LIB   音楽庫\tF2"),
        tr(L"SYS   Settings\tF3",L"SYS   设置\tF3",L"SYS   設定\tF3")};
    for(int i=0;i<3;++i)AppendMenuW(m,MF_STRING|(i==int(page)?MF_CHECKED:0),10+i,name[i]);
    AppendMenuW(m,MF_SEPARATOR,0,nullptr);
    AppendMenuW(m,MF_STRING|(g_app.settings->scaleMode?MF_CHECKED:0),
                 kIntegerScaleCommand,
                 tr(L"Integer scaling\tF6",L"整数倍缩放\tF6",L"整数倍拡大\tF6"));
    HMENU stages=CreatePopupMenu();
    if(stages){
        for(int i=0;i<stage::count;i++)AppendMenuW(stages,MF_STRING|(g_app.settings->stageMode==i?MF_CHECKED:0),30+i,stage::name(i));
        AppendMenuW(m,MF_POPUP,(UINT_PTR)stages,tr(L"Playback visuals   F7",L"播放演出   F7",L"再生演出   F7"));
    }
    HMENU wings=CreatePopupMenu();
    if(wings){
        for(int i=0;i<stage::wingCount;i++)AppendMenuW(wings,MF_STRING|(g_app.settings->wingMode==i?MF_CHECKED:0),40+i,stage::wingName(i));
        AppendMenuW(m,MF_POPUP,(UINT_PTR)wings,tr(L"Side displays   F8",L"侧翼效果   F8",L"サイド表示   F8"));
    }
    HMENU comps=CreatePopupMenu();
    if(comps){
        for(int i=0;i<layout::Count;i++)
            AppendMenuW(comps,MF_STRING|(g_app.settings->layoutMode==i?MF_CHECKED:0),
                        50+i,layout::name(i,lib::language()));
        AppendMenuW(m,MF_POPUP,(UINT_PTR)comps,tr(L"Composition   F9",L"画面布局   F9",L"画面構成   F9"));
    }
    if(page==Page::Library) {
        AppendMenuW(m,MF_SEPARATOR,0,nullptr);
        AppendMenuW(m,MF_STRING,1,tr(L"Import folder...",L"导入文件夹…",L"フォルダーを追加…"));
        UINT flags=MF_STRING|(matches.empty()?MF_GRAYED:0);
        AppendMenuW(m,flags,2,tr(L"Remove from library",L"从曲库移除",L"リストから除外"));
        AppendMenuW(m,flags,3,tr(L"Move earlier",L"向前移动",L"前に移動"));
        AppendMenuW(m,flags,4,tr(L"Move later",L"向后移动",L"後ろに移動"));
        AppendMenuW(m,flags,5,tr(L"Sort by filename",L"按文件名排序",L"ファイル名順"));
    }
    int cmd=TrackPopupMenu(m,TPM_RETURNCMD|TPM_RIGHTBUTTON,pt.x,pt.y,0,hwnd,nullptr);
    DestroyMenu(m);
    if(cmd>=10 && cmd<=12)selectPage(hwnd,Page(cmd-10));
    else if(cmd==int(kIntegerScaleCommand))toggleIntegerScale(hwnd);
    else if(cmd>=40 && cmd<40+stage::wingCount){g_app.settings->wingMode=cmd-40;InvalidateRect(hwnd,nullptr,FALSE);}
    else if(cmd>=30 && cmd<30+stage::count){g_app.settings->stageMode=cmd-30;InvalidateRect(hwnd,nullptr,FALSE);}
    else if(cmd>=50 && cmd<50+layout::Count){g_app.settings->layoutMode=cmd-50;fieldGrid=false;InvalidateRect(hwnd,nullptr,FALSE);}
    else if(cmd>0)catalogAction(hwnd,cmd);
}
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    PlayerApp& app = g_app;
    if (msg == WM_MOUSEMOVE || msg == WM_MOUSEWHEEL || msg == WM_KEYDOWN ||
        msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK ||
        msg == WM_CONTEXTMENU || msg == WM_SIZE || msg == WM_DROPFILES) {
        noteRailActivity();
    }
    switch (msg) {
        case WM_SYSCOMMAND:
            if ((wParam & 0xfff0) == kLibraryCommand) { OpenLibrary(hwnd); return 0; }
            break;
        case WM_DROPFILES: {
            HDROP drop = reinterpret_cast<HDROP>(wParam);
            UINT n = DragQueryFileW(drop, 0xFFFFFFFF, NULL, 0);
            int playIdx = -1;
            for (UINT i = 0; i < n; ++i) {
                UINT length = DragQueryFileW(drop, i, NULL, 0);
                std::vector<wchar_t> path(length + 1);
                DragQueryFileW(drop, i, path.data(), length + 1);
                if (length) {
                    int idx = AddLocal(path.data());
                    if (playIdx < 0) playIdx = idx;
                }
            }
            DragFinish(drop);
            refreshIndex();
            if (playIdx >= 0) OpenMusic(app.playlist[playIdx]);
            return 0;
        }
        case WM_CTLCOLOREDIT:
            if ((HWND)lParam == searchEdit) {
                auto dc = (HDC)wParam;
                SetTextColor(dc, Th().ink);
                SetBkColor(dc, Th().stripHot);
                SetDCBrushColor(dc, Th().stripHot);
                return (LRESULT)GetStockObject(DC_BRUSH);
            }
            break;
        case WM_COMMAND:
            if (LOWORD(wParam) == kIntegerScaleCommand) {
                toggleIntegerScale(hwnd);
                return 0;
            }
            if(searchEdit && (HWND)lParam==searchEdit && HIWORD(wParam)==EN_CHANGE) {
                std::wstring buf(GetWindowTextLengthW(searchEdit)+1,L'\0');
                buf.resize(GetWindowTextW(searchEdit,buf.data(),(int)buf.size()));query=buf;refreshIndex();
            }
            return 0;
        case WM_MOUSEMOVE: {
            int x=(int)floor(((short)LOWORD(lParam)-g_offX)/g_scale);
            int y=(int)floor(((short)HIWORD(lParam)-g_offY)/g_scale);
            if(draggingSeek) previewSeek(hwnd,((short)LOWORD(lParam)-g_offX)/g_scale);
            int channel=(y>=272 && y<288 && x>=railX && x<railX+3*channelWidth)?(x-railX)/channelWidth:-1;
            if(channel>=0) {
                SetCursor(LoadCursorW(nullptr,MAKEINTRESOURCEW(32649)));
                if(hoverChannel<0) {
                    TRACKMOUSEEVENT track={sizeof(track),TME_LEAVE,hwnd,0};TrackMouseEvent(&track);
                }
            }
            hoverChannel=channel;
            return 0;
        }
        case WM_CONTEXTMENU: popup(hwnd,lParam);return 0;
        case WM_MOUSELEAVE: hoverChannel=-1; return 0;
        case WM_MOUSEWHEEL:
            if(page==Page::Library) moveCursor(GET_WHEEL_DELTA_WPARAM(wParam)>0?-1:1);
            return 0;
        case WM_SIZE: InvalidateRect(hwnd, NULL, FALSE); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: OnPaint(hwnd); return 0;
        case WM_TIMER: OnTimer(hwnd); return 0;
        case WM_KEYDOWN: OnKeyDown(hwnd, wParam); return 0;
        case WM_CAPTURECHANGED: draggingSeek=false;return 0;
        case WM_LBUTTONUP:
            if(draggingSeek) {
                dragFraction=seekFraction(((short)LOWORD(lParam)-g_offX)/g_scale);
                draggingSeek=false;ReleaseCapture();
                if(app.current==kTrackCount)app.audio.seek(app.audio.durationSeconds()*dragFraction);
            }
            return 0;
        case WM_LBUTTONDOWN: OnMouseDown(hwnd, LOWORD(lParam), HIWORD(lParam), false); return 0;
        case WM_LBUTTONDBLCLK: OnMouseDown(hwnd, LOWORD(lParam), HIWORD(lParam), true); return 0;
        case WM_DESTROY:
            {
                int selected=matches.empty()?loadedSelection:matches[cursor];
                if(selected<0 || selected>=(int)app.playlist.size())selected=-1;
                if(app.playlist!=loadedPaths || localOrder!=loadedOrder || (!app.playlist.empty() && selected!=loadedSelection))
                    catalog::save(app.playlist,selected,localOrder);
            }
            if(panelFont) DeleteObject(panelFont);
            KillTimer(hwnd, 1);
            app.audio.shutdown();
            if (app.backBmp) DeleteObject(app.backBmp);
            if (app.backDC) DeleteDC(app.backDC);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

} // namespace

int RunPlayerWindow(Resolver& resolver, PlayerSettings& settings, std::string& err) {
    PlayerApp& app = g_app;
    app.resolver = &resolver;
    app.settings = &settings;
    g_themeIdx=std::clamp(settings.theme,0,kThemeCount-1);
    app.selected = 0;
    app.current = -1;
    for (int i = 0; i < kTrackCount; ++i) {
        if (!resolver.gameDir().empty()) app.tracks[i].src = resolver.resolve(kTrackNames[i]);
        app.tracks[i].ok = app.tracks[i].src.ok();
    }

    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleA(0);
    wc.hCursor = LoadCursorA(0, IDC_ARROW);
    wc.lpszClassName = "hsbgm_sound_memoirs";
    wc.hbrBackground = NULL;
    RegisterClassA(&wc);

    // Resizable / maximizable: the scene scale follows the client area so the
    // vector UI grows proportionally with resolution and stays crisp.
    const DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
    RECT rc = { 0, 0, 1280, 960 };
    if (!settings.shotPath.empty() && settings.shotScale == 1) {
        rc.right = 640;
        rc.bottom = 480;
    }
    AdjustWindowRect(&rc, style, FALSE);
    HWND hwnd = CreateWindowExA(WS_EX_APPWINDOW, wc.lpszClassName, "SOUND MEMOIRS",
                                style,
                                CW_USEDEFAULT, CW_USEDEFAULT,
                                rc.right - rc.left, rc.bottom - rc.top,
                                0, 0, wc.hInstance, 0);
    if (!hwnd) {
        err = "CreateWindow failed";
        return 1;
    }
    app.hwnd = hwnd;
    int restored=-1;
    catalog::load(app.playlist,restored,localOrder);
    localOrder=std::clamp(localOrder,0,3);
    loadedOrder=localOrder;loadedSelection=restored;loadedPaths=app.playlist;
    for(auto& path:app.playlist)tagReader.enqueue(path);
    refreshIndex();
    if(restored>=0 && restored<(int)matches.size())cursor=restored;
    searchEdit=CreateWindowExW(0,L"EDIT",L"",WS_CHILD|ES_AUTOHSCROLL,0,0,0,0,hwnd,nullptr,GetModuleHandleW(nullptr),nullptr);
    if (HMENU menu = GetSystemMenu(hwnd, FALSE)) {
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kLibraryCommand, L"Music Library / 曲库\tCtrl+L");
    }
    DragAcceptFiles(hwnd, TRUE);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    // --shot: render one settled frame (entrance fades and the panel frame
    // expansion are over by tick 200) and exit.  Used to diff layout changes.
    if (!settings.shotPath.empty()) {
        app.tick = 200;
        page = Page(std::clamp(settings.shotPage, 0, 2));
        // Synthetic data only for the new compositions; Original keeps the
        // silent-frame baseline byte-identical.
        if (settings.layoutMode != layout::Original) FillShotSpectrum(app);
        HDC shotDC = GetDC(hwnd);
        EnsureBackBuffer(app, shotDC);
        PaintScene(app.backDC);
        const bool saved = SaveBackBufferBmp(app, settings.shotPath);
        ReleaseDC(hwnd, shotDC);
        if (!saved) err = "could not write " + settings.shotPath;
        return saved ? 0 : 1;
    }
    // 60 fps like the original: blink parity (24 frames), selection pulse
    // (5 frames) and the entrance ripple (64 frames) all key off this tick.
    // timeBeginPeriod is required or Windows coalesces WM_TIMER to ~31 ms.
    timeBeginPeriod(1);
    SetTimer(hwnd, 1, 16, 0);

    // autoplay: main.cpp passes --play <track> through PlayerSettings
    if (settings.initialTrack >= 0 && settings.initialTrack < kTrackCount) {
        app.selected = settings.initialTrack;
        if (!settings.initialFile.empty()) OpenMusic(settings.initialFile);
        else if (settings.autoPlay) StartTrack(settings.initialTrack);
    }

    MSG msg;
    while (GetMessageW(&msg, 0, 0, 0) > 0) {
        if(msg.hwnd==searchEdit && msg.message==WM_KEYDOWN) {
            noteRailActivity();
            if(msg.wParam>=VK_F1 && msg.wParam<=VK_F3) {selectPage(hwnd,Page(msg.wParam-VK_F1));continue;}
            if(msg.wParam==VK_ESCAPE || msg.wParam==VK_RETURN || msg.wParam==VK_DOWN) {
                focusSearch(false); continue;
            }
            if(msg.wParam=='L' && GetKeyState(VK_CONTROL)<0) {OpenLibrary(hwnd);continue;}
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    timeEndPeriod(1);
    return 0;
}
