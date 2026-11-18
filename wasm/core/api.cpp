#include "decoder.h"
#include "pak_index.h"
#include "scene_state.h"
#include "scene.h"
#include "gfx.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#define MEMOIRS_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define MEMOIRS_EXPORT
#endif

namespace {

std::vector<memoirs::PakEntry> g_entries;
PcmBuffer g_pcm;
SceneState g_scene;
// Full stage memory, advanced per frame by the shared Memory::advance (the
// revision-1 branch kept only the wing envelopes; the shared painter also
// consumes the ridge/trace history for the stage compositions).
stage::Memory g_memory;

} // namespace

extern "C" {

MEMOIRS_EXPORT void* alloc(std::uint32_t size) {
    return size == 0 ? nullptr : std::malloc(size);
}

MEMOIRS_EXPORT void dealloc(void* ptr) {
    std::free(ptr);
}

MEMOIRS_EXPORT std::uint32_t parse_pak(const unsigned char* header,
                                       std::uint32_t header_length,
                                       std::uint32_t file_size) {
    if (!header) return 0;

    std::vector<memoirs::PakEntry> parsed;
    std::string error;
    if (!memoirs::parseIndex(header, header_length, file_size, parsed, error)) {
        return 0;
    }
    if (parsed.size() > UINT32_MAX) return 0;

    g_entries.swap(parsed);
    return static_cast<std::uint32_t>(g_entries.size());
}

MEMOIRS_EXPORT const char* entry_name(std::uint32_t index) {
    if (index >= g_entries.size()) return nullptr;
    return g_entries[index].name.c_str();
}

MEMOIRS_EXPORT std::uint32_t entry_offset(std::uint32_t index) {
    if (index >= g_entries.size()) return 0;
    return static_cast<std::uint32_t>(memoirs::PAK_DATA_OFF) +
           g_entries[index].offset;
}

MEMOIRS_EXPORT std::uint32_t entry_length(std::uint32_t index) {
    if (index >= g_entries.size()) return 0;
    return static_cast<std::uint32_t>(g_entries[index].length);
}

MEMOIRS_EXPORT std::int32_t decode(const unsigned char* data,
                                   std::uint32_t length) {
    if (!data || length == 0) return 0;

    std::string error;
    bool decoded = false;
    if (length >= 4 && std::memcmp(data, "OggS", 4) == 0) {
        decoded = DecodeOgg(data, length, g_pcm, error, 0);
    } else if (length >= 12 && std::memcmp(data, "RIFF", 4) == 0 &&
               std::memcmp(data + 8, "WAVE", 4) == 0) {
        decoded = DecodeWav(data, length, g_pcm, error, 0);
    }

    if (!decoded) {
        g_pcm.clear();
        return 0;
    }
    return g_pcm.frames();
}

MEMOIRS_EXPORT const std::int16_t* samples() {
    if (g_pcm.samples.empty()) return nullptr;
    return reinterpret_cast<const std::int16_t*>(g_pcm.samples.data());
}

MEMOIRS_EXPORT void clear_pcm() {
    std::vector<short>().swap(g_pcm.samples);
    g_pcm.clear();
}

// ---- shared SOUND MEMOIRS scene ------------------------------------------
// These exports drive the same src/scene.cpp painter the desktop shell
// (ui.cpp) uses; only the state source differs (JS snapshot vs PlayerApp).
MEMOIRS_EXPORT std::uint32_t scene_state_size() {
    return (std::uint32_t)sizeof(SceneState);
}
MEMOIRS_EXPORT void scene_begin() {
    gfx_reset();
}
MEMOIRS_EXPORT void scene_set_state(const void* ptr) {
    if (!ptr) return;
    SceneState incoming = *static_cast<const SceneState*>(ptr);
    // Wing envelopes are accumulated inside WASM across frames; packers
    // that don't simulate them write zeros, so preserve the running values.
    incoming.wing[0] = g_memory.wing[0];
    incoming.wing[1] = g_memory.wing[1];
    g_scene = incoming;
}
MEMOIRS_EXPORT void scene_advance(int id, double seconds, int playing, int paused) {
    sceneAdvance(g_scene, g_memory, id, seconds, playing != 0, paused != 0);
}
MEMOIRS_EXPORT void scene_render() {
    // The browser replays the logical 640x480 screen directly (scale 1),
    // matching the desktop --shot-scale 1 pixel baseline.
    sceneRender(g_scene, 1.0f, g_memory, nullptr);
}
MEMOIRS_EXPORT const GfxCmd* scene_commands() {
    return gfx_commands();
}
MEMOIRS_EXPORT const GfxPoint* scene_points() {
    return gfx_points();
}
MEMOIRS_EXPORT std::uint32_t scene_count() {
    return (std::uint32_t)gfx_count();
}
MEMOIRS_EXPORT const GfxTextCmd* scene_texts() {
    return gfx_texts();
}
MEMOIRS_EXPORT const std::uint32_t* scene_text_points() {
    return gfx_text_points();
}
MEMOIRS_EXPORT std::uint32_t scene_text_count() {
    return (std::uint32_t)gfx_text_count();
}

} // extern "C"
