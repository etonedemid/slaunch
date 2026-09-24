#include <switch.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <SDL2/SDL_image.h>
#include <curl/curl.h>
#include <minizip/unzip.h>
#include <dirent.h>
#include <sys/stat.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <functional>
#include <vector>
#include <cmath>
#include <unistd.h>

static const int W = 1280, H = 720;

static SDL_Renderer *g_ren = nullptr;
static TTF_Font *g_fL = nullptr, *g_fM = nullptr, *g_fS = nullptr;

struct Col { Uint8 r, g, b; };
// AMOLED palette
static const Col kBlack{0, 0, 0}, kBg{5, 5, 8}, kBg2{10, 10, 14},
                 kFg{240, 242, 248}, kDim{40, 42, 50},
                 kAccent{255, 255, 255}, kGreen{100, 220, 140}, kRed{235, 80, 80};

static void FillRect(int x, int y, int w, int h, Col c, Uint8 a = 255) {
    SDL_SetRenderDrawColor(g_ren, c.r, c.g, c.b, a);
    SDL_Rect r{x, y, w, h};
    SDL_RenderFillRect(g_ren, &r);
}

static void Text(TTF_Font *f, int x, int y, Col c, const char *s, bool center = false) {
    if (!f || !s || !s[0]) return;
    SDL_Surface *surf = TTF_RenderUTF8_Blended(f, s, SDL_Color{c.r, c.g, c.b, 255});
    if (!surf) return;
    SDL_Texture *tex = SDL_CreateTextureFromSurface(g_ren, surf);
    SDL_Rect dst{center ? x - surf->w / 2 : x, y, surf->w, surf->h};
    SDL_FreeSurface(surf);
    if (tex) { SDL_RenderCopy(g_ren, tex, nullptr, &dst); SDL_DestroyTexture(tex); }
}

// ---- recursive copy / delete ---------------------------------------------
static int g_total = 0, g_done = 0;

static void CountTree(const std::string &src) {
    DIR *d = opendir(src.c_str());
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        std::string s = src + "/" + e->d_name;
        struct stat st;
        if (stat(s.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) CountTree(s);
        else g_total++;
    }
    closedir(d);
}

static void Mkdirs(const std::string &path) {
    for (size_t i = 1; i < path.size(); i++)
        if (path[i] == '/') mkdir(path.substr(0, i).c_str(), 0777);
    mkdir(path.c_str(), 0777);
}

// User data we don't want an install/update to clobber: settings, added
// wallpapers, added music, and the (regenerable) icon cache.
static bool IsUserData(const std::string &dst) {
    static const char *dirs[] = { "/slaunch/config/", "/slaunch/themes/",
                                  "/slaunch/music/",  "/slaunch/cache/" };
    for (auto d : dirs) if (dst.find(d) != std::string::npos) return true;
    return false;
}

static bool CopyFile(const std::string &src, const std::string &dst) {
    FILE *in = fopen(src.c_str(), "rb");
    if (!in) return false;
    FILE *out = fopen(dst.c_str(), "wb");
    if (!out) { fclose(in); return false; }
    char buf[65536];
    size_t n;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
        if (fwrite(buf, 1, n, out) != n) { ok = false; break; }
    fclose(in);
    fclose(out);
    return ok;
}

// ---------------------------------------------------------------------------
// Boot splash
//
// package3 (magic "PK31", exactly 8 MiB) holds a second header at the offset
// stored in its own byte 4: "FSS0" magic, then a table of (offset, size,
// type, name) content entries - this is what hekate's own pkg3 loader reads
// (bootloader/hos/pkg3.c upstream). It defines a CNT_TYPE_BMP content type
// for exactly this purpose but never processes it - that code path only ever
// existed in fusee-primary, which Atmosphere removed in 1.10.0. So a splash
// needs two things, neither of which existed here before: a patched hekate
// that actually draws CNT_TYPE_BMP (scripts/hekate-pkg3-splash.patch, applied
// to a matching hekate source checkout and reflashed as payload.bin /
// bootloader/update.bin - this installer can't do that part, it only owns
// package3) and a TOC entry pointing at the pixel data, which is what this
// writes.
//
// Bytes 0x400000..0x7C0000 are free in every content layout observed so far
// (nothing else claims that range; the entry named "fusee" always starts
// exactly at 0x7C0000), so the pixel data goes there, same spot the old
// fusee-primary format used. Free doesn't mean guaranteed, though, so this
// still checks for a collision before writing anything.
//
// The stock package3 is copied out first. Without that there is no way back:
// the original only exists inside the package3 being overwritten, and a later
// Atmosphere update is the only thing that would restore it.
static constexpr const char *kPackage3     = "sdmc:/atmosphere/package3";
static constexpr const char *kPackage3Back = "sdmc:/slaunch/backup/package3.stock";
static constexpr size_t kPackage3Size    = 0x800000;
static constexpr u32    kPkg3Fss0Magic   = 0x30535346; // "FSS0"
static constexpr u32    kCntTypeBmp      = 7;
static constexpr size_t kSplashOffset    = 0x400000;
static constexpr size_t kSplashSize      = 720 * 1280 * 4;
static constexpr size_t kMetaOffsetAddr  = 0x4;  // where the FSS0 header's own offset is stored

#pragma pack(push, 1)
struct Pkg3Meta {
    u32 magic, size, crt0_off, cnt_off, cnt_count, hos_ver, version, git_rev;
};
struct Pkg3Content {
    u32 offset, size;
    u8  type, flags0, flags1, flags2;
    u32 rsvd1;
    char name[0x10];
};
#pragma pack(pop)
static_assert(sizeof(Pkg3Meta) == 0x20);
static_assert(sizeof(Pkg3Content) == 0x20);

