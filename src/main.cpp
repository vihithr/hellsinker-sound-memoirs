// main.cpp - CLI entry points (--list, --selftest, --play, --bench) and the GUI.
#include "common.h"
#include "resolver.h"
#include "decoder.h"
#include "ui.h"
#include <shellapi.h>

namespace {

const char* kVersion = "hsbgm 1.1";

std::string IniPath(const std::string& exeDir) { return JoinPath(exeDir, "hsbgm.ini"); }

std::string GetIniString(const std::string& ini, const char* key, const char* def) {
    char buf[2048];
    buf[0] = 0;
    GetPrivateProfileStringA("hsbgm", key, def ? def : "", buf, sizeof(buf), ini.c_str());
    return Trim(std::string(buf));
}

int GetIniInt(const std::string& ini, const char* key, int def) {
    std::string s = GetIniString(ini, key, "");
    if (s.empty()) return def;
    return atoi(s.c_str());
}

void PutIniString(const std::string& ini, const char* key, const std::string& value) {
    WritePrivateProfileStringA("hsbgm", key, value.c_str(), ini.c_str());
}

void PutIniInt(const std::string& ini, const char* key, int value) {
    PutIniString(ini, key, IntToStr(value));
}

bool LooksLikeGameDir(const std::string& dir) {
    if (dir.empty() || !DirExists(dir)) return false;
    std::string data = JoinPath(dir, "backboneteller");
    if (!DirExists(data)) return false;
    if (FileExists(JoinPath(data, "bbt.pak"))) return true;
    if (FileExists(JoinPath(data, "bbt2.pak"))) return true;
    if (FileExists(JoinPath(data, "bbt3.pak"))) return true;
    if (DirExists(JoinPath(data, "bbt"))) return true;
    return false;
}

std::string FindGameDir(const std::string& exeDir, const std::string& ini, const std::string& fromArg) {
    if (!fromArg.empty()) {
        if (LooksLikeGameDir(fromArg)) return fromArg;
        return std::string();
    }
    std::string cfg = GetIniString(ini, "GameDir", "");
    if (!cfg.empty() && LooksLikeGameDir(cfg)) return cfg;

    const char* candidates[] = {
        "F:\\Games\\Hellsinker",
        "D:\\Program Files (x86)\\Steam\\steamapps\\common\\Hellsinker",
        "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Hellsinker",
        "D:\\Steam\\steamapps\\common\\Hellsinker",
        "C:\\Steam\\steamapps\\common\\Hellsinker",
        "D:\\Games\\Hellsinker",
        "C:\\Games\\Hellsinker"
    };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        if (LooksLikeGameDir(candidates[i])) return std::string(candidates[i]);
    }
    // Last resort: a "Hellsinker" sibling of the exe's parent folder.
    std::string parent = exeDir;
    for (int up = 0; up < 3; ++up) {
        size_t pos = parent.find_last_of("\\/");
        if (pos == std::string::npos) break;
        parent = parent.substr(0, pos);
        std::string cand = JoinPath(parent, "Hellsinker");
        if (LooksLikeGameDir(cand)) return cand;
    }
    return std::string();
}

unsigned int Fnv1a(const unsigned char* data, size_t len) {
    unsigned int h = 2166136261u;
    for (size_t i = 0; i < len; ++i) {
        h ^= (unsigned int)data[i];
        h *= 16777619u;
    }
    return h;
}

