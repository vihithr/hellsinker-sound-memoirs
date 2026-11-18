// Derived from src/layout.cpp (kOriginalTracks / kOriginalSpec) and the
// file-scope constants in src/ui.cpp. Desktop sources remain untouched.
#pragma once

struct TracksGeom {
    int gridX, gridY, colW, rowH, cols, rows;
    int exitX, exitY;
    int infoX, infoY1, infoY2;
    int hintX, hintY;
    int railX, channelW;
    int seekL, seekR, seekY;
};

struct SpecGeom {
    int l, t, r, axisY, barBase, barW, pitch, bars;
};

inline constexpr TracksGeom kTracks = {
    168, 296, 56, 16, 5, 10,
    168, 280,
    220, 256, 264,
    144, 472,
    366, 40,
    154, 486, 266
};

inline constexpr SpecGeom kSpec = {
    148, 102, 492, 236, 232, 7, 8, 43
};

inline constexpr int kLogicalW = 640;
inline constexpr int kLogicalH = 480;
inline constexpr int kPanelL = 142, kPanelR = 497, kPanelT = 4, kPanelB = 476;
inline constexpr int kExitIndex = -1;
inline constexpr int kTrackCount = 46;
inline constexpr int kSegH = 2;
inline constexpr int kBarBaseY = 232;
inline constexpr int kTitleX = 160, kTitleY = 32;
inline constexpr int kSeamX[8] = { 171, 195, 243, 291, 339, 387, 435, 475 };
inline constexpr const char* kFreqLabel[8] = {
    "52", "110", "540", "1060", "2080", "4160", "12K", "22K"
};
