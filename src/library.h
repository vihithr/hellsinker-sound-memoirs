#pragma once

#include <string>
#include <vector>

namespace lib {

enum class Lang {
    EN,
    ZH,
    JA
};

Lang language();
void cycleLanguage(int direction = 1);
const wchar_t* tr(const wchar_t* en, const wchar_t* zh, const wchar_t* ja);

} // namespace lib
