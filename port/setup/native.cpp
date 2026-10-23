// The setup's own installer, for a release folder (Tenkaichi3Decomp, Tenkaichi3Decomp.dat and Tenkaichi3Decomp-setup together, no build tools):
// reads the user's disc image directly, checks it, unpacks the game data and runs the self-test. No Python, no 7z.
// It reports through the same event lines as install.py (@step, @note, @ok, @skip, @fail, @stopped, @done), so the
// window (setup.cpp) shows both the same way.
//
//   disc image   ISO 9660: the volume descriptor at sector 16, directory records from the root
//   checks       SHA-1 of BIN/DBZP.BIN and of SLUS_216.78 laid out as a flat image from 0x100000 (the values of
//                config/DBZP.yaml and config/SLUS_216.78.yaml)
//   game data    gamedata/disc/<loose files>, and every DATA/*.AFS archive split into gamedata/<name>/00000.bin ...
//                (the layout of port/tools/extract_disc.py)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <SDL3/SDL.h>
#include "native.h"
#include "../src/port_release.h"

namespace {

const char *kRomSha1 = "caee6c2269bba89fc51eea9e7beac3adbec9dc52";
const char *kDbzpSha1 = "4f910969e05d9b25c83af7642b60da9949a4348b";
// Budokai Tenkaichi 4 (TeamBT4's B14 REV2): its program and menu files, whole. Its disc is unpacked as
// research/bt4/extract_bt4.py does: every file, and the archives without a header split by BIN/DBZ4.BIN's records.
const char *kBt4ElfSha1 = "caea50ebcdab5ae80b47e12c6d4bf3247e50784f";
const char *kBt4DbzpSha1 = "de91913f40bd675d70ee748aa16a42a96dff5919";
// Its English version (B14 REV2 ENG): the same layout and data runs, English text and voices.
const char *kBt4EngElfSha1 = "0716ef8ba62e297eb0df5b352c0da1c474134735";
const char *kBt4EngDbzpSha1 = "1d9f8da80c200bb50e7df3210fff30827e5dae9c";
bool sBt4;
const Uint32 kRomBase = 0x100000, kRomEnd = 0x2FF180;
#ifdef _WIN32
const char *kGame = "Tenkaichi3Decomp.exe";
const char *kAsset = "Tenkaichi3Decomp-windows-x64.zip"; // (the release's file for this system)
const char *kCurl = "curl.exe", *kTar = "tar.exe";        // (part of Windows 10 and later)
#else
const char *kGame = "Tenkaichi3Decomp";
const char *kAsset = "Tenkaichi3Decomp-linux-x64.zip";
const char *kCurl = "curl", *kTar = "tar";
#endif

std::mutex sLock;
std::vector<std::string> sLines;
std::thread sThread;
std::atomic<bool> sRunning(false), sCancel(false);

void say(const std::string &line) {
    std::lock_guard<std::mutex> lock(sLock);
    sLines.push_back(line);
}

struct Stop { std::string why; };

// ------------------------------------------------------------------------------------------------------- SHA-1
struct Sha1 {
    Uint32 h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    Uint8 block[64];
    Uint64 total = 0;
    size_t fill = 0;

    static Uint32 rol(Uint32 v, int n) { return (v << n) | (v >> (32 - n)); }
    void compress() {
        Uint32 w[80], a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 16; i++) {
            w[i] = (Uint32)block[i * 4] << 24 | (Uint32)block[i * 4 + 1] << 16 | (Uint32)block[i * 4 + 2] << 8 | block[i * 4 + 3];
        }
        for (int i = 16; i < 80; i++) {
            w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }
        for (int i = 0; i < 80; i++) {
            Uint32 f = i < 20 ? (b & c) | (~b & d) : i < 40 ? b ^ c ^ d : i < 60 ? (b & c) | (b & d) | (c & d) : b ^ c ^ d;
            Uint32 k = i < 20 ? 0x5A827999 : i < 40 ? 0x6ED9EBA1 : i < 60 ? 0x8F1BBCDC : 0xCA62C1D6;
            Uint32 t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    void add(const Uint8 *p, size_t n) {
        total += n;
        while (n > 0) {
            size_t take = 64 - fill < n ? 64 - fill : n;
            memcpy(block + fill, p, take);
            fill += take; p += take; n -= take;
            if (fill == 64) {
                compress();
                fill = 0;
            }
        }
    }
    std::string hex() {
        Uint64 bits = total * 8;
        Uint8 pad = 0x80, zero = 0, len[8];
        char out[41];
        add(&pad, 1);
        while (fill != 56) {
            add(&zero, 1);
        }
        for (int i = 0; i < 8; i++) {
            len[i] = (Uint8)(bits >> (56 - i * 8));
        }
        add(len, 8);
        for (int i = 0; i < 5; i++) {
            snprintf(out + i * 8, 9, "%08x", h[i]);
        }
        return out;
    }
};

// ------------------------------------------------------------------------------------------------- the disc image
struct IsoFile { std::string path; Uint64 offset, size; };

struct Iso {
    SDL_IOStream *io = NULL;
    std::vector<IsoFile> files;

