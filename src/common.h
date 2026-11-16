// common.h - small shared helpers for hsbgm.
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
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <algorithm>

std::string ToLowerAscii(const std::string& s);
std::string ToUpperAscii(const std::string& s);
std::string Trim(const std::string& s);
bool StartsWithNoCase(const std::string& s, const std::string& prefix);
std::string JoinPath(const std::string& a, const std::string& b);
bool FileExists(const std::string& path);
bool DirExists(const std::string& path);
bool ReadWholeFile(const std::string& path, std::vector<unsigned char>& out, std::string& err);
std::string ExeDir();
std::string BaseName(const std::string& path);
unsigned int FileSize(const std::string& path);
std::string FormatTime(double seconds);
std::string FormatBytes(unsigned long long bytes);
std::string IntToStr(long long v);

// Optional diagnostic logging (enabled with HSBGM_DEBUG=1; writes hsbgm_debug.log).
void DbgEnable(bool on);
bool DbgOn();
void DbgLog(const char* fmt, ...);
