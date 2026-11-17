// catalog_platform.h - internal platform boundary for catalog.cpp.
#pragma once

#include "catalog.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace catalog {
namespace platform {

struct ByteRange {
    const std::uint8_t* data;
    std::size_t size;
};

enum class FileReadStatus {
    Success,
    Missing,
    Unreadable
};

std::wstring ExecutableDirectory();

// Reads a whole file up to maxBytes. Missing and failure are distinct so the
// catalog policy can preserve its existing missing-catalog behavior.
FileReadStatus ReadFile(const std::wstring& path, std::uint64_t maxBytes,
                        std::vector<std::uint8_t>& bytes);

// Writes one file through a flushed temporary file, then installs it
// atomically. An existing destination is retained as backup.
bool WriteFileAtomically(const std::wstring& destination,
                         const std::wstring& backup, ByteRange first,
                         ByteRange second);

Tags ReadTags(const std::wstring& path);
void RecoverLegacyJapaneseTags(const std::wstring& path, Tags& tags);

std::vector<std::wstring> ChooseFolder(HWND owner);

} // namespace platform
} // namespace catalog