    void read(Uint64 offset, void *buf, size_t n) {
        if (SDL_SeekIO(io, (Sint64)offset, SDL_IO_SEEK_SET) < 0 || SDL_ReadIO(io, buf, n) != n) {
            throw Stop{"The disc image could not be read to its end. It may be incomplete."};
        }
    }
    void walk(Uint32 lba, Uint32 size, const std::string &dir, int depth) {
        std::vector<Uint8> d(size);
        read((Uint64)lba * 2048, d.data(), size);
        for (size_t pos = 0; pos + 34 <= size;) {
            Uint8 len = d[pos];
            if (len == 0) { // records do not cross sectors: the rest of this one is empty
                pos = (pos / 2048 + 1) * 2048;
                continue;
            }
            if (pos + len > size) {
                break;
            }
            Uint32 ext, bytes;
            memcpy(&ext, &d[pos + 2], 4);
            memcpy(&bytes, &d[pos + 10], 4);
            Uint8 flags = d[pos + 25], nameLen = d[pos + 32];
            std::string name((const char *)&d[pos + 33], nameLen);
            pos += len;
            if (nameLen == 1 && (name[0] == 0 || name[0] == 1)) {
                continue; // this directory and its parent
            }
            size_t semi = name.find(';');
            if (semi != std::string::npos) {
                name.resize(semi);
            }
            if (!name.empty() && name.back() == '.') {
                name.pop_back();
            }
            if (flags & 2) {
                if (depth < 8) {
                    walk(ext, bytes, dir + name + "/", depth + 1);
                }
            } else if (!files.empty() && files.back().path == dir + name) {
                files.back().size += bytes; // a file in several extents: they follow each other
            } else {
                files.push_back({dir + name, (Uint64)ext * 2048, bytes});
            }
        }
    }
    void open(const std::string &path) {
        Uint8 pvd[2048];
        Uint32 lba, size;
        io = SDL_IOFromFile(path.c_str(), "rb");
        if (io == NULL) {
            throw Stop{"The disc image could not be opened: " + path};
        }
        if (SDL_SeekIO(io, 16 * 2048, SDL_IO_SEEK_SET) < 0 || SDL_ReadIO(io, pvd, sizeof(pvd)) != sizeof(pvd) || pvd[0] != 1 ||
            memcmp(pvd + 1, "CD001", 5) != 0) {
            throw Stop{"This file is not a disc image (.iso). Compressed images (.chd, .cso, .gz) have to be converted to .iso first."};
        }
        memcpy(&lba, pvd + 156 + 2, 4);
        memcpy(&size, pvd + 156 + 10, 4);
        walk(lba, size, "", 0);
    }
    const IsoFile *find(const char *path) {
        for (auto &f : files) {
            if (SDL_strcasecmp(f.path.c_str(), path) == 0) {
                return &f;
            }
        }
        return NULL;
    }
    ~Iso() {
        if (io != NULL) {
            SDL_CloseIO(io);
        }
    }
};

std::string lower(std::string s) {
    for (auto &c : s) {
        c = (char)tolower((unsigned char)c);
    }
    return s;
}

std::string upper(std::string s) {
    for (auto &c : s) {
        c = (char)toupper((unsigned char)c);
    }
    return s;
}

void check_cancel() {
    if (sCancel) {
        throw Stop{"Cancelled. Install continues from where it stopped."};
    }
}

// Copies `size` bytes of the image at `offset` into a new file.
void copy_out(Iso &iso, Uint64 offset, Uint64 size, const std::string &path, std::vector<Uint8> &buf) {
    SDL_IOStream *out = SDL_IOFromFile(path.c_str(), "wb");
    if (out == NULL) {
        throw Stop{"Could not write " + path + ". Is the disk full or the folder read-only?"};
    }
    while (size > 0) {
        size_t n = size < buf.size() ? (size_t)size : buf.size();
        iso.read(offset, buf.data(), n);
        if (SDL_WriteIO(out, buf.data(), n) != n) {
            SDL_CloseIO(out);
            throw Stop{"Could not write " + path + ". Is the disk full?"};
        }
        offset += n;
        size -= n;
    }
    if (!SDL_CloseIO(out)) {
        throw Stop{"Could not write " + path + ". Is the disk full?"};
    }
}

void step_disc(Iso &iso, const std::string &path) {
    say("@step 1 3 Disc image");
    iso.open(path);
    sBt4 = false;
    if (const IsoFile *bt4 = iso.find("SLUS_219.78")) {
        const IsoFile *menu4 = iso.find("BIN/DBZP.BIN");
        if (menu4 == NULL || iso.find("BIN/DBZ4.BIN") == NULL) {
            throw Stop{"This Budokai Tenkaichi 4 image is incomplete (BIN/DBZP.BIN or BIN/DBZ4.BIN is missing)."};
        }
        say("@note checking");
        std::vector<Uint8> elf4((size_t)bt4->size), menu((size_t)menu4->size);
        iso.read(bt4->offset, elf4.data(), elf4.size());
        iso.read(menu4->offset, menu.data(), menu.size());
        Sha1 a, b;
        a.add(elf4.data(), elf4.size());
        b.add(menu.data(), menu.size());
        std::string ha = a.hex(), hb = b.hex();
        bool eng = ha == kBt4EngElfSha1 && hb == kBt4EngDbzpSha1;
        if (!eng && (ha != kBt4ElfSha1 || hb != kBt4DbzpSha1)) {
            throw Stop{"This is a Budokai Tenkaichi 4 image, but not the version the port supports.\n"
                       "The port needs TeamBT4's B14 REV2 (SLUS_219.78.DBZBT4B14REV2) or its English version."};
        }
        sBt4 = true;
        say(eng ? "@ok Budokai Tenkaichi 4 (TeamBT4 B14 REV2, English), checksums match"
                : "@ok Budokai Tenkaichi 4 (TeamBT4 B14 REV2), checksums match");
        return;
    }
    const IsoFile *slus = iso.find("SLUS_216.78"), *dbzp = iso.find("BIN/DBZP.BIN");
    if (slus == NULL || dbzp == NULL) {
        throw Stop{"This is not a disc image of the USA release of Budokai Tenkaichi 3 (no SLUS_216.78 on it).\n"
                   "Other regions and the Wii version have different programs and are not supported."};
    }
    say("@note checking");
    std::vector<Uint8> elf((size_t)slus->size), rom(kRomEnd - kRomBase, 0), menu((size_t)dbzp->size);
    iso.read(slus->offset, elf.data(), elf.size());
    iso.read(dbzp->offset, menu.data(), menu.size());
    if (elf.size() > 0x34 && memcmp(elf.data(), "\177ELF", 4) == 0) {
        Uint32 shoff;
        Uint16 shentsize, shnum;
        memcpy(&shoff, &elf[0x20], 4);
        memcpy(&shentsize, &elf[0x2E], 2);
        memcpy(&shnum, &elf[0x30], 2);
        for (Uint32 i = 0; i < shnum && (size_t)shoff + (size_t)(i + 1) * shentsize <= elf.size(); i++) {
            Uint32 sh[6]; // name, type, flags, address, offset, size
            memcpy(sh, &elf[shoff + (size_t)i * shentsize], sizeof(sh));
            if ((sh[2] & 2) && sh[1] != 8 && sh[5] != 0 && sh[3] >= kRomBase && sh[3] + sh[5] <= kRomEnd && (size_t)sh[4] + sh[5] <= elf.size()) {
                memcpy(&rom[sh[3] - kRomBase], &elf[sh[4]], sh[5]);
            }
        }
    }
    Sha1 a, b;
    a.add(rom.data(), rom.size());
    b.add(menu.data(), menu.size());
    if (a.hex() != kRomSha1 || b.hex() != kDbzpSha1) {
        throw Stop{"The game's programs on this disc do not have the expected checksums.\n"
                   "The port needs the unmodified USA release (SLUS-21678); a patched or damaged image will not work."};
    }
    say("@ok the USA release, checksums match");
}

void step_data(Iso &iso, const std::string &root) {
    std::string data = root + "gamedata/", marker = data + ".installed";
    SDL_PathInfo info;
    say("@step 2 3 Game data");
    if (SDL_GetPathInfo(marker.c_str(), &info)) {
        say("@skip already unpacked");
        return;
    }
    std::vector<const IsoFile *> take, headerless;
    Uint64 total = 0, done = 0;
    for (auto &f : iso.files) {
        std::string u = upper(f.path);
        if (sBt4 || u == "SLUS_216.78" || u.rfind("BIN/", 0) == 0 || u.rfind("DATA/", 0) == 0) { // (BT4: every file)
            take.push_back(&f);
            total += f.size;
        }
    }
    std::vector<Uint8> buf(1 << 20);
    int files = 0, lastPercent = -1;
    auto progress = [&](Uint64 n) {
        done += n;
        int percent = total ? (int)(done * 100 / total) : 0;
        if (percent != lastPercent) {
            char line[64];
            snprintf(line, sizeof(line), "@note %d%%, %d files", percent, files);
            say(line);
            lastPercent = percent;
        }
        check_cancel();
    };
    for (const IsoFile *f : take) {
        std::string u = upper(f->path);
        bool afs = u.rfind("DATA/", 0) == 0 && u.size() > 4 && u.compare(u.size() - 4, 4, ".AFS") == 0;
        if (!afs) {
            std::string out = data + "disc/" + u; // upper case, as the game asks for them
            SDL_CreateDirectory(out.substr(0, out.rfind('/')).c_str());
            copy_out(iso, f->offset, f->size, out, buf);
            files++;
            progress(f->size);
            continue;
        }
        // an AFS archive: "AFS\0", the number of entries, then (offset, size) of each; one loose file per entry
        std::string stem = lower(u.substr(5, u.size() - 9)), dir = data + stem + "/";
        Uint8 head[8];
        Uint32 count;
        iso.read(f->offset, head, 8);
        if (sBt4 && memcmp(head, "AFS\0", 4) != 0) { // (BT4's have no header: their tables are in BIN/DBZ4.BIN)
            headerless.push_back(f);
            continue;
        }
        SDL_CreateDirectory(dir.c_str());
        memcpy(&count, head + 4, 4);
        if (memcmp(head, "AFS\0", 4) != 0 || (Uint64)count * 8 + 8 > f->size) {
            throw Stop{"An archive on the disc (" + f->path + ") is damaged."};
        }
        std::vector<Uint32> table((size_t)count * 2);
        iso.read(f->offset + 8, table.data(), table.size() * 4);
        Uint64 inArchive = 0;
        for (Uint32 i = 0; i < count; i++) {
            char name[32];
            snprintf(name, sizeof(name), "%05u.bin", i);
            if ((Uint64)table[i * 2] + table[i * 2 + 1] > f->size) {
                throw Stop{"An archive on the disc (" + f->path + ") is damaged."};
            }
            copy_out(iso, f->offset + table[i * 2], table[i * 2 + 1], dir + name, buf);
            files++;
            inArchive += table[i * 2 + 1];
            progress(table[i * 2 + 1]);
        }
        progress(f->size > inArchive ? f->size - inArchive : 0); // the archive's table and padding
    }
    if (!headerless.empty()) {
        // BIN/DBZ4.BIN: one record per archive (16-byte aligned): +4 its size, +8 the file count, +0x10 the archive's
        // name, +0x11A one u16 per file, its size in 2048-byte sectors; the files lie one after another from sector 0
        // and are written at their sector size (the game reads sectors only)
        const IsoFile *t = iso.find("BIN/DBZ4.BIN");
        std::vector<Uint8> tbl((size_t)t->size);
        iso.read(t->offset, tbl.data(), tbl.size());
        for (size_t off = 0; off + 0x11A <= tbl.size();) {
            Uint32 size, count;
            memcpy(&size, &tbl[off + 4], 4);
            memcpy(&count, &tbl[off + 8], 4);
            std::string name;
            for (size_t i = off + 0x10; i < off + 0x11A && tbl[i] != 0; i++) {
                name += (char)tbl[i];
            }
            name = upper(name);
            const IsoFile *a = iso.find(("DATA/" + name).c_str());
            if (a == NULL || size == 0 || off + 0x11A + (size_t)count * 2 > tbl.size() || name.size() < 5) {
                throw Stop{"An archive table on the disc (BIN/DBZ4.BIN) is damaged."};
            }
            std::string dir = data + lower(name.substr(0, name.size() - 4)) + "/";
            Uint64 at = 0;
            SDL_CreateDirectory(dir.c_str());
            for (Uint32 k = 0; k < count; k++) {
                Uint16 sectors;
                char fname[32];
                memcpy(&sectors, &tbl[off + 0x11A + (size_t)k * 2], 2);
                if (at + (Uint64)sectors * 2048 > a->size) {
                    throw Stop{"An archive on the disc (" + a->path + ") is damaged."};
                }
                snprintf(fname, sizeof(fname), "%05u.bin", k);
                copy_out(iso, a->offset + at, (Uint64)sectors * 2048, dir + fname, buf);
                files++;
                at += (Uint64)sectors * 2048;
                progress((Uint64)sectors * 2048);
            }
            progress(a->size > at ? a->size - at : 0);
            off = (off + size + 15) & ~(size_t)15;
        }
    }
    SDL_IOStream *m = SDL_IOFromFile(marker.c_str(), "wb");
    if (m != NULL) {
        SDL_CloseIO(m);
    }
    char line[64];
    snprintf(line, sizeof(line), "@ok %d files in gamedata/", files);
    say(line);
}

void step_test(const std::string &root) {
    std::string exe = root + kGame, saves = root + "gamedata/.selftest";
    const char *args[2] = {exe.c_str(), NULL};
    say("@step 3 3 Self-test");
    if (sBt4) {
        say("@skip Budokai Tenkaichi 4 has no demo fight to test with");
        return;
    }
    say("@note the game's demo fight, no window");
    SDL_Environment *env = SDL_CreateEnvironment(true);
    SDL_SetEnvironmentVariable(env, "BT3_DEMO", "1", true);
    SDL_SetEnvironmentVariable(env, "BT3_SETTINGS", "", true);
    SDL_SetEnvironmentVariable(env, "BT3_SAVES", saves.c_str(), true);
    SDL_SetEnvironmentVariable(env, "BT3_GS", "none", true); // no window: a release program opens one by default
    for (const char *name : {"BT3_REPLAY", "BT3_PAD_PLAY", "BT3_PAD_REC", "BT3_DATA"}) {
        SDL_UnsetEnvironmentVariable(env, name);
    }
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, (void *)args);
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ENVIRONMENT_POINTER, env);
    SDL_SetStringProperty(props, SDL_PROP_PROCESS_CREATE_WORKING_DIRECTORY_STRING, root.c_str());
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
    SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);
    SDL_Process *proc = SDL_CreateProcessWithProperties(props);
    SDL_DestroyProperties(props);
    SDL_DestroyEnvironment(env);
    if (proc == NULL) {
        throw Stop{std::string("The game could not be started: ") + SDL_GetError()};
    }
    size_t size = 0;
    int code = -1;
    char *out = (char *)SDL_ReadProcess(proc, &size, &code);
    std::string text = out != NULL ? std::string(out, size) : "";
    SDL_free(out);
    SDL_DestroyProcess(proc);
    // How the game ended goes into the log too: when it never got as far as printing anything (the system refused
    // to start it, or something stopped it), the exit code is all there is to go on.
    char ended[160];
    const char *why = "";
    switch ((unsigned)code) {
    case 0xC0000135u: why = " (a DLL the program needs was not found)"; break;
    case 0xC0000018u: why = " (the program could not be loaded at its fixed address)"; break;
    case 0xC0000022u: why = " (access denied: often security software blocking the program)"; break;
    case 0xC000007Bu: why = " (a DLL of the wrong kind was loaded)"; break;
    case 0xC0000005u: why = " (crashed: bad memory access)"; break;
    case 0xC0000409u: why = " (stopped by a security check)"; break;
    default: break;
    }
    snprintf(ended, sizeof(ended), "\n[setup] the game exited with code %d (0x%08X)%s\n", code, (unsigned)code, why);
    text += ended;
    {   // a crash report the game wrote itself
        size_t n = 0;
        char *crash = (char *)SDL_LoadFile((root + "bt3_crash.txt").c_str(), &n);
        if (crash != NULL) {
            text += "[setup] bt3_crash.txt:\n" + std::string(crash, n);
            SDL_free(crash);
        }
    }
    SDL_IOStream *log = SDL_IOFromFile((root + "install.log").c_str(), "wb");
    if (log != NULL) {
        SDL_WriteIO(log, text.data(), text.size());
        SDL_CloseIO(log);
    }
    if (text.find("battle finished") == std::string::npos) {
        size_t end = text.find_last_not_of("\r\n"), begin = end == std::string::npos ? std::string::npos : text.rfind('\n', end);
        std::string last = end == std::string::npos ? "no output" : text.substr(begin == std::string::npos ? 0 : begin + 1, end - (begin == std::string::npos ? 0 : begin + 1) + 1);
        throw Stop{"The game's demo fight did not finish. Its last line was:\n" + last + "\nThe full output is in install.log."};
    }
    say("@ok the demo fight ran to its end");
}

