#include "decoder.h"
#include "pak_index.h"

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
    return g_entries[index].length;
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

} // extern "C"
