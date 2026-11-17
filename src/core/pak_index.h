#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace memoirs {
constexpr std::size_t PAK_DATA_OFF = 0x203A4;
struct PakEntry {
    std::string name;      // stored as-is (typically "backboneteller\\X.ogg")
    std::string nameLower; // normalized lower-case, '\\' separators
    unsigned int offset;   // relative to payload base
    unsigned int length;
};

bool parseIndex(const unsigned char* header, std::size_t headerSize, std::uint64_t fileSize, std::vector<PakEntry>& entries, std::string& err);
const PakEntry* findEntry(const std::vector<PakEntry>& entries, const std::string& name);
}