static bool BackupPackage3() {
    struct stat st;
    if (stat(kPackage3Back, &st) == 0 && (size_t)st.st_size == kPackage3Size)
        return true;            // already have a good one - never overwrite it
    Mkdirs("sdmc:/slaunch/backup");
    return CopyFile(kPackage3, kPackage3Back);
}

// Returns false and changes nothing unless package3 is exactly what we expect
// and there's genuinely room to add (or update) a splash entry.
static bool InstallSplash() {
    struct stat st;
    if (stat(kPackage3, &st) != 0 || (size_t)st.st_size != kPackage3Size) return false;

    std::vector<char> pkg3(kPackage3Size);
    FILE *f = fopen(kPackage3, "rb");
    if (!f || fread(pkg3.data(), 1, kPackage3Size, f) != kPackage3Size) {
        if (f) fclose(f);
        return false;
    }
    fclose(f);

    if (memcmp(pkg3.data(), "PK31", 4) != 0) return false;

    const u32 metaOff = *(u32 *)(pkg3.data() + kMetaOffsetAddr);
    if ((size_t)metaOff + sizeof(Pkg3Meta) > kPackage3Size) return false;
    auto *meta = (Pkg3Meta *)(pkg3.data() + metaOff);
    if (meta->magic != kPkg3Fss0Magic) return false;

    const size_t cntBase = (size_t)metaOff + meta->cnt_off;
    if (cntBase + (size_t)meta->cnt_count * sizeof(Pkg3Content) > kPackage3Size) return false;
    auto *entries = (Pkg3Content *)(pkg3.data() + cntBase);

    FILE *blob = fopen("romfs:/splash.bin", "rb");
    if (!blob) return false;
    std::vector<char> pixels(kSplashSize);
    const size_t got = fread(pixels.data(), 1, kSplashSize, blob);
    fclose(blob);
    if (got != kSplashSize) return false;

    // Re-running Install/Re-install should update the existing entry in
    // place rather than pile up duplicates.
    Pkg3Content *bmp = nullptr;
    u32 minContentOff = (u32)kPackage3Size;
    for (u32 i = 0; i < meta->cnt_count; i++) {
        if (entries[i].type == kCntTypeBmp) bmp = &entries[i];
        else if (entries[i].offset < minContentOff) minContentOff = entries[i].offset;
    }

    u32 targetOff;
    if (bmp && bmp->size == kSplashSize) {
        targetOff = bmp->offset;
    } else {
        targetOff = (u32)kSplashOffset;
        // Free window must not collide with any *other* declared content.
        for (u32 i = 0; i < meta->cnt_count; i++) {
            if (&entries[i] == bmp) continue;
            const u32 a0 = entries[i].offset, a1 = a0 + entries[i].size;
            const u32 b0 = targetOff, b1 = targetOff + (u32)kSplashSize;
            if (a0 < b1 && b0 < a1) return false;
        }
        if (targetOff + kSplashSize > meta->size) return false;

        if (!bmp) {
            // Need room for one more entry right after the table, before the
            // first real content - true today, but verify rather than assume.
            if (cntBase + ((size_t)meta->cnt_count + 1) * sizeof(Pkg3Content) > minContentOff)
                return false;
            bmp = &entries[meta->cnt_count];
            memset(bmp, 0, sizeof(*bmp));
            strncpy(bmp->name, "splash", sizeof(bmp->name) - 1);
            bmp->type = (u8)kCntTypeBmp;
            meta->cnt_count += 1;
        }
        bmp->offset = targetOff;
        bmp->size   = (u32)kSplashSize;
    }

    memcpy(pkg3.data() + targetOff, pixels.data(), kSplashSize);

    if (!BackupPackage3()) return false;

    f = fopen(kPackage3, "wb");
    if (!f) return false;
    const bool ok = fwrite(pkg3.data(), 1, kPackage3Size, f) == kPackage3Size;
    fclose(f);
    return ok;
}

// Put the stock splash back, if we still have it.
static bool RestoreSplash() {
    struct stat st;
    if (stat(kPackage3Back, &st) != 0 || (size_t)st.st_size != kPackage3Size) return false;
    return CopyFile(kPackage3Back, kPackage3);
}

static bool CopyTree(const std::string &src, const std::string &dst,
                     const std::function<void()> &tick) {
    Mkdirs(dst);
    DIR *d = opendir(src.c_str());
    if (!d) return false;
    struct dirent *e;
    bool ok = true;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        std::string s = src + "/" + e->d_name, t = dst + "/" + e->d_name;
        struct stat st;
        if (stat(s.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
            if (!CopyTree(s, t, tick)) ok = false;
        } else {
            struct stat es;
            // Preserve the user's existing settings/wallpapers/music on re-install.
            if (!(IsUserData(t) && stat(t.c_str(), &es) == 0)) {
                if (!CopyFile(s, t)) ok = false;
            }
            g_done++;
            tick();
        }
    }
    closedir(d);
    return ok;
}

static bool DeleteTree(const std::string &path) {
    DIR *d = opendir(path.c_str());
    if (!d) return remove(path.c_str()) == 0; 

    struct dirent *e;
    bool ok = true;
    
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        
        std::string s = path + "/" + e->d_name;
        struct stat st;
        
        if (lstat(s.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
            if (!DeleteTree(s)) ok = false;
        } else {
            if (remove(s.c_str()) != 0) ok = false;
        }
    }
    
    closedir(d);
    
    if (rmdir(path.c_str()) != 0) {
        ok = false;
    }
    
    return ok;
}

static constexpr const char *kQlaunchLive     = "sdmc:/atmosphere/contents/0100000000001000";
static constexpr const char *kQlaunchDisabled = "sdmc:/slaunch/backup/0100000000001000.disabled";

static bool IsInstalled() {
    struct stat st;
    return stat(kQlaunchLive, &st) == 0;
}

