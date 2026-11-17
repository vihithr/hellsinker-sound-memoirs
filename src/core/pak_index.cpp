#include "pak_index.h"

namespace memoirs {
namespace {

const unsigned int PAK_NUM_OFF  = 0x1F400;
const unsigned int PAK_NAME_OFF = 0x1;     // name table starts at file offset 0x1
const unsigned int PAK_NAME_LEN = 0x100;   // stride; 0xFF usable bytes
const unsigned int PAK_LEN_OFF  = 0x1F404;
const unsigned int PAK_OFS_OFF  = 0x1FBD4;

const unsigned int PAK_MAX_ENTRIES = 500;  // name/length/offset tables hold 500 slots

unsigned int ReadU32(const unsigned char* p) {
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
           ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

std::string NormalizeName(const std::string& raw) {
    std::string s = raw;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] >= 'A' && s[i] <= 'Z') s[i] += 'a' - 'A';
        if (s[i] == '/') s[i] = '\\';
    }
    return s;
}

} // namespace

bool parseIndex(const unsigned char* head, std::size_t headerSize, std::uint64_t fileSize, std::vector<PakEntry>& entries, std::string& err) {
    entries.clear();
    err.clear();
    if (!head || headerSize < PAK_DATA_OFF || fileSize < PAK_DATA_OFF) {
        err = "pak too small";
        return false;
    }
    unsigned int n = ReadU32(&head[PAK_NUM_OFF]);
    if (n == 0 || n > PAK_MAX_ENTRIES) {
        err = "implausible pak entry count";
        return false;
    }

    entries.reserve(n);
    for (unsigned int i = 0; i < n; ++i) {
        const unsigned char* np = &head[PAK_NAME_OFF + i * PAK_NAME_LEN];
        char raw[PAK_NAME_LEN + 1];
        unsigned int k = 0;
        for (; k < PAK_NAME_LEN - 1; ++k) {
            if (np[k] == 0) break;
            raw[k] = (char)np[k];
        }
        raw[k] = 0;

        PakEntry e;
        e.name = std::string(raw);
        e.nameLower = NormalizeName(e.name);
        e.offset = ReadU32(&head[PAK_OFS_OFF + i * 4]);
        e.length = ReadU32(&head[PAK_LEN_OFF + i * 4]);

        if (e.length > fileSize || e.offset > fileSize - PAK_DATA_OFF ||
            e.length > fileSize - PAK_DATA_OFF - e.offset) {
            entries.clear();
            err = "corrupt pak entry table";
            return false;
        }
        entries.push_back(e);
    }

    return true;
}

static std::string baseName(const std::string& path) { return path.substr(path.find_last_of('\\') + 1); }

const PakEntry* findEntry(const std::vector<PakEntry>& entries, const std::string& fileName) {
    std::string want = NormalizeName(fileName);
    // Exact (normalized) match first.
    for (size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].nameLower == want) return &entries[i];
    }
    // Bare-name match (strip directories from both sides).
    std::string wantBase = baseName(want);
    for (size_t i = 0; i < entries.size(); ++i) {
        if (baseName(entries[i].nameLower) == wantBase) return &entries[i];
    }
    return NULL;
}

} // namespace memoirs
