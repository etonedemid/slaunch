#include <switch.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <SDL2/SDL_image.h>
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
            if (!CopyFile(s, t)) ok = false;
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
                        Installing, Removing, Done, Failed };
    Screen screen = Screen::MainMenu;

    bool installed = IsInstalled();

    // Main installer menu
    struct InstallerItem { const char *label; };
    std::vector<InstallerItem> installerItems = {
        {installed ? "Re-install sLaunch" : "Install sLaunch"},
        {installed ? "Remove sLaunch" : " "},
        {"Exit installer"},
    };
    int instCursor = 0;
    float instScroll = 0.0f;

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
                switch (instCursor) {
                    case 0: // Install / Re-install
                        dialogCursor = 1;
                        screen = Screen::ConfirmInstall;
                        break;
                    case 1: // Remove (blank + ignored when not installed)
                        if (installed) {
                            dialogCursor = 1;
                            screen = Screen::ConfirmRemove;
                        }
                        break;
                    case 2: // Exit
                        goto cleanup;
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

        } else if (screen == Screen::Installing || screen == Screen::Removing) {
            // no input during operation

        } else if (screen == Screen::Done || screen == Screen::Failed) {
            if (down & (HidNpadButton_A | HidNpadButton_B)) goto cleanup;
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
            for (auto &it : installerItems) instMock.push_back({it.label, false, false});
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
            for (auto &it : installerItems) instMock.push_back({it.label, false, false});
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
            for (auto &it : installerItems) instMock.push_back({it.label, false, false});
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
            Text(g_fM, 120, H / 2 - 20, kAccent, "Done", true);
            Text(g_fS, 120, H / 2 + 20, kDim, "Please reboot", true);
            Text(g_fS, 120, H - 40, kDim, "Press A to exit");

        } else if (screen == Screen::Failed) {
            Text(g_fM, 120, H / 2 - 20, kRed, "Operation failed", true);
            Text(g_fS, 120, H / 2 + 20, kDim, "Check your SD card and try again", true);
            Text(g_fS, 120, H - 40, kDim, "Press A to exit");
        }

        SDL_RenderPresent(g_ren);

        // ---- operation logic (run once after first draw) ----
        if (screen == Screen::Installing && g_total == 0) {
            CountTree("romfs:/payload");
            if (g_total == 0) g_total = 1;
            ok = CopyTree("romfs:/payload", "sdmc:", [&]() {
                FillRect(0, 0, W, H, kBlack);
                Text(g_fM, 120, H / 2 - 30, kFg, "Installing sLaunch...");
                int bx = 120, by = H / 2 + 10, bw = 600, bh = 3;
                FillRect(bx, by, bw, bh, kBg2);
                float p = (float)g_done / (float)g_total;
                FillRect(bx, by, (int)(bw * p), bh, kAccent);
                char cnt[64];
                snprintf(cnt, sizeof(cnt), "%d / %d files", g_done, g_total);
                Text(g_fS, 120, H / 2 + 30, kDim, cnt);
                SDL_RenderPresent(g_ren);
            });
            installed = ok;
            screen = ok ? Screen::Done : Screen::Failed;
        }

        if (screen == Screen::Removing && g_total == 0) {
            if (installed) {
                CountTree("sdmc:/atmosphere/contents/0100000000001000");
                if (g_total == 0) g_total = 1;
                ok = DeleteTree("sdmc:/atmosphere/contents/0100000000001000");
                ok = DeleteTree("sdmc:/slaunch/bin");
                installed = !ok; 
                screen = ok ? Screen::Done : Screen::Failed;
            }
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