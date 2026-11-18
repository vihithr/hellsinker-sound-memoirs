// ui.h - the SOUND MEMOIRS window.
#pragma once

#include "resolver.h"
#include "audio.h"

struct PlayerSettings {
    std::wstring initialFile;
    int initialTrack;   // -1 = none selected beyond track 0
    bool autoPlay;
    int volume;         // 0..100
    int theme;          // palette index, ORIGINAL = 0
    int stageMode;
    int wingMode;
    int layoutMode;     // composition: 0 = original panel layout
    int libFont;        // 0 = original atlas-first, 1 = Fusion Pixel only
    int scaleMode;      // 0 = fit window, 1 = integer scale when possible
    std::string shotPath;  // --shot: render one frame here and exit (BMP)
    int shotPage = 0;      // --page: 0 tracks, 1 library, 2 system
    int shotScale = 2;     // --shot-scale: 1 or 2
    PlayerSettings()
        : initialTrack(-1), autoPlay(false), volume(80), theme(0), stageMode(0), wingMode(0),
          layoutMode(0), libFont(1), scaleMode(1) {}
};

// Runs the window until the user closes it. Returns process exit code.
int RunPlayerWindow(Resolver& resolver, PlayerSettings& settings, std::string& err);
