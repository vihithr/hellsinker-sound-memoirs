#include "core/spectrum.h"
#include "stage.h"
#include "scene.h"
#include "gfx.h"
// ui.cpp - the Win32 shell of HellSinker "SOUND MEMOIRS": window, input,
// audio playlist, backbuffer and the shell-owned overlays. The scene
// painter itself lives in scene.cpp and is shared with the WASM build.
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
// The scene painter lives in scene.cpp (shared with the WASM build); this
// file is the Win32 shell: window, input, audio playlist, backbuffer and
// the shell-owned overlays (archive / settings CJK pages).
const int kLogicalW = scene::kLogicalWidth;
const int kLogicalH = scene::kLogicalHeight;

// ---- themes -------------------------------------------------------------
// Screen RGB sampled from game frames for the "Original" preset; the packed
// draw colors are texture-modulated on top of it.
// Additional presets recolor the whole vector scene; text is drawn with the
// private Hellsinker TTF so every element stays crisp at any scale.
#include "themes.generated.h"
int g_themeIdx = 0;
static inline const Theme& Th() { return kThemes[g_themeIdx]; }

#define kColOuter     (Th().outer)

const char* kTitle = "SOUND MEMOIRS";

#include "themed_art_runs.h"

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
    if (page == Page::Tracks) return std::clamp((logicalX - 16) / 608., 0., 1.);
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

// Shell rect fill (letterbox margins, archive page, seek preview). The
// shared scene painter uses the gfx backend instead; this direct GDI helper
// stays for the few shell-owned draws outside the shared painter.
void FillRectCol(HDC dc, int l, int t, int r, int b, COLORREF c) {
    RECT rc;
    rc.left = l; rc.top = t; rc.right = r; rc.bottom = b;
    HBRUSH br = CreateSolidBrush(c);
    FillRect(dc, &rc, br);
    DeleteObject(br);
}

// ---- themed bitmap art --------------------------------------------------
// The themed cell atlas (resources cells_<theme>, base id 110) is owned by
// the GDI gfx backend; the shell's text path (DrawPanelText) borrows it.
HDC PanelAtlas() { return gfx_win32_atlas(g_themeIdx); }

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
    // The selection grid is always the original 5x10 layout, whatever
    // composition is currently displayed.
    const layout::TracksGeom& tg = layout::tracks(layout::Original);
    const int gridRows = tg.rows, gridCols = tg.cols;
    int col = app.selected / gridRows;
    int row = app.selected % gridRows;
    col += dcol;
    row += drow;
    if (row < 0) {
        if (col > 0) { col--; row = gridRows - 1; }
        else { app.selected = kExitIndex; InvalidateRect(app.hwnd, NULL, FALSE); return; }
    }
    if (row >= gridRows) {
        if (col < gridCols - 1) { col++; row = 0; }
        else row = gridRows - 1;
    }
    if (col < 0) col = 0;
    if (col > gridCols - 1) col = gridCols - 1;
    int idx = col * gridRows + row;
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
// All scene painting now lives in the shared scene.cpp (see SceneHost below
// for the shell-owned overlays that stay in this file).

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
    if (page == Page::Tracks) { InvalidateRect(hwnd, nullptr, FALSE); return; }
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
// ---- shared scene bridge -----------------------------------------------
// Pack the shell globals into the platform-independent snapshot and drive
// the shared painter through the GDI backend. The host hooks keep the
// shell-owned overlays (archive / settings CJK pages, Fusion Pixel titles)
// at the exact paint positions the former inline code used.
void PackSceneState(SceneState& st) {
    PlayerApp& app = g_app;
    st.spectrum = app.spectrum;
    st.wing[0] = stageMemory.wing[0];
    st.wing[1] = stageMemory.wing[1];
    st.tick = (int)app.tick;
    st.selected = app.selected;
    st.current = app.current;
    for (int i = 0; i < kTrackCount; ++i) st.available[i] = app.tracks[i].ok ? 1 : 0;
    st.theme = g_themeIdx;
    st.stageMode = app.settings->stageMode;
    st.wingMode = app.settings->wingMode;
    st.layoutMode = app.settings->layoutMode;
    st.page = int(page);
    st.hoverChannel = hoverChannel;
    st.seconds = app.audio.positionSeconds();
    st.localFile = (app.current == kTrackCount) ? 1 : 0;
    st.loading = app.loading ? 1 : 0;
    st.railActivityTick = (int)lastRailActivityTick;
    st.switchTick = (int)switchedAt;
    st.showGridOverlay = fieldGrid ? 1 : 0;
    st.duration = app.current >= 0 ? app.audio.durationSeconds() : 0.0;
    if (draggingSeek && page == Page::Tracks) st.seconds = st.duration * dragFraction;
    // Uppercased ASCII fallback of the local title for the shared atlas
    // path; the host hook below still paints the true name (Fusion Pixel
    // font) whenever it contains non-ASCII characters.
    memset(st.localTitle, 0, sizeof(st.localTitle));
    if (st.localFile && app.localIndex >= 0 && app.localIndex < (int)app.playlist.size()) {
        std::wstring name = basenameW(app.playlist[app.localIndex]);
        std::string narrow;
        for (wchar_t c : name) {
            if (c >= L'a' && c <= L'z') c -= 32;
            narrow += (c >= 32 && c < 127) ? (char)c : '?';
        }
        if (narrow.size() > 24) narrow = narrow.substr(0, 24);
        memcpy(st.localTitle, narrow.c_str(), std::min(narrow.size(), sizeof(st.localTitle) - 1));
    }
}

