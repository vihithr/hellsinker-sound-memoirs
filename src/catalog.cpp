#include "catalog.h"

#include <propsys.h>
#include <propkey.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace {

using Microsoft::WRL::ComPtr;

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

std::wstring ModulePath() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        SetLastError(ERROR_SUCCESS);
        const DWORD copied = GetModuleFileNameW(
            nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (copied == 0) return std::wstring();

        if (copied < path.size()) {
            path.resize(copied);
            return path;
        }
        if (path.size() >= 32768) return std::wstring();
        path.resize(std::min<std::size_t>(path.size() * 2, 32768));
    }
}

std::wstring ExecutableDirectory() {
    const std::wstring path = ModulePath();
    if (path.empty()) return std::wstring();
    const std::size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return L".";
    if (slash == 0) return path.substr(0, 1);
    return path.substr(0, slash);
}

std::wstring JoinPath(const std::wstring& directory,
                      const std::wstring& name) {
    if (directory.empty()) return name;
    const wchar_t last = directory.back();
    if (last == L'\\' || last == L'/') return directory + name;
    return directory + L"\\" + name;
}

std::wstring CatalogPath() {
    const std::wstring directory = ExecutableDirectory();
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

bool WriteAll(HANDLE file, const void* data, std::size_t size) {
    const std::uint8_t* cursor = static_cast<const std::uint8_t*>(data);
    while (size != 0) {
        const DWORD chunk =
            static_cast<DWORD>(std::min<std::size_t>(
                size, static_cast<std::size_t>(
                          std::numeric_limits<DWORD>::max())));
        DWORD written = 0;
        if (!WriteFile(file, cursor, chunk, &written, nullptr) ||
            written != chunk) {
            return false;
        }
        cursor += written;
        size -= written;
    }
    return true;
}

std::wstring UniqueTempPath(const std::wstring& destination,
                            HANDLE& file) {
    file = INVALID_HANDLE_VALUE;
    const std::wstring stem =
        destination + L".tmp." +
        std::to_wstring(GetCurrentProcessId()) + L"." +
        std::to_wstring(GetTickCount64()) + L".";

    for (unsigned int attempt = 0; attempt < 100; ++attempt) {
        const std::wstring candidate = stem + std::to_wstring(attempt);
        file = CreateFileW(candidate.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_NEW,
                           FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_WRITE_THROUGH,
                           nullptr);
        if (file != INVALID_HANDLE_VALUE) return candidate;
        if (GetLastError() != ERROR_FILE_EXISTS &&
            GetLastError() != ERROR_ALREADY_EXISTS) {
            return std::wstring();
        }
    }
    return std::wstring();
}

bool InstallAtomically(const std::wstring& temp,
                       const std::wstring& destination) {
    SetLastError(ERROR_SUCCESS);
    const DWORD attributes = GetFileAttributesW(destination.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
            return false;
        }
        return MoveFileExW(temp.c_str(), destination.c_str(),
                           MOVEFILE_WRITE_THROUGH) != FALSE;
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) return false;

    const std::wstring backup = destination + kBackupSuffix;
    if (!DeleteFileW(backup.c_str()) &&
        GetLastError() != ERROR_FILE_NOT_FOUND) {
        return false;
    }
    return ReplaceFileW(destination.c_str(), temp.c_str(), backup.c_str(),
                        REPLACEFILE_WRITE_THROUGH, nullptr, nullptr) != FALSE;
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

    HANDLE file = INVALID_HANDLE_VALUE;
    const std::wstring temp = UniqueTempPath(path, file);
    if (temp.empty() || file == INVALID_HANDLE_VALUE) return false;

    bool ok = WriteAll(file, header.data(), header.size()) &&
              WriteAll(file, payload.data(), payload.size()) &&
              FlushFileBuffers(file) != FALSE;
    if (!CloseHandle(file)) ok = false;

    if (ok) ok = InstallAtomically(temp, path);
    if (!ok) DeleteFileW(temp.c_str());
    return ok;
}

