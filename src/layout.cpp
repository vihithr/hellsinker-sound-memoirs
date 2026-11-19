// layout.cpp - geometry tables and display names for the composition modes.
//
// kOriginalTracks / kOriginalSpec must stay byte-for-byte equal to the
// constants that used to live in ui.cpp.  Painting through them is how the
// original layout keeps its exact pixel output after the parameterisation.
#include "layout.h"

namespace layout {

namespace {

const TracksGeom kOriginalTracks = {
    168, 296, 56, 16, 5, 10,     // TR grid: x = 168+56*col, y = 296+16*row
    168, 280,                    // EXIT cells
    220, 256, 264,               // NOW PLAYING / PLAY TIME lines
    144, 472,                    // hint strip
    366, 40,                     // page rail and channel width
    154, 486, 266                // seek rail
};

const SpecGeom kOriginalSpec = {
    148, 102, 492, 236, 232, 7, 8, 43
};

const wchar_t* const kNames[Count][3] = {
    // EN, ZH, JA
    { L"Original", L"原版布局", L"原版" },
    { L"Field", L"全幅场", L"全幅" },
    { L"Stack", L"三栏", L"三欄" },
    { L"Ring", L"环形", L"環状" },
    { L"Cross", L"四象限", L"四象限" },
    { L"Sink", L"沉井", L"沈降" },
    { L"Dive", L"坠入", L"潜航" },
    { L"Sono", L"声谱瀑布", L"ソノグラム" },
    { L"Track", L"八轨卷帘", L"トラッカー" },
    { L"Deck", L"磁带卡座", L"カセットデッキ" },
};

} // namespace

const TracksGeom& tracks(int mode) {
    (void)mode;
    return kOriginalTracks;
}

const SpecGeom& spec(int mode) {
    (void)mode;
    return kOriginalSpec;
}

const wchar_t* name(int mode, lib::Lang lang) {
    if (mode < 0 || mode >= Count) mode = Original;
    const int slot = lang == lib::Lang::ZH ? 1 : lang == lib::Lang::JA ? 2 : 0;
    return kNames[mode][slot];
}

} // namespace layout
