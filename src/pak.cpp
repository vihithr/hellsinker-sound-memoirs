#include "pak.h"

using memoirs::PAK_DATA_OFF;

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

    if (!memoirs::parseIndex(head.data(), head.size(), fileSize_, entries_, err)) {
        fclose(f);
        err += ": " + path;
        return false;
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
    return memoirs::findEntry(entries_, fileName);
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
