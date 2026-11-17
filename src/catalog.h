// catalog.h - persistent local music catalog and file metadata.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <string>
#include <vector>

namespace catalog {

struct Tags {
    std::wstring title;
    std::wstring artist;
    std::wstring album;
};

// A missing catalog initializes the outputs to an empty catalog.  If an
// existing catalog and its backup are malformed or unreadable, the outputs
// are left unchanged.
void load(std::vector<std::wstring>& paths, int& selected, int& mode);

// Writes a checksummed UTF-16 catalog beside the executable.  Replacement is
// atomic and preserves the previous catalog as hsbgm.catalog.bak.
void save(const std::vector<std::wstring>& paths, int selected, int mode);

Tags tags(const std::wstring& path);

// Lets the user choose a directory, then returns supported audio files below
// it.  Directory symlinks and junctions are skipped.
std::vector<std::wstring> folder(HWND owner);

} // namespace catalog
