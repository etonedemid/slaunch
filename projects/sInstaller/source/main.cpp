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
                 kAccent{95, 200, 255}, kGreen{100, 220, 140}, kRed{235, 80, 80};

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

static int TextWidth(TTF_Font *f, const char *s) {
    if (!f || !s || !s[0]) return 0;
    int w = 0, h = 0;
    TTF_SizeUTF8(f, s, &w, &h);
    return w;
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

static bool IsInstalled() {
    struct stat st;
    return stat("sdmc:/atmosphere/contents/0100000000001000", &st) == 0;
}

// ---- full uninstall --------------------------------------------------------
// Remove EVERYTHING sLaunch put on the SD: the qlaunch override and the whole
// slaunch/ tree (bin, fonts, music, sounds, themes, widgets, icons, lang,
// config, cache). Leaves the installer NRO so the user can reinstall.
static bool RemoveEverything() {
    bool ok = true;
    struct stat st;
    if (stat("sdmc:/atmosphere/contents/0100000000001000", &st) == 0)
        if (!DeleteTree("sdmc:/atmosphere/contents/0100000000001000")) ok = false;
    if (stat("sdmc:/slaunch", &st) == 0)
        if (!DeleteTree("sdmc:/slaunch")) ok = false;
    return ok;
}

// ---- reboot ----------------------------------------------------------------
static void RebootSystem() {
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
static void DrawProgress(const char *title, double frac, const char *sub) {
    FillRect(0, 0, W, H, kBlack);
    Text(g_fM, 120, H / 2 - 30, kFg, title);
    int bx = 120, by = H / 2 + 10, bw = 600, bh = 3;
    FillRect(bx, by, bw, bh, kBg2);
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    FillRect(bx, by, (int)(bw * frac), bh, kAccent);
    if (sub && sub[0]) Text(g_fS, 120, H / 2 + 30, kDim, sub);
    SDL_RenderPresent(g_ren);
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
        if (!ok) break;
        if (unzGoToNextFile(uf) != UNZ_OK) break;
    }
    unzClose(uf);
    return ok;
}

// ---- carousel helpers ------------------------------------------------------
struct MockItem {
    const char *label;
    bool        running;
    bool        favourite;
};

static void DrawCarousel(const std::vector<MockItem> &items, int cursor,
                         float &scroll_pos, int center_y, int spacing,
                         bool left_align = false, int margin = 120) {
    scroll_pos += (cursor - scroll_pos) * 0.30f;
    if (std::abs(cursor - scroll_pos) < 0.01f) scroll_pos = (float)cursor;

    const int span = 7;
    for (int off = -span; off <= span; off++) {
        int idx = (int)lroundf(scroll_pos) + off;
        if (idx < 0 || idx >= (int)items.size()) continue;

        const float vdist = std::abs((float)idx - scroll_pos);
        const bool  big   = vdist < 0.5f;
        const bool  sel   = (idx == cursor);
        TTF_Font   *fs    = big ? g_fM : g_fS;
        const float alpha = std::max(0.06f, 1.0f - vdist * 0.13f);

        char text[256];
        text[0] = '\0';
        if (items[idx].running) strcat(text, "\xE2\x97\x8F ");
        if (items[idx].favourite) strcat(text, "\xE2\x98\x85 ");
        strcat(text, items[idx].label);

        int lw = TextWidth(fs, text);
        int tx = left_align ? margin : (W - lw) / 2;
        int y  = center_y + (int)((idx - scroll_pos) * spacing) - (big ? 14 : 10);

        if (sel) {
            Text(g_fM, tx - 34, y, kAccent, ">");
        }

        Col nameCol;
        if (sel || big) {
            nameCol = items[idx].running ? kAccent : kFg;
        } else {
            nameCol = items[idx].running ? Col{26, 58, 74} : kDim;
        }

        Uint8 a = (Uint8)(alpha * 255);
        SDL_Surface *surf = TTF_RenderUTF8_Blended(fs, text,
            SDL_Color{nameCol.r, nameCol.g, nameCol.b, a});
        if (surf) {
            SDL_Texture *tex = SDL_CreateTextureFromSurface(g_ren, surf);
            SDL_Rect dst{tx, y, surf->w, surf->h};
            SDL_FreeSurface(surf);
            if (tex) { SDL_RenderCopy(g_ren, tex, nullptr, &dst); SDL_DestroyTexture(tex); }
        }

        if (items[idx].running) {
            const char *tag = "running";
            int tw = TextWidth(g_fS, tag);
            int ty = y + TTF_FontHeight(fs) - TTF_FontHeight(g_fS) - 2;
            int tag_x = tx + lw + 18;
            SDL_Surface *ts = TTF_RenderUTF8_Blended(g_fS, tag,
                SDL_Color{kAccent.r, kAccent.g, kAccent.b, a});
            if (ts) {
                SDL_Texture *tt = SDL_CreateTextureFromSurface(g_ren, ts);
                SDL_Rect tdst{tag_x, ty, ts->w, ts->h};
                SDL_FreeSurface(ts);
                if (tt) { SDL_RenderCopy(g_ren, tt, nullptr, &tdst); SDL_DestroyTexture(tt); }
            }
        }
    }
}

// ---- dialog ----------------------------------------------------------------
static void DrawConfirmDialog(const char *title, const char *subtitle,
                              int cursor, bool isDanger = false) {
    // Solid black background (no tint, no transparency)
    FillRect(0, 0, W, H, kBlack);
    
    int cy = H / 2 - 60;

    // Explicit warning header
    if (isDanger) {
        Text(g_fL, W / 2, cy - 80, kRed, "WARNING", true);
    }

    Text(g_fL, W / 2, cy, kFg, title, true);
    if (subtitle && subtitle[0])
        Text(g_fM, W / 2, cy + 60, kDim, subtitle, true);

    const char *opts[2] = {"Yes", "No"};
    for (int i = 0; i < 2; i++) {
        bool sel = (i == cursor);
        int y = cy + 150 + i * 65;
        Text(g_fM, W / 2, y, sel ? (isDanger ? kRed : kAccent) : kDim, opts[i], true);
    }
}

// ---- mock home menu data ---------------------------------------------------
// ---------------------------------------------------------------------------
int main() {
    romfsInit();
    plInitialize(PlServiceType_User);
    socketInitializeDefault();          // bsd sockets for the online update check
    curl_global_init(CURL_GLOBAL_DEFAULT);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

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

    SDL_Texture *icon = IMG_LoadTexture(g_ren, "romfs:/icon.png");

    // --- installer state ---
    enum class Screen { MainMenu, ConfirmInstall, ConfirmRemove,
                        Installing, Removing, Done, Failed,
                        Checking, UpToDate, ConfirmUpdate, Downloading, Extracting };
    Screen screen = Screen::MainMenu;

    bool installed = IsInstalled();
    bool rebootAfter = false;   // Done came from an install/update (offer reboot)

    // Online-update state.
    enum class Act { Install, Update, Remove, Exit };
    struct InstallerItem { std::string label; Act act; };
    std::vector<InstallerItem> installerItems;
    auto buildMenu = [&]() {
        installerItems.clear();
        installerItems.push_back({installed ? "Re-install sLaunch" : "Install sLaunch", Act::Install});
        installerItems.push_back({"Check for updates", Act::Update});
        if (installed) installerItems.push_back({"Remove sLaunch", Act::Remove});
        installerItems.push_back({"Exit installer", Act::Exit});
    };
    buildMenu();
    int instCursor = 0;
    float instScroll = 0.0f;

    std::string latestTag, zipUrl;   // filled by the update check
    const char *updErr = "";         // failure detail for the Failed screen

    // Dialog state
    int dialogCursor = 1; // 0 = Yes, 1 = No (default to No for safety)
    bool ok = false;

    const u64 freq        = armGetSystemTickFreq();
    const u64 RepeatDelay = (360 * freq) / 1000;
    auto ms = [&](u64 m) { return (m * freq) / 1000; };
    
    int held_v = 0;
    u64 next_v = 0, start_v = 0;

    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 down = padGetButtonsDown(&pad);
        u64 held = padGetButtons(&pad); 


        int dir_v = 0;
        if (held & (HidNpadButton_Up | HidNpadButton_StickLUp | HidNpadButton_StickRUp)) dir_v = -1;
        else if (held & (HidNpadButton_Down | HidNpadButton_StickLDown | HidNpadButton_StickRDown)) dir_v = 1;

        const u64 now = armGetSystemTick();
        bool move_up = false;
        bool move_down = false;

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

        step(dir_v, held_v, next_v, start_v, move_up, move_down);

        // ---- input ----
        if (screen == Screen::MainMenu) {
            if (move_up)   instCursor = (instCursor + (int)installerItems.size() - 1) % (int)installerItems.size();
            if (move_down) instCursor = (instCursor + 1) % (int)installerItems.size();

            if (down & HidNpadButton_A) {
                switch (installerItems[instCursor].act) {
                    case Act::Install: dialogCursor = 1; screen = Screen::ConfirmInstall; break;
                    case Act::Update:  screen = Screen::Checking; break;
                    case Act::Remove:  dialogCursor = 1; screen = Screen::ConfirmRemove; break;
                    case Act::Exit:    goto cleanup;
                }
            }
            if (down & HidNpadButton_B) goto cleanup;

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

        // ---- draw ----
        FillRect(0, 0, W, H, kBlack);

        if (screen == Screen::MainMenu) {
            time_t now = time(nullptr);
            struct tm tm_now;
            localtime_r(&now, &tm_now);
            char clock[32];
            strftime(clock, sizeof(clock), "%H:%M   %a %b %d", &tm_now);
            Text(g_fS, 40, 16, kDim, clock);

            u32 charge = 0;
            psmGetBatteryChargePercentage(&charge);
            char batt[16];
            snprintf(batt, sizeof(batt), "%lu%%", (unsigned long)charge);
            int bw = TextWidth(g_fS, batt);
            Text(g_fS, W - 40 - bw, 16, kDim, batt);

            Text(g_fL, W / 2, 100, kFg, "sLaunch", true);
            Text(g_fS, W / 2, 160, kDim, "Home menu replacement", true);
            FillRect(W / 2 - 200, 190, 400, 1, kBg2);

            std::vector<MockItem> instMock;
            for (auto &it : installerItems) instMock.push_back({it.label.c_str(), false, false});
            DrawCarousel(instMock, instCursor, instScroll, 340, 48, true);

        } else if (screen == Screen::ConfirmInstall) {
            time_t now = time(nullptr);
            struct tm tm_now;
            localtime_r(&now, &tm_now);
            char clock[32];
            strftime(clock, sizeof(clock), "%H:%M   %a %b %d", &tm_now);
            Text(g_fS, 40, 16, kDim, clock);
            u32 charge = 0;
            psmGetBatteryChargePercentage(&charge);
            char batt[16];
            snprintf(batt, sizeof(batt), "%lu%%", (unsigned long)charge);
            int bw = TextWidth(g_fS, batt);
            Text(g_fS, W - 40 - bw, 16, kDim, batt);
            Text(g_fL, W / 2, 100, kFg, "sLaunch Installer", true);
            Text(g_fS, W / 2, 160, kDim, "A clean HOME Menu replacement", true);
            FillRect(W / 2 - 200, 190, 400, 1, kBg2);
            std::vector<MockItem> instMock;
            for (auto &it : installerItems) instMock.push_back({it.label.c_str(), false, false});
            DrawCarousel(instMock, instCursor, instScroll, 340, 48, true);

            DrawConfirmDialog("Install sLaunch?",
                "This will copy files to your SD card.", dialogCursor, false);

        } else if (screen == Screen::ConfirmRemove) {
            time_t now = time(nullptr);
            struct tm tm_now;
            localtime_r(&now, &tm_now);
            char clock[32];
            strftime(clock, sizeof(clock), "%H:%M   %a %b %d", &tm_now);
            Text(g_fS, 40, 16, kDim, clock);
            u32 charge = 0;
            psmGetBatteryChargePercentage(&charge);
            char batt[16];
            snprintf(batt, sizeof(batt), "%lu%%", (unsigned long)charge);
            int bw = TextWidth(g_fS, batt);
            Text(g_fS, W - 40 - bw, 16, kDim, batt);
            Text(g_fL, W / 2, 100, kFg, "sLaunch Installer", true);
            Text(g_fS, W / 2, 160, kDim, "A clean HOME Menu replacement", true);
            FillRect(W / 2 - 200, 190, 400, 1, kBg2);
            std::vector<MockItem> instMock;
            for (auto &it : installerItems) instMock.push_back({it.label.c_str(), false, false});
            DrawCarousel(instMock, instCursor, instScroll, 340, 48, true);

            DrawConfirmDialog(installed ? "Remove sLaunch?" : "Do nothing?",
                installed ? "This will delete all sLaunch files." : "This will do nothing.",
                dialogCursor, true);

        } else if (screen == Screen::Installing) {
            Text(g_fM, 120, H / 2 - 30, kFg, "Installing sLaunch...");
            int bx = 120, by = H / 2 + 10, bw = 600, bh = 3;
            FillRect(bx, by, bw, bh, kBg2);
            float p = g_total > 0 ? (float)g_done / (float)g_total : 0.0f;
            FillRect(bx, by, (int)(bw * p), bh, kAccent);
            char cnt[64];
            snprintf(cnt, sizeof(cnt), "%d / %d files", g_done, g_total);
            Text(g_fS, 120, H / 2 + 30, kDim, cnt);

        } else if (screen == Screen::Removing) {
            Text(g_fM, 120, H / 2 - 30, kFg, installed ? "Removing sLaunch..." : "Doing nothing...");
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
                DrawProgress("Installing sLaunch...",
                             (double)g_done / (double)g_total, nullptr);
            });
            installed = ok;
            rebootAfter = ok;
            buildMenu();
            screen = ok ? Screen::Done : Screen::Failed;
        }

        if (screen == Screen::Removing && g_total == 0) {
            ok = RemoveEverything();          // qlaunch override + the whole slaunch/ tree
            installed = IsInstalled();
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
            installed = IsInstalled();
            rebootAfter = ok;
            buildMenu();
            if (!ok) updErr = "Could not extract the update";
            screen = ok ? Screen::Done : Screen::Failed;
        }
    }

cleanup:
    if (icon) SDL_DestroyTexture(icon);
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