struct DesktopHost : SceneHost {
    void drawArchiveBody() override { archivePaint(g_app.backDC); }
    void drawSystemPanel() override {
        const config_panel::State state = {
            settingRow, g_app.settings->volume, Th().name,
            lib::language(), g_app.settings->libFont,
            layout::name(g_app.settings->layoutMode, lib::language())
        };
        const config_panel::DrawContext drawing = {
            g_app.backDC, g_scale, PanelAtlas(),
            {Th().panel, Th().frame, Th().bar, Th().ink, Th().strip,
             Th().stripHot, Th().seam, Th().outer, Th().digit, Th().selEdge}
        };
        config_panel::Paint(drawing, state);
    }
    bool drawLocalFileTitle() override {
        PlayerApp& app = g_app;
        if (!(app.current == kTrackCount && app.localIndex >= 0 &&
              app.localIndex < (int)app.playlist.size()))
            return false;
        const std::wstring name = basenameW(app.playlist[app.localIndex]);
        // Pure-ASCII names go through the shared cell atlas; anything else
        // keeps the desktop Fusion Pixel rendering of the original code.
        if (name.find_first_not_of(
                L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 ._-()")
            == std::wstring::npos)
            return false;
        lineText(g_app.backDC, 216, 249, 486, 260, name, Th().digit);
        return true;
    }
    void drawChannelRailText(int channel, bool active) override {
        static const wchar_t* configChannels[] = { L"TR", L"LIB", L"SYS" };
        const int x = railX + channel * channelWidth;
        lineText(g_app.backDC, x + 5, 273, x + 38, 287, configChannels[channel],
                 active ? Th().digit : Th().ink);
    }
    void drawHoverHint(int channel) override {
        const wchar_t* hints[] = {
            tr(L"F1   Original jukebox", L"F1   原版点唱机", L"F1   原版ジュークボックス"),
            tr(L"F2   Local archive", L"F2   本地曲库", L"F2   ローカル音楽庫"),
            tr(L"F3   System settings", L"F3   系统设置", L"F3   システム設定")
        };
        lineText(g_app.backDC, 154, 462, 486, 478, hints[channel], Th().digit, DT_RIGHT);
    }
};

void PaintScene(HDC dc) {
    gfx_bind(dc);
    SceneState st;
    memset(&st, 0, sizeof(st));
    PackSceneState(st);
    DesktopHost host;
    sceneRender(st, g_scale, stageMemory, &host);
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
    if(key==VK_TAB && page==Page::Tracks && (app.settings->layoutMode==layout::Field ||
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
        // compact index: x 460..620, two columns of 23, 80x16 cells (y 58+)
        if (lx >= 460 && lx < 620 && ly >= 58 && ly < 426) {
            const int col = (lx - 460) / 80, row = (ly - 58) / 16;
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
    // Field / Cross: the EXIT label is always live; the TAB grid keeps the
    // original coordinates so its hit test is the original one.
    if (!fieldGrid && mode == layout::Cross && lx >= 432 && lx < 480 && ly >= 344 && ly < 362) {
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
        if (!fieldGrid && !app.loading && app.current >= 0 && lx >= 16 && lx <= 624 &&
            ly >= 448 && ly <= 462 && app.audio.durationSeconds() > 0) {
            draggingSeek = true;
            SetCapture(hwnd);
            previewSeek(hwnd, lx);
            return;
        }
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
                if(app.current>=0 && !app.loading)app.audio.seek(app.audio.durationSeconds()*dragFraction);
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