bool ReadWholeFile(HANDLE file, std::uint64_t size,
                   std::vector<std::uint8_t>& bytes) {
    if (size > std::numeric_limits<std::size_t>::max()) return false;
    bytes.resize(static_cast<std::size_t>(size));
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const DWORD chunk =
            static_cast<DWORD>(std::min<std::size_t>(
                bytes.size() - offset,
                static_cast<std::size_t>(
                    std::numeric_limits<DWORD>::max())));
        DWORD read = 0;
        if (!ReadFile(file, bytes.data() + offset, chunk, &read, nullptr) ||
            read == 0) {
            return false;
        }
        offset += read;
    }
    return true;
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
    HANDLE file = CreateFileW(
        path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            return ReadResult::Missing;
        }
        return ReadResult::Unreadable;
    }

    LARGE_INTEGER fileSize = {};
    std::vector<std::uint8_t> bytes;
    bool ok = GetFileSizeEx(file, &fileSize) != FALSE &&
              fileSize.QuadPart >= static_cast<LONGLONG>(kHeaderSize) &&
              fileSize.QuadPart <= static_cast<LONGLONG>(
                  kHeaderSize + kMaxPayloadBytes) &&
              ReadWholeFile(file, static_cast<std::uint64_t>(fileSize.QuadPart),
                            bytes);
    CloseHandle(file);
    if (!ok) return ReadResult::Malformed;

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

class ComApartment {
public:
    explicit ComApartment(DWORD model)
        : result_(CoInitializeEx(nullptr, model)),
          uninitialize_(SUCCEEDED(result_)) {}

    ~ComApartment() {
        if (uninitialize_) CoUninitialize();
    }

    bool ready() const {
        return SUCCEEDED(result_) || result_ == RPC_E_CHANGED_MODE;
    }

private:
    HRESULT result_;
    bool uninitialize_;
};

void AppendValue(std::wstring& output, const std::wstring& value) {
    if (value.empty()) return;
    if (!output.empty()) output += L"; ";
    output += value;
}

std::wstring NarrowToWide(const char* value) {
    if (!value || !*value) return std::wstring();
    const int length = MultiByteToWideChar(CP_ACP, 0, value, -1, nullptr, 0);
    if (length <= 1) return std::wstring();
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    const int written =
        MultiByteToWideChar(CP_ACP, 0, value, -1, wide.data(), length);
    if (written != length) return std::wstring();
    wide.resize(static_cast<std::size_t>(length - 1));
    return wide;
}

std::wstring PropertyString(const PROPVARIANT& value) {
    const VARTYPE type = value.vt;
    std::wstring result;

    if (type == VT_LPWSTR && value.pwszVal) {
        result = value.pwszVal;
    } else if (type == VT_BSTR && value.bstrVal) {
        result.assign(value.bstrVal, SysStringLen(value.bstrVal));
    } else if (type == VT_LPSTR && value.pszVal) {
        result = NarrowToWide(value.pszVal);
    } else if (type == (VT_VECTOR | VT_LPWSTR) && value.calpwstr.pElems) {
        for (ULONG i = 0; i < value.calpwstr.cElems; ++i) {
            const wchar_t* element = value.calpwstr.pElems[i];
            if (element) AppendValue(result, element);
        }
    } else if (type == (VT_VECTOR | VT_BSTR) && value.cabstr.pElems) {
        for (ULONG i = 0; i < value.cabstr.cElems; ++i) {
            const BSTR element = value.cabstr.pElems[i];
            if (element) {
                AppendValue(result,
                            std::wstring(element, SysStringLen(element)));
            }
        }
    } else if (type == (VT_VECTOR | VT_LPSTR) && value.calpstr.pElems) {
        for (ULONG i = 0; i < value.calpstr.cElems; ++i) {
            AppendValue(result, NarrowToWide(value.calpstr.pElems[i]));
        }
    }
    return result;
}

std::wstring ReadProperty(IPropertyStore* store, REFPROPERTYKEY key) {
    PROPVARIANT value;
    PropVariantInit(&value);
    std::wstring result;
    if (SUCCEEDED(store->GetValue(key, &value))) {
        result = PropertyString(value);
    }
    PropVariantClear(&value);
    return result;
}

bool HasExtendedPrefix(const std::wstring& path) {
    return path.size() >= 4 && path[0] == L'\\' && path[1] == L'\\' &&
           (path[2] == L'?' || path[2] == L'.') && path[3] == L'\\';
}