int RunList(Resolver& res) {
    printf("%s --list\n", kVersion);
    printf("game dir : %s\n", res.gameDir().c_str());
    printf("loose dir: %s\n", res.looseDir().c_str());
    for (int i = 0; i < res.pakCount(); ++i) {
        const PakFile& p = res.pak(i);
        if (p.isOpen()) {
            printf("pak      : %-9s %3u entries  %s\n", Resolver::pakSlotName(i), (unsigned)p.count(),
                   FormatBytes(FileSize(p.path())).c_str());
        } else if (FileExists(JoinPath(res.gameDir() + "\\backboneteller",
                                       Resolver::pakSlotName(i)))) {
            printf("pak      : %-9s PRESENT BUT UNREADABLE\n", Resolver::pakSlotName(i));
        } else {
            printf("pak      : %-9s absent\n", Resolver::pakSlotName(i));
        }
    }
    printf("\n  #  track               source        size\n");
    int counts[8];
    for (int i = 0; i < 8; ++i) counts[i] = 0;
    int resolved = 0;
    for (int i = 0; i < kTrackCount; ++i) {
        ResolvedSource src = res.resolve(kTrackNames[i]);
        std::string srcLabel = src.ok() ? src.label : "MISSING";
        std::string sizeStr = src.ok() ? FormatBytes(src.size) : "-";
        printf("%3d  %-19s %-13s %s\n", i, kTrackNames[i], srcLabel.c_str(), sizeStr.c_str());
        if (src.ok()) {
            ++resolved;
            if (src.kind == SRC_PAK) {
                if (src.pakSlot == 0) ++counts[0];
                else if (src.pakSlot == 1) ++counts[1];
                else if (src.pakSlot == 2) ++counts[2];
                else if (src.pakSlot == 3) ++counts[3];
            } else if (src.kind == SRC_LOOSE_WAV) {
                ++counts[4];
            } else if (src.kind == SRC_LOOSE_OGG) {
                ++counts[5];
            } else if (src.kind == SRC_LOOSE_MP3) {
                ++counts[6];
            }
        } else {
            ++counts[7];
        }
    }
    printf("\nresolved: %d/%d tracks\n", resolved, kTrackCount);
    printf("per pak : bbll.pak=%d  bbt3.pak=%d  bbt2.pak=%d  bbt.pak=%d  "
           "loose wav=%d  loose ogg=%d  loose mp3=%d  missing=%d\n",
           counts[0], counts[1], counts[2], counts[3], counts[4], counts[5], counts[6], counts[7]);
    return 0;
}

int RunSelfTest(Resolver& res) {
    printf("%s --selftest\n", kVersion);
    printf("game dir: %s\n", res.gameDir().c_str());
    bool anyPak = false;
    for (int i = 0; i < res.pakCount(); ++i) {
        const PakFile& p = res.pak(i);
        if (p.isOpen()) {
            anyPak = true;
            printf("[pak] %-9s entries=%u size=%u bytes OK\n", Resolver::pakSlotName(i),
                   (unsigned)p.count(), FileSize(p.path()));
        } else {
            printf("[pak] %-9s not present\n", Resolver::pakSlotName(i));
        }
    }
    if (!anyPak) {
        printf("selftest: FAIL (no readable pak)\n");
        return 1;
    }

    int resolved = 0;
    int missing = 0;
    for (int i = 0; i < kTrackCount; ++i) {
        ResolvedSource src = res.resolve(kTrackNames[i]);
        if (src.ok()) {
            ++resolved;
        } else {
            ++missing;
            printf("[track] MISSING %02d %s\n", i, kTrackNames[i]);
        }
    }
    printf("[tracks] resolved=%d/%d missing=%d\n", resolved, kTrackCount, missing);

    int failures = 0;
    for (int i = 0; i < 3; ++i) {
        ResolvedSource src = res.resolve(kTrackNames[i]);
        if (!src.ok()) {
            printf("[decode] track %02d %-16s FAIL: not resolved\n", i, kTrackNames[i]);
            ++failures;
            continue;
        }
        std::vector<unsigned char> bytes;
        std::string err;
        if (!res.loadBytes(src, bytes, err)) {
            printf("[decode] track %02d %-16s FAIL: %s\n", i, kTrackNames[i], err.c_str());
            ++failures;
            continue;
        }
        PcmBuffer pcm;
        bool ok;
        if (src.kind == SRC_LOOSE_WAV) {
            ok = DecodeWav(bytes.empty() ? NULL : &bytes[0], (unsigned long)bytes.size(),
                           pcm, err, 44100 * 2);
        } else if (src.kind == SRC_LOOSE_MP3) {
            printf("[decode] track %02d %-16s SKIP (mp3 fallback, MCI only)\n", i, kTrackNames[i]);
            ++failures;
            continue;
        } else {
            ok = DecodeOgg(bytes.empty() ? NULL : &bytes[0], (unsigned long)bytes.size(),
                           pcm, err, 44100 * 2);
        }
        if (!ok) {
            printf("[decode] track %02d %-16s FAIL: %s\n", i, kTrackNames[i], err.c_str());
            ++failures;
            continue;
        }
        unsigned int sum = Fnv1a((const unsigned char*)&pcm.samples[0], pcm.samples.size() * 2);
        printf("[decode] track %02d %-16s src=%-9s payload=%u bytes pcm=%u bytes "
               "(%d samples, %.2f s) checksum=0x%08X\n",
               i, kTrackNames[i], src.label.c_str(), (unsigned)bytes.size(),
               (unsigned)(pcm.samples.size() * 2), (int)pcm.samples.size(),
               pcm.samples.size() / 2.0 / 44100.0, sum);
        if (pcm.samples.empty()) {
            printf("[decode] track %02d FAIL: zero samples\n", i);
            ++failures;
        }
    }

    printf("selftest: %s\n", failures == 0 && missing == 0 ? "PASS" : "FAIL");
    return (failures == 0 && missing == 0) ? 0 : 1;
}

