#include "catalog.h"
#include "catalog_platform.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr wchar_t kCatalogName[] = L"hsbgm.catalog";
constexpr wchar_t kBackupSuffix[] = L".bak";
constexpr char kMagic[8] = {'H', 'S', 'B', 'G', 'M', 'C', 'A', 'T'};
constexpr std::uint32_t kFormatVersion = 1;
constexpr std::size_t kHeaderSize = 24;
constexpr std::size_t kPayloadPrefixSize = 12;
constexpr std::uint32_t kMaxPayloadBytes = 256u * 1024u * 1024u;
constexpr std::uint32_t kMaxPaths = 1000000u;
constexpr std::uint32_t kMaxPathBytes = 16u * 1024u * 1024u;

static_assert(sizeof(wchar_t) == 2, "catalog uses UTF-16 wchar_t");

std::mutex& CatalogMutex() {
    static std::mutex mutex;
    return mutex;
}

std::wstring JoinPath(const std::wstring& directory,
                      const std::wstring& name) {
    if (directory.empty()) return name;
    const wchar_t last = directory.back();
    if (last == L'\\' || last == L'/') return directory + name;
    return directory + L"\\" + name;
}

std::wstring CatalogPath() {
    const std::wstring directory = catalog::platform::ExecutableDirectory();
    if (directory.empty()) return kCatalogName;
    return JoinPath(directory, kCatalogName);
}

void AppendU32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8));
    bytes.push_back(static_cast<std::uint8_t>(value >> 16));
    bytes.push_back(static_cast<std::uint8_t>(value >> 24));
}

void SetU32(std::array<std::uint8_t, kHeaderSize>& bytes, std::size_t offset,
            std::uint32_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value);
    bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8);
    bytes[offset + 2] = static_cast<std::uint8_t>(value >> 16);
    bytes[offset + 3] = static_cast<std::uint8_t>(value >> 24);
}

std::uint32_t ReadU32(const std::uint8_t* bytes) {
    return static_cast<std::uint32_t>(bytes[0]) |
           (static_cast<std::uint32_t>(bytes[1]) << 8) |
           (static_cast<std::uint32_t>(bytes[2]) << 16) |
           (static_cast<std::uint32_t>(bytes[3]) << 24);
}

std::uint32_t Crc32(const std::uint8_t* bytes, std::size_t size) {
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> values = {};
        for (std::uint32_t i = 0; i < values.size(); ++i) {
            std::uint32_t value = i;
            for (int bit = 0; bit < 8; ++bit) {
                value = (value >> 1) ^
                        ((value & 1u) ? 0xedb88320u : 0u);
            }
            values[i] = value;
        }
        return values;
    }();

    std::uint32_t crc = 0xffffffffu;
    for (std::size_t i = 0; i < size; ++i) {
        crc = table[(crc ^ bytes[i]) & 0xffu] ^ (crc >> 8);
    }
    return ~crc;
}

bool AppendUtf16(std::vector<std::uint8_t>& bytes,
                 const std::wstring& value) {
    if (value.empty()) return false;
    if (value.size() >
        static_cast<std::size_t>(kMaxPathBytes / sizeof(wchar_t))) {
        return false;
    }
    if (value.find(L'\0') != std::wstring::npos) return false;

    const std::uint32_t byteCount =
        static_cast<std::uint32_t>(value.size() * sizeof(wchar_t));
    if (bytes.size() >
        static_cast<std::size_t>(kMaxPayloadBytes) - 4u - byteCount) {
        return false;
    }
    AppendU32(bytes, byteCount);
    for (wchar_t unit : value) {
        const std::uint16_t codeUnit = static_cast<std::uint16_t>(unit);
        bytes.push_back(static_cast<std::uint8_t>(codeUnit));
        bytes.push_back(static_cast<std::uint8_t>(codeUnit >> 8));
    }
    return true;
}