void work(std::string root, std::string path) {
    try {
        Iso iso;
        step_disc(iso, path);
        check_cancel();
        step_data(iso, root);
        check_cancel();
        step_test(root);
        say("@done " + root + kGame);
    } catch (const Stop &s) {
        std::string why = s.why;
        for (size_t i = 0; (i = why.find('\n', i)) != std::string::npos;) {
            why.replace(i, 1, "\\n");
        }
        say("@fail");
        say("@stopped " + why);
    }
    sRunning = false;
}

// -------------------------------------------------------------------------------------------------------- update
// Tenkaichi3Decomp-setup --update (the game's "Update now", F1): the newest release of PORT_UPDATE_REPO, when newer
// than this program's PORT_RELEASE, is downloaded and unpacked over this folder with the system's own curl and tar.
// Only the release's files are written: gamedata/, the saves, the settings and what was added by hand stay.

// Runs a program of the system without a console window. `out`: what it printed. `grows`: a file it writes, whose
// size is reported as it grows (of `total` bytes). False: it could not be started.
bool run(const char *const *args, std::string *out, const std::string &grows = "", Uint64 total = 0) {
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, (void *)args);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, out != NULL ? SDL_PROCESS_STDIO_APP : SDL_PROCESS_STDIO_NULL);
    SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true);
    SDL_Process *p = SDL_CreateProcessWithProperties(props);
    SDL_DestroyProperties(props);
    if (p == NULL) {
        return false;
    }
    if (out != NULL) {
        size_t n = 0;
        char *data = (char *)SDL_ReadProcess(p, &n, NULL);
        *out = data != NULL ? std::string(data, n) : "";
        SDL_free(data);
    } else {
        int shown = -1;
        while (!SDL_WaitProcess(p, false, NULL)) {
            SDL_PathInfo info;
            if (!grows.empty() && SDL_GetPathInfo(grows.c_str(), &info) && (int)(info.size >> 20) != shown) {
                char line[64];
                shown = (int)(info.size >> 20);
                snprintf(line, sizeof(line), "@note %d of %d MB", shown, (int)(total >> 20));
                say(line);
            }
            if (sCancel) {
                SDL_KillProcess(p, true);
            }
            SDL_Delay(200);
        }
    }
    SDL_DestroyProcess(p);
    return true;
}

