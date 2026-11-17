// Windows local-media adapter. The in-memory codecs have no OS dependencies.
#pragma once
#include "core/decoder.h"

bool DecodeLocalMusic(const std::wstring& path, PcmBuffer& out, std::string& err);