bool BuildPayload(const std::vector<std::wstring>& paths, int selected,
                  int mode, std::vector<std::uint8_t>& payload) {
    if (paths.size() > kMaxPaths) return false;

    payload.clear();
    payload.reserve(static_cast<std::size_t>(kPayloadPrefixSize) +
                    std::min<std::size_t>(
                        paths.size(), static_cast<std::size_t>(65536)) * 64u);
    AppendU32(payload, static_cast<std::uint32_t>(selected));
    AppendU32(payload, static_cast<std::uint32_t>(mode));
    AppendU32(payload, static_cast<std::uint32_t>(paths.size()));

    for (const std::wstring& path : paths) {
        if (!AppendUtf16(payload, path)) return false;
    }
    return payload.size() <= kMaxPayloadBytes;
}

bool WriteCatalog(const std::wstring& path,
                  const std::vector<std::wstring>& paths, int selected,
                  int mode) {
    std::vector<std::uint8_t> payload;
    if (!BuildPayload(paths, selected, mode, payload)) return false;

    std::array<std::uint8_t, kHeaderSize> header = {};
    std::memcpy(header.data(), kMagic, sizeof(kMagic));
    SetU32(header, 8, kFormatVersion);
    SetU32(header, 12, static_cast<std::uint32_t>(kHeaderSize));
    SetU32(header, 16, static_cast<std::uint32_t>(payload.size()));
    SetU32(header, 20, Crc32(payload.data(), payload.size()));

    return catalog::platform::WriteFileAtomically(
        path, path + kBackupSuffix,
        {header.data(), header.size()},
        {payload.data(), payload.size()});
}

bool DecodeUtf16(const std::uint8_t* bytes, std::uint32_t byteCount,
                 std::wstring& value) {
    if (byteCount == 0 || (byteCount & 1u) != 0) return false;
    const std::size_t units = byteCount / sizeof(wchar_t);
    value.clear();
    value.reserve(units);

    for (std::size_t i = 0; i < units; ++i) {
        const std::uint16_t codeUnit = static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(bytes[i * 2]) |
            (static_cast<std::uint16_t>(bytes[i * 2 + 1]) << 8));
        if (codeUnit == 0) return false;

        if (codeUnit >= 0xd800u && codeUnit <= 0xdbffu) {
            if (i + 1 >= units) return false;
            const std::uint16_t next = static_cast<std::uint16_t>(
                static_cast<std::uint16_t>(bytes[(i + 1) * 2]) |
                (static_cast<std::uint16_t>(bytes[(i + 1) * 2 + 1]) << 8));
            if (next < 0xdc00u || next > 0xdfffu) return false;
            value.push_back(static_cast<wchar_t>(codeUnit));
            value.push_back(static_cast<wchar_t>(next));
            ++i;
        } else {
            if (codeUnit >= 0xdc00u && codeUnit <= 0xdfffu) return false;
            value.push_back(static_cast<wchar_t>(codeUnit));
        }
    }
    return true;
}

enum class ReadResult {
    Missing,
    Valid,
    Malformed,
    Unreadable
};