int RunBench(Resolver& res, const std::string& name) {
    int idx = -1;
    for (int i = 0; i < kTrackCount; ++i) {
        if (ToLowerAscii(kTrackNames[i]) == ToLowerAscii(name)) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        printf("unknown track: %s\n", name.c_str());
        return 1;
    }
    ResolvedSource src = res.resolve(kTrackNames[idx]);
    if (!src.ok()) {
        printf("track not resolved: %s\n", name.c_str());
        return 1;
    }
    std::vector<unsigned char> bytes;
    std::string err;
    if (!res.loadBytes(src, bytes, err)) {
        printf("load failed: %s\n", err.c_str());
        return 1;
    }
    DWORD t0 = GetTickCount();
    PcmBuffer pcm;
    bool ok;
    if (src.kind == SRC_LOOSE_WAV) {
        ok = DecodeWav(bytes.empty() ? NULL : &bytes[0], (unsigned long)bytes.size(), pcm, err, 0);
    } else {
        ok = DecodeOgg(bytes.empty() ? NULL : &bytes[0], (unsigned long)bytes.size(), pcm, err, 0);
    }
    DWORD dt = GetTickCount() - t0;
    if (!ok) {
        printf("decode failed: %s\n", err.c_str());
        return 1;
    }
    printf("%s: src=%s payload=%u bytes pcm=%u bytes (%.2f s) decode=%u ms\n",
           name.c_str(), src.label.c_str(), (unsigned)bytes.size(),
           (unsigned)(pcm.samples.size() * 2), pcm.seconds(), (unsigned)dt);
    return 0;
}