bool AbsolutePath(std::wstring& path) {
    if (HasExtendedPrefix(path)) return true;
    if (path.size() >= 3 && path[1] == L':' &&
        (path[2] == L'\\' || path[2] == L'/')) {
        return true;
    }
    if (path.size() >= 2 &&
        (path[0] == L'\\' || path[0] == L'/') &&
        (path[1] == L'\\' || path[1] == L'/')) {
        return true;
    }

    DWORD size = MAX_PATH;
    for (;;) {
        std::wstring full(size, L'\0');
        const DWORD length =
            GetFullPathNameW(path.c_str(), size, full.data(), nullptr);
        if (length == 0) return false;
        if (length < size) {
            full.resize(length);
            path.swap(full);
            return true;
        }
        if (length >= 32768) return false;
        size = length;
    }
}

std::wstring ExtendedPath(const std::wstring& original) {
    if (HasExtendedPrefix(original)) return original;
    std::wstring path = original;
    if (!AbsolutePath(path)) return std::wstring();

    if (path.size() >= 2 &&
        (path[0] == L'\\' || path[0] == L'/') &&
        (path[1] == L'\\' || path[1] == L'/')) {
        return L"\\\\?\\UNC\\" + path.substr(2);
    }
    return L"\\\\?\\" + path;
}

std::wstring PlainPath(const std::wstring& path) {
    if (path.compare(0, 8, L"\\\\?\\UNC\\") == 0) {
        return L"\\\\" + path.substr(8);
    }
    if (path.size() >= 4 && path[0] == L'\\' && path[1] == L'\\' &&
        path[2] == L'?' && path[3] == L'\\') {
        return path.substr(4);
    }
    return path;
}

std::wstring LongPathJoin(const std::wstring& directory,
                          const wchar_t* name) {
    if (directory.empty()) return name;
    const wchar_t last = directory.back();
    if (last == L'\\' || last == L'/') return directory + name;
    return directory + L"\\" + name;
}

bool IsAudioPath(const wchar_t* name) {
    static constexpr const wchar_t* extensions[] = {
        L".wav", L".wave", L".ogg", L".oga", L".opus", L".mp3",
        L".flac", L".m4a", L".aac", L".wma", L".mp4"
    };
    const wchar_t* extension = std::wcsrchr(name, L'.');
    if (!extension) return false;
    for (const wchar_t* candidate : extensions) {
        if (_wcsicmp(extension, candidate) == 0) return true;
    }
    return false;
}

bool IsDotEntry(const wchar_t* name) {
    return (name[0] == L'.' && name[1] == L'\0') ||
           (name[0] == L'.' && name[1] == L'.' && name[2] == L'\0');
}

int ComparePaths(const std::wstring& left, const std::wstring& right) {
    const int folded = CompareStringOrdinal(
        left.c_str(), -1, right.c_str(), -1, TRUE);
    if (folded != CSTR_EQUAL) return folded;
    return CompareStringOrdinal(
        left.c_str(), -1, right.c_str(), -1, FALSE);
}

