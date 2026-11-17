#include "pak_index.h"
#include <cstring>
#include <iostream>

#define CHECK(expr) do { if (!(expr)) { std::cerr << #expr << " failed\n"; return 1; } } while (0)
int main() {
    using namespace memoirs;
    std::vector<unsigned char> header(PAK_DATA_OFF);
    std::vector<PakEntry> entries;
    std::string err;
    const auto parse = [&] { return parseIndex(header.data(), header.size(), PAK_DATA_OFF + 3, entries, err); };
    CHECK(!parse());
    header[0x1F400] = 2;
    std::strcpy(reinterpret_cast<char*>(&header[1]), "A/Intro.ogg");
    std::strcpy(reinterpret_cast<char*>(&header[257]), "B/Intro.ogg");
    header[0x1F404] = 2;
    header[0x1F408] = 1;
    header[0x1FBD8] = 2;
    CHECK(parse() && entries.size() == 2);
    CHECK(findEntry(entries, "b\\INTRO.ogg") == &entries[1]);
    CHECK(findEntry(entries, "intro.OGG") == &entries[0]);
    header[0x1F408] = 2;
    CHECK(!parse() && entries.empty());
    CHECK(!parseIndex(header.data(), 20, PAK_DATA_OFF, entries, err));
}