int RunAudioTest(Resolver& res, const std::string& name, int seconds) {
    int idx = -1;
    for (int i = 0; i < kTrackCount; ++i) {
        if (ToLowerAscii(kTrackNames[i]) == ToLowerAscii(name)) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        printf("unknown track: %s\n", name.c_str());
        return 1;
    }
    ResolvedSource src = res.resolve(kTrackNames[idx]);
    if (!src.ok()) {
        printf("track not resolved: %s\n", name.c_str());
        return 1;
    }
    std::vector<unsigned char> bytes;
    std::string err;
    if (!res.loadBytes(src, bytes, err)) {
        printf("load failed: %s\n", err.c_str());
        return 1;
    }
    PcmBuffer pcm;
    if (!DecodeOgg(bytes.empty() ? NULL : &bytes[0], (unsigned long)bytes.size(), pcm, err, 0)) {
        printf("decode failed: %s\n", err.c_str());
        return 1;
    }
    printf("decoded %s: %.2f s, %u bytes\n", kTrackNames[idx], pcm.seconds(),
           (unsigned)pcm.samples.size() * 2);
    AudioEngine audio;
    if (!audio.init(err)) {
        printf("audio init failed: %s\n", err.c_str());
        return 1;
    }
    audio.setVolume(50);
    if (!audio.playPcm(pcm, err)) {
        printf("play failed: %s\n", err.c_str());
        audio.shutdown();
        return 1;
    }
    printf("format 44100/16/2, duration=%.2f s\n", audio.durationSeconds());
    int loops = 0;
    double lastPos = 0.0;
    for (int i = 0; i < seconds * 5; ++i) {
        Sleep(200);
        double pos = audio.positionSeconds();
        if (pos + 0.5 < lastPos) {
            ++loops;
            printf("  [loop detected at t=%.1fs, pos reset to %.2f]\n", i * 0.2, pos);
        }
        lastPos = pos;
        if (i % 5 == 0) printf("  t=%5.1fs  playing=%d  pos=%6.2f / %6.2f\n", i * 0.2,
                               (int)audio.isPlaying(), pos, audio.durationSeconds());
        fflush(stdout);
    }
    audio.stop();
    audio.shutdown();
    printf("audiotest: %s (%d loops observed)\n", loops > 0 ? "PASS" : "FAIL", loops);
    return loops > 0 ? 0 : 1;
}

void PrintUsage() {
    printf("%s - HellSinker. SOUND MEMOIRS jukebox\n\n", kVersion);
    printf("usage: hsbgm [options]\n");
    printf("  (no options)        open the SOUND MEMOIRS window\n");
    printf("  --list              list all 46 tracks with resolved pak/size, exit\n");
    printf("  --selftest          headless pak/resolve/decode check, exit code 0 on pass\n");
    printf("  --play <name>       open the window and start the named track\n");
    printf("  --file <path>       open and loop a local music file (also Ctrl+O or drop)\n");
    printf("  --gamedir <path>    override the HellSinker. install folder\n");
    printf("  --bench <name>      decode one track and report timing\n");
    printf("  --help              this text\n\n");
    printf("controls: UP/DOWN/LEFT/RIGHT select, ENTER/SPACE loopplay,\n");
    printf("          S/ESC stop, +/- volume, CTRL+Q quit (ALT+F4 also works)\n");
}

} // namespace

