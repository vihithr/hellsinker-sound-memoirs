// pak.h - reader for HellSinker's .pak containers (bbt.pak / bbt2.pak / bbt3.pak / bbll.pak).
//
// Layout (verified against the game's own loader and cross-checked with
// F:\PlayGround\hs_probe\hs_tools.py):
//   u8     unknown/flags       @ 0x0
//   char   names[500][0x100]   @ 0x1        (0xFF usable bytes, NUL terminated,
//                                            names look like "backboneteller\\X.ogg")
//   u32    count               @ 0x1F400
//   u32    lengths[500]        @ 0x1F404
//   u32    offsets[500]        @ 0x1FBD4    (relative to payload base)
//   payload                    @ 0x203A4 + offset
#pragma once

#include "common.h"

struct PakEntry {
    std::string name;      // stored as-is (typically "backboneteller\\X.ogg")
    std::string nameLower; // normalized lower-case, '\\' separators
    unsigned int offset;   // relative to payload base
    unsigned int length;
};

class PakFile {
public:
    PakFile();
    ~PakFile();

    bool open(const std::string& path, std::string& err);
    void close();

    bool isOpen() const { return file_ != NULL; }
    const std::string& path() const { return path_; }
    size_t count() const { return entries_.size(); }
    const std::vector<PakEntry>& entries() const { return entries_; }

    // Case-insensitive lookup. Accepts either a full stored-style name
    // ("backboneteller\\track.ogg") or a bare file name ("track.ogg").
    const PakEntry* find(const std::string& fileName) const;

    bool read(const PakEntry& e, std::vector<unsigned char>& out, std::string& err) const;

private:
    FILE* file_;
    std::string path_;
    unsigned int fileSize_;
    std::vector<PakEntry> entries_;

    PakFile(const PakFile&);
    PakFile& operator=(const PakFile&);
};
