// layout.h - composition ("layout") geometry for the desktop player.
//
// A composition decides where the fixed 640x480 logical screen puts its
// pieces.  Layout 0.  is the original SOUND MEMOIRS panel arrangement and is
// the reference every other mode must leave untouched: the geometry tables
// below repeat its constants verbatim, so code that used to read the old
// file-scope constants can read them from here without changing a pixel.
#pragma once

#include "library.h"

namespace layout {

enum Mode : int {
    Original = 0, // the original panel layout: wings + top panel + TR grid
    Field,        // full-frame oscilloscope field, no panel frame
    Stack,        // vertical three-column instrument rack
    Ring,         // circular instrument dial
    Cross,        // four-quadrant instrument split
    Helix,        // SINK descent shaft: angular per-channel lattice
    Dive,         // first-person fall through a spectrum light-gate tunnel
    Sono,         // SDR-style spectrogram waterfall
    Track,        // PC-98 / FastTracker II 8-channel hex pattern scroll
    Deck,         // cassette / DAT deck: linked reels + VFD 7-segment clock
    Count
};

// Three-language display name.  mode is clamped to Original.
const wchar_t* name(int mode, lib::Lang lang);

// Geometry of the pieces shared by the track page.  Only Original currently
// consumes these; the other modes draw their own arrangements, but they get
// the same table so a future mode can reuse PaintGrid/PaintInfo verbatim.
struct TracksGeom {
    int gridX, gridY, colW, rowH, cols, rows;
    int exitX, exitY;
    int infoX, infoY1, infoY2;
    int hintX, hintY;
    int railX, channelW;
    int seekL, seekR, seekY;
};

// Geometry of the spectrum window inside the panel.
struct SpecGeom {
    int l, t, r, axisY, barBase, barW, pitch, bars;
};

const TracksGeom& tracks(int mode);
const SpecGeom& spec(int mode);

} // namespace layout