int main(int argc, char** argv) {
    const std::string exeDir = ExeDir();
    const std::string ini = IniPath(exeDir);

    if (getenv("HSBGM_DEBUG") != NULL) {
        DbgEnable(true);
        remove(JoinPath(exeDir, "hsbgm_debug.log").c_str());
        DbgLog("hsbgm starting, exeDir=%s", exeDir.c_str());
    }

    bool wantList = false;
    bool wantSelfTest = false;
    bool havePlay = false;
    bool haveBench = false;
    bool haveAudioTest = false;
    int audioTestSeconds = 15;
    std::wstring localFile;
    std::string playName;
    std::string benchName;
    std::string gameDirArg;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--list") {
            wantList = true;
        } else if (a == "--selftest") {
            wantSelfTest = true;
        } else if (a == "--play") {
            if (i + 1 >= argc) {
                printf("--play needs a track name\n");
                return 2;
            }
            havePlay = true;
            playName = argv[++i];
        } else if (a == "--file") {
            if (++i >= argc) { printf("--file needs a local music path\n"); return 2; }
            int count = 0;
            LPWSTR* wideArgs = CommandLineToArgvW(GetCommandLineW(), &count);
            if (wideArgs && i < count) localFile = wideArgs[i];
            LocalFree(wideArgs);
            if (localFile.empty()) return 2;
        } else if (a == "--bench") {
            if (i + 1 >= argc) {
                printf("--bench needs a track name\n");
                return 2;
            }
            haveBench = true;
            benchName = argv[++i];
        } else if (a == "--audiotest") {
            if (i + 1 >= argc) {
                printf("--audiotest needs a track name\n");
                return 2;
            }
            haveAudioTest = true;
            benchName = argv[++i];
            if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9') {
                audioTestSeconds = atoi(argv[++i]);
            }
        } else if (a == "--gamedir") {
            if (i + 1 >= argc) {
                printf("--gamedir needs a path\n");
                return 2;
            }
            gameDirArg = argv[++i];
        } else if (a == "--help" || a == "-h" || a == "/?") {
            PrintUsage();
            return 0;
        } else {
            printf("unknown option: %s\n", a.c_str());
            PrintUsage();
            return 2;
        }
    }

    std::string gameDir = FindGameDir(exeDir, ini, gameDirArg);
    const bool needsGame = wantList || wantSelfTest || haveBench || haveAudioTest || havePlay || !gameDirArg.empty();
    if (gameDir.empty() && needsGame) {
        const char* msg =
            "Could not find a HellSinker. installation.\n\n"
            "Expected a folder that contains 'backboneteller\\bbt.pak'.\n"
            "Set GameDir=<path> in hsbgm.ini (next to hsbgm.exe),\n"
            "or pass --gamedir <path>.";
        if (wantList || wantSelfTest || haveBench || haveAudioTest) {
            printf("%s\n", msg);
            return 1;
        }
        MessageBoxA(NULL, msg, "SOUND MEMOIRS", MB_ICONERROR | MB_OK);
        return 1;
    }

    Resolver resolver;
    std::string err;
    if (!gameDir.empty() && !resolver.init(gameDir, err) && needsGame) {
        if (wantList || wantSelfTest || haveBench || haveAudioTest) {
            printf("cannot open game data: %s\n", err.c_str());
            return 1;
        }
        MessageBoxA(NULL, err.c_str(), "SOUND MEMOIRS", MB_ICONERROR | MB_OK);
        return 1;
    }

    if (wantList) return RunList(resolver);
    if (wantSelfTest) return RunSelfTest(resolver);
    if (haveAudioTest) return RunAudioTest(resolver, benchName, audioTestSeconds);
    if (haveBench) return RunBench(resolver, benchName);

    PlayerSettings settings;
    settings.initialFile = localFile;
    settings.volume = GetIniInt(ini, "Volume", 80);
    if (settings.volume < 0) settings.volume = 0;
    if (settings.volume > 100) settings.volume = 100;
    settings.theme = GetIniInt(ini, "Theme", 0);
    if (settings.theme < 0 || settings.theme > 3) settings.theme = 0;
    settings.libFont = GetIniInt(ini, "LibFont", 1);
    if (settings.libFont < 0 || settings.libFont > 1) settings.libFont = 1;
    settings.scaleMode = GetIniInt(ini, "ScaleMode", 1);
    if (settings.scaleMode < 0 || settings.scaleMode > 1) settings.scaleMode = 1;
    settings.initialTrack = GetIniInt(ini, "LastTrack", 0);
    if (settings.initialTrack < 0 || settings.initialTrack >= kTrackCount) settings.initialTrack = 0;
    if (havePlay) {
        int idx = -1;
        for (int i = 0; i < kTrackCount; ++i) {
            if (ToLowerAscii(kTrackNames[i]) == ToLowerAscii(playName)) {
                idx = i;
                break;
            }
        }
        if (idx < 0) {
            printf("unknown track: %s\n", playName.c_str());
            return 2;
        }
        settings.initialTrack = idx;
        settings.autoPlay = true;
    }

    // GUI mode: drop the console window (we are a console-subsystem binary so
    // --list/--selftest can print; interactive mode should not keep a console
    // around, especially when double-clicked from Explorer).
    FreeConsole();

    int rc = RunPlayerWindow(resolver, settings, err);
    if (rc != 0) {
        MessageBoxA(NULL, err.c_str(), "SOUND MEMOIRS", MB_ICONERROR | MB_OK);
        return rc;
    }

    if (!gameDir.empty()) PutIniString(ini, "GameDir", gameDir);
    PutIniInt(ini, "Volume", settings.volume);
    PutIniInt(ini, "Theme", settings.theme);
    PutIniInt(ini, "LibFont", settings.libFont);
    PutIniInt(ini, "ScaleMode", settings.scaleMode);
    PutIniInt(ini, "LastTrack", settings.initialTrack);
    return 0;
}