// Deletes a folder this program made, with what is in it.
void remove_tree(const std::string &dir) {
    int count = 0;
    char **list = SDL_GlobDirectory(dir.c_str(), NULL, 0, &count);
    std::vector<std::string> dirs;
    for (int i = 0; list != NULL && i < count; i++) {
        std::string p = dir + list[i];
        SDL_PathInfo info;
        if (SDL_GetPathInfo(p.c_str(), &info) && info.type == SDL_PATHTYPE_DIRECTORY) {
            dirs.push_back(p);
        } else {
            SDL_RemovePath(p.c_str());
        }
    }
    SDL_free(list);
    std::sort(dirs.begin(), dirs.end(), [](const std::string &a, const std::string &b) { return a.size() > b.size(); });
    for (auto &d : dirs) { // (the deepest first)
        SDL_RemovePath(d.c_str());
    }
    SDL_RemovePath(dir.c_str());
}

// Copies every file under `from` (ending in '/') to the same place under `to`. A file in use (this program's own and
// its SDL3.dll; the game, still closing) is moved aside to <name>.old, which Windows allows, and the new one written
// in its place, tried for ten seconds; failing that the old one is put back, so the folder always keeps a setup that
// can try again. fonts/fonts.tsv is never replaced once there (the user lists fonts of their own in it).
void copy_tree(const std::string &from, const std::string &to) {
    int count = 0;
    char **list = SDL_GlobDirectory(from.c_str(), NULL, 0, &count);
    if (list == NULL) {
        throw Stop{"The downloaded release could not be read."};
    }
    for (int i = 0; i < count; i++) {
        std::string rel = list[i];
        std::replace(rel.begin(), rel.end(), '\\', '/'); // (Windows names them with its own separator)
        std::string src = from + rel, dst = to + rel, old = dst + ".old";
        size_t slash = rel.rfind('/');
        SDL_PathInfo info;
        bool ok, moved = false;
        if (!SDL_GetPathInfo(src.c_str(), &info) || info.type == SDL_PATHTYPE_DIRECTORY ||
            (rel == "fonts/fonts.tsv" && SDL_GetPathInfo(dst.c_str(), &info))) {
            continue;
        }
        if (slash != std::string::npos) {
            SDL_CreateDirectory((to + rel.substr(0, slash)).c_str());
        }
        ok = SDL_CopyFile(src.c_str(), dst.c_str());
        for (int t = 0; !ok && t < 40; t++) {
            if (!moved) {
                SDL_RemovePath(old.c_str()); // (a previous update's, no longer in use)
                moved = SDL_RenamePath(dst.c_str(), old.c_str());
            }
            if (!moved || t > 0) {
                SDL_Delay(250);
            }
            ok = SDL_CopyFile(src.c_str(), dst.c_str());
        }
        if (!ok) {
            std::string why = SDL_GetError();
            if (moved) {
                SDL_RenamePath(old.c_str(), dst.c_str());
            }
            SDL_free(list);
            throw Stop{"Could not write " + dst + " (" + why + ").\nClose the game and press Update again."};
        }
        if (moved) {
            SDL_RemovePath(old.c_str()); // (still in use: it goes with the next update)
        }
    }
    SDL_free(list);
}