// Moved aside rather than deleted: everything else (fonts, themes, config,
// cache, the boot splash) stays exactly as it was, so enabling again is
// instant and doesn't need the SD payload at all.
static bool IsDisabled() {
    struct stat st;
    return stat(kQlaunchDisabled, &st) == 0;
}

// ---- disable / re-enable (stock HOME menu without losing anything) --------
static bool DisableSlaunch() {
    struct stat st;
    if (stat(kQlaunchLive, &st) != 0) return false;      // nothing live to disable
    Mkdirs("sdmc:/slaunch/backup");
    if (stat(kQlaunchDisabled, &st) == 0) DeleteTree(kQlaunchDisabled); // stale leftover
    return rename(kQlaunchLive, kQlaunchDisabled) == 0;
}

static bool EnableSlaunch() {
    struct stat st;
    if (stat(kQlaunchDisabled, &st) != 0) return false;  // nothing to re-enable
    if (stat(kQlaunchLive, &st) == 0) return false;       // already live - don't clobber it
    return rename(kQlaunchDisabled, kQlaunchLive) == 0;
}

// ---- full uninstall --------------------------------------------------------
// Remove EVERYTHING sLaunch put on the SD: the qlaunch override and the whole
// slaunch/ tree (bin, fonts, music, sounds, themes, widgets, icons, lang,
// config, cache). Leaves the installer NRO so the user can reinstall.
static bool RemoveEverything() {
    bool ok = true;
    struct stat st;
    // Stock boot logo first: the backup lives under slaunch/, which is about to
    // be deleted, so putting it back afterwards would be putting it back from
    // nothing. Failure here is not fatal - an uninstall that leaves our splash
    // is better than one that refuses to run.
    RestoreSplash();
    // Whichever state it's in - live or disabled-aside - only one of these
    // exists, and deleting the whole slaunch/ tree below would sweep up a
    // disabled copy anyway, but a live one sits outside it.
    if (stat(kQlaunchLive, &st) == 0)
        if (!DeleteTree(kQlaunchLive)) ok = false;
    if (stat("sdmc:/slaunch", &st) == 0)
        if (!DeleteTree("sdmc:/slaunch")) ok = false;
    return ok;
}

// ---- reboot ----------------------------------------------------------------
// Orderly reboot via spsm (the full shutdown state machine, same thing the
// reboot_to_payload homebrew uses); raw bpc only as a fallback, since yanking
// the PMIC from a fully-awake system can leave it half-asleep instead.
static void RebootSystem() {
    if (R_SUCCEEDED(spsmInitialize())) {
        Result rc = spsmShutdown(true);
        spsmExit();
        if (R_SUCCEEDED(rc)) {
            while (true) svcSleepThread(100'000'000ULL);  // reboot is coming
        }
    }
    if (R_SUCCEEDED(bpcInitialize())) { bpcRebootSystem(); bpcExit(); }
}

// ---- version compare -------------------------------------------------------
static void ParseVer(const char *s, int v[3]) {
    v[0] = v[1] = v[2] = 0;
    if (s && (*s == 'v' || *s == 'V')) s++;
    if (s) sscanf(s, "%d.%d.%d", &v[0], &v[1], &v[2]);
}
static int CmpVer(const char *a, const char *b) {   // >0 if a is newer than b
    int va[3], vb[3]; ParseVer(a, va); ParseVer(b, vb);
    for (int i = 0; i < 3; i++) if (va[i] != vb[i]) return va[i] - vb[i];
    return 0;
}

// ---- tiny JSON helpers (GitHub releases API) -------------------------------
static std::string JsonStr(const std::string &j, const char *key) {
    std::string pat = std::string("\"") + key + "\"";
    size_t p = j.find(pat); if (p == std::string::npos) return "";
    p = j.find(':', p + pat.size()); if (p == std::string::npos) return "";
    size_t s = j.find('"', p); if (s == std::string::npos) return "";
    size_t e = j.find('"', s + 1); if (e == std::string::npos) return "";
    return j.substr(s + 1, e - s - 1);
}
// The first release asset download URL ending in "-sd.zip".
static std::string FindZipUrl(const std::string &j) {
    const char *key = "\"browser_download_url\"";
    size_t p = 0;
    while ((p = j.find(key, p)) != std::string::npos) {
        size_t c = j.find(':', p);           if (c == std::string::npos) break;
        size_t s = j.find('"', c);           if (s == std::string::npos) break;
        size_t e = j.find('"', s + 1);       if (e == std::string::npos) break;
        std::string url = j.substr(s + 1, e - s - 1);
        if (url.size() >= 7 && url.compare(url.size() - 7, 7, "-sd.zip") == 0) return url;
        p = e + 1;
    }
    return "";
}

// ---- shared progress screen ------------------------------------------------
// The single painter for every "working on it" screen. The blocking operations
// below drive it from their own progress callbacks while the main loop is
// stalled inside them, so anything the main loop draws on that screen has to
// be drawn from here too - otherwise the callback repaints a stripped-down
// version over it. Draws only; the caller presents.
static void DrawProgress(const char *title, double frac, const char *sub,
                         const std::function<void()> &anim = {}) {
    FillRect(0, 0, W, H, kBlack);
    Text(g_fM, 120, H / 2 - 30, kFg, title);
    int bx = 120, by = H / 2 + 10, bw = 600, bh = 3;
    FillRect(bx, by, bw, bh, kBg2);
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    FillRect(bx, by, (int)(bw * frac), bh, kAccent);
    if (sub && sub[0]) Text(g_fS, 120, H / 2 + 30, kDim, sub);
    if (anim) anim();
}

