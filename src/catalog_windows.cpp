#include "catalog_platform.h"

#include <propsys.h>
#include <propkey.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {

using Microsoft::WRL::ComPtr;

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
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) {
            return std::wstring();
        }
    }
    return std::wstring();
}

bool InstallAtomically(const std::wstring& temp,
                       const std::wstring& destination,
                       const std::wstring& backup) {
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

    if (!DeleteFileW(backup.c_str()) &&
        GetLastError() != ERROR_FILE_NOT_FOUND) {
        return false;
    }
    return ReplaceFileW(destination.c_str(), temp.c_str(), backup.c_str(),
                        REPLACEFILE_WRITE_THROUGH, nullptr, nullptr) != FALSE;
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
void RecoverLegacyJapaneseTagsImpl(const std::wstring& path,
                                   catalog::Tags& tags) {
    if (path.size() < 4 ||
        _wcsicmp(path.c_str() + path.size() - 4, L".mp3") != 0) {
        return;
    }

    FILE* raw = nullptr;
    if (_wfopen_s(&raw, path.c_str(), L"rb") != 0 || !raw) return;
    unsigned char head[3] = {};
    unsigned char tail[160] = {};
    const bool read =
        std::fread(head, 1, 3, raw) == 3 &&
        std::memcmp(head, "ID3", 3) != 0 &&
        _fseeki64(raw, -160, SEEK_END) == 0 &&
        std::fread(tail, 1, 160, raw) == 160;
    std::fclose(raw);
    if (!read || std::memcmp(tail + 32, "TAG", 3) != 0 ||
        std::memcmp(tail, "APETAGEX", 8) == 0) {
        return;
    }

    auto decode = [&](int start) {
        const char* field = reinterpret_cast<const char*>(tail + 32 + start);
        int count = 0;
        while (count < 30 && field[count]) ++count;
        while (count && field[count - 1] == ' ') --count;

        wchar_t output[30];
        const int written = MultiByteToWideChar(
            932, MB_ERR_INVALID_CHARS, field, count, output, 30);
        return written > 0 ? std::wstring(output, written) : std::wstring();
    };

    auto title = decode(3);
    auto artist = decode(33);
    auto album = decode(63);
    auto japanese = [](wchar_t character) {
        return (character >= 0x3040 && character <= 0x30ff) ||
               (character >= 0x3400 && character <= 0x9fff);
    };
    const std::size_t slash = path.find_last_of(L"/\\");
    const std::wstring filename =
        path.substr(slash == std::wstring::npos ? 0 : slash + 1);
    const bool titleMatch =
        std::any_of(title.begin(), title.end(), japanese) &&
        filename.find(title) != std::wstring::npos;

    // Album folders often omit the volume suffix, e.g. 羽の詩Vol.01.
    std::size_t prefix = 0;
    while (prefix < album.size() && japanese(album[prefix])) ++prefix;
    const bool albumMatch =
        prefix >= 3 && slash != std::wstring::npos &&
        path.substr(0, slash).find(album.substr(0, prefix)) !=
            std::wstring::npos;
    if (!titleMatch && !albumMatch) return;
    if (!title.empty()) tags.title = title;
    if (!artist.empty()) tags.artist = artist;
    if (!album.empty()) tags.album = album;
}

} // namespace

namespace catalog {
namespace platform {

std::wstring ExecutableDirectory() {
    const std::wstring path = ModulePath();
    if (path.empty()) return std::wstring();
    const std::size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return L".";
    if (slash == 0) return path.substr(0, 1);
    return path.substr(0, slash);
}

FileReadStatus ReadFile(const std::wstring& path, std::uint64_t maxBytes,
                        std::vector<std::uint8_t>& bytes) {
    HANDLE file = CreateFileW(
        path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            return FileReadStatus::Missing;
        }
        return FileReadStatus::Unreadable;
    }

    LARGE_INTEGER fileSize = {};
    const bool sized = GetFileSizeEx(file, &fileSize) != FALSE;
    const bool ok =
        sized && fileSize.QuadPart >= 0 &&
        static_cast<std::uint64_t>(fileSize.QuadPart) <= maxBytes &&
        ReadWholeFile(file,
                      static_cast<std::uint64_t>(fileSize.QuadPart), bytes);
    CloseHandle(file);
    return ok ? FileReadStatus::Success : FileReadStatus::Unreadable;
}

bool WriteFileAtomically(const std::wstring& destination,
                         const std::wstring& backup, ByteRange first,
                         ByteRange second) {
    HANDLE file = INVALID_HANDLE_VALUE;
    const std::wstring temp = UniqueTempPath(destination, file);
    if (temp.empty() || file == INVALID_HANDLE_VALUE) return false;

    bool ok = WriteAll(file, first.data, first.size) &&
              WriteAll(file, second.data, second.size) &&
              FlushFileBuffers(file) != FALSE;
    if (!CloseHandle(file)) ok = false;

    if (ok) ok = InstallAtomically(temp, destination, backup);
    if (!ok) DeleteFileW(temp.c_str());
    return ok;
}

Tags ReadTags(const std::wstring& path) {
    Tags result;
    ComApartment com(COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (!com.ready()) return result;

    ComPtr<IPropertyStore> store;
    const HRESULT hr = SHGetPropertyStoreFromParsingName(
        path.c_str(), nullptr, GPS_DEFAULT, IID_PPV_ARGS(&store));
    if (FAILED(hr) || !store) return result;

    result.title = ReadProperty(store.Get(), PKEY_Title);
    result.artist = ReadProperty(store.Get(), PKEY_Music_Artist);
    result.album = ReadProperty(store.Get(), PKEY_Music_AlbumTitle);
    return result;
}

void RecoverLegacyJapaneseTags(const std::wstring& path, Tags& tags) {
    RecoverLegacyJapaneseTagsImpl(path, tags);
}

std::vector<std::wstring> ChooseFolder(HWND owner) {
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
}

} // namespace platform
} // namespace catalog
