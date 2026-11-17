// resolver.h - the jukebox track list + the game's pak override chain.
//
// The game resolves a track name N in this order (first hit wins):
//   1. loose file  <datadir>\backboneteller\N.wav
//   2. bbll.pak
//   3. bbt3.pak
//   4. bbt2.pak
//   5. bbt.pak
// Later paks override earlier ones. We mirror that for *.ogg entries.  As an
// extra convenience for modded setups we also accept a loose .ogg and finally
// a loose .mp3 when a track is not present in any pak.
#pragma once

#include "pak.h"

#include "core/game_tracks.h"

enum SourceKind {
    SRC_NONE = 0,
    SRC_PAK,
    SRC_LOOSE_WAV,
    SRC_LOOSE_OGG,
    SRC_LOOSE_MP3
};

struct ResolvedSource {
    SourceKind kind;
    std::string label;     // "bbt.pak", "bbt2.pak", "bbt3.pak", "loose wav", "loose ogg", "loose mp3"
    std::string loosePath; // valid for loose sources
    int pakSlot;           // index into Resolver::paks_ (0=bbll, 1=bbt3, 2=bbt2, 3=bbt)
    PakEntry entry;        // valid for SRC_PAK
    unsigned int size;     // payload size in bytes

    ResolvedSource() : kind(SRC_NONE), pakSlot(-1), size(0) {}
    bool ok() const { return kind != SRC_NONE; }
};

class Resolver {
public:
    Resolver();
    ~Resolver();

    // gameDir is the folder that contains the "backboneteller" subfolder.
    bool init(const std::string& gameDir, std::string& err);
    void shutdown();

    const std::string& gameDir() const { return gameDir_; }
    const std::string& looseDir() const { return looseDir_; }

    ResolvedSource resolve(const std::string& trackName) const;
    bool loadBytes(const ResolvedSource& src, std::vector<unsigned char>& out, std::string& err) const;

    int pakCount() const { return 4; }
    const PakFile& pak(int i) const { return paks_[i]; }
    static const char* pakSlotName(int i);

private:
    PakFile paks_[4];   // 0=bbll.pak (usually absent), 1=bbt3, 2=bbt2, 3=bbt
    bool pakOk_[4];
    std::string gameDir_;
    std::string dataDir_;  // <gameDir>\backboneteller
    std::string looseDir_; // <dataDir>\bbt\backboneteller
    std::string looseDirAlt_; // <dataDir>
};