// ---- HTTP (GitHub API + release download) ----------------------------------
static size_t WrStr(char *p, size_t s, size_t n, void *u) {
    ((std::string *)u)->append(p, s * n); return s * n;
}
static bool HttpGet(const char *url, std::string &out) {
    out.clear();
    CURL *c = curl_easy_init(); if (!c) return false;
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, WrStr);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &out);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "sLaunch-Installer");
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
    CURLcode rc = curl_easy_perform(c);
    long http = 0; curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http);
    curl_easy_cleanup(c);
    return rc == CURLE_OK && http >= 200 && http < 300;
}
static size_t WrFile(char *p, size_t s, size_t n, void *u) {
    return fwrite(p, s, n, (FILE *)u) * s;
}
static int DlProgressCb(void *, curl_off_t dltotal, curl_off_t dlnow, curl_off_t, curl_off_t) {
    double frac = dltotal > 0 ? (double)dlnow / (double)dltotal : 0.0;
    char sub[64];
    snprintf(sub, sizeof(sub), "%.1f / %.1f MB", dlnow / 1048576.0, dltotal / 1048576.0);
    DrawProgress("Downloading update...", frac, sub);
    SDL_RenderPresent(g_ren);
    return 0;
}
static bool HttpDownload(const char *url, const char *path) {
    FILE *fp = fopen(path, "wb"); if (!fp) return false;
    CURL *c = curl_easy_init(); if (!c) { fclose(fp); remove(path); return false; }
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, WrFile);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, fp);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 600L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "sLaunch-Installer");
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, DlProgressCb);
    CURLcode rc = curl_easy_perform(c);
    long http = 0; curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http);
    curl_easy_cleanup(c); fclose(fp);
    bool ok = rc == CURLE_OK && http >= 200 && http < 300;
    if (!ok) remove(path);
    return ok;
}

// ---- unzip the release payload onto the SD (preserving user data) -----------
static bool ExtractZip(const char *zipPath) {
    unzFile uf = unzOpen64(zipPath);
    if (!uf) return false;
    unz_global_info64 gi;
    if (unzGetGlobalInfo64(uf, &gi) != UNZ_OK) { unzClose(uf); return false; }
    const int total = (int)gi.number_entry;
    int done = 0;
    bool ok = (unzGoToFirstFile(uf) == UNZ_OK);
    while (ok) {
        char name[600]; unz_file_info64 fi;
        if (unzGetCurrentFileInfo64(uf, &fi, name, sizeof(name), nullptr, 0, nullptr, 0) != UNZ_OK) {
            ok = false; break;
        }
        std::string dst = std::string("sdmc:/") + name;
        size_t len = strlen(name);
        struct stat es;
        if (len && (name[len - 1] == '/' || name[len - 1] == '\\')) {
            Mkdirs(dst);                                   // directory entry
        } else if (IsUserData(dst) && stat(dst.c_str(), &es) == 0) {
            // keep the user's existing settings/wallpapers/music - skip
        } else {
            size_t slash = dst.find_last_of('/');
            if (slash != std::string::npos) Mkdirs(dst.substr(0, slash));
            if (unzOpenCurrentFile(uf) != UNZ_OK) { ok = false; break; }
            FILE *out = fopen(dst.c_str(), "wb");
            if (!out) { unzCloseCurrentFile(uf); ok = false; break; }
            char buf[65536]; int n;
            while ((n = unzReadCurrentFile(uf, buf, sizeof(buf))) > 0)
                if (fwrite(buf, 1, n, out) != (size_t)n) { ok = false; break; }
            fclose(out);
            unzCloseCurrentFile(uf);
            if (n < 0) ok = false;
        }
        done++;
        DrawProgress("Installing update...", total > 0 ? (double)done / total : 0.0, nullptr);
        SDL_RenderPresent(g_ren);
        if (!ok) break;
        if (unzGoToNextFile(uf) != UNZ_OK) break;
    }
    unzClose(uf);
    return ok;
}

// ---- main-menu tiles -------------------------------------------------------
// Shelf-style row (the menu's Shelf mode, minus the scrolling): uniform tiles
// centred as a group, the selected one lifted by a highlight card and an
// accent frame. The row is centred either way, so it just gets wider or
// narrower as tiles join or drop out.
enum class Act { Install, Update, Toggle, Remove };

struct Tile {
    const char  *label;
    const char  *desc;
    Act          act;
    SDL_Texture *icon;
};

static constexpr int kTileSize = 200;
static constexpr int kTileGap  = 56;
static constexpr int kTileTop  = (H - kTileSize) / 2;   // row centred on the screen

static void DrawTileRow(const Tile *tiles, int count, int cursor) {
    const int rowW = count * kTileSize + (count - 1) * kTileGap;
    const int x0   = (W - rowW) / 2;

    for (int i = 0; i < count; i++) {
        const int  x   = x0 + i * (kTileSize + kTileGap);
        const bool sel = (i == cursor);

        // Highlight card, extending under the label like the Shelf's info card.
        if (sel) FillRect(x - 14, kTileTop - 14, kTileSize + 28, kTileSize + 76, kFg, 14);

        if (tiles[i].icon) {
            // The icons are white line art on black. Additive blending drops
            // their background so the card shows through, and the colour mod
            // dims the two that aren't selected.
            const Uint8 v = sel ? 255 : 90;
            SDL_SetTextureColorMod(tiles[i].icon, v, v, v);
            // They are 64px pixel art: draw them at an exact 2x inside the tile.
            // A stretch to the full 200 would land on fractional pixels and turn
            // the thin strokes ragged.
            const int art = 128, pad = (kTileSize - art) / 2;
            SDL_Rect dst{ x + pad, kTileTop + pad, art, art };
            SDL_RenderCopy(g_ren, tiles[i].icon, nullptr, &dst);
        } else {
            FillRect(x, kTileTop, kTileSize, kTileSize, kBg2);
        }

        FillRect(x, kTileTop, kTileSize, 3, kAccent, sel ? 255 : 80);   // accent strip

        if (sel) {   // thin frame, clear of the art
            FillRect(x - 4,         kTileTop - 4,         kTileSize + 8, 4,             kAccent);
            FillRect(x - 4,         kTileTop + kTileSize, kTileSize + 8, 4,             kAccent);
            FillRect(x - 4,         kTileTop - 4,         4,             kTileSize + 8, kAccent);
            FillRect(x + kTileSize, kTileTop - 4,         4,             kTileSize + 8, kAccent);
        }

        Text(g_fM, x + kTileSize / 2, kTileTop + kTileSize + 18, sel ? kFg : kDim,
             tiles[i].label, true);
    }

    // One line about whatever is selected, under the row.
    Text(g_fS, W / 2, kTileTop + kTileSize + 78, kDim, tiles[cursor].desc, true);
}