ReadResult ReadCatalog(const std::wstring& path,
                       std::vector<std::wstring>& paths, int& selected,
                       int& mode) {
    std::vector<std::uint8_t> bytes;
    const catalog::platform::FileReadStatus status =
        catalog::platform::ReadFile(
            path, kHeaderSize + kMaxPayloadBytes, bytes);
    if (status == catalog::platform::FileReadStatus::Missing) {
        return ReadResult::Missing;
    }
    if (status == catalog::platform::FileReadStatus::Unreadable) {
        return ReadResult::Unreadable;
    }
    if (bytes.size() < kHeaderSize) return ReadResult::Malformed;

    if (std::memcmp(bytes.data(), kMagic, sizeof(kMagic)) != 0 ||
        ReadU32(bytes.data() + 8) != kFormatVersion ||
        ReadU32(bytes.data() + 12) != kHeaderSize) {
        return ReadResult::Malformed;
    }

    const std::uint32_t payloadBytes = ReadU32(bytes.data() + 16);
    if (payloadBytes != bytes.size() - kHeaderSize ||
        payloadBytes < kPayloadPrefixSize || payloadBytes > kMaxPayloadBytes) {
        return ReadResult::Malformed;
    }

    const std::uint8_t* payload = bytes.data() + kHeaderSize;
    if (Crc32(payload, payloadBytes) != ReadU32(bytes.data() + 20)) {
        return ReadResult::Malformed;
    }

    const int loadedSelected =
        static_cast<int>(static_cast<std::int32_t>(ReadU32(payload)));
    const int loadedMode =
        static_cast<int>(static_cast<std::int32_t>(ReadU32(payload + 4)));
    const std::uint32_t count = ReadU32(payload + 8);
    if (count > kMaxPaths) return ReadResult::Malformed;

    std::size_t offset = kPayloadPrefixSize;
    std::vector<std::wstring> loadedPaths;
    loadedPaths.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        if (offset > payloadBytes || payloadBytes - offset < 4) {
            return ReadResult::Malformed;
        }
        const std::uint32_t byteCount = ReadU32(payload + offset);
        offset += 4;
        if (byteCount == 0 || byteCount > kMaxPathBytes ||
            offset > payloadBytes || payloadBytes - offset < byteCount) {
            return ReadResult::Malformed;
        }
        std::wstring value;
        if (!DecodeUtf16(payload + offset, byteCount, value)) {
            return ReadResult::Malformed;
        }
        loadedPaths.push_back(std::move(value));
        offset += byteCount;
    }
    if (offset != payloadBytes) return ReadResult::Malformed;

    paths.swap(loadedPaths);
    selected = loadedSelected;
    mode = loadedMode;
    return ReadResult::Valid;
}

} // namespace

namespace catalog {

void load(std::vector<std::wstring>& paths, int& selected, int& mode) {
    std::lock_guard<std::mutex> lock(CatalogMutex());
    const std::wstring path = CatalogPath();

    std::vector<std::wstring> loaded;
    int loadedSelected = 0;
    int loadedMode = 0;
    const ReadResult result =
        ReadCatalog(path, loaded, loadedSelected, loadedMode);
    if (result == ReadResult::Valid) {
        paths.swap(loaded);
        selected = loadedSelected;
        mode = loadedMode;
        return;
    }
    if (result == ReadResult::Missing) {
        paths.clear();
        selected = 0;
        mode = 0;
        return;
    }

    // A previous atomic write may still be usable even if the live catalog was
    // damaged after replacement.  If recovery also fails, leave caller data
    // untouched.
    loaded.clear();
    loadedSelected = 0;
    loadedMode = 0;
    if (ReadCatalog(path + kBackupSuffix, loaded, loadedSelected, loadedMode) ==
        ReadResult::Valid) {
        paths.swap(loaded);
        selected = loadedSelected;
        mode = loadedMode;
    }
}

void save(const std::vector<std::wstring>& paths, int selected, int mode) {
    std::lock_guard<std::mutex> lock(CatalogMutex());
    try {
        const std::wstring path = CatalogPath();
        if (!path.empty()) WriteCatalog(path, paths, selected, mode);
    } catch (const std::bad_alloc&) {
        // The existing catalog remains intact when construction fails.
    }
}

Tags tags(const std::wstring& path) {
    if (path.empty()) return Tags();

    Tags result;
    try {
        result = platform::ReadTags(path);
    } catch (const std::bad_alloc&) {
        return Tags();
    }
    platform::RecoverLegacyJapaneseTags(path, result);
    return result;
}

std::vector<std::wstring> folder(HWND owner) {
    try {
        return platform::ChooseFolder(owner);
    } catch (const std::bad_alloc&) {
        return {};
    }
}

} // namespace catalog
