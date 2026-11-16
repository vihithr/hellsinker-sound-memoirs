#include "pak.h"

namespace {

const unsigned int PAK_NUM_OFF  = 0x1F400;
const unsigned int PAK_NAME_OFF = 0x1;     // name table starts at file offset 0x1
const unsigned int PAK_NAME_LEN = 0x100;   // stride; 0xFF usable bytes
const unsigned int PAK_LEN_OFF  = 0x1F404;
const unsigned int PAK_OFS_OFF  = 0x1FBD4;
const unsigned int PAK_DATA_OFF = 0x203A4;
const unsigned int PAK_MAX_ENTRIES = 500;  // name/length/offset tables hold 500 slots

unsigned int ReadU32(const unsigned char* p) {
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
           ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

std::string NormalizeName(const std::string& raw) {
    std::string s = ToLowerAscii(raw);
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '/') s[i] = '\\';
    }
    return s;
}

} // namespace

PakFile::PakFile() : file_(NULL), fileSize_(0) {}

PakFile::~PakFile() { close(); }

bool PakFile::open(const std::string& path, std::string& err) {
    close();
    err.clear();

    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        err = "cannot open pak: " + path;
        return false;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        err = "cannot seek pak: " + path;
        return false;
    }
    long sz = ftell(f);
    if (sz < (long)PAK_DATA_OFF) {
        fclose(f);
        err = "pak too small: " + path;
        return false;
    }
    fileSize_ = (unsigned int)sz;

    std::vector<unsigned char> head(PAK_DATA_OFF);
    if (fseek(f, 0, SEEK_SET) != 0 || fread(&head[0], 1, PAK_DATA_OFF, f) != PAK_DATA_OFF) {
        fclose(f);
        err = "cannot read pak header: " + path;
        return false;
    }

    unsigned int n = ReadU32(&head[PAK_NUM_OFF]);
    if (n == 0 || n > PAK_MAX_ENTRIES) {
        fclose(f);
        err = "implausible pak entry count in " + path;
        return false;
    }

    entries_.clear();
    entries_.reserve(n);
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

        if (e.length > fileSize_ || e.offset > fileSize_ - PAK_DATA_OFF ||
            e.length > fileSize_ - PAK_DATA_OFF - e.offset) {
            fclose(f);
            entries_.clear();
            err = "corrupt pak entry table in " + path;
            return false;
        }
        entries_.push_back(e);
    }

    file_ = f;
    path_ = path;
    return true;
}

void PakFile::close() {
    if (file_) {
        fclose(file_);
        file_ = NULL;
    }
    entries_.clear();
    path_.clear();
    fileSize_ = 0;
}

const PakEntry* PakFile::find(const std::string& fileName) const {
    std::string want = NormalizeName(fileName);
    // Exact (normalized) match first.
    for (size_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i].nameLower == want) return &entries_[i];
    }
    // Bare-name match (strip directories from both sides).
    std::string wantBase = BaseName(want);
    for (size_t i = 0; i < entries_.size(); ++i) {
        if (BaseName(entries_[i].nameLower) == wantBase) return &entries_[i];
    }
    return NULL;
}

bool PakFile::read(const PakEntry& e, std::vector<unsigned char>& out, std::string& err) const {
    out.clear();
    if (!file_) {
        err = "pak not open";
        return false;
    }
    if (e.length == 0) return true;
    out.resize(e.length);
    if (fseek(file_, (long)(PAK_DATA_OFF + e.offset), SEEK_SET) != 0) {
        out.clear();
        err = "pak seek failed";
        return false;
    }
    size_t got = fread(&out[0], 1, e.length, file_);
    if (got != e.length) {
        out.clear();
        err = "short pak read";
        return false;
    }
    return true;
}
