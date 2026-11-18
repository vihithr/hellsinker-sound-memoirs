// Derived painter for the WASM branch. Desktop sources are not modified.
#pragma once
#include "scene_state.h"

// Record the full frame into the gfx command buffer.
void sceneRender(const SceneState& s);
// Update wing envelopes; restricted to the data PaintWings consumes.
void sceneAdvanceWing(SceneState& s, int id, double seconds, bool playing, bool paused);