// ---- dialog ----------------------------------------------------------------
// Left-aligned at the same x=120 margin as the progress screens, rather than
// centred, so a dialog doesn't read as a visually different kind of screen.
static void DrawConfirmDialog(const char *title, const char *subtitle,
                              int cursor, bool isDanger = false) {
    FillRect(0, 0, W, H, kBlack);

    int cy = H / 2 - 60;

    if (isDanger) Text(g_fL, 120, cy - 80, kRed, "WARNING");

    Text(g_fL, 120, cy, kFg, title);
    if (subtitle && subtitle[0])
        Text(g_fM, 120, cy + 60, kDim, subtitle);

    const char *opts[2] = {"Yes", "No"};
    for (int i = 0; i < 2; i++) {
        bool sel = (i == cursor);
        int y = cy + 150 + i * 65;
        Text(g_fM, 120, y, sel ? (isDanger ? kRed : kAccent) : kDim, opts[i]);
    }
}

// ---- Installing / Checking-for-updates animations --------------------------
// Both loop continuously for as long as the screen is up, since the real
// operation behind them (a file copy, an HTTP request) can finish at any
// point in the cycle - there's no "done" frame to land on, just a loop that
// keeps communicating "this is still working."
static void DrawIcon(SDL_Texture *t, int cx, int cy, int w, int h) {
    if (!t) return;
    SDL_Rect r{ cx - w / 2, cy - h / 2, w, h };
    SDL_RenderCopy(g_ren, t, nullptr, &r);
}

// The sLaunch mark slides down from above and settles onto an SD card, holds,
// then resets - "putting sLaunch on the card," looped.
static void DrawInstallAnim(u64 elapsed_ms, SDL_Texture *mark, SDL_Texture *card) {
    const int cx = 980, cardCy = 460;
    DrawIcon(card, cx, cardCy, 130, 169);   // 231x300 source aspect

    constexpr u64 kFall = 650, kHold = 550, kPause = 300;
    constexpr u64 kPeriod = kFall + kHold + kPause;
    const u64 t = elapsed_ms % kPeriod;
    if (t >= kFall + kHold) return;   // paused between loops - card sits empty a beat

    const int restY = cardCy - 30;
    int y = restY;
    if (t < kFall) {
        float f = (float)t / (float)kFall;
        f = 1.0f - (1.0f - f) * (1.0f - f);            // ease-out
        y = restY - (int)((1.0f - f) * 220.0f);
    }
    DrawIcon(mark, cx, y, 84, 84);
}

// The Switch rises toward the GitHub mark (standing in for "a satellite"),
// pulses a few beats while in contact, then the loop resets.
static void DrawSatelliteAnim(u64 elapsed_ms, SDL_Texture *nx, SDL_Texture *sat) {
    const int cx = 980, satCy = 160, meetCy = 280, startCy = 500;
    DrawIcon(sat, cx, satCy, 100, 100);

    constexpr u64 kRise = 700, kTalk = 900, kPause = 300;
    constexpr u64 kPeriod = kRise + kTalk + kPause;
    const u64 t = elapsed_ms % kPeriod;
    if (t >= kRise + kTalk) return;

    if (t < kRise) {
        float f = (float)t / (float)kRise;
        f = 1.0f - (1.0f - f) * (1.0f - f);
        int y = startCy - (int)(f * (startCy - meetCy));
        DrawIcon(nx, cx, y, 76, 76);
    } else {
        DrawIcon(nx, cx, meetCy, 76, 76);
        // Three dots ticking up the gap between them, staggered, like a
        // handshake in progress.
        const u64 tt = t - kRise;
        for (int i = 0; i < 3; i++) {
            const u64 phase = (tt + i * 220) % 660;
            if (phase >= 500) continue;
            const float f = (float)phase / 500.0f;
            const int y = meetCy - 40 - (int)(f * (meetCy - satCy - 60));
            const Uint8 a = (Uint8)(255 * (1.0f - f));
            FillRect(cx - 3, y, 6, 6, kAccent, a);
        }
    }
}