std::vector<std::wstring> ScanFolder(const std::wstring& selected) {
    std::vector<std::wstring> audio;
    const std::wstring root = ExtendedPath(selected);
    if (root.empty()) return audio;

    const DWORD rootAttributes = GetFileAttributesW(root.c_str());
    if (rootAttributes == INVALID_FILE_ATTRIBUTES ||
        (rootAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (rootAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return audio;
    }

    std::vector<std::wstring> pending;
    pending.push_back(root);
    while (!pending.empty()) {
        std::wstring directory = std::move(pending.back());
        pending.pop_back();

        const std::wstring pattern = LongPathJoin(directory, L"*");
        WIN32_FIND_DATAW data = {};
        HANDLE find = FindFirstFileExW(
            pattern.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch,
            nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (find == INVALID_HANDLE_VALUE) continue;

        do {
            if (IsDotEntry(data.cFileName) ||
                (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
                continue;
            }

            const std::wstring full =
                LongPathJoin(directory, data.cFileName);
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                pending.push_back(full);
            } else if (IsAudioPath(data.cFileName)) {
                audio.push_back(PlainPath(full));
            }
        } while (FindNextFileW(find, &data));
        FindClose(find);
    }

    std::sort(audio.begin(), audio.end(),
              [](const std::wstring& left, const std::wstring& right) {
                  return ComparePaths(left, right) == CSTR_LESS_THAN;
              });
    return audio;
}

// ID3v1 has no encoding marker. Japanese releases often used Windows-31J;
// only override the shell when an independent Unicode path confirms it.
void RecoverLegacyJapaneseTags(const std::wstring& path, catalog::Tags& tags) {
    if (path.size()<4 || _wcsicmp(path.c_str()+path.size()-4,L".mp3")!=0) return;
    FILE* raw=nullptr;
    if (_wfopen_s(&raw,path.c_str(),L"rb") || !raw) return;
    unsigned char head[3]{}, tail[160]{};
    bool read=fread(head,1,3,raw)==3 && memcmp(head,"ID3",3)!=0 &&
              _fseeki64(raw,-160,SEEK_END)==0 && fread(tail,1,160,raw)==160;
    fclose(raw);
    if (!read || memcmp(tail+32,"TAG",3)!=0 || memcmp(tail,"APETAGEX",8)==0) return;

    auto decode=[&](int start) {
        const char* field=reinterpret_cast<const char*>(tail+32+start);
        int n=0;while(n<30 && field[n]) ++n;
        while(n && field[n-1]==' ') --n;
        wchar_t out[30];
        int count=MultiByteToWideChar(932,MB_ERR_INVALID_CHARS,field,n,out,30);
        return count>0?std::wstring(out,count):std::wstring();
    };
    auto title=decode(3), artist=decode(33), album=decode(63);
    auto japanese=[](wchar_t c) {return (c>=0x3040 && c<=0x30ff) ||
                                         (c>=0x3400 && c<=0x9fff);};
    auto slash=path.find_last_of(L"/\\");
    auto filename=path.substr(slash==std::wstring::npos?0:slash+1);
    bool titleMatch=std::any_of(title.begin(),title.end(),japanese) &&
                    filename.find(title)!=std::wstring::npos;
    // Album folders often omit the volume suffix, e.g. 羽の詩Vol.01.
    size_t prefix=0;while(prefix<album.size() && japanese(album[prefix])) ++prefix;
    bool albumMatch=prefix>=3 && slash!=std::wstring::npos &&
        path.substr(0,slash).find(album.substr(0,prefix))!=std::wstring::npos;
    if (!titleMatch && !albumMatch) return;
    if (!title.empty()) tags.title=title;
    if (!artist.empty()) tags.artist=artist;
    if (!album.empty()) tags.album=album;
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
    Tags result;
    if (path.empty()) return result;

    try {
        ComApartment com(COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        if (!com.ready()) return result;

        ComPtr<IPropertyStore> store;
        const HRESULT hr = SHGetPropertyStoreFromParsingName(
            path.c_str(), nullptr, GPS_DEFAULT, IID_PPV_ARGS(&store));
        if (FAILED(hr) || !store) return result;

        result.title = ReadProperty(store.Get(), PKEY_Title);
        result.artist = ReadProperty(store.Get(), PKEY_Music_Artist);
        result.album = ReadProperty(store.Get(), PKEY_Music_AlbumTitle);
    } catch (const std::bad_alloc&) {
        return Tags();
    }
    RecoverLegacyJapaneseTags(path,result);
    return result;
}

std::vector<std::wstring> folder(HWND owner) {
    try {
        ComApartment com(COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        if (!com.ready()) return {};

        ComPtr<IFileOpenDialog> dialog;
        HRESULT hr = CoCreateInstance(
            CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&dialog));
        if (FAILED(hr) || !dialog) return {};

        DWORD options = 0;
        if (FAILED(dialog->GetOptions(&options))) return {};
        hr = dialog->SetOptions(
            options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM |
            FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR);
        if (FAILED(hr)) return {};
        hr = dialog->Show(owner);
        if (FAILED(hr)) return {};

        ComPtr<IShellItem> item;
        hr = dialog->GetResult(&item);
        if (FAILED(hr) || !item) return {};

        PWSTR selectedRaw = nullptr;
        hr = item->GetDisplayName(SIGDN_FILESYSPATH, &selectedRaw);
        if (FAILED(hr) || !selectedRaw) return {};
        const std::wstring selected = selectedRaw;
        CoTaskMemFree(selectedRaw);
        return ScanFolder(selected);
    } catch (const std::bad_alloc&) {
        return {};
    }
}

} // namespace catalog