void work_update(std::string root) {
    try {
        std::string api = std::string("https://api.github.com/repos/") + PORT_UPDATE_REPO + "/releases/latest", json, url;
        std::string zip = root + "update.zip", dir = root + ".update/";
        const char *ask[] = {kCurl, "-s", "-L", "-m", "20", "-A", "Tenkaichi3Decomp-setup", api.c_str(), NULL};
        int a = 0, b = -1, c = -1, latest;
        int mine = getenv("BT3_UPDATE_FROM") != NULL ? atoi(getenv("BT3_UPDATE_FROM")) : PORT_RELEASE; // (testing: an older one)
        Uint64 size = 0;
        size_t at;
        char line[160];

        say("@step 1 3 Newest release");
        if (PORT_UPDATE_REPO[0] == '\0') {
            throw Stop{"This copy of the game names no place it is published, so it cannot update itself."};
        }
        bool asked = run(ask, &json);
        at = json.find("\"tag_name\"");
        at = at == std::string::npos ? at : json.find('v', at + 10);
        if (!asked || at == std::string::npos || sscanf(json.c_str() + at, "v%d.%d.%d", &a, &b, &c) != 3 || a != 0 || b < 0 || c < 0 ||
            c >= 100) {
            throw Stop{"GitHub could not be asked for the newest release.\nIs this computer connected to the internet?"};
        }
        latest = b * 100 + c; // (as PORT_RELEASE counts: 0.2.1 is 201)
        if (latest <= mine) {
            snprintf(line, sizeof(line), "@ok this is the newest release, 0.%d.%d", mine / 100, mine % 100);
            say(line);
            say("@step 2 3 Download");
            say("@skip nothing to download");
            say("@step 3 3 Install");
            say("@skip nothing to install");
            say("@done " + root + kGame);
            sRunning = false;
            return;
        }
        for (size_t pos = 0; (pos = json.find("\"browser_download_url\"", pos)) != std::string::npos; pos += 22) {
            size_t q1 = json.find('"', pos + 22), q2 = q1 == std::string::npos ? q1 : json.find('"', q1 + 1);
            std::string v = q2 == std::string::npos ? "" : json.substr(q1 + 1, q2 - q1 - 1);
            if (v.size() > strlen(kAsset) && v.compare(v.size() - strlen(kAsset), strlen(kAsset), kAsset) == 0) {
                size_t s = json.rfind("\"size\"", pos); // (the asset's own fields come before its address)
                url = v;
                size = s != std::string::npos ? strtoull(json.c_str() + json.find(':', s) + 1, NULL, 10) : 0;
                break;
            }
        }
        if (url.empty()) {
            snprintf(line, sizeof(line), "Release 0.%d.%d has no download for this system (%s).", b, c, kAsset);
            throw Stop{line};
        }
        snprintf(line, sizeof(line), "@ok 0.%d.%d (this is 0.%d.%d)", b, c, mine / 100, mine % 100);
        say(line);
        check_cancel();

        say("@step 2 3 Download");
        SDL_RemovePath(zip.c_str());
        {
            const char *get[] = {kCurl, "-s", "-L", "--fail", "-A", "Tenkaichi3Decomp-setup", "-o", zip.c_str(), url.c_str(), NULL};
            SDL_PathInfo info;
            if (!run(get, NULL, zip, size) || !SDL_GetPathInfo(zip.c_str(), &info) || info.size == 0 || (size != 0 && info.size != size)) {
                check_cancel();
                throw Stop{"The download did not finish. Press Update to try again."};
            }
        }
        say("@ok downloaded");
        check_cancel();

        say("@step 3 3 Install");
        remove_tree(dir);
        SDL_CreateDirectory(dir.c_str());
        {
            const char *unpack[] = {kTar, "-xf", zip.c_str(), "-C", dir.c_str(), NULL};
            SDL_PathInfo info;
            if (!run(unpack, NULL) || !SDL_GetPathInfo((dir + "Tenkaichi3Decomp/" + kGame).c_str(), &info)) {
                throw Stop{"The downloaded release could not be unpacked."};
            }
        }
        copy_tree(dir + "Tenkaichi3Decomp/", root);
        remove_tree(dir);
        SDL_RemovePath(zip.c_str());
        snprintf(line, sizeof(line), "@ok updated to 0.%d.%d", b, c);
        say(line);
        say("@done " + root + kGame);
    } catch (const Stop &s) {
        std::string why = s.why;
        for (size_t i = 0; (i = why.find('\n', i)) != std::string::npos;) {
            why.replace(i, 1, "\\n");
        }
        say("@fail");
        say("@stopped " + why);
    }
    sRunning = false;
}

} // namespace

void Native_StartUpdate(const std::string &root) {
    if (sThread.joinable()) {
        sThread.join();
    }
    sCancel = false;
    sRunning = true;
    sThread = std::thread(work_update, root);
}

void Native_Start(const std::string &root, const std::string &iso) {
    if (sThread.joinable()) {
        sThread.join();
    }
    sCancel = false;
    sRunning = true;
    sThread = std::thread(work, root, iso);
}

bool Native_Poll(std::string &line) {
    std::lock_guard<std::mutex> lock(sLock);
    if (sLines.empty()) {
        return false;
    }
    line = sLines.front();
    sLines.erase(sLines.begin());
    return true;
}

bool Native_Running() { return sRunning; }

void Native_Cancel() { sCancel = true; }

void Native_Join() {
    sCancel = true;
    if (sThread.joinable()) {
        sThread.join();
    }
}

bool Native_Installed(const std::string &root) {
    SDL_PathInfo info;
    return SDL_GetPathInfo((root + "gamedata/.installed").c_str(), &info);
}