// ---------------------------------------------------------------------------
int main() {
    romfsInit();
    plInitialize(PlServiceType_User);
    socketInitializeDefault();          // bsd sockets for the online update check
    curl_global_init(CURL_GLOBAL_DEFAULT);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    // Set before any texture is created - SDL2 captures the scale mode at
    // creation time, so a hint set later would leave existing textures on
    // the old (nearest, blocky) mode. The tile icons are 512px vector art
    // scaled down to fit; without this they'd alias instead of downsampling
    // cleanly.
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    SDL_Init(SDL_INIT_VIDEO);
    SDL_Window *win = SDL_CreateWindow("sInstaller", 0, 0, W, H, 0);
    g_ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);
    TTF_Init();
    IMG_Init(IMG_INIT_PNG);

    PlFontData fd;
    if (R_SUCCEEDED(plGetSharedFontByType(&fd, PlSharedFontType_Standard))) {
        g_fL = TTF_OpenFontRW(SDL_RWFromConstMem(fd.address, fd.size), 1, 46);
        g_fM = TTF_OpenFontRW(SDL_RWFromConstMem(fd.address, fd.size), 1, 28);
        g_fS = TTF_OpenFontRW(SDL_RWFromConstMem(fd.address, fd.size), 1, 22);
    }

    // Action icons for the tile row (and the title mark below). They are white
    // line art on an opaque black square, so additive blending is what makes
    // the black disappear.
    auto loadTileIcon = [&](const char *path) {
        SDL_Texture *t = IMG_LoadTexture(g_ren, path);
        if (t) SDL_SetTextureBlendMode(t, SDL_BLENDMODE_ADD);
        return t;
    };
    SDL_Texture *icon = loadTileIcon("romfs:/icon.png");
    SDL_Texture *icInstall   = loadTileIcon("romfs:/ui/install.png");
    SDL_Texture *icUpdate    = loadTileIcon("romfs:/ui/update.png");
    SDL_Texture *icDisable   = loadTileIcon("romfs:/ui/disable.png");
    SDL_Texture *icUninstall = loadTileIcon("romfs:/ui/uninstall.png");
    // Sprites for the Installing / Checking-for-updates animations.
    SDL_Texture *icSdcard = loadTileIcon("romfs:/ui/sdcard.png");
    SDL_Texture *icSwitch = loadTileIcon("romfs:/ui/switch.png");
    SDL_Texture *icGithub = loadTileIcon("romfs:/ui/github.png");

    // --- installer state ---
    enum class Screen { MainMenu, ConfirmInstall, ConfirmRemove,
                        Installing, Removing, Done, Failed,
                        Checking, UpToDate, ConfirmUpdate, Downloading, Extracting };
    Screen screen = Screen::MainMenu;

    bool installed = IsInstalled();
    bool disabled  = IsDisabled();   // present on the SD, just not the active HOME menu
    bool rebootAfter = false;   // Done came from an install/update (offer reboot)

    // Fixed row of actions. Toggle/Remove only join once there is something to
    // toggle or remove - the row is centred either way, so it simply gets
    // narrower or wider. Exit isn't a tile - it's Plus, hinted at the bottom.
    Tile tiles[4] = {};
    int  tileCount  = 2;
    int  instCursor = 0;
    auto buildMenu = [&]() {
        tiles[0] = { installed ? "Re-install" : "Install",
                     installed ? "Copy this build over the installed one"
                               : "Copy sLaunch to your SD card",
                     Act::Install, icInstall };
        tiles[1] = { "Update", "Check GitHub for a newer release", Act::Update, icUpdate };
        int n = 2;
        if (installed || disabled)
            tiles[n++] = { disabled ? "Enable" : "Disable",
                           disabled ? "Switch back to sLaunch as the HOME menu"
                                    : "Switch back to the stock HOME menu (keeps everything)",
                           Act::Toggle, icDisable };
        if (installed || disabled)
            tiles[n++] = { "Remove", "Delete every sLaunch file from the SD card",
                           Act::Remove, icUninstall };
        tileCount = n;
        if (instCursor >= tileCount) instCursor = tileCount - 1;
    };
    buildMenu();

    std::string latestTag, zipUrl;   // filled by the update check
    const char *updErr = "";         // failure detail for the Failed screen

    // Dialog state
    int dialogCursor = 1; // 0 = Yes, 1 = No (default to No for safety)
    bool ok = false;

    const u64 freq        = armGetSystemTickFreq();
    const u64 RepeatDelay = (360 * freq) / 1000;
    auto ms = [&](u64 m) { return (m * freq) / 1000; };

    int held_v = 0, held_h = 0;
    u64 next_v = 0, start_v = 0, next_h = 0, start_h = 0;

    // Restarted on every screen change so the Installing/Checking loop
    // animations always begin from frame zero, not wherever main() happened
    // to be running long.
    Screen lastScreen = screen;
    u64 animStartTick = armGetSystemTick();
    auto installAnim = [&] {
        DrawInstallAnim(((armGetSystemTick() - animStartTick) * 1000) / freq, icon, icSdcard);
    };

    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 down = padGetButtonsDown(&pad);
        u64 held = padGetButtons(&pad); 


        int dir_v = 0;
        if (held & (HidNpadButton_Up | HidNpadButton_StickLUp | HidNpadButton_StickRUp)) dir_v = -1;
        else if (held & (HidNpadButton_Down | HidNpadButton_StickLDown | HidNpadButton_StickRDown)) dir_v = 1;

        // The tile row is horizontal, so left/right drives the main screen while
        // up/down keeps driving the Yes/No dialogs.
        int dir_h = 0;
        if (held & (HidNpadButton_Left | HidNpadButton_StickLLeft | HidNpadButton_StickRLeft)) dir_h = -1;
        else if (held & (HidNpadButton_Right | HidNpadButton_StickLRight | HidNpadButton_StickRRight)) dir_h = 1;

        const u64 now = armGetSystemTick();
        bool move_up = false;
        bool move_down = false;
        bool move_left = false;
        bool move_right = false;

        auto step = [&](int dir, int &held_state, u64 &next, u64 &start, bool &fire_up, bool &fire_down) {
            if (dir == 0) { held_state = 0; return; }
            const bool fresh = (dir != held_state);
            bool fire = false;
            
            if (fresh) {
                fire = true; start = now; next = now + RepeatDelay;
            } else if (now >= next) {
                fire = true;
                const u64 held_ms = ((now - start) * 1000) / freq;
                const u64 iv = held_ms < 700 ? 90 : held_ms < 1500 ? 55 : 32;
                next = now + ms(iv);
            }
            
            held_state = dir;
            if (fire) {
                if (dir < 0) fire_up = true;
                else fire_down = true;
            }
        };

        step(dir_v, held_v, next_v, start_v, move_up,   move_down);
        step(dir_h, held_h, next_h, start_h, move_left, move_right);

        // ---- input ----
        if (screen == Screen::MainMenu) {
            if (move_left)  instCursor = (instCursor + tileCount - 1) % tileCount;
            if (move_right) instCursor = (instCursor + 1) % tileCount;

            if (down & HidNpadButton_A) {
                switch (tiles[instCursor].act) {
                    case Act::Install: dialogCursor = 1; screen = Screen::ConfirmInstall; break;
                    case Act::Update:  screen = Screen::Checking; break;
                    case Act::Toggle:
                        // A single rename(), so no progress screen, and reversible
                        // either way, so no confirmation - but the running session
                        // doesn't care until next boot either way, same as
                        // Install/Remove, so it still ends on the same reboot offer.
                        ok = disabled ? EnableSlaunch() : DisableSlaunch();
                        installed = IsInstalled();
                        disabled  = IsDisabled();
                        buildMenu();
                        rebootAfter = ok;
                        screen = ok ? Screen::Done : Screen::Failed;
                        break;
                    case Act::Remove:  dialogCursor = 1; screen = Screen::ConfirmRemove; break;
                }
            }
            if (down & (HidNpadButton_B | HidNpadButton_Plus)) goto cleanup;

        } else if (screen == Screen::ConfirmInstall || screen == Screen::ConfirmRemove) {
            if (move_up || move_down) dialogCursor ^= 1;
            if (down & HidNpadButton_B) {
                screen = Screen::MainMenu;
            }
            if (down & HidNpadButton_A) {
                if (dialogCursor == 0) { // Yes
                    g_total = 0;
                    g_done = 0;
                    if (screen == Screen::ConfirmInstall) {
                        screen = Screen::Installing;
                    } else {
                        screen = Screen::Removing;
                    }
                } else { // No
                    screen = Screen::MainMenu;
                }
            }

        } else if (screen == Screen::ConfirmUpdate) {
            if (move_up || move_down) dialogCursor ^= 1;
            if (down & HidNpadButton_B) screen = Screen::MainMenu;
            if (down & HidNpadButton_A)
                screen = (dialogCursor == 0) ? Screen::Downloading : Screen::MainMenu;

        } else if (screen == Screen::UpToDate) {
            if (down & (HidNpadButton_A | HidNpadButton_B)) screen = Screen::MainMenu;

        } else if (screen == Screen::Installing || screen == Screen::Removing ||
                   screen == Screen::Checking   || screen == Screen::Downloading ||
                   screen == Screen::Extracting) {
            // no input during a running operation

        } else if (screen == Screen::Done) {
            if (rebootAfter && (down & HidNpadButton_A)) RebootSystem();   // reboot now
            if (down & (HidNpadButton_A | HidNpadButton_B)) goto cleanup;  // else exit

        } else if (screen == Screen::Failed) {
            if (down & (HidNpadButton_A | HidNpadButton_B)) { buildMenu(); screen = Screen::MainMenu; }
        }

        if (screen != lastScreen) { animStartTick = armGetSystemTick(); lastScreen = screen; }
        const u64 animMs = ((armGetSystemTick() - animStartTick) * 1000) / freq;

        // ---- draw ----
        FillRect(0, 0, W, H, kBlack);

        if (screen == Screen::MainMenu) {
            DrawTileRow(tiles, tileCount, instCursor);
            Text(g_fS, W / 2, H - 40, kDim, "+  Exit", true);

        } else if (screen == Screen::ConfirmInstall) {
            // The dialog paints the whole screen, so nothing is drawn behind it.
            DrawConfirmDialog(installed ? "Re-install sLaunch?" : "Install sLaunch?",
                "This will copy files to your SD card.", dialogCursor, false);

        } else if (screen == Screen::ConfirmRemove) {
            DrawConfirmDialog((installed || disabled) ? "Remove sLaunch?" : "Do nothing?",
                (installed || disabled) ? "This will delete all sLaunch files." : "This will do nothing.",
                dialogCursor, true);

        } else if (screen == Screen::Installing) {
            char cnt[64];
            snprintf(cnt, sizeof(cnt), "%d / %d files", g_done, g_total);
            DrawProgress("Installing sLaunch...",
                         g_total > 0 ? (double)g_done / (double)g_total : 0.0,
                         cnt, installAnim);

        } else if (screen == Screen::Removing) {
            Text(g_fM, 120, H / 2 - 30, kFg, (installed || disabled) ? "Removing sLaunch..." : "Doing nothing...");
            int bx = 120, by = H / 2 + 10, bw = 600, bh = 3;
            FillRect(bx, by, bw, bh, kBg2);
            float p = g_total > 0 ? (float)g_done / (float)g_total : 0.0f;
            FillRect(bx, by, (int)(bw * p), bh, kAccent);
            char cnt[64];
            snprintf(cnt, sizeof(cnt), "%d / %d files", g_done, g_total);
            Text(g_fS, 120, H / 2 + 30, kDim, cnt);

        } else if (screen == Screen::Done) {
            Text(g_fM, W / 2, H / 2 - 20, kAccent, "Done", true);
            if (rebootAfter) {
                Text(g_fS, W / 2, H / 2 + 20, kDim, "A reboot is needed to apply the changes", true);
                Text(g_fS, W / 2, H - 60, kAccent, "A: Reboot now", true);
                Text(g_fS, W / 2, H - 34, kDim, "B: Exit (reboot later)", true);
            } else {
                Text(g_fS, W / 2, H - 40, kDim, "Press A to exit", true);
            }

        } else if (screen == Screen::Failed) {
            Text(g_fM, W / 2, H / 2 - 20, kRed, "Operation failed", true);
            Text(g_fS, W / 2, H / 2 + 20, kDim, updErr[0] ? updErr : "Check your SD card and try again", true);
            Text(g_fS, W / 2, H - 40, kDim, "Press A to go back", true);

        } else if (screen == Screen::Checking) {
            Text(g_fM, W / 2, H / 2 - 15, kFg, "Checking for updates...", true);
            Text(g_fS, W / 2, H / 2 + 30, kDim, "Current version " SL_VERSION, true);
            DrawSatelliteAnim(animMs, icSwitch, icGithub);

        } else if (screen == Screen::UpToDate) {
            Text(g_fM, W / 2, H / 2 - 20, kGreen, "You're up to date", true);
            Text(g_fS, W / 2, H / 2 + 25, kDim, "Installed: v" SL_VERSION, true);
            Text(g_fS, W / 2, H - 40, kDim, "Press A to go back", true);

        } else if (screen == Screen::ConfirmUpdate) {
            char sub[96];
            snprintf(sub, sizeof(sub), "You have v%s - download and install %s?",
                     SL_VERSION, latestTag.c_str());
            DrawConfirmDialog("Update available", sub, dialogCursor, false);

        } else if (screen == Screen::Downloading) {
            Text(g_fM, W / 2, H / 2 - 15, kFg, "Preparing download...", true);

        } else if (screen == Screen::Extracting) {
            Text(g_fM, W / 2, H / 2 - 15, kFg, "Installing update...", true);
        }

        SDL_RenderPresent(g_ren);

        // ---- operation logic (run once after first draw) ----
        if (screen == Screen::Installing && g_total == 0) {
            CountTree("romfs:/payload");
            if (g_total == 0) g_total = 1;
            ok = CopyTree("romfs:/payload", "sdmc:", [&]() {
                char cnt[64];
                snprintf(cnt, sizeof(cnt), "%d / %d files", g_done, g_total);
                DrawProgress("Installing sLaunch...",
                             (double)g_done / (double)g_total, cnt, installAnim);
                SDL_RenderPresent(g_ren);
            });
            // Splash last, and never fatal: a console whose package3 is not the
            // size or shape we expect still gets a working sLaunch, just with
            // the stock boot logo.
            if (ok) InstallSplash();
            // A fresh copy just landed at the live path; a disabled-aside copy
            // from before would otherwise sit there stale, looking like this
            // install is still disabled.
            if (ok) { struct stat st; if (stat(kQlaunchDisabled, &st) == 0) DeleteTree(kQlaunchDisabled); }
            installed = ok;
            disabled  = IsDisabled();
            rebootAfter = ok;
            buildMenu();
            screen = ok ? Screen::Done : Screen::Failed;
        }

        if (screen == Screen::Removing && g_total == 0) {
            ok = RemoveEverything();          // qlaunch override + the whole slaunch/ tree
            installed = IsInstalled();
            disabled  = IsDisabled();
            rebootAfter = ok;                 // reboot returns to the stock HOME menu
            buildMenu();
            screen = ok ? Screen::Done : Screen::Failed;
        }

        // ---- online update ----
        if (screen == Screen::Checking) {
            std::string body;
            if (!HttpGet("https://api.github.com/repos/etonedemid/slaunch/releases/latest", body)) {
                updErr = "Could not reach GitHub (no internet?)";
                screen = Screen::Failed;
            } else {
                latestTag = JsonStr(body, "tag_name");   // e.g. "v0.6.0"
                zipUrl    = FindZipUrl(body);
                if (latestTag.empty()) {
                    updErr = "Could not read the latest version";
                    screen = Screen::Failed;
                } else if (CmpVer(latestTag.c_str(), SL_VERSION) > 0 && !zipUrl.empty()) {
                    dialogCursor = 1;
                    screen = Screen::ConfirmUpdate;   // newer release available
                } else {
                    screen = Screen::UpToDate;
                }
            }
        }

        if (screen == Screen::Downloading) {
            ok = HttpDownload(zipUrl.c_str(), "sdmc:/slaunch_update.zip");
            if (!ok) { updErr = "Download failed"; screen = Screen::Failed; }
            else       screen = Screen::Extracting;
        }

        if (screen == Screen::Extracting) {
            ok = ExtractZip("sdmc:/slaunch_update.zip");
            remove("sdmc:/slaunch_update.zip");
            // Same reasoning as a fresh Install: a stale disabled-aside copy
            // would otherwise outlive the update it was disabled before.
            if (ok) { struct stat st; if (stat(kQlaunchDisabled, &st) == 0) DeleteTree(kQlaunchDisabled); }
            installed = IsInstalled();
            disabled  = IsDisabled();
            rebootAfter = ok;
            buildMenu();
            if (!ok) updErr = "Could not extract the update";
            screen = ok ? Screen::Done : Screen::Failed;
        }
    }

cleanup:
    if (icon) SDL_DestroyTexture(icon);
    if (icInstall)   SDL_DestroyTexture(icInstall);
    if (icUpdate)    SDL_DestroyTexture(icUpdate);
    if (icDisable)   SDL_DestroyTexture(icDisable);
    if (icUninstall) SDL_DestroyTexture(icUninstall);
    if (icSdcard)    SDL_DestroyTexture(icSdcard);
    if (icSwitch)    SDL_DestroyTexture(icSwitch);
    if (icGithub)    SDL_DestroyTexture(icGithub);
    if (g_fL) TTF_CloseFont(g_fL);
    if (g_fM) TTF_CloseFont(g_fM);
    if (g_fS) TTF_CloseFont(g_fS);
    IMG_Quit();
    TTF_Quit();
    SDL_DestroyRenderer(g_ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    plExit();
    romfsExit();
    return 0;
}