// ui.h - the SOUND MEMOIRS window.
#pragma once

#include "resolver.h"
#include "audio.h"

struct PlayerSettings {
    std::wstring initialFile;
    int initialTrack;   // -1 = none selected beyond track 0
    bool autoPlay;
    int volume;         // 0..100
    PlayerSettings() : initialTrack(-1), autoPlay(false), volume(80) {}
};

// Runs the window until the user closes it. Returns process exit code.
int RunPlayerWindow(Resolver& resolver, PlayerSettings& settings, std::string& err);
