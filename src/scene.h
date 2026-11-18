// scene.h - the shared SOUND MEMOIRS scene painter.
//
// scene.cpp is the single painter for every platform: the desktop shell
// (ui.cpp) and the WASM core (wasm/core/api.cpp) both drive it through
// SceneState snapshots, so the rendered output cannot drift between builds.
#pragma once
#include "scene_state.h"
#include "stage.h"

// Logical screen size (the whole scene is authored in this space).
namespace scene {
constexpr int kLogicalWidth = 640;
constexpr int kLogicalHeight = 480;
}

// Platform hooks for shell-owned content that the browser replica does not
// render: the desktop draws the local-archive page, the settings panel and
// their CJK text through its own GDI text path. The shared painter calls
// them at the exact point the former desktop code drew them, keeping the
// paint order (and therefore the pixels) identical. WASM passes a null host
// and the painter simply skips those overlays.
struct SceneHost {
    virtual ~SceneHost() = default;
    // Library page body (desktop archivePaint).
    virtual void drawArchiveBody() {}
    // System page body (desktop config_panel::Paint).
    virtual void drawSystemPanel() {}
    // True when the host painted the "NOW PLAYING" line itself (desktop
    // renders non-ASCII local file names through the Fusion Pixel font).
    virtual bool drawLocalFileTitle() { return false; }
    // Channel rail glyph for the System page (desktop draws it with the
    // CJK font; other pages use the shared cell atlas).
    virtual void drawChannelRailText(int channel, bool active) {}
    // Hover hint strip under the channel rail.
    virtual void drawHoverHint(int channel) {}
};

// Advance the shared stage memory from the playback position and mirror the
// wing envelopes into the scene state (the WASM advance entry point; the
// desktop shell advances its own stage::Memory directly).
void sceneAdvance(SceneState& s, stage::Memory& memory, int id, double seconds,
                  bool playing, bool paused);

// Record one full frame into the active gfx backend. `scale` is the
// logical-to-device pixel multiplier (desktop backbuffer scale; WASM replay
// passes 1.0). `host` may be null.
void sceneRender(const SceneState& s, float scale, const stage::Memory& memory,
                 SceneHost* host = nullptr);

// Replace the LIB archive list with `count` fixed-width (40-byte) ASCII
// records read from `pool`. Passing count <= 0 clears the list, which makes
// the painter show its "no local tracks" hint instead of placeholder rows.
void sceneSetLocalNames(const char* pool, int count);
