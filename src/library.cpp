#include "library.h"

#include <climits>
#include <cstddef>
#include <cwchar>
#include <iterator>
#include <windows.h>

namespace {

constexpr wchar_t kIniSection[] = L"hsbgm";
constexpr wchar_t kLanguageKey[] = L"Language";

std::wstring FoldCase(const std::wstring& value) {
    if (value.empty()) return std::wstring();
    if (value.size() > static_cast<std::size_t>(INT_MAX)) return value;

    const int sourceLength = static_cast<int>(value.size());
    const int needed = LCMapStringEx(
        LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE,
        value.data(), sourceLength,
        nullptr, 0, nullptr, nullptr, 0);
    if (needed <= 0) return value;

    std::wstring folded(static_cast<std::size_t>(needed), L'\0');
    const int written = LCMapStringEx(
        LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE,
        value.data(), sourceLength,
        folded.data(), needed, nullptr, nullptr, 0);
    if (written <= 0) return value;
    folded.resize(static_cast<std::size_t>(written));
    return folded;
}

bool MatchesLanguageCode(const std::wstring& code, const wchar_t* primary) {
    const std::size_t length = std::wcslen(primary);
    if (code.size() == length) return code.compare(0, length, primary) == 0;
    if (code.size() <= length || code.compare(0, length, primary) != 0) {
        return false;
    }
    return code[length] == L'-' || code[length] == L'_';
}

std::wstring ExecutableDirectory() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(
            nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0) return std::wstring();
        if (length < path.size()) {
            path.resize(length);
            break;
        }
        if (path.size() >= 32768) return std::wstring();
        path.resize(path.size() * 2);
    }

    const std::size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return L".";
    if (slash == 0) return path.substr(0, 1);
    return path.substr(0, slash);
}

std::wstring IniPath() {
    const std::wstring directory = ExecutableDirectory();
    if (directory.empty()) return L"hsbgm.ini";
    const wchar_t last = directory.back();
    if (last == L'\\' || last == L'/') return directory + L"hsbgm.ini";
    return directory + L"\\hsbgm.ini";
}

lib::Lang LanguageFromCode(const std::wstring& value) {
    const std::wstring code = FoldCase(value);
    if (MatchesLanguageCode(code, L"zh")) return lib::Lang::ZH;
    if (MatchesLanguageCode(code, L"ja")) return lib::Lang::JA;
    return lib::Lang::EN;
}

const wchar_t* DefaultLanguageCode() {
    const LANGID ui = PRIMARYLANGID(GetUserDefaultUILanguage());
    if (ui == LANG_CHINESE) return L"zh";
    if (ui == LANG_JAPANESE) return L"ja";
    return L"en";
}

lib::Lang LoadLanguage() {
    wchar_t value[32] = {};
    GetPrivateProfileStringW(
        kIniSection, kLanguageKey, DefaultLanguageCode(), value,
        static_cast<DWORD>(std::size(value)), IniPath().c_str());
    return LanguageFromCode(value);
}

const wchar_t* LanguageCode(lib::Lang language) {
    switch (language) {
        case lib::Lang::ZH: return L"zh";
        case lib::Lang::JA: return L"ja";
        case lib::Lang::EN:
        default: return L"en";
    }
}

void SaveLanguage(lib::Lang language) {
    WritePrivateProfileStringW(
        kIniSection, kLanguageKey, LanguageCode(language), IniPath().c_str());
}

lib::Lang& CurrentLanguage() {
    static lib::Lang current = LoadLanguage();
    return current;
}

} // namespace

namespace lib {

Lang language() {
    return CurrentLanguage();
}

void cycleLanguage(int direction) {
    Lang& current = CurrentLanguage();
    current = static_cast<Lang>((static_cast<int>(current) + (direction < 0 ? 2 : 1)) % 3);
    SaveLanguage(current);
}

const wchar_t* tr(const wchar_t* en, const wchar_t* zh, const wchar_t* ja) {
    switch (language()) {
        case Lang::ZH: return zh ? zh : en;
        case Lang::JA: return ja ? ja : en;
        case Lang::EN:
        default: return en;
    }
}

} // namespace lib
