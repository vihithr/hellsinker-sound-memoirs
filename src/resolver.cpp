#include "resolver.h"

// The jukebox pointer table order used by the game executable.
const char* const kTrackNames[kTrackCount] = {
    "graveyard", "hs_stg1e", "hs_boss1", "hs_stg2e", "hs_stg3", "hs_stg4",
    "hs_boss4", "ontheedge", "hs_task", "hs_stg1", "hs_stg2", "bs2",
    "hs_stg3b", "hs_inter", "hs_stg5", "hs_stg5b", "hs_boss5", "hs_boss5b",
    "hs_stg7", "hs_stg7b", "hs_pra_intro", "hs_pra", "hs_stg6", "hs_ep_pr",
    "hs_boss3b", "hs_inter3", "hs_stg8", "over", "hs_cs00", "hs_cs01",
    "hs_cs_luna", "hs_ep_luna", "hs_cs_sol", "hs_ep_sol", "epitaph_enter",
    "hs_cstr", "hs_cstr_fw", "epitaph_leave", "hs_inter4", "jointo",
    "way_of_all_flesh", "hs_inter5", "hs_inter2", "gameover",
    "PRACTICECHAMBER", "PRACTICECHAMBER2"
};

const char* Resolver::pakSlotName(int i) {
    switch (i) {
        case 0: return "bbll.pak";
        case 1: return "bbt3.pak";
        case 2: return "bbt2.pak";
        case 3: return "bbt.pak";
        default: return "?";
    }
}

Resolver::Resolver() {
    for (int i = 0; i < 4; ++i) pakOk_[i] = false;
}

Resolver::~Resolver() { shutdown(); }

bool Resolver::init(const std::string& gameDir, std::string& err) {
    shutdown();
    err.clear();
    if (!DirExists(gameDir)) {
        err = "game directory not found: " + gameDir;
        return false;
    }
    gameDir_ = gameDir;
    dataDir_ = JoinPath(gameDir_, "backboneteller");
    looseDir_ = JoinPath(dataDir_, "bbt\\backboneteller");
    looseDirAlt_ = dataDir_;

    if (!DirExists(dataDir_)) {
        err = "missing data folder: " + dataDir_;
        return false;
    }

    static const char* kPakFiles[4] = { "bbll.pak", "bbt3.pak", "bbt2.pak", "bbt.pak" };
    std::string firstErr;
    int opened = 0;
    for (int i = 0; i < 4; ++i) {
        std::string p = JoinPath(dataDir_, kPakFiles[i]);
        if (!FileExists(p)) continue; // bbll.pak is normally absent; others may be too
        std::string e;
        if (paks_[i].open(p, e)) {
            pakOk_[i] = true;
            ++opened;
        } else if (firstErr.empty()) {
            firstErr = e;
        }
    }
    if (opened == 0) {
        bool haveLoose = DirExists(looseDir_) || DirExists(looseDirAlt_);
        if (!haveLoose) {
            err = firstErr.empty() ? (std::string("no paks and no loose data under ") + dataDir_) : firstErr;
            return false;
        }
    }
    return true;
}

void Resolver::shutdown() {
    for (int i = 0; i < 4; ++i) {
        paks_[i].close();
        pakOk_[i] = false;
    }
}

ResolvedSource Resolver::resolve(const std::string& trackName) const {
    ResolvedSource r;
    const std::string lower = ToLowerAscii(trackName);

    // 1) loose <datadir>\backboneteller\N.wav (game's top-priority override)
    {
        std::string cands[2];
        cands[0] = JoinPath(looseDir_, lower + ".wav");
        cands[1] = JoinPath(looseDirAlt_, lower + ".wav");
        for (int c = 0; c < 2; ++c) {
            if (FileExists(cands[c])) {
                r.kind = SRC_LOOSE_WAV;
                r.label = "loose wav";
                r.loosePath = cands[c];
                r.size = FileSize(cands[c]);
                return r;
            }
        }
    }

    // 2..5) paks, later overrides earlier: bbll -> bbt3 -> bbt2 -> bbt
    const std::string pakPath = "backboneteller\\" + lower + ".ogg";
    static const int kOrder[4] = { 0, 1, 2, 3 };
    for (int k = 0; k < 4; ++k) {
        int slot = kOrder[k];
        if (!pakOk_[slot]) continue;
        const PakEntry* e = paks_[slot].find(pakPath);
        if (e) {
            r.kind = SRC_PAK;
            r.label = pakSlotName(slot);
            r.pakSlot = slot;
            r.entry = *e;
            r.size = e->length;
            return r;
        }
    }

    // Optional fallbacks for modded / partial installs.
    {
        std::string cands[2];
        cands[0] = JoinPath(looseDir_, lower + ".ogg");
        cands[1] = JoinPath(looseDirAlt_, lower + ".ogg");
        for (int c = 0; c < 2; ++c) {
            if (FileExists(cands[c])) {
                r.kind = SRC_LOOSE_OGG;
                r.label = "loose ogg";
                r.loosePath = cands[c];
                r.size = FileSize(cands[c]);
                return r;
            }
        }
        cands[0] = JoinPath(looseDir_, lower + ".mp3");
        cands[1] = JoinPath(looseDirAlt_, lower + ".mp3");
        for (int c = 0; c < 2; ++c) {
            if (FileExists(cands[c])) {
                r.kind = SRC_LOOSE_MP3;
                r.label = "loose mp3";
                r.loosePath = cands[c];
                r.size = FileSize(cands[c]);
                return r;
            }
        }
    }

    return r;
}

bool Resolver::loadBytes(const ResolvedSource& src, std::vector<unsigned char>& out,
                         std::string& err) const {
    out.clear();
    err.clear();
    switch (src.kind) {
        case SRC_PAK: {
            if (src.pakSlot < 0 || src.pakSlot >= 4 || !pakOk_[src.pakSlot]) {
                err = "pak unavailable";
                return false;
            }
            return paks_[src.pakSlot].read(src.entry, out, err);
        }
        case SRC_LOOSE_WAV:
        case SRC_LOOSE_OGG:
        case SRC_LOOSE_MP3:
            return ReadWholeFile(src.loosePath, out, err);
        default:
            err = "track not found";
            return false;
    }
}
