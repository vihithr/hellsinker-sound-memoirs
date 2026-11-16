#include "common.h"
#include <cstdarg>

std::string ToLowerAscii(const std::string& s) {
    std::string r(s);
    for (size_t i = 0; i < r.size(); ++i) {
        char c = r[i];
        if (c >= 'A' && c <= 'Z') r[i] = (char)(c - 'A' + 'a');
    }
    return r;
}

std::string ToUpperAscii(const std::string& s) {
    std::string r(s);
    for (size_t i = 0; i < r.size(); ++i) {
        char c = r[i];
        if (c >= 'a' && c <= 'z') r[i] = (char)(c - 'a' + 'A');
    }
    return r;
}

std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr(a, b - a);
}

bool StartsWithNoCase(const std::string& s, const std::string& prefix) {
    if (s.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        char a = s[i], b = prefix[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

std::string JoinPath(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    char last = a[a.size() - 1];
    if (last == '\\' || last == '/') return a + b;
    return a + "\\" + b;
}

bool FileExists(const std::string& path) {
    if (path.empty()) return false;
    DWORD attr = GetFileAttributesA(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) return false;
    return (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool DirExists(const std::string& path) {
    if (path.empty()) return false;
    DWORD attr = GetFileAttributesA(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) return false;
    return (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool ReadWholeFile(const std::string& path, std::vector<unsigned char>& out, std::string& err) {
    out.clear();
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        err = "cannot open " + path;
        return false;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        err = "seek failed: " + path;
        return false;
    }
    long sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        err = "tell failed: " + path;
        return false;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        err = "seek failed: " + path;
        return false;
    }
    out.resize((size_t)sz);
    if (sz > 0) {
        size_t got = fread(&out[0], 1, (size_t)sz, f);
        if (got != (size_t)sz) {
            fclose(f);
            out.clear();
            err = "short read: " + path;
            return false;
        }
    }
    fclose(f);
    return true;
}

std::string ExeDir() {
    char buf[MAX_PATH + 16];
    DWORD n = GetModuleFileNameA(NULL, buf, MAX_PATH);
    if (n == 0) return ".";
    std::string p(buf, n);
    size_t pos = p.find_last_of("\\/");
    if (pos == std::string::npos) return ".";
    if (pos == 0) return p.substr(0, 1);
    return p.substr(0, pos);
}

std::string BaseName(const std::string& path) {
    size_t pos = path.find_last_of("\\/");
    if (pos == std::string::npos) return path;
    return path.substr(pos + 1);
}

unsigned int FileSize(const std::string& path) {
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &fad)) return 0;
    return (unsigned int)fad.nFileSizeLow;
}

std::string FormatTime(double seconds) {
    if (seconds < 0) seconds = 0;
    int total = (int)(seconds + 0.0005);
    int m = total / 60;
    int s = total % 60;
    char buf[32];
    sprintf(buf, "%02d:%02d", m, s);
    return std::string(buf);
}

std::string FormatBytes(unsigned long long bytes) {
    char buf[64];
    if (bytes >= 1024ull * 1024ull) {
        sprintf(buf, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    } else if (bytes >= 1024ull) {
        sprintf(buf, "%.1f KB", (double)bytes / 1024.0);
    } else {
        sprintf(buf, "%I64u B", bytes);
    }
    return std::string(buf);
}

std::string IntToStr(long long v) {
    char buf[40];
    sprintf(buf, "%I64d", v);
    return std::string(buf);
}

namespace {
bool g_dbgOn = false;
FILE* g_dbgFile = NULL;
CRITICAL_SECTION g_dbgCs;
bool g_dbgCsInit = false;
} // namespace

void DbgEnable(bool on) {
    g_dbgOn = on;
    if (on && !g_dbgCsInit) {
        InitializeCriticalSection(&g_dbgCs);
        g_dbgCsInit = true;
    }
}

bool DbgOn() { return g_dbgOn; }

void DbgLog(const char* fmt, ...) {
    if (!g_dbgOn || !g_dbgCsInit) return;
    EnterCriticalSection(&g_dbgCs);
    if (!g_dbgFile) {
        std::string path = JoinPath(ExeDir(), "hsbgm_debug.log");
        g_dbgFile = fopen(path.c_str(), "a");
    }
    if (g_dbgFile) {
        fprintf(g_dbgFile, "[%lu][t=%lu] ", (unsigned long)GetCurrentThreadId(),
                (unsigned long)GetTickCount());
        va_list ap;
        va_start(ap, fmt);
        vfprintf(g_dbgFile, fmt, ap);
        va_end(ap);
        fputc('\n', g_dbgFile);
        fflush(g_dbgFile);
    }
    LeaveCriticalSection(&g_dbgCs);
}
