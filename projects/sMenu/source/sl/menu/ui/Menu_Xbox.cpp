#include <sl/menu/ui/Menu.hpp>
#include <sl/menu/ui/Locale.hpp>
#include <SDL3_image/SDL_image.h>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <ctime>
#include <algorithm>
#include <sys/stat.h>
#include <dirent.h>
#include <map>
#include <vector>
#include <sl/menu/net/Http.hpp>
#include <sl/menu/net/ContentFilter.hpp>
#include "Menu_Internal.hpp"
// The JSON reader comes with the Discord client, so the game info is built
// where that is (the console's qext build and the simulator), not elsewhere.
#if __has_include("third_party/yyjson.h")
#include "third_party/yyjson.h"
#define SL_GAME_INFO 1
#endif

// The two Xbox 360 dashboards that came before Metro, drawn from scratch.
//
// Blades (2005): every category is a curved blade. The open one fills the
// screen in its own colour, the rest stack up as slivers at either edge with
// their names running along them, and moving sideways sweeps a blade across.
// The gamer card sits top right, the list in a pale panel under the title.
//
// NXE (2008): the channels are names running down the left, and the open
// one's entries are a row of panels going off into the distance; the next
// channel's row peeks in at the bottom, and moving down brings it up.
//
// Both are views of the bar XMB already builds (m_xmb_cols, m_xmb_col,
// m_xmb_item and their eased scrolls), so the item model, searching and the
// letter jump are shared; NXE only swaps the two axes on the way in.
namespace sl::menu::ui {

    namespace {
        constexpr int   W = gfx::Gfx::Width, H = gfx::Gfx::Height;
        constexpr SDL_Color kWhite{ 255, 255, 255, 255 };
        enum { XF_Light, XF_Semi, XF_Bold };   // the faces MText knows
        const char *const kXbFont = "sdmc:/slaunch/fonts/metro/selawksl.ttf";

        // ---- Blades ---------------------------------------------------------
        constexpr float kBlBow   = 24.0f;   // how far a stack leans on the open blade at mid height
        constexpr int   kBlRowH  = 46;
        constexpr int   kBlRows  = 6;
        constexpr int   kBlListY = 236;
        constexpr int   kBlListW = 500;
        constexpr int   kBlTabY  = 150;     // where a blade's name tab starts
        constexpr int   kBlStrip = 6;       // a blade drawn by hand (no baked tile) goes in strips this tall
        constexpr int   kBlSeg   = 24;      // a baked blade or a face is one band of this many spans:
                                            // the bow is 24 px, so a span's chord is under a tenth of one off it
        constexpr int   kBlCardW = 330, kBlCardH = 96;
        constexpr SDL_Color kBlSilver{ 226, 229, 233, 255 };
        // The baked blades and lighting sLaunch ships (scripts/gen-blades-art.py).
        const char *const kXbArt = "sdmc:/slaunch/ui/blades/";
        // ...and NXE's (scripts/gen-nxe-art.py).
        const char *const kNxArt = "sdmc:/slaunch/ui/nxe/";

        struct BladeGeo {
            int   n = 1;
            float tab = 50.0f, scroll = 0.0f;
            BladeGeo(int count, float at) : n(std::max(1, count)), scroll(at) {
                // A card full of RetroArch playlists is a lot of blades.
                if (n > 1) tab = std::clamp(520.0f / (float)(n - 1), 22.0f, 50.0f);
            }
            // 0 = stacked on the left, 1 = stacked on the right.
            float Across(int c) const { return std::clamp((float)c - scroll, 0.0f, 1.0f); }
            float X(int c) const {
                if (c <= 0) return 0.0f;
                if (c >= n) return (float)W;
                return (float)c * tab + ((float)W - (float)n * tab) * Across(c);
            }
            // The left edge of blade c at height y. Both stacks lean on the open
            // blade: furthest in at mid height, back in their own place top and bottom.
            float Edge(int c, float y) const {
                if (c >= n) return X(c);
                const float u = (y - H / 2.0f) / (H / 2.0f);
                return X(c) + kBlBow * (1.0f - u * u) * (1.0f - 2.0f * Across(c));
            }
            // The contents keep clear of the edges where they come in furthest.
            int Left(int c) const  { return (int)(Edge(c, H / 2.0f) + tab) + 30; }
            int Right(int c) const { return (int)Edge(c + 1, H / 2.0f) - 30; }
        };

        // Above 1 it runs toward white, the way a glossy surface washes out.
        SDL_Color Gloss(SDL_Color c, float f) {
            const float m = std::min(f, 1.0f), wsh = std::clamp(f - 1.0f, 0.0f, 1.0f);
            auto ch = [&](Uint8 v) { return (Uint8)((float)v * m + (255.0f - (float)v * m) * wsh); };
            return SDL_Color{ ch(c.r), ch(c.g), ch(c.b), 255 };
        }
        SDL_Color Shade(SDL_Color c, float f) {
            auto ch = [f](Uint8 v) { return (Uint8)std::clamp((float)v * f, 0.0f, 255.0f); };
            return SDL_Color{ ch(c.r), ch(c.g), ch(c.b), 255 };
        }
        // The 360 wrote its blades in lower case. ASCII only: other scripts pass through.
        std::string Lower(std::string s) {
            for (char &ch : s) if (ch >= 'A' && ch <= 'Z') ch = (char)(ch + 32);
            return s;
        }

        // The first row shown, eased along with the selection.
        float BladeTop(float item_scroll, int count) {
            return std::clamp(item_scroll - 3.0f, 0.0f, (float)std::max(0, count - kBlRows));
        }
        SDL_Color Mix(SDL_Color a, SDL_Color b, float t) {
            auto ch = [t](Uint8 x, Uint8 y) { return (Uint8)((float)x + ((float)y - (float)x) * t); };
            return SDL_Color{ ch(a.r, b.r), ch(a.g, b.g), ch(a.b, b.b), 255 };
        }

        // ---- Blades, off the main screen ------------------------------------
        // A settings list fills the blade from under the title band to the legend's.
        constexpr int   kBsY     = 126;
        constexpr int   kBsRowH  = 45;
        constexpr int   kBsRows  = 10;
        constexpr int   kBsMaxW  = 860;
        constexpr int   kBsWideX = 76;      // a full-screen blade's contents keep this far from its sides
        constexpr int   kBsRim   = 12;      // ...and this much of a blade shows at each
        // Its bands are slimmer and nearer the edges than the main screen's, where
        // the screens laid out for the whole display keep their header and legend.
        constexpr int   kBsWideTitleY = 14, kBsWideLegendY = 648, kBsWideBandH = 62;
        float BsTop(float scroll, int count) {
            return std::clamp(scroll - (kBsRows - 1) / 2.0f, 0.0f, (float)std::max(0, count - kBsRows));
        }

        // ---- NXE ------------------------------------------------------------
        // Measured off the 2008 dashboard at 1280x720: the front panel is the
        // dashboard's own 420x320 at (96, 248), and each one behind it is set
        // against the right edge of the one before, smaller and a little lower.
        constexpr int   kNxX      = 96;
        constexpr int   kNxTitleY = 202;    // centre line of the open channel's name
        constexpr int   kNxPitch  = 300;    // finger travel per panel
        constexpr float kNxW[] = { 420, 310, 184, 124, 86, 62, 46, 34, 26, 20 };
        constexpr float kNxH[] = { 320, 238, 186, 156, 134, 118, 106, 96, 88, 82 };
        constexpr float kNxCy  = 407.0f;    // the front panel's centre line
        constexpr int   kNxN   = 10;

        // Panel d places from the front one: left edge, top, size, alpha, light.
        void NxPlace(float d, float &x, float &y, float &w, float &h, float &a, float &lit) {
            if (d >= 0.0f) {
                const int i = std::min((int)d, kNxN - 2);
                const float f = std::min(d - (float)i, 1.0f);
                w = kNxW[i] + (kNxW[i + 1] - kNxW[i]) * f;
                h = kNxH[i] + (kNxH[i + 1] - kNxH[i]) * f;
                // The left edge: the front one's, plus every whole panel ahead of this one.
                x = (float)kNxX;
                for (int k = 0; k < i; k++) x += kNxW[k];
                x += kNxW[i] * f;
                const float cy = kNxCy - (kNxH[0] - h) * 0.07f;
                y = cy - h / 2.0f;
                a = std::clamp((float)(kNxN - 1) - d, 0.0f, 1.0f);
                lit = std::clamp(1.0f - 0.05f * d, 0.6f, 1.0f);
            } else {   // folding away to the left
                w = kNxW[0]; h = kNxH[0];
                x = kNxX + d * (kNxW[0] + 40.0f);
                y = kNxCy - h / 2.0f;
                a = std::clamp(1.0f + d * 1.6f, 0.0f, 1.0f);
                lit = 1.0f;
            }
        }
        // A channel's row, rd channels from the open one: rows above rise and
        // fade, the next one comes up from below.
        void NxRow(float rd, float &yoff, float &a) {
            yoff = rd * 260.0f;
            a = std::clamp(1.0f - std::abs(rd) * 1.4f, 0.0f, 1.0f);
        }
        // The names: the open channel large, the ones before it stacked above,
        // smaller and fainter the further back; none below.
        float NxNameY(float d) {
            if (d >= 0.0f) return kNxTitleY + d * 50.0f;
            const float u = -d;
            return kNxTitleY - 43.0f * std::min(u, 1.0f) - 27.0f * std::max(0.0f, u - 1.0f);
        }
        float NxNameAlpha(float d) {
            if (d > 0.0f) return std::clamp(1.0f - d * 1.5f, 0.0f, 1.0f);
            return std::clamp(1.0f + d * 0.22f, 0.0f, 1.0f);
        }
        int NxNameSize(float d) {
            const float u = std::abs(d);
            return (int)lroundf(u < 1.0f ? 40.0f - 12.0f * u : std::max(16.0f, 28.0f - 4.0f * (u - 1.0f)));
        }
    } // namespace

    // ---------------------------------------------------------------------------
    // Skins
    // ---------------------------------------------------------------------------
    // Every file is optional; whatever is missing is drawn the built-in way.
    // The categories, in XmbCat order, as skin files and skin.ini keys name them.
    static const char *const kSkinCat[6] = { "settings", "media", "user", "network", "games", "homebrew" };

    // ---- importing a dashboard's own packages ---------------------------------
    // The Xbox 360 dashboard keeps its pictures in XUIZ packages (res.xzp,
    // shrdres.xzp): a big-endian table of { size, offset, name } then the data.
    // Version 1 names are UTF-16, version 3 ones 8-bit. Nothing from them ships
    // with sLaunch; this only reads files the user copied from their console.
    namespace {
        // n bytes of a file from `at`. A skin package can run to 100 MB, so
        // only its table and the few pictures wanted are ever read.
        bool ReadAt(const std::string &path, size_t at, size_t n, std::string &out) {
            out.clear();
            FILE *f = fopen(path.c_str(), "rb");
            if (!f) return false;
            out.resize(n);
            const bool ok = fseek(f, (long)at, SEEK_SET) == 0 && fread(&out[0], 1, n, f) == n;
            fclose(f);
            return ok;
        }
        bool Exists(const std::string &p) { struct stat st {}; return stat(p.c_str(), &st) == 0; }
        u32 BE32(const std::string &d, size_t p) {
            return (u32)(u8)d[p] << 24 | (u32)(u8)d[p + 1] << 16 | (u32)(u8)d[p + 2] << 8 | (u8)d[p + 3];
        }
        // name (lower case, no folder) -> where its bytes are in the file. `d` is the
        // file's head, holding the whole table; `size` the file's length.
        // False: not a package, or a broken one.
        bool XzpIndex(const std::string &d, size_t size, std::map<std::string, std::pair<size_t, size_t>> &out) {
            if (d.size() < 0x16 || d.compare(0, 4, "XUIZ") != 0) return false;
            const u32 ver = BE32(d, 4);
            const unsigned count = (u8)d[0x14] << 8 | (u8)d[0x15];
            struct E { std::string name; u32 off, size; };
            std::vector<E> ents;
            size_t p = 0x16;
            for (unsigned i = 0; i < count; i++) {
                if (p + 9 > d.size()) return false;
                E e{ {}, BE32(d, p + 4), BE32(d, p) };
                const unsigned n = (u8)d[p + 8];
                p += 9;
                const size_t bytes = ver == 1 ? 2u * n : n;
                if (p + bytes > d.size()) return false;
                for (unsigned k = 0; k < n; k++) {   // UTF-16: the low byte is enough for these names
                    const char ch = ver == 1 ? d[p + 2 * k + 1] : d[p + k];
                    e.name += (char)tolower((unsigned char)ch);
                }
                p += bytes;
                const size_t slash = e.name.find_last_of("/\\");
                if (slash != std::string::npos) e.name.erase(0, slash + 1);
                ents.push_back(std::move(e));
            }
            if (size < p) return false;
            for (const E &e : ents) {
                if ((size_t)e.off + e.size > size - p) return false;   // runs past the end
                out.emplace(e.name, std::make_pair(p + e.off, (size_t)e.size));
            }
            return true;
        }

        // A community remake of Blades paints both stacks on one screen-sized
        // picture with a clear middle (fond1.png), names and all. The innermost
        // right-hand blade becomes the tile every blade is drawn from: followed
        // down its leading edge so it comes out straight, and with the name
        // wiped off its tab (those rows are bridged from the clean ones either
        // side). Its mirror image serves the left-hand stack.
        bool CutBladeTile(const std::string &png, const std::string &out_left, const std::string &out_right) {
            SDL_IOStream *io = SDL_IOFromConstMem(png.data(), png.size());
            SDL_Surface *raw = io ? IMG_Load_IO(io, true) : nullptr;
            if (!raw) return false;
            SDL_Surface *s = SDL_ConvertSurface(raw, SDL_PIXELFORMAT_RGBA32);
            SDL_DestroySurface(raw);
            if (!s) return false;
            const int w = s->w, h = s->h, sw = w * 56 / 1280;
            auto src = [&](int x, int y) {
                return (const Uint8 *)s->pixels + y * s->pitch + 4 * std::clamp(x, 0, w - 1);
            };
            SDL_Surface *o = (w >= 640 && h >= 360 && src(w / 2, h / 2)[3] < 128)
                           ? SDL_CreateSurface(sw, h, SDL_PIXELFORMAT_RGBA32) : nullptr;
            if (!o) { SDL_DestroySurface(s); return false; }
            auto dst = [&](int x, int y) { return (Uint8 *)o->pixels + y * o->pitch + 4 * x; };
            int found = 0;
            for (int y = 0; y < h; y++) {
                int xi = w / 2;
                while (xi < w && src(xi, y)[3] < 128) xi++;
                if (xi >= w) { memset(dst(0, y), 0, 4 * sw); continue; }
                found++;
                // Column 0 sits a pixel before the edge, so its soft pixel comes along.
                const float x0 = (float)xi - src(xi - 1, y)[3] / 255.0f - 1.0f;
                for (int j = 0; j < sw; j++) {
                    const float x = x0 + (float)j, f = x - floorf(x);
                    const Uint8 *a = src((int)floorf(x), y), *b = src((int)floorf(x) + 1, y);
                    // Weighed by coverage, so a clear pixel's colour does not leak into the edge.
                    const float wa = a[3] * (1.0f - f), wb = b[3] * f, al = wa + wb;
                    Uint8 *d = dst(j, y);
                    for (int k = 0; k < 3; k++) d[k] = al > 0.0f ? (Uint8)((a[k] * wa + b[k] * wb) / al + 0.5f) : 0;
                    d[3] = (Uint8)(al + 0.5f);
                }
            }
            SDL_DestroySurface(s);
            // The name: the longest run of rows with something dark across the tab's
            // middle (the tab's own outline is a row or two by itself).
            const int c0 = sw * 30 / 100, c1 = sw * 72 / 100;
            int best = 0, b0 = 0, b1 = 0, s0 = -1, last = -1;
            for (int y = 0; y < std::min(h, h / 2 + h / 9); y++) {
                int mx = 0, mn = 255;
                for (int x = c0; x < c1; x++) {
                    const Uint8 *d = dst(x, y);
                    const int lum = (d[0] + d[1] + d[2]) / 3;
                    mx = std::max(mx, lum); mn = std::min(mn, lum);
                }
                if (mx - mn <= 40) continue;
                if (s0 < 0 || y - last > 8) s0 = y;
                last = y;
                if (last - s0 > best) { best = last - s0; b0 = s0; b1 = last; }
            }
            if (best > h / 60) {
                const int ya = std::max(0, b0 - 4), yb = std::min(h - 1, b1 + 4);
                for (int y = ya + 1; y < yb; y++) {
                    const float t = (float)(y - ya) / (float)(yb - ya);
                    for (int i = 0; i < 4 * sw; i++)
                        dst(0, y)[i] = (Uint8)(dst(0, ya)[i] * (1.0f - t) + dst(0, yb)[i] * t + 0.5f);
                }
            }
            bool ok = found > h * 3 / 4 && IMG_SavePNG(o, out_right.c_str());
            ok = ok && SDL_FlipSurface(o, SDL_FLIP_HORIZONTAL) && IMG_SavePNG(o, out_left.c_str());
            SDL_DestroySurface(o);
            return ok;
        }

        // skin file <- the first package picture present.
        struct Pick { const char *dst; const char *src[3]; };
        const Pick kPickBoth[] = {
            { "button_a", { "a-button.png" } },
            { "button_b", { "b-button.png" } },
            { "button_x", { "x-button.png", "btn_x.png" } },
            { "button_y", { "y-button.png", "btn_y.png" } },
            { "highlight", { "highlight.png" } },
        };
        const Pick kPickBlades[] = { { "panel", { "panel_1.png" } }, { "detail", { "panel_2inset.png" } } };
    }

    // made[0] / made[1]: the Blades / NXE skin this made, if any.
    int Menu::XboxSkinImport(std::string *made_names) {
        int made = 0;
        for (const char *layout : { "blades", "nxe" }) {
            const std::string in = std::string(kSkinDir) + "/import/" + layout;
            DIR *dir = opendir(in.c_str());
            if (!dir) continue;
            // Every package in the folder, merged; the first copy of a name wins.
            // The dashboard's own packages make the skin "dashboard"; a skin
            // somebody built is a skin of its own, under its file's name.
            std::vector<std::string> pkgs;
            std::map<std::string, std::pair<int, std::pair<size_t, size_t>>> files;
            std::string name = "dashboard";
            while (struct dirent *e = readdir(dir)) {
                const char *dot = strrchr(e->d_name, '.');
                if (!dot || strcasecmp(dot, ".xzp")) continue;
                const std::string path = in + "/" + e->d_name;
                struct stat st {};
                std::string head;
                std::map<std::string, std::pair<size_t, size_t>> idx;
                if (stat(path.c_str(), &st) != 0 ||
                    !ReadAt(path, 0, std::min<size_t>((size_t)st.st_size, 1u << 20), head) ||
                    !XzpIndex(head, (size_t)st.st_size, idx)) continue;
                pkgs.push_back(path);
                for (auto &kv : idx) files.emplace(kv.first, std::make_pair((int)pkgs.size() - 1, kv.second));
                std::string stem;
                for (const char *c = e->d_name; c < dot && stem.size() < 32; c++)
                    stem += isalnum((unsigned char)*c) ? (char)tolower((unsigned char)*c) : '_';
                if (stem != "res" && stem != "shrdres" && stem != "mediasite" && !stem.empty()) name = stem;
            }
            closedir(dir);
            if (files.empty()) continue;
            const std::string out = std::string(kSkinDir) + "/" + layout + "/" + name;
            mkdir(kSkinDir, 0777);
            mkdir((std::string(kSkinDir) + "/" + layout).c_str(), 0777);
            mkdir(out.c_str(), 0777);
            int wrote = 0;
            auto fetch = [&](const char *src, std::string &d) {
                auto f = files.find(src);
                const size_t n = f == files.end() ? 0 : f->second.second.second;
                return n >= 8 && n <= (32u << 20) && ReadAt(pkgs[f->second.first], f->second.second.first, n, d) &&
                       d.compare(0, 4, "\x89PNG") == 0;
            };
            auto take = [&](const Pick &pk) {
                std::string d;
                for (const char *src : pk.src) {
                    if (!src) break;
                    if (!fetch(src, d)) continue;
                    if (FILE *w = fopen((out + "/" + pk.dst + ".png").c_str(), "wb")) {
                        const bool ok = fwrite(d.data(), 1, d.size(), w) == d.size();
                        fclose(w);
                        if (ok) { wrote++; return; }
                    }
                }
            };
            for (const Pick &pk : kPickBoth) take(pk);
            if (!strcmp(layout, "blades")) {
                for (const Pick &pk : kPickBlades) take(pk);
                std::string d;
                if (fetch("fond1.png", d) && CutBladeTile(d, out + "/spine.png", out + "/spine_right.png")) wrote++;
            } else {
                // Other dashboards' packages (FreeStyle Dash and the like) name
                // their pictures their own way: the biggest picture whose name
                // has the word in it, for whatever the exact names missed.
                auto takeLike = [&](const char *dst, std::initializer_list<const char *> words) {
                    if (Exists(out + "/" + dst + ".png")) return;
                    const std::string *best = nullptr;
                    size_t best_n = 0;
                    for (auto &kv : files) {
                        const std::string &n = kv.first;
                        if (n.size() < 5 || n.compare(n.size() - 4, 4, ".png")) continue;
                        bool hit = false;
                        for (const char *w : words) hit = hit || n.find(w) != std::string::npos;
                        if (hit && kv.second.second.second > best_n) { best = &n; best_n = kv.second.second.second; }
                    }
                    if (best) { const Pick pk{ dst, { best->c_str() } }; take(pk); }
                };
                takeLike("background", { "background", "wallpaper", "bg" });
                // The 360 dashboard's own panel_bg.png is corner pieces round a
                // see-through middle, not a list panel: sLaunch's is used instead.
                if (name != "dashboard") takeLike("panel", { "panel", "window", "box" });
                takeLike("highlight", { "highlight", "select", "focus", "hover" });
            }
            const std::string ini = out + "/skin.ini";
            struct stat st {};
            if (stat(ini.c_str(), &st) != 0)   // a hand-edited one is kept
                if (FILE *w = fopen(ini.c_str(), "w")) {
                    fputs("; made from the packages in skins/import - edit freely, see skins/README.txt\n"
                          "panel_border = 16\nhighlight_border = 16\n", w);
                    fclose(w);
                }
            if (!wrote) continue;
            // Done with: renamed, so they are not read again at every visit.
            for (const std::string &u : pkgs) rename(u.c_str(), (u + ".done").c_str());
            if (made_names) made_names[!strcmp(layout, "nxe")] = name;
            made++;
        }
        return made;
    }

    struct Menu::XboxSkin {
        std::string dir;                                    // "" = built-in
        std::string themed;                                 // NXE in the theme's colours: what it was built for
        std::vector<SDL_Texture *> tinted;                  // ...its grey panels, drawn in the tint
        SDL_Color tint{}, icon{};                           // ...the colours they carry now
        SDL_Texture *bg = nullptr;                          // NXE: the whole screen
        SDL_Texture *face = nullptr;                        // Blades: any open blade, tinted its colour
        SDL_Texture *spine = nullptr, *spine_r = nullptr;   // Blades: one blade's tile, left / right stack
        SDL_Texture *light = nullptr;                       // Blades: the lighting over an open blade
        SDL_Texture *light_w = nullptr;                     // ...and over one that fills the screen (no bands)
        SDL_Texture *info = nullptr, *star = nullptr, *star_off = nullptr;   // Blades: a game's details
        SDL_Texture *cat_face[6] = {}, *cat_spine[6] = {};  // Blades: one category's, drawn as is
        SDL_Texture *panel = nullptr, *highlight = nullptr, *card = nullptr, *detail = nullptr;
        SDL_Texture *frame = nullptr, *front = nullptr;     // NXE: over every panel / the front one
        SDL_Texture *btn[4] = {};                           // a b x y
        SDL_Texture *tile = nullptr, *floor = nullptr;      // NXE: a panel's face, the stage floor's glow
        std::map<std::string, SDL_Texture *> icons;         // NXE: icon_<name>, the skin's or sLaunch's (NxeIcon)
        SDL_Texture *p_games = nullptr, *p_videos = nullptr, *p_music = nullptr, *p_photos = nullptr;    // NXE: panel faces
        SDL_Texture *shadow = nullptr, *av_shadow = nullptr;   // NXE: right of a panel, under the account picture
        int b_panel = 16, b_highlight = 12, b_card = 12, b_frame = 12;   // nine-slice borders, px
        SDL_Color cat[6] = {};
        bool has_cat[6] = {};
        SDL_Color text{}, dim{}, list_text{}, list_sel{};
        bool has_text = false, has_dim = false, has_list = false, has_sel = false;
    };

    namespace {
        bool ParseHex(const char *s, SDL_Color &c) {
            while (*s == ' ' || *s == '#') s++;
            unsigned v = 0;
            int n = 0;
            if (sscanf(s, "%x%n", &v, &n) != 1 || (n != 6 && n != 8)) return false;
            if (n == 6) v = (v << 8) | 0xFF;
            c = SDL_Color{ (Uint8)(v >> 24), (Uint8)(v >> 16), (Uint8)(v >> 8), (Uint8)v };
            return true;
        }
    }

    Menu::XboxSkin &Menu::Skin() {
        // Packages dropped in skins/import become a skin the first time a
        // dashboard layout is shown, and are used if no skin was chosen yet.
        if (!m_xb_imported) {
            m_xb_imported = true;
            std::string made[2];
            if (XboxSkinImport(made) > 0) {
                // A skin somebody built is put on at once; the dashboard's own only if none is.
                if (!made[0].empty() && (m_skin_blades.empty() || made[0] != "dashboard")) m_skin_blades = made[0];
                if (!made[1].empty() && (m_skin_nxe.empty() || made[1] != "dashboard")) m_skin_nxe = made[1];
                SaveSettings();
                XboxSoundsReload();
                SetStatus(T("Dashboard skin imported"));
            }
        }
        const std::string &name = XboxSkinName();
        const std::string dir = name.empty() ? std::string()
            : std::string(kSkinDir) + (m_ui_mode == UiMode::NXE ? "/nxe/" : "/blades/") + name;
        // NXE in the theme's colours: every panel grey under one tint, every
        // icon in one colour. A change of either (or of theme) builds it again.
        const bool themed = m_ui_mode == UiMode::NXE && m_nxe_themed;
        char key[16] = "";
        if (themed) snprintf(key, sizeof(key), "theme %d", m_theme.CurrentIndex());
        auto same = [](SDL_Color a, SDL_Color b) { return a.r == b.r && a.g == b.g && a.b == b.b; };
        if (m_xskin && m_xskin->dir == dir && m_xskin->themed == key) {
            // A new tint or icon colour is only a colour mod on what is loaded.
            XboxSkin &k = *m_xskin;
            if (themed && !same(k.tint, m_nxe_tint)) {
                for (SDL_Texture *t : k.tinted) SDL_SetTextureColorMod(t, m_nxe_tint.r, m_nxe_tint.g, m_nxe_tint.b);
                k.tint = m_nxe_tint;
                const int luma = (m_nxe_tint.r * 299 + m_nxe_tint.g * 587 + m_nxe_tint.b * 114) / 1000;
                k.list_sel = luma > 165 ? SDL_Color{ 25, 25, 27, 255 } : SDL_Color{ 255, 255, 255, 255 };
            }
            if (themed && !same(k.icon, m_nxe_icon)) {
                for (auto &kv : k.icons) if (kv.second) SDL_SetTextureColorMod(kv.second, m_nxe_icon.r, m_nxe_icon.g, m_nxe_icon.b);
                k.icon = m_nxe_icon;
            }
            return k;
        }
        XboxSkinDrop();
        m_xskin = new XboxSkin;
        XboxSkin &s = *m_xskin;
        s.dir = dir;
        s.themed = key;
        s.tint = m_nxe_tint;
        s.icon = m_nxe_icon;
        // The theme's backdrop stands in for the skin's own picture and floor.
        auto load = [&](const std::string &p, const std::string &base) -> SDL_Texture * {
            if (themed && (base == "background" || base == "floor")) return nullptr;
            if (themed && base.compare(0, 7, "button_") != 0 && base.compare(0, 4, "star") != 0) {
                SDL_Texture *t = m_gfx->LoadImageTinted(p.c_str(), m_nxe_tint);
                if (t) s.tinted.push_back(t);
                return t;
            }
            return m_gfx->LoadImage(p.c_str());
        };
        // ...and its text in the theme's colours, dark or light on the tinted bar.
        auto themeText = [&] {
            if (!themed) return;
            const Theme &t = m_theme.At(m_theme.CurrentIndex());
            s.text = s.list_text = t.fg;
            s.dim = t.dim;
            const int luma = (m_nxe_tint.r * 299 + m_nxe_tint.g * 587 + m_nxe_tint.b * 114) / 1000;
            s.list_sel = luma > 165 ? SDL_Color{ 25, 25, 27, 255 } : SDL_Color{ 255, 255, 255, 255 };
            s.has_text = s.has_list = s.has_dim = s.has_sel = true;
        };
        // What sLaunch ships, for whatever the skin leaves alone.
        auto builtin = [&] {
            auto own = [&](const char *file) -> SDL_Texture * {
                const std::string p = std::string(m_ui_mode == UiMode::NXE ? kNxArt : kXbArt) + file;
                return Exists(p) ? load(p, std::string(file, strcspn(file, "."))) : nullptr;
            };
            if (m_ui_mode == UiMode::NXE) {
                if (!s.bg) s.bg = own("background.png");
                if (!s.panel) {
                    s.panel = own("panel.png");
                    s.b_panel = 16;
                    if (!s.has_list) { s.list_text = SDL_Color{ 236, 240, 245, 255 }; s.has_list = true; }
                }
                if (!s.highlight) {
                    s.highlight = own("highlight.png");
                    s.b_highlight = 16;
                    if (!s.has_sel) { s.list_sel = SDL_Color{ 255, 255, 255, 255 }; s.has_sel = true; }
                }
                if (!s.tile) s.tile = own("tile.png");
                // The game details' stars are Blades' (DrawBladesGameInfo).
                auto blades = [&](const char *file) -> SDL_Texture * {
                    const std::string p = std::string(kXbArt) + file;
                    return Exists(p) ? m_gfx->LoadImage(p.c_str()) : nullptr;
                };
                if (!s.star) s.star = blades("star.png");
                if (!s.star_off) s.star_off = blades("star_off.png");
                return;
            }
            if (m_ui_mode != UiMode::Blades) return;
            bool theirs = s.spine != nullptr;
            for (SDL_Texture *t : s.cat_spine) theirs = theirs || t;
            if (!theirs) { s.spine = own("spine.png"); s.spine_r = own("spine_right.png"); }
            if (!s.light) s.light = own("light.png");
            if (!s.light_w) s.light_w = own("light_wide.png");
            if (!s.info) s.info = own("info.png");
            if (!s.star) s.star = own("star.png");
            if (!s.star_off) s.star_off = own("star_off.png");
        };
        if (dir.empty()) { builtin(); themeText(); return s; }
        auto img = [&](const std::string &base) -> SDL_Texture * {
            for (const char *ext : { ".png", ".jpg" }) {
                const std::string p = dir + "/" + base + ext;
                struct stat st {};
                if (stat(p.c_str(), &st) == 0)
                    if (SDL_Texture *t = load(p, base)) return t;
            }
            return nullptr;
        };
        s.bg = img("background");
        s.face = img("blade");
        s.spine = img("spine");
        s.spine_r = img("spine_right");
        s.light = img("light");
        s.light_w = img("light_wide");
        for (int i = 0; i < 6; i++) {
            s.cat_face[i]  = img(std::string("blade_") + kSkinCat[i]);
            s.cat_spine[i] = img(std::string("spine_") + kSkinCat[i]);
        }
        // Older importers put the dashboard's panel_bg.png here, which draws as
        // four corner squares; the shipped dashboard skin has no panel of its own.
        if (!(m_ui_mode == UiMode::NXE && name == "dashboard")) s.panel = img("panel");
        s.highlight = img("highlight");
        s.card = img("gamercard");
        s.detail = img("detail");
        s.frame = img("frame");
        s.front = img("front");
        s.tile = img("tile");
        s.floor = img("floor");
        s.p_videos = img("panel_videos");
        s.p_games = img("panel_games");
        s.p_music = img("panel_music");
        s.p_photos = img("panel_photos");
        s.shadow = img("shadow");
        s.av_shadow = img("avatar_shadow");
        const char *btn[4] = { "button_a", "button_b", "button_x", "button_y" };
        for (int i = 0; i < 4; i++) s.btn[i] = img(btn[i]);
        struct stat st {};
        const std::string font = dir + "/font.ttf";
        m_xb_font = stat(font.c_str(), &st) == 0 ? font : std::string();
        // Text on a picture panel: light on a dark one, dark on a light one,
        // unless skin.ini says otherwise (read below, so it wins).
        for (const char *ext : { ".png", ".jpg" }) {
            SDL_Surface *raw = s.panel ? IMG_Load((dir + "/panel" + ext).c_str()) : nullptr;
            if (!raw) continue;
            if (SDL_Surface *px = SDL_ConvertSurface(raw, SDL_PIXELFORMAT_RGBA32)) {
                // The middle half, away from the frame.
                long sum = 0, n = 0;
                for (int y = px->h / 4; y < px->h * 3 / 4; y++) {
                    const Uint8 *row = (const Uint8 *)px->pixels + y * px->pitch;
                    for (int x = px->w / 4; x < px->w * 3 / 4; x++, n++)
                        sum += (row[4 * x] * 3 + row[4 * x + 1] * 6 + row[4 * x + 2]) * row[4 * x + 3] / 2550;
                }
                SDL_DestroySurface(px);
                if (n && sum / n < 110) {   // dark (or see-through) panel
                    s.list_text = SDL_Color{ 236, 238, 242, 255 };
                    s.has_list = true;
                }
            }
            SDL_DestroySurface(raw);
            break;
        }
        // skin.ini: key = value, sections and ; comments ignored.
        if (FILE *fp = fopen((dir + "/skin.ini").c_str(), "r")) {
            char line[160];
            while (fgets(line, sizeof(line), fp)) {
                char key[48] = {};
                char *eq = strchr(line, '=');
                if (!eq || line[0] == ';' || line[0] == '[') continue;
                sscanf(line, " %47[^ =]", key);
                const char *val = eq + 1;
                for (int i = 0; i < 6; i++)
                    if (!strcmp(key, kSkinCat[i])) s.has_cat[i] = ParseHex(val, s.cat[i]);
                if (!strcmp(key, "text"))              s.has_text = ParseHex(val, s.text);
                if (!strcmp(key, "dim"))               s.has_dim  = ParseHex(val, s.dim);
                if (!strcmp(key, "list_text"))         s.has_list = ParseHex(val, s.list_text);
                if (!strcmp(key, "list_text_selected")) s.has_sel = ParseHex(val, s.list_sel);
                int v = 0;
                if (sscanf(val, "%d", &v) == 1 && v >= 0 && v < 512) {
                    if (!strcmp(key, "panel_border"))     s.b_panel = v;
                    if (!strcmp(key, "highlight_border")) s.b_highlight = v;
                    if (!strcmp(key, "gamercard_border")) s.b_card = v;
                    if (!strcmp(key, "frame_border"))     s.b_frame = v;
                }
            }
            fclose(fp);
        }
        builtin();
        themeText();
        return s;
    }

    // A panel's glyph, by name: the skin's icon_<name>, else the one sLaunch
    // draws (scripts/gen-nxe-art.py). Loaded on first use; nullptr is kept too.
    SDL_Texture *Menu::NxeIcon(const char *name) {
        XboxSkin &s = Skin();
        auto f = s.icons.find(name);
        if (f != s.icons.end()) return f->second;
        SDL_Texture *t = nullptr;
        std::vector<std::string> paths;
        if (!s.dir.empty())
            for (const char *ext : { ".png", ".jpg" }) paths.push_back(s.dir + "/icon_" + name + ext);
        paths.push_back(std::string(kNxArt) + "icon_" + name + ".png");
        for (const std::string &p : paths) {
            struct stat st {};
            if (stat(p.c_str(), &st) == 0 &&
                (t = m_nxe_themed ? m_gfx->LoadImageTinted(p.c_str(), m_nxe_icon) : m_gfx->LoadImage(p.c_str()))) break;
        }
        s.icons[name] = t;
        return t;
    }

    void Menu::XboxSkinDrop() {
        if (!m_xskin) return;
        XboxSkin &s = *m_xskin;
        std::vector<SDL_Texture *> all = { s.bg, s.face, s.spine, s.spine_r, s.light, s.light_w, s.panel, s.highlight, s.card,
                                           s.detail, s.frame, s.front, s.info, s.star, s.star_off,
                                           s.tile, s.floor,
                                           s.p_videos, s.p_games, s.p_music, s.p_photos, s.shadow, s.av_shadow };
        all.insert(all.end(), std::begin(s.cat_face), std::end(s.cat_face));
        all.insert(all.end(), std::begin(s.cat_spine), std::end(s.cat_spine));
        all.insert(all.end(), std::begin(s.btn), std::end(s.btn));
        for (auto &kv : s.icons) all.push_back(kv.second);
        for (SDL_Texture *t : all) if (t) m_gfx->FreeImage(t);
        delete m_xskin;
        m_xskin = nullptr;
        m_xb_font.clear();
    }

    // The skin's sounds folder when it has one, else Metro's set.
    std::string Menu::XboxSoundPack() const {
        const std::string &name = m_ui_mode == UiMode::NXE ? m_skin_nxe : m_skin_blades;
        if (!name.empty()) {
            const std::string p = std::string(kSkinDir) + (m_ui_mode == UiMode::NXE ? "/nxe/" : "/blades/")
                                + name + "/sounds";
            struct stat st {};
            if (stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) return p;
        }
        return kMetroSoundDir;
    }

    void Menu::XboxSoundsReload() {
        if (!m_deferred_joined || !m_deferred_audio) return;   // the first load has not happened yet
        m_sfx.Exit();
        m_sfx.Init(true, XboxOld() ? XboxSoundPack().c_str()
                         : m_ui_mode == UiMode::Metro ? kMetroSoundDir : nullptr);
    }

    SDL_Texture *Menu::SkinButton(char k) {
        const int i = k == 'a' ? 0 : k == 'b' ? 1 : k == 'x' ? 2 : k == 'y' ? 3 : -1;
        return i < 0 ? nullptr : Skin().btn[i];
    }

    // Each blade's own colour, as the 360 gave Games green, Media blue, Live
    // orange and System grey. Playlists take turns with the ones left over.
    static SDL_Color BladeColor(int cat, bool playlist, int index) {
        static const SDL_Color kCat[] = {
            { 124, 132, 146, 255 },   // Settings (System)
            {  30, 112, 204, 255 },   // Media
            { 146,  88, 190, 255 },   // User
            { 236, 140,  18, 255 },   // Network (Xbox Live)
            { 104, 178,  32, 255 },   // Games
            {  26, 156, 148, 255 },   // Homebrew
        };
        static const SDL_Color kMore[] = {
            { 200,  66,  66, 255 }, { 66, 96, 200, 255 }, { 190, 110, 40, 255 }, { 60, 150, 110, 255 },
        };
        if (playlist) return kMore[index % 4];
        return kCat[std::clamp(cat, 0, 5)];
    }

    // The line under a selected entry: running, play time, an update waiting.
    std::string Menu::XboxInfo(const MenuItem &sel) {
        std::string info;
        if (sel.app_id != 0 && sel.app_id == m_suspended) info = T("Running");
        else if (sel.is_gamecard)                         info = T("Game card");
        if (const play::PlayInfo *pi = Play(sel.app_id)) {
            if (pi->seconds > 0) {
                if (!info.empty()) info += "   ";
                info += play::FormatPlaytime(pi->seconds) + "   " +
                        play::FormatLastPlayed(pi->last_played);
            }
        }
        if (sel.needs_update) {
            if (!info.empty()) info += "   ";
            info += T("Update available");
        }
        return info;
    }

    namespace {
        std::string InfoPath(u64 id) {
            char p[96];
            snprintf(p, sizeof(p), "sdmc:/slaunch/covers/%016llX_info.txt", (unsigned long long)id);
            return p;
        }
#ifdef SL_GAME_INFO
        std::string UrlQuery(const std::string &s) {
            std::string q;
            for (unsigned char c : s) {
                if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') q += (char)c;
                else { char b[4]; snprintf(b, sizeof(b), "%%%02X", c); q += b; }
            }
            return q;
        }
        const char *JStr(yyjson_val *o, const char *k) { const char *s = yyjson_get_str(yyjson_obj_get(o, k)); return s ? s : ""; }
        yyjson_val *Value(yyjson_val *claim) {
            return yyjson_obj_get(yyjson_obj_get(yyjson_obj_get(claim, "mainsnak"), "datavalue"), "value");
        }
        // A Wikidata claim's qualifier `prop`: the item it names, or "".
        const char *Qual(yyjson_val *claim, const char *prop) {
            yyjson_val *q = yyjson_arr_get_first(yyjson_obj_get(yyjson_obj_get(claim, "qualifiers"), prop));
            return JStr(yyjson_obj_get(yyjson_obj_get(q, "datavalue"), "value"), "id");
        }
        // The items a property names, at most `max`.
        std::vector<std::string> Items(yyjson_val *claims, const char *prop, size_t max) {
            std::vector<std::string> out;
            yyjson_val *c;
            size_t i, n;
            yyjson_arr_foreach(yyjson_obj_get(claims, prop), i, n, c) {
                const char *id = JStr(Value(c), "id");
                if (*id && out.size() < max) out.push_back(id);
            }
            return out;
        }
        // "single-player video game" -> "Single-player", "racing video game" -> "Racing".
        std::string Tidy(std::string s) {
            for (const char *suf : { " video game", " game", " mode" })
                if (s.size() > strlen(suf) && s.compare(s.size() - strlen(suf), strlen(suf), suf) == 0)
                    s.resize(s.size() - strlen(suf));
            if (!s.empty()) s[0] = (char)toupper((unsigned char)s[0]);
            return s;
        }
#endif
    }

    // Worker. A game the store sells gets its genre, maker, date, players and
    // ESRB rating from the store's own search; Wikidata fills what the store
    // has not - all of it for the forwarder of a port of a PC or Xbox game -
    // and gives the critics' score. Every match must really be this game
    // (net::TitlesMatch), so an unknown title shows nothing rather than a namesake.
    void Menu::InfoTrampoline(void *self) {
        Menu *m = static_cast<Menu *>(self);
        GameInfo &gi = m->m_info_result;
        gi = GameInfo{};
        m->m_info_ok = false;
        const std::string path = InfoPath(m->m_info_id);
        if (FILE *f = fopen(path.c_str(), "r")) {      // fetched on an earlier run
            char ln[512];
            while (fgets(ln, sizeof(ln), f)) {
                std::string s(ln);
                while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
                const size_t eq = s.find('=');
                if (eq == std::string::npos) continue;
                const std::string k = s.substr(0, eq), v = s.substr(eq + 1);
                if (k == "genre") gi.genre = v;
                else if (k == "maker") gi.maker = v;
                else if (k == "players") gi.players = v;
                else if (k == "esrb") gi.esrb = v;
                else if (k == "source") gi.source = v;
                else if (k == "year") gi.year = atoi(v.c_str());
                else if (k == "score") gi.score = atoi(v.c_str());
                else if (k == "recommend") gi.recommend = atoi(v.c_str());
            }
            fclose(f);
            if (gi.score >= 0 && gi.source.empty()) gi.source = "OpenCritic";   // written before there was a choice
            m->m_info_ok = true;
            m->m_info_done.store(true, std::memory_order_release);
            return;
        }
#ifdef SL_GAME_INFO
        const std::string name = net::SearchName(m->m_info_name), q = UrlQuery(name);
        std::string body;
        // Only a title id in Nintendo's own range (0x01...) is a game the store
        // can sell. A port's forwarder (0x05..., as the forwarder makers number
        // them) only shares a name with one - "Moonlight", "Call of Duty" - so
        // it goes to Wikidata alone. ponytail: a forwarder given an 0x01 id
        // still asks the store; the store's records carry no title id to check.
        const bool official = (m->m_info_id >> 56) == 0x01;
        // Nothing is written unless the store answered: offline, it is tried again next run.
        const bool online = !q.empty() && (!official || net::Get((
            "https://u3b6gr4ua3-dsn.algolia.net/1/indexes/store_game_en_us"
            "?x-algolia-application-id=U3B6GR4UA3"
            "&x-algolia-api-key=a29c6927638bfd8cee23993e51e721c9"
            "&hitsPerPage=8&attributesToRetrieve=title,platform,softwarePublisher,softwareDeveloper,"
            "releaseDate,playerCount,contentRatingCode,gameGenreLabels"
            "&attributesToHighlight=&attributesToSnippet=&query=" + q).c_str(), body, 10));
        if (online) {
            yyjson_doc *doc = yyjson_read(body.data(), body.size(), 0);
            yyjson_val *best = nullptr, *hit;
            size_t i, n;
            yyjson_arr_foreach(yyjson_obj_get(yyjson_doc_get_root(doc), "hits"), i, n, hit) {
                if (!net::TitlesMatch(JStr(hit, "title"), name)) continue;
                if (!best || !strcmp(JStr(hit, "platform"), "Nintendo Switch")) best = hit;   // Switch before Switch 2
                if (!strcmp(JStr(best, "platform"), "Nintendo Switch")) break;
            }
            if (best) {
                yyjson_val *g;
                size_t gi_i, gi_n;
                yyjson_arr_foreach(yyjson_obj_get(best, "gameGenreLabels"), gi_i, gi_n, g) {
                    if (gi_i >= 3) break;
                    if (!gi.genre.empty()) gi.genre += ", ";
                    gi.genre += yyjson_get_str(g) ? yyjson_get_str(g) : "";
                }
                gi.maker = *JStr(best, "softwarePublisher") ? JStr(best, "softwarePublisher") : JStr(best, "softwareDeveloper");
                gi.year = atoi(JStr(best, "releaseDate"));                 // "2018-12-07T..."
                gi.players = JStr(best, "playerCount");
                gi.esrb = JStr(best, "contentRatingCode");
                if (gi.esrb == "E10") gi.esrb += "+";
            }
            yyjson_doc_free(doc);

            const std::string api = "https://www.wikidata.org/w/api.php?format=json";
            // The game of that name (or alias: "Need for Speed Most Wanted" is
            // one) whose description gives the store's year, else the first
            // video game of that name - never the franchise, a series or a
            // bundle. ponytail: ten hits - a game ranked below ten namesakes
            // shows nothing from here.
            std::string entity, fallback;
            const std::string year = gi.year > 0 ? std::to_string(gi.year) : "";
            // False once Wikidata fails to answer (it rate-limits with 429):
            // what the store gave is shown, but not kept, so a later run asks again.
            bool wd = net::Get((api + "&action=wbsearchentities&language=en&type=item&limit=10&search=" + q).c_str(), body, 10);
            if (wd) {
                doc = yyjson_read(body.data(), body.size(), 0);
                yyjson_arr_foreach(yyjson_obj_get(yyjson_doc_get_root(doc), "search"), i, n, hit) {
                    const char *desc = JStr(hit, "description");
                    if (!strstr(desc, "game") || strstr(desc, "franchise") || strstr(desc, "series") ||
                        strstr(desc, "bundle") || strstr(desc, "character")) continue;
                    if (!net::TitlesMatch(JStr(hit, "label"), name) &&
                        !net::TitlesMatch(JStr(yyjson_obj_get(hit, "match"), "text"), name)) continue;
                    if (!year.empty() && strstr(desc, year.c_str())) { entity = JStr(hit, "id"); break; }
                    if (fallback.empty() && strstr(desc, "video game")) fallback = JStr(hit, "id");
                }
                yyjson_doc_free(doc);
            }
            if (entity.empty()) entity = fallback;
            if (!entity.empty() && !(wd = net::Get((api + "&action=wbgetentities&props=claims&ids=" + UrlQuery(entity)).c_str(), body, 15)))
                entity.clear();
            if (!entity.empty()) {
                doc = yyjson_read(body.data(), body.size(), 0);
                yyjson_val *claims = yyjson_obj_get(yyjson_obj_get(yyjson_obj_get(yyjson_doc_get_root(doc), "entities"), entity.c_str()), "claims");
                // The score: OpenCritic's top critics, else Metacritic's best
                // platform, else Kritikanstvo's - the aggregators Wikidata keeps.
                int oc = -1, mc = -1, kr = -1;
                yyjson_val *c;
                yyjson_arr_foreach(yyjson_obj_get(claims, "P444"), i, n, c) {
                    const char *v = yyjson_get_str(Value(c)), *by = Qual(c, "P447");
                    if (!v) continue;
                    if (!strcmp(by, "Q21039459")) {                                    // OpenCritic
                        if (!strcmp(Qual(c, "P459"), "Q114712322")) oc = atoi(v);           // top critic average, "89/100"
                        else if (!strcmp(Qual(c, "P459"), "Q119576498")) gi.recommend = atoi(v); // critics recommend, "97%"
                    } else if (strstr(v, "/100")) {
                        if (!strcmp(by, "Q150248")) mc = std::max(mc, atoi(v));           // Metacritic
                        else if (!strcmp(by, "Q117190337")) kr = std::max(kr, atoi(v));   // Kritikanstvo
                    }
                }
                if (oc >= 0)      { gi.score = oc; gi.source = "OpenCritic"; }
                else if (mc >= 0) { gi.score = mc; gi.source = "Metacritic"; }
                else if (kr >= 0) { gi.score = kr; gi.source = "Kritikanstvo"; }
                // The rest, where the store had nothing to say.
                yyjson_arr_foreach(yyjson_obj_get(claims, "P577"), i, n, c) {     // first release
                    const int y = atoi(JStr(Value(c), "time") + 1);                      // "+2003-10-29T..."
                    if (y > 0 && (!best || !gi.year) && (gi.year == 0 || y < gi.year)) gi.year = y;
                }
                std::vector<std::string> genre, maker, mode, esrb;
                if (gi.genre.empty())   genre = Items(claims, "P136", 3);
                if (gi.maker.empty())   { maker = Items(claims, "P123", 1); if (maker.empty()) maker = Items(claims, "P178", 1); }
                if (gi.players.empty()) mode = Items(claims, "P404", 3);
                if (gi.esrb.empty())    esrb = Items(claims, "P852", 1);
                yyjson_doc_free(doc);
                std::string ids;
                for (const auto *v : { &genre, &maker, &mode, &esrb })
                    for (const std::string &id : *v) ids += (ids.empty() ? "" : "|") + id;
                std::map<std::string, std::string> label;
                if (!ids.empty() && !(wd = net::Get((api + "&action=wbgetentities&props=labels&languages=en%7Cmul&ids=" + UrlQuery(ids)).c_str(), body, 10)))
                    ids.clear();
                if (!ids.empty()) {
                    doc = yyjson_read(body.data(), body.size(), 0);
                    yyjson_val *k, *e;
                    yyjson_obj_foreach(yyjson_obj_get(yyjson_doc_get_root(doc), "entities"), i, n, k, e) {
                        yyjson_val *ls = yyjson_obj_get(e, "labels");
                        const char *en = JStr(yyjson_obj_get(ls, "en"), "value");
                        label[yyjson_get_str(k)] = *en ? en : JStr(yyjson_obj_get(ls, "mul"), "value");
                    }
                    yyjson_doc_free(doc);
                }
                auto join = [&](const std::vector<std::string> &v) {
                    std::string s;
                    for (const std::string &id : v)
                        if (!label[id].empty()) s += (s.empty() ? "" : ", ") + Tidy(label[id]);
                    return s;
                };
                if (gi.genre.empty())   gi.genre = join(genre);
                if (gi.maker.empty())   gi.maker = maker.empty() ? "" : label[maker[0]];
                if (gi.players.empty()) gi.players = join(mode);
                if (gi.esrb.empty())    gi.esrb = esrb.empty() ? "" : label[esrb[0]];   // "Mature 17+"
                // Not in the store, and Wikidata names no publisher: a namesake.
                // A forwarder for a tool (Moonlight, Checkpoint) finds some
                // obscure game of that name with a genre and nothing else;
                // every real game a port is made of has a publisher.
                if (!best && gi.maker.empty()) gi = GameInfo{};
            }

            mkdir("sdmc:/slaunch/covers", 0777);
            if (FILE *f = wd ? fopen(path.c_str(), "w") : nullptr) {
                fprintf(f, "genre=%s\nmaker=%s\nplayers=%s\nesrb=%s\nsource=%s\nyear=%d\nscore=%d\nrecommend=%d\n",
                        gi.genre.c_str(), gi.maker.c_str(), gi.players.c_str(), gi.esrb.c_str(),
                        gi.source.c_str(), gi.year, gi.score, gi.recommend);
                fclose(f);
            }
            m->m_info_ok = true;
        }
#endif
        m->m_info_done.store(true, std::memory_order_release);
    }

    const Menu::GameInfo *Menu::BladesInfo(const MenuItem &it) {
        if (m_info_running && m_info_done.load(std::memory_order_acquire)) {
            threadWaitForExit(&m_info_thread);
            threadClose(&m_info_thread);
            m_info_running = false;
            if (m_info_ok) m_info[m_info_id] = std::move(m_info_result);
        }
        const u64 key = it.kind == ItemKind::Game ? it.app_id : 0;
        if (!key) return nullptr;
        const auto f = m_info.find(key);
        if (f != m_info.end()) return &f->second;
        // One request at a time, once per game per run, and only for the row
        // the list has settled on - not every game scrolled past.
        if (m_info_running || m_info_tried.count(key) ||
            std::abs(m_xmb_item_scroll - (float)m_xmb_item) > 0.05f ||
            net::ContentFilter::ShouldFilterGameByName(it.name))
            return nullptr;
        m_info_tried.insert(key);
        m_info_id = key;
        m_info_name = it.name;
        m_info_done.store(false, std::memory_order_release);
        if (R_SUCCEEDED(threadCreate(&m_info_thread, &Menu::InfoTrampoline, this, nullptr, 0x10000, 0x3B, -2))) {
            threadStart(&m_info_thread);
            m_info_running = true;
        }
        return nullptr;
    }

    // Shared by both: the bar's two eased positions. False: nothing to draw.
    bool Menu::XboxStep() {
        m_icons.SetScale(gfx::IconCache::GridScale);
        if (m_items.empty() || m_xmb_cols.empty()) {
            DrawTopBar(nullptr);
            DrawMainEmpty();
            return false;
        }
        if (!m_xmb_placed) XmbOpenDefaultColumn();
        if (m_xmb_col < 0) { m_xmb_col = 0; m_xmb_item = 0; }
        const float cd = (float)m_xmb_col - m_xmb_col_scroll;
        if (m_ui_mode == UiMode::Blades) {
            // A blade crosses at a steady pace and settles at the end. A plain
            // ease covers half the way in its first three frames, which reads
            // as a jump; this keeps the frames it does have evenly spaced, and
            // keeps up when several blades are asked for at once.
            const float cap = 0.17f * std::max(1.0f, std::abs(cd));
            m_xmb_col_scroll += std::clamp(cd * 0.38f, -cap, cap);
        } else {
            m_xmb_col_scroll += cd * 0.20f;
        }
        if (!ScrollBusy())   // a finger or a throw owns the scroll instead
            m_xmb_item_scroll += ((float)m_xmb_item - m_xmb_item_scroll) * 0.26f;
        if (std::abs((float)m_xmb_col  - m_xmb_col_scroll)  < 0.004f) m_xmb_col_scroll  = (float)m_xmb_col;
        if (std::abs((float)m_xmb_item - m_xmb_item_scroll) < 0.004f) m_xmb_item_scroll = (float)m_xmb_item;
        return true;
    }

    // ---------------------------------------------------------------------------
    // Blades, everywhere
    // ---------------------------------------------------------------------------
    bool Menu::BladesUi() const {
        if (m_ui_mode != UiMode::Blades) return false;
        switch (m_screen) {
            // Setup and the splash keep the theme's look, and so do the screens
            // where a theme is picked and edited: there the theme is the subject.
            case Screen::Oobe: case Screen::Welcome:
            case Screen::Themes: case Screen::ThemeEditor: case Screen::ColorPicker:
                return false;
            default: return true;
        }
    }
    bool Menu::BladesWide() const {
        if (m_xmb_cols.empty()) return true;
        switch (m_screen) {
            // A plain list stands in the open blade, between the stacks, as the
            // main screen does. Anything laid out for the whole screen gets it
            // (Fonts among them: its preview line sits where the legend band is).
            case Screen::Main: case Screen::Theming: case Screen::Widgets:
            case Screen::WidgetOptions: case Screen::SysEntries: case Screen::Network:
            case Screen::Bluetooth: case Screen::Power: case Screen::Payloads: case Screen::FlowMenu:
                return false;
            default: return true;
        }
    }
    void Menu::BladesSpan(int &left, int &right) const {
        if (BladesWide()) { left = kBsWideX; right = W - kBsWideX; return; }
        const int n = (int)m_xmb_cols.size(), c = std::clamp(m_xmb_col, 0, n - 1);
        const BladeGeo g(n, m_xmb_col_scroll);
        left = g.Left(c); right = g.Right(c);
    }
    int Menu::BladesTitleY() const { return BladesWide() ? kBsWideTitleY + 12 : 50; }
    int Menu::BladesHintY() const  { return BladesWide() ? kBsWideLegendY + 18 : 633; }
    SDL_Color Menu::BladesColorOf(int c) {
        const XboxSkin &sk = Skin();
        if (c < 0 || c >= (int)m_xmb_cols.size()) return sk.has_cat[0] ? sk.cat[0] : BladeColor(0, false, 0);
        const int cat = (int)m_xmb_cols[c].cat;
        const bool pl = !m_xmb_cols[c].label.empty();
        if (!pl && cat >= 0 && cat < 6 && sk.has_cat[cat]) return sk.cat[cat];
        int index = 0;   // which playlist this is, counting from the left
        for (int i = 0; i < c; i++) if (!m_xmb_cols[i].label.empty()) index++;
        return BladeColor(cat, pl, index);
    }
    // What the other screens draw in while they stand on a blade: white text
    // and pale highlights over shades of the blade's own colour.
    void Menu::BladesTheme(Theme &t) {
        const SDL_Color base = BladesColorOf(m_xmb_col);
        t.bg_top    = Shade(base, 0.60f);
        t.bg_bottom = Shade(base, 0.34f);
        t.fg = t.title = t.icon_fg = kWhite;
        t.dim    = Mix(base, kWhite, 0.78f);
        t.accent = SDL_Color{ 255, 249, 196, 255 };   // lit, and told apart from plain white
    }
    void Menu::XbCard(float x, float y, float w, float h, float r, float shadow, SDL_Color fill, Uint8 a) {
        gfx::Gfx::CardStyle cs;
        cs.radius = r; cs.shadow = shadow; cs.fill = fill;
        if (!m_gfx->Card(nullptr, x, y, w, h, cs, a) && fill.a)
            m_gfx->FillRect((int)x, (int)y, (int)w, (int)h, WithAlpha(fill, (Uint8)(fill.a * a / 255)));
    }

    // The frame every Blades screen stands in: the metal behind, the open
    // blade's lit face, and the stacks of the others either side of it. A
    // screen laid out for the full width gets one blade right across instead,
    // with just the edge of a blade showing at each side.
    void Menu::DrawBladesFrame() {
        const XboxSkin &sk = Skin();
        const int n = (int)m_xmb_cols.size();
        if (BladesWide()) {
            const int oc = n ? std::clamp(m_xmb_col, 0, n - 1) : -1;
            const int ci = (oc < 0 || !m_xmb_cols[oc].label.empty()) ? -1 : std::clamp((int)m_xmb_cols[oc].cat, 0, 5);
            const bool own = ci >= 0 && sk.cat_face[ci];
            const SDL_Color base = BladesColorOf(oc);
            if (SDL_Texture *faceT = own ? sk.cat_face[ci] : sk.face) {
                m_gfx->DrawImageTinted(faceT, 0, 0, W, H, own ? kWhite : base);
            } else {
                m_gfx->FillRect(0, 0, W, H, base);
                if (sk.light_w) m_gfx->DrawImage(sk.light_w, 0, 0, W, H);
                // The title's band and the legend's: glass, lit along the top.
                for (int y : { kBsWideTitleY, kBsWideLegendY }) {
                    const float x = W * 0.035f, w = W * 0.93f;
                    XbCard(x, (float)y, w, (float)kBsWideBandH, 16.0f, 0.0f, SDL_Color{ 0, 0, 0, 34 });
                    XbCard(x, (float)y, w, (float)kBsWideBandH, 16.0f, 0.0f, SDL_Color{ 255, 255, 255, 30 });
                    m_gfx->FillRect((int)x + 16, y, (int)w - 32, 1, SDL_Color{ 255, 255, 255, 64 });
                }
            }
            if (sk.spine) {
                SDL_Texture *rt = sk.spine_r ? sk.spine_r : sk.spine;
                constexpr float tab = 50.0f;
                gfx::Gfx::BandPt lp[kBlSeg + 1], rp[kBlSeg + 1];
                for (int i = 0; i <= kBlSeg; i++) {
                    const float v = (float)i / kBlSeg, u = v * 2.0f - 1.0f, bow = kBlBow * (1.0f - u * u);
                    lp[i] = { kBsRim + bow - tab, kBsRim + bow, v * H, 0.0f, 1.0f, v };
                    rp[i] = { W - kBsRim - bow, W - kBsRim - bow + tab, v * H, 0.0f, 1.0f, v };
                }
                m_gfx->DrawBand(sk.spine, lp, kBlSeg + 1);
                m_gfx->DrawBand(rt, rp, kBlSeg + 1);
            }
            return;
        }
        // Off the main screen nothing is sweeping: the frame stands where it settled.
        if (m_screen != Screen::Main) m_xmb_col_scroll = (float)std::clamp(m_xmb_col, 0, n - 1);
        const BladeGeo g(n, m_xmb_col_scroll);

        // The metal the stacks are cut from, behind everything.
        m_gfx->GradientV(SDL_Color{ 178, 181, 186, 255 }, SDL_Color{ 140, 143, 148, 255 });

        struct Blade { SDL_Color base; float prox; int ci; };
        std::vector<Blade> bl(n);
        for (int c = 0; c < n; c++)
            bl[c] = { BladesColorOf(c), std::max(0.0f, 1.0f - std::abs((float)c - m_xmb_col_scroll)),
                      !m_xmb_cols[c].label.empty() ? -1 : std::clamp((int)m_xmb_cols[c].cat, 0, 5) };
        auto mix = Mix;
        const bool tiles = sk.spine != nullptr;   // baked blades: every edge comes out smooth

        // --- the open blade's face (two, while one sweeps across). Only it shows
        // its colour. Its sides run a little way under the blades beside it,
        // which hide the steps the strips leave ---
        for (int c = 0; c < n; c++) {
            const Blade &b = bl[c];
            if (b.prox <= 0.01f) continue;
            const bool own = b.ci >= 0 && sk.cat_face[b.ci];
            SDL_Texture *faceT = own ? sk.cat_face[b.ci] : sk.face;
            const SDL_Color faceCol = mix(Shade(kBlSilver, 0.80f), b.base, sqrtf(b.prox));
            // The lighting spans the blade's full width, where its sides stand top and bottom.
            const float boxL = g.X(c) + g.tab, boxR = c + 1 < n ? g.X(c + 1) : (float)W;
            // One band from the blade's own tab to the next blade's edge. u: 0 = the
            // picture's left edge, pinned to the screen or to the blade's full width.
            auto face = [&](SDL_Texture *t, SDL_Color col, Uint8 a, bool pinned) {
                gfx::Gfx::BandPt pts[kBlSeg + 1];
                for (int i = 0; i <= kBlSeg; i++) {
                    const float v = (float)i / kBlSeg, y = v * H;
                    const float l = g.Edge(c, y) + g.tab - 3.0f;
                    const float r = std::max(l, c + 1 < n ? g.Edge(c + 1, y) + 3.0f : (float)W);
                    const float ul = pinned ? l / W : std::clamp((l - boxL) / (boxR - boxL), 0.0f, 1.0f);
                    const float ur = pinned ? r / W : std::clamp((r - boxL) / (boxR - boxL), 0.0f, 1.0f);
                    pts[i] = { l, r, y, ul, ur, v };
                }
                m_gfx->DrawBand(t, pts, kBlSeg + 1, col, a);
            };
            if (faceT) face(faceT, own ? kWhite : b.base, 255, true);   // the skin's picture, pinned to the screen
            else {
                face(nullptr, faceCol, 255, true);
                if (sk.light && boxR > boxL) face(sk.light, kWhite, (Uint8)(255.0f * sqrtf(b.prox)), false);
            }
        }

        // --- the blades ---
        for (int c = 0; c < n; c++) {
            const Blade &b = bl[c];
            const SDL_Color base = b.base;
            const float prox = b.prox;
            const bool rstack = g.Across(c) > 0.5f;
            // 0 on the left stack, 1 on the right, eased through the crossing.
            const float ac = std::clamp((g.Across(c) - 0.25f) / 0.5f, 0.0f, 1.0f), side = ac * ac * (3.0f - 2.0f * ac);
            SDL_Texture *ownT = b.ci >= 0 ? sk.cat_spine[b.ci] : nullptr;
            // The 360 wrote these in lower case, running down the tab.
            const std::string name = Lower(b.ci < 0 ? m_xmb_cols[c].label : std::string(XmbCatName(m_xmb_cols[c].cat)));
            TTF_Font *tf = m_gfx->Face(m_xb_font.empty() ? kXbFont : m_xb_font.c_str(), g.tab >= 36.0f ? 22 : 14);
            if (tiles) {
                // One baked tile, bent to the stack's curve: a single band along its edge.
                // The open blade's takes its colour; the rest stay the metal they are.
                const SDL_Color mod = ownT ? kWhite : mix(kWhite, Gloss(base, 1.18f), prox);
                auto tile = [&](SDL_Texture *t, Uint8 a) {
                    gfx::Gfx::BandPt pts[kBlSeg + 1];
                    for (int i = 0; i <= kBlSeg; i++) {
                        const float v = (float)i / kBlSeg, x = g.Edge(c, v * H);
                        pts[i] = { x, x + g.tab, v * H, 0.0f, 1.0f, v };
                    }
                    m_gfx->DrawBand(t, pts, kBlSeg + 1, mod, a);
                };
                // Crossing from one stack to the other its tab changes sides:
                // the right-hand tile fades in over the left-hand one.
                if (ownT || !sk.spine_r || side <= 0.0f) tile(ownT ? ownT : sk.spine, 255);
                else if (side >= 1.0f) tile(sk.spine_r, 255);
                else { tile(sk.spine, 255); tile(sk.spine_r, (Uint8)(255.0f * side)); }
            } else {
                // No baked tile to hand (the files are missing, or an older skin
                // brings a picture per category): the blade drawn in strips.
                const SDL_Color metal = mix(kBlSilver, Gloss(base, 1.12f), prox);
                const int nameW = tf ? m_gfx->TextWidth(tf, name.c_str()) : 0;
                const float tab0 = (float)kBlTabY, tab1 = tab0 + std::max(150.0f, (float)nameW + 40.0f);
                float sw = 0, sh = 0;
                if (ownT) SDL_GetTextureSize(ownT, &sw, &sh);
                for (int y = 0; y < H; y += kBlStrip) {
                    const float my = y + kBlStrip / 2.0f, v = (float)y / (float)H;
                    const int l = (int)floorf(g.Edge(c, my)), tw = (int)g.tab;
                    if (ownT) {
                        m_gfx->DrawRegion(ownT, SDL_FRect{ 0, sh * v, sw, sh * kBlStrip / H },
                                          SDL_FRect{ (float)l, (float)y, (float)tw, (float)kBlStrip }, kWhite);
                        continue;
                    }
                    const float glint = 1.0f - std::abs(v - 0.42f) * 1.1f;
                    const float sl = 0.76f + 0.22f * glint + 0.04f * prox;
                    // How far into the tab this row is; its outer corners are rounded
                    // off by letting the outermost band start later than the next.
                    const float in = std::min(my - tab0, tab1 - my);
                    static const float kAcross[4] = { 1.04f, 1.0f, 0.97f, 0.93f };
                    for (int k = 0; k < 4; k++) {
                        const bool outer = rstack ? k >= 2 : k < 2;
                        const bool edge = rstack ? k == 3 : k == 0;
                        const float on = std::clamp((in - (edge ? 9.0f : 0.0f)) / 6.0f, 0.0f, 1.0f);
                        const SDL_Color col = outer ? mix(Shade(kBlSilver, edge ? 0.90f : 0.82f), metal, on) : metal;
                        m_gfx->FillRect(l + tw * k / 4, y, tw * (k + 1) / 4 - tw * k / 4, kBlStrip,
                                        Shade(col, sl * kAcross[k]));
                    }
                    m_gfx->FillRect(l, y, 2, kBlStrip, SDL_Color{ 255, 255, 255, 90 });
                    m_gfx->FillRect(l - 2, y, 2, kBlStrip, SDL_Color{ 0, 0, 0, 70 });
                }
            }
            if (tf) {
                // A baked tab sits a little off the tile's middle, clear of the blade's edge.
                const float off = tiles ? (side * 6.0f - 3.0f) * g.tab / 50.0f : 0.0f;
                const int lx = (int)(g.Edge(c, kBlTabY + 90.0f) + (g.tab - m_gfx->LineHeight(tf)) / 2.0f + off);
                const int ty = kBlTabY + 22;
                if (ownT) m_gfx->TextDown(tf, lx, ty, kWhite, name.c_str());
                else {   // pressed into the metal
                    const SDL_Color ink = mix(SDL_Color{ 92, 96, 104, 255 }, Shade(base, 0.40f), prox);
                    m_gfx->TextDown(tf, lx - 1, ty + 1, SDL_Color{ 255, 255, 255, 150 }, name.c_str());
                    m_gfx->TextDown(tf, lx, ty, ink, name.c_str());
                }
            }
        }

    }

    // The title in the blade's top band, with the clock and battery at its far end.
    void Menu::DrawBladesHeader(const char *title) {
        const XboxSkin &sk = Skin();
        int left, right;
        BladesSpan(left, right);
        const SDL_Color ink = sk.has_text ? sk.text : kWhite;
        time_t now = time(nullptr);
        struct tm tm_now;
        localtime_r(&now, &tm_now);
        char clock[16], batt[24];
        strftime(clock, sizeof(clock), "%H:%M", &tm_now);
        snprintf(batt, sizeof(batt), sys::Charging() ? "+%lu%%" : "%lu%%", (unsigned long)sys::BatteryPercent());
        const int ty = BladesTitleY();
        int x = right - MWidth(XF_Semi, 18, batt);
        MText(XF_Semi, 18, x, ty + 9, ink, batt);
        x -= 20 + MWidth(XF_Semi, 18, clock);
        MText(XF_Semi, 18, x, ty + 9, WithAlpha(ink, 205), clock);
        DrawUsbTag(x - 20, ty + 9);
        if (title && *title)
            MText(XF_Semi, 30, left, ty, ink, MFit(XF_Semi, 30, title, x - 90 - left).c_str());
    }

    // A settings list as the main screen draws its own: a sheet of glass, a rule
    // under every row, the selection a lit bar with dark text, values at the right.
    void Menu::DrawCarouselBlades(const std::vector<std::string> &labels,
                                  const std::vector<std::string> &values,
                                  int cursor, float &scroll_pos) {
        const XboxSkin &sk = Skin();
        const SDL_Color base = BladesColorOf(m_xmb_col);
        const int n = (int)labels.size();
        scroll_pos += (cursor - scroll_pos) * 0.30f;
        if (std::abs(cursor - scroll_pos) < 0.01f) scroll_pos = (float)cursor;

        int left, right;
        BladesSpan(left, right);
        // The sheet is as tall as what it holds, up to a page.
        const int shown = std::min(n, kBsRows);
        const int pw = std::min(right - left, kBsMaxW), ph = shown * kBsRowH + 16;
        if (sk.panel) m_gfx->Draw9(sk.panel, left, kBsY, pw, ph, sk.b_panel, kWhite);
        else XbCard((float)left, (float)kBsY, (float)pw, (float)ph, 8.0f, 0.0f, SDL_Color{ 255, 255, 255, 24 });
        const bool marker = n > kBsRows;   // more than a page: a marker down the right edge
        const int rw = pw - (marker ? 12 : 0);
        const float top = BsTop(scroll_pos, n);
        const int y0 = kBsY + 8, y1 = y0 + shown * kBsRowH;
        const float by = std::clamp(y0 + (scroll_pos - top) * kBsRowH, (float)y0, (float)(y1 - kBsRowH));
        if (sk.highlight) m_gfx->Draw9(sk.highlight, left + 6, (int)by + 2, rw - 12, kBsRowH - 4, sk.b_highlight, kWhite);
        else {
            XbCard(left + 4.0f, by + 1.0f, rw - 8.0f, kBsRowH - 2.0f, 7.0f, 8.0f, WithAlpha(Gloss(base, 1.45f), 235));
            XbCard(left + 6.0f, by + 3.0f, rw - 12.0f, (kBsRowH - 6.0f) / 2.0f, 6.0f, 0.0f, SDL_Color{ 255, 255, 255, 70 });
        }
        const SDL_Color plain = sk.has_list ? sk.list_text : kWhite;
        const SDL_Color lit = sk.has_sel ? sk.list_sel : (sk.highlight ? kWhite : Shade(base, 0.22f));
        for (int i = std::max(0, (int)top); i < n && i <= (int)top + kBsRows; i++) {
            const float ry = y0 + ((float)i - top) * kBsRowH;
            float vis = 1.0f;
            if (ry < y0) vis = 1.0f - (y0 - ry) / kBsRowH;
            if (ry + kBsRowH > y1) vis = std::min(vis, 1.0f - (ry + kBsRowH - y1) / kBsRowH);
            if (vis <= 0.03f) continue;
            const Uint8 a = (Uint8)(255.0f * vis);
            const bool sel = i == cursor;
            if (!sk.panel && !sel)
                m_gfx->FillRect(left + 10, (int)ry + kBsRowH - 1, rw - 20, 1, SDL_Color{ 255, 255, 255, (Uint8)(a * 70 / 255) });
            const std::string &v = i < (int)values.size() ? values[i] : std::string();
            // A long value gives way to its label, never the other way round.
            const std::string vs = v.empty() ? v : MFit(XF_Semi, 18, v, rw / 2 - 24);
            const int vw = vs.empty() ? 0 : MWidth(XF_Semi, 18, vs.c_str());
            const SDL_Color c = WithAlpha(sel ? lit : plain, a);
            MText(XF_Semi, 21, left + 20, (int)ry + (kBsRowH - 29) / 2, c,
                  MFit(XF_Semi, 21, labels[i], rw - 40 - (vw ? vw + 24 : 0)).c_str());
            if (vw) MText(XF_Semi, 18, left + rw - 20 - vw, (int)ry + (kBsRowH - 25) / 2,
                          WithAlpha(sel ? lit : plain, (Uint8)(a * (sel ? 230 : 190) / 255)), vs.c_str());
        }
        if (marker) {
            const int bh = kBsRows * kBsRowH, th = std::max(28, bh * kBsRows / n);
            const int ty = y0 + (int)((bh - th) * (top / (float)(n - kBsRows)));
            m_gfx->FillRect(left + pw - 9, y0, 3, bh, SDL_Color{ 255, 255, 255, 46 });
            m_gfx->FillRect(left + pw - 9, ty, 3, th, SDL_Color{ 255, 255, 255, 215 });
        }
    }
    int Menu::BladesListRowAt(int y, float scroll, int rows) const {
        return (int)floorf(BsTop(scroll, rows) + (float)(y - (kBsY + 8)) / (float)kBsRowH);
    }

    // Pop-ups: the screen dimmed, and a pale card in the blade's colour with a darker
    // bar for its title, as the gamer card has, centred on the screen. Returns its left edge.
    int Menu::BladesPopup(int cw, int ch, const std::string &title) {
        const SDL_Color base = BladesColorOf(m_xmb_col);
        const int x = (W - cw) / 2, y = std::max(12, (H - ch) / 2);
        m_gfx->FillRect(0, 0, W, H, SDL_Color{ 0, 0, 0, 135 });
        XbCard((float)x, (float)y, (float)cw, (float)ch, 10.0f, 20.0f, WithAlpha(Gloss(base, 1.58f), 250));
        XbCard(x + 3.0f, y + 3.0f, cw - 6.0f, 36.0f, 8.0f, 0.0f, WithAlpha(Shade(base, 0.48f), 250));
        MText(XF_Semi, 19, x + 16, y + 7, kWhite, MFit(XF_Semi, 19, title, cw - 32).c_str());
        return x;
    }
    namespace {
        constexpr int kBpRowH = 44, kBpHead = 48;
    }
    void Menu::DrawBladesOptions() {
        const int n = (int)m_options.size();
        const SDL_Color base = BladesColorOf(m_xmb_col);
        constexpr int cw = 520;
        const int ch = kBpHead + n * kBpRowH + 10, y = std::max(12, (H - ch) / 2);
        const int x = BladesPopup(cw, ch, OptionsTitle());
        for (int i = 0; i < n; i++) {
            const int ry = y + kBpHead + i * kBpRowH;
            const bool sel = i == m_options_cursor;
            if (sel) {   // on the pale card the selection is the dark one
                XbCard(x + 6.0f, ry + 1.0f, cw - 12.0f, kBpRowH - 2.0f, 7.0f, 6.0f, Shade(base, 0.50f));
                XbCard(x + 8.0f, ry + 3.0f, cw - 16.0f, (kBpRowH - 6.0f) / 2.0f, 6.0f, 0.0f, SDL_Color{ 255, 255, 255, 36 });
            } else if (i + 1 < n && i + 1 != m_options_cursor) {
                m_gfx->FillRect(x + 16, ry + kBpRowH - 1, cw - 32, 1, WithAlpha(Shade(base, 0.45f), 70));
            }
            MText(XF_Semi, 21, x + 22, ry + (kBpRowH - 29) / 2, sel ? kWhite : Shade(base, 0.20f),
                  MFit(XF_Semi, 21, m_options[i].label, cw - 44).c_str());
        }
    }
    void Menu::DrawBladesDialog() {
        const SDL_Color base = BladesColorOf(m_xmb_col);
        constexpr int cw = 600;
        // The note, wrapped to the card.
        std::vector<std::string> lines;
        {
            std::string line, word;
            auto flush = [&]() {
                if (word.empty()) return;
                const std::string t = line.empty() ? word : line + " " + word;
                if (!line.empty() && MWidth(XF_Semi, 19, t.c_str()) > cw - 48) { lines.push_back(line); line = word; }
                else line = t;
                word.clear();
            };
            for (char c : m_dialog_note) { if (c == ' ' || c == '\n') flush(); else word += c; }
            flush();
            if (!line.empty()) lines.push_back(line);
        }
        const int nopts = m_dialog == Dialog::CrashReport ? 1 : 2;   // a notice has only OK
        const int noteH = lines.empty() ? 0 : (int)lines.size() * 26 + 14;
        const int ch = kBpHead + noteH + nopts * kBpRowH + 10, y = std::max(12, (H - ch) / 2);
        const int x = BladesPopup(cw, ch, m_dialog_title.empty() ? std::string(T("Close running application?"))
                                                                : m_dialog_title);
        int ty = y + kBpHead + 2;
        for (const std::string &l : lines) { MText(XF_Semi, 19, x + 24, ty, Shade(base, 0.24f), l.c_str()); ty += 26; }
        const char *opts[2] = { T("Yes"), T("No") };
        if (nopts == 1) opts[0] = T("OK");
        for (int i = 0; i < nopts; i++) {
            const int ry = y + kBpHead + noteH + i * kBpRowH;
            const bool sel = i == m_dialog_cursor;
            if (sel) {
                XbCard(x + 6.0f, ry + 1.0f, cw - 12.0f, kBpRowH - 2.0f, 7.0f, 6.0f, Shade(base, 0.50f));
                XbCard(x + 8.0f, ry + 3.0f, cw - 16.0f, (kBpRowH - 6.0f) / 2.0f, 6.0f, 0.0f, SDL_Color{ 255, 255, 255, 36 });
            }
            MText(XF_Semi, 21, x + 22, ry + (kBpRowH - 29) / 2, sel ? kWhite : Shade(base, 0.20f), opts[i]);
        }
    }

    // ---------------------------------------------------------------------------
    // NXE, everywhere
    // ---------------------------------------------------------------------------
    // Every screen stands on NXE's backdrop, its name large at the top left as
    // a channel's is, its list on a sheet of dark glass with the selection a
    // bright bar. A skin replaces any of the pictures (skins/nxe/<name>).
    namespace {
        constexpr int kNxListY = 128, kNxRowH = 46, kNxRows = 10, kNxListW = 820, kNxHintY = 650;
        constexpr SDL_Color kNxInk{ 246, 248, 246, 255 }, kNxDim{ 200, 206, 210, 255 };
        float NxTop(float scroll, int count) {
            return std::clamp(scroll - (kNxRows - 1) / 2.0f, 0.0f, (float)std::max(0, count - kNxRows));
        }
    }
    bool Menu::NxeUi() const {
        if (m_ui_mode != UiMode::NXE) return false;
        switch (m_screen) {
            case Screen::Oobe: case Screen::Welcome:
            case Screen::Themes: case Screen::ThemeEditor: case Screen::ColorPicker:
                return false;
            default: return true;
        }
    }
    void Menu::NxeTheme(Theme &t) {
        if (m_nxe_themed) return;   // the theme's own colours (Theming > NXE colours)
        const XboxSkin &sk = Skin();
        t.bg_top    = SDL_Color{ 120, 170, 40, 255 };
        t.bg_bottom = SDL_Color{ 200, 204, 206, 255 };
        t.fg = t.title = t.icon_fg = sk.has_text ? sk.text : kNxInk;
        t.dim    = sk.has_dim ? sk.dim : SDL_Color{ 236, 240, 236, 255 };
        t.accent = SDL_Color{ 150, 210, 40, 255 };   // the 360's green
    }
    void Menu::DrawNxeFrame() {
        const XboxSkin &sk = Skin();
        if (sk.bg) m_gfx->DrawCover(sk.bg);
        else m_gfx->GradientV(SDL_Color{ 120, 170, 40, 255 }, SDL_Color{ 200, 204, 206, 255 });
        if (sk.floor) m_gfx->DrawImage(sk.floor, -160, 372, 1600, 75);   // the stage's lit edge
    }
    void Menu::DrawNxeHeader(const char *title) {
        const XboxSkin &sk = Skin();
        const SDL_Color ink = sk.has_text ? sk.text : kNxInk;
        time_t now = time(nullptr);
        struct tm tm_now;
        localtime_r(&now, &tm_now);
        char clock[16], batt[24];
        strftime(clock, sizeof(clock), "%H:%M", &tm_now);
        snprintf(batt, sizeof(batt), sys::Charging() ? "+%lu%%" : "%lu%%", (unsigned long)sys::BatteryPercent());
        int x = W - 60 - MWidth(XF_Semi, 18, batt);
        MText(XF_Semi, 18, x, 40, ink, batt);
        x -= 20 + MWidth(XF_Semi, 18, clock);
        MText(XF_Semi, 18, x, 40, WithAlpha(ink, 205), clock);
        DrawUsbTag(x - 20, 40);
        if (title && *title)
            MText(XF_Light, 44, kNxX, 34, ink, MFit(XF_Light, 44, title, x - 60 - kNxX).c_str());
    }
    void Menu::DrawCarouselNxe(const std::vector<std::string> &labels,
                               const std::vector<std::string> &values, int cursor, float &scroll_pos) {
        const XboxSkin &sk = Skin();
        const int n = (int)labels.size();
        scroll_pos += (cursor - scroll_pos) * 0.30f;
        if (std::abs(cursor - scroll_pos) < 0.01f) scroll_pos = (float)cursor;
        const int shown = std::min(n, kNxRows);
        const int left = kNxX - 16, pw = kNxListW, ph = shown * kNxRowH + 16;
        if (sk.panel) m_gfx->Draw9(sk.panel, left, kNxListY, pw, ph, sk.b_panel, kWhite);
        else XbCard((float)left, (float)kNxListY, (float)pw, (float)ph, 12.0f, 0.0f, SDL_Color{ 16, 20, 26, 200 });
        const bool marker = n > kNxRows;
        const int rw = pw - (marker ? 12 : 0);
        const float top = NxTop(scroll_pos, n);
        const int y0 = kNxListY + 8, y1 = y0 + shown * kNxRowH;
        const float by = std::clamp(y0 + (scroll_pos - top) * kNxRowH, (float)y0, (float)(y1 - kNxRowH));
        if (sk.highlight) m_gfx->Draw9(sk.highlight, left + 6, (int)by + 2, rw - 12, kNxRowH - 4, sk.b_highlight, kWhite);
        else XbCard(left + 6.0f, by + 2.0f, rw - 12.0f, kNxRowH - 4.0f, 8.0f, 6.0f, SDL_Color{ 236, 238, 240, 255 });
        const SDL_Color plain = sk.has_list ? sk.list_text : kNxInk;
        const SDL_Color lit = sk.has_sel ? sk.list_sel : kWhite;
        for (int i = std::max(0, (int)top); i < n && i <= (int)top + kNxRows; i++) {
            const float ry = y0 + ((float)i - top) * kNxRowH;
            float vis = 1.0f;
            if (ry < y0) vis = 1.0f - (y0 - ry) / kNxRowH;
            if (ry + kNxRowH > y1) vis = std::min(vis, 1.0f - (ry + kNxRowH - y1) / kNxRowH);
            if (vis <= 0.03f) continue;
            const Uint8 a = (Uint8)(255.0f * vis);
            const bool sel = i == cursor;
            if (!sel && i + 1 != cursor && i + 1 < n)   // a hairline between rows, as NXE ruled its lists
                m_gfx->FillRect(left + 14, (int)ry + kNxRowH - 1, rw - 28, 1, SDL_Color{ 255, 255, 255, (Uint8)(a * 40 / 255) });
            const std::string &v = i < (int)values.size() ? values[i] : std::string();
            const std::string vs = v.empty() ? v : MFit(XF_Semi, 18, v, rw / 2 - 24);
            const int vw = vs.empty() ? 0 : MWidth(XF_Semi, 18, vs.c_str());
            MText(XF_Semi, 21, left + 22, (int)ry + (kNxRowH - 29) / 2, WithAlpha(sel ? lit : plain, a),
                  MFit(XF_Semi, 21, labels[i], rw - 44 - (vw ? vw + 24 : 0)).c_str());
            if (vw) MText(XF_Semi, 18, left + rw - 22 - vw, (int)ry + (kNxRowH - 25) / 2,
                          WithAlpha(sel ? lit : plain, (Uint8)(a * (sel ? 230 : 180) / 255)), vs.c_str());
        }
        if (marker) {
            const int bh = kNxRows * kNxRowH, th = std::max(28, bh * kNxRows / n);
            const int ty = y0 + (int)((bh - th) * (top / (float)(n - kNxRows)));
            m_gfx->FillRect(left + pw - 9, y0, 3, bh, SDL_Color{ 255, 255, 255, 40 });
            m_gfx->FillRect(left + pw - 9, ty, 3, th, SDL_Color{ 255, 255, 255, 200 });
        }
    }
    int Menu::NxeListRowAt(int y, float scroll, int rows) const {
        return (int)floorf(NxTop(scroll, rows) + (float)(y - (kNxListY + 8)) / (float)kNxRowH);
    }
    // Pop-ups: the screen dimmed, a sheet of glass in the middle, its title in
    // the light face above a hairline.
    int Menu::NxePopup(int cw, int ch, const std::string &title) {
        const XboxSkin &sk = Skin();
        const int x = (W - cw) / 2, y = std::max(12, (H - ch) / 2);
        m_gfx->FillRect(0, 0, W, H, SDL_Color{ 0, 0, 0, 150 });
        if (sk.panel) m_gfx->Draw9(sk.panel, x, y, cw, ch, sk.b_panel, kWhite);
        else XbCard((float)x, (float)y, (float)cw, (float)ch, 12.0f, 20.0f, SDL_Color{ 16, 20, 26, 235 });
        MText(XF_Light, 24, x + 20, y + 8, sk.has_list ? sk.list_text : kNxInk, MFit(XF_Light, 24, title, cw - 40).c_str());
        m_gfx->FillRect(x + 16, y + 44, cw - 32, 1, SDL_Color{ 255, 255, 255, 50 });
        return x;
    }
    void Menu::DrawNxeRow(int x, int y, int w, bool sel, const char *label) {
        const XboxSkin &sk = Skin();
        if (sel) {
            if (sk.highlight) m_gfx->Draw9(sk.highlight, x + 6, y + 2, w - 12, kBpRowH - 4, sk.b_highlight, kWhite);
            else XbCard(x + 6.0f, y + 2.0f, w - 12.0f, kBpRowH - 4.0f, 8.0f, 6.0f, SDL_Color{ 236, 238, 240, 255 });
        }
        const SDL_Color c = sel ? (sk.has_sel ? sk.list_sel : kWhite)
                                : (sk.has_list ? sk.list_text : kNxInk);
        MText(XF_Semi, 21, x + 22, y + (kBpRowH - 29) / 2, c, MFit(XF_Semi, 21, label, w - 44).c_str());
    }
    void Menu::DrawNxeOptions() {
        const int n = (int)m_options.size();
        constexpr int cw = 520;
        const int ch = kBpHead + n * kBpRowH + 10, y = std::max(12, (H - ch) / 2);
        const int x = NxePopup(cw, ch, OptionsTitle());
        for (int i = 0; i < n; i++)
            DrawNxeRow(x, y + kBpHead + i * kBpRowH, cw, i == m_options_cursor, m_options[i].label.c_str());
    }
    void Menu::DrawNxeDialog() {
        const XboxSkin &sk = Skin();
        constexpr int cw = 600;
        std::vector<std::string> lines;
        {
            std::string line, word;
            auto flush = [&]() {
                if (word.empty()) return;
                const std::string t = line.empty() ? word : line + " " + word;
                if (!line.empty() && MWidth(XF_Semi, 19, t.c_str()) > cw - 48) { lines.push_back(line); line = word; }
                else line = t;
                word.clear();
            };
            for (char c : m_dialog_note) { if (c == ' ' || c == '\n') flush(); else word += c; }
            flush();
            if (!line.empty()) lines.push_back(line);
        }
        const int nopts = m_dialog == Dialog::CrashReport ? 1 : 2;
        const int noteH = lines.empty() ? 0 : (int)lines.size() * 26 + 14;
        const int ch = kBpHead + noteH + nopts * kBpRowH + 10, y = std::max(12, (H - ch) / 2);
        const int x = NxePopup(cw, ch, m_dialog_title.empty() ? std::string(T("Close running application?"))
                                                             : m_dialog_title);
        int ty = y + kBpHead + 2;
        const SDL_Color note = sk.has_dim ? sk.dim : kNxDim;
        for (const std::string &l : lines) { MText(XF_Semi, 19, x + 24, ty, note, l.c_str()); ty += 26; }
        const char *opts[2] = { T("Yes"), T("No") };
        if (nopts == 1) opts[0] = T("OK");
        for (int i = 0; i < nopts; i++)
            DrawNxeRow(x, y + kBpHead + noteH + i * kBpRowH, cw, i == m_dialog_cursor, opts[i]);
    }

    // ---------------------------------------------------------------------------
    // Blades: the main screen
    // ---------------------------------------------------------------------------
    void Menu::DrawBladesGameInfo(const MenuItem &it, const GameInfo *gi, int x, int y, int w, int h, Uint8 A) {
        const XboxSkin &sk = Skin();
        const SDL_Color white = WithAlpha(kWhite, A), grey = WithAlpha(SDL_Color{ 165, 165, 165, 255 }, A);
        if (sk.detail)    m_gfx->Draw9(sk.detail, x, y, w, h, sk.b_panel, kWhite, A);
        else if (sk.info) m_gfx->DrawImage(sk.info, x, y, w, h, A);
        else if (m_ui_mode == UiMode::NXE && sk.panel) m_gfx->Draw9(sk.panel, x, y, w, h, sk.b_panel, kWhite, A);
        else              XbCard((float)x, (float)y, (float)w, (float)h, 12.0f, 0.0f, SDL_Color{ 12, 12, 12, 235 }, A);

        // The picture in a pale frame, the name beside it, the stars under that.
        constexpr int pad = 16, bs = 112;
        m_gfx->FillRect(x + pad - 2, y + pad - 2, bs + 4, bs + 4, WithAlpha(SDL_Color{ 255, 255, 255, 70 }, A));
        DrawXmbIcon(it, x + pad + bs / 2, y + pad + bs / 2, bs, A);
        const int tx = x + pad + bs + 14, tw = x + w - pad - tx;
        int ty = y + pad - 2;
        std::vector<std::string> name = WrapText(FontSize::Small, it.name, tw);
        if (name.size() > 2) {   // two lines, the second cut short with "..."
            for (size_t k = 2; k < name.size(); k++) name[1] += " " + name[k];
            name.resize(2);
        }
        for (const std::string &ln : name) {
            MText(XF_Semi, 22, tx, ty, white, MFit(XF_Semi, 22, ln, tw).c_str());
            ty += 28;
        }
        ty += 6;
        if (gi && gi->score >= 0) {
            // Out of five in half stars, from the critics' average out of 100.
            const int halves = std::clamp((int)lround(gi->score / 10.0), 0, 10);
            constexpr int ss = 22, step = 25;
            for (int k = 0; k < 5; k++) {
                const int sx = tx + k * step;
                if (sk.star_off) m_gfx->DrawImage(sk.star_off, sx, ty, ss, ss, A);
                if (!sk.star || halves <= 2 * k) continue;
                if (halves >= 2 * k + 2) m_gfx->DrawImage(sk.star, sx, ty, ss, ss, A);
                else {
                    float sw = 0, sh = 0;
                    SDL_GetTextureSize(sk.star, &sw, &sh);
                    m_gfx->DrawRegion(sk.star, SDL_FRect{ 0, 0, sw / 2, sh }, SDL_FRect{ (float)sx, (float)ty, ss / 2.0f, (float)ss }, kWhite, A);
                }
            }
            char sc[48];
            snprintf(sc, sizeof(sc), "%s %d", gi->source.empty() ? "OpenCritic" : gi->source.c_str(), gi->score);
            MText(XF_Semi, 15, tx, ty + ss + 4, grey, sc);
        } else if (!gi && m_info_running && m_info_id == it.app_id) {
            MText(XF_Semi, 16, tx, ty, grey, T("Looking it up..."));
        }

        // Labelled rows under a rule, the way the dashboard listed a game's details.
        const int ry = y + pad + bs + 14;
        m_gfx->FillRect(x + pad, ry, w - 2 * pad, 1, WithAlpha(SDL_Color{ 255, 255, 255, 60 }, A));
        if (gi) {
            const std::string year = gi->year > 0 ? std::to_string(gi->year) : std::string();
            // The ESRB code in words, as the box on the case says it.
            static const std::pair<const char *, const char *> kEsrb[] = {
                { "E", "Everyone" }, { "E10+", "Everyone 10+" }, { "T", "Teen" },
                { "M", "Mature 17+" }, { "AO", "Adults Only 18+" }, { "RP", "Rating Pending" } };
            std::string rating = gi->esrb;
            for (const auto &e : kEsrb)
                if (gi->esrb == e.first) rating = T(e.second);
            const std::pair<const char *, const std::string *> rows[] = {
                { T("Genre:"), &gi->genre }, { T("Publisher:"), &gi->maker },
                { T("Released:"), &year }, { T("Players:"), &gi->players }, { T("Rating:"), &rating } };
            int lw = 0;
            for (const auto &r : rows) lw = std::max(lw, MWidth(XF_Semi, 17, r.first));
            int yy = ry + 12;
            for (const auto &r : rows) {
                if (r.second->empty()) continue;
                if (yy + 24 > y + h - pad - 22) break;   // clear of the time played along the bottom
                MText(XF_Semi, 17, x + pad, yy, grey, r.first);
                MText(XF_Semi, 17, x + pad + lw + 10, yy, white, MFit(XF_Semi, 17, *r.second, w - 2 * pad - lw - 10).c_str());
                yy += m_ui_mode == UiMode::NXE ? 24 : 28;   // NXE's card is shorter, beside a 7-row list
            }
        }
        // Time played and when, along the bottom, and who is online in it.
        const std::string played = MFit(XF_Semi, 16, XboxInfo(it), w - 2 * pad - 120);
        int px = x + pad;
        const int py = y + h - pad - 22;
        if (!played.empty()) {
            MText(XF_Semi, 16, px, py, grey, played.c_str());
            px += MWidth(XF_Semi, 16, played.c_str()) + MWidth(XF_Semi, 16, "   ");
        }
        if (const int on = OnlineCount(it); on >= 0) {
            char n[32];
            snprintf(n, sizeof(n), "%d %s", on, T("online"));
            OnlineDot(px, py + 7, 9, on, A);
            MText(XF_Semi, 16, px + 15, py, grey, n);
        }
    }

    void Menu::DrawMainBlades() {
        if (!XboxStep()) return;
        const XboxSkin &sk = Skin();
        const SDL_Color hText = sk.has_text ? sk.text : kWhite;
        const int n = (int)m_xmb_cols.size();
        const BladeGeo g(n, m_xmb_col_scroll);
        auto card = [&](SDL_Texture *, float x, float y, float w, float h, float r, float sh,
                        SDL_Color fill, Uint8 a) { XbCard(x, y, w, h, r, sh, fill, a); };
        // An entry's picture. The profile's is the account's own avatar.
        auto icon = [&](const MenuItem &it, int cx, int cy, int size, Uint8 a) {
            SDL_Texture *av = it.kind == ItemKind::UserPage ? MetroAvatar() : nullptr;
            if (av) m_gfx->DrawImage(av, cx - size / 2, cy - size / 2, size, size, a);
            else DrawXmbIcon(it, cx, cy, size, a);
        };
        DrawBladesFrame();

        // --- the open blade's contents, gone while a blade sweeps across ---
        const float slide = std::min(1.0f, std::abs((float)m_xmb_col - m_xmb_col_scroll) * 1.6f);
        const Uint8 A = (Uint8)(255.0f * (1.0f - slide));
        const XmbColumn &col = m_xmb_cols[m_xmb_col];
        const int count = (int)col.items.size();
        const int left = g.Left(m_xmb_col), right = g.Right(m_xmb_col);
        if (A > 8 && count > 0) {
            const SDL_Color base = BladesColorOf(m_xmb_col);
            const SDL_Color ink = WithAlpha(sk.has_list ? sk.list_text : kWhite, A);
            const SDL_Color soft = WithAlpha(sk.has_dim ? sk.dim : SDL_Color{ 255, 255, 255, 255 }, (Uint8)(A * 200 / 255));
            std::string header = col.label.empty() ? std::string(XmbCatName(col.cat)) : col.label;
            if (!m_search.empty()) header = std::string(T("Search")) + ": \"" + m_search + "\"";
            MText(XF_Semi, 30, left, 50, WithAlpha(hText, A), MFit(XF_Semi, 30, header, right - left).c_str());

            const int pw = std::min(kBlListW, right - left), ph = kBlRows * kBlRowH + 16;

            // The gamer card heads the list: who is playing, and what they have to show for it.
            {
                const int gx = left, gy = 124, gw = pw, gh = 102;
                if (sk.card) m_gfx->Draw9(sk.card, gx, gy, gw, gh, sk.b_card, kWhite, A);
                else {   // a pale card in the blade's colour, its name bar darker
                    card(nullptr, (float)gx, (float)gy, (float)gw, (float)gh, 9.0f, 10.0f, WithAlpha(Gloss(base, 1.58f), 225), A);
                    card(nullptr, gx + 3.0f, gy + 3.0f, gw - 6.0f, 27.0f, 7.0f, 0.0f, WithAlpha(Shade(base, 0.48f), 240), A);
                }
                MText(XF_Semi, 17, gx + 12, gy + 4, WithAlpha(kWhite, A), MFit(XF_Semi, 17, m_nickname, gw - 110).c_str());
                time_t now = time(nullptr);
                struct tm tm_now;
                localtime_r(&now, &tm_now);
                char clock[16], batt[24];
                strftime(clock, sizeof(clock), "%H:%M", &tm_now);
                snprintf(batt, sizeof(batt), sys::Charging() ? "+%lu%%" : "%lu%%", (unsigned long)sys::BatteryPercent());
                MText(XF_Semi, 17, gx + gw - 12 - MWidth(XF_Semi, 17, clock), gy + 4, WithAlpha(kWhite, A), clock);
                // The gamer picture: the account's own, or a plain figure without one.
                card(nullptr, gx + 10.0f, gy + 37.0f, 56.0f, 56.0f, 5.0f, 0.0f, Shade(base, 0.62f), A);
                if (SDL_Texture *mh = MiiRender(1))   // the Mii's head, as a gamer picture
                    m_gfx->DrawImage(mh, gx + 10, gy + 37, 56, 56, A);
                else if (SDL_Texture *av = MetroAvatar())
                    m_gfx->DrawImage(av, gx + 12, gy + 39, 52, 52, A);
                else if (SDL_Texture *ic = SystemIcon(ItemKind::UserPage))
                    m_gfx->DrawImageTinted(ic, gx + 18, gy + 45, 40, 40, WithAlpha(kWhite, A));
                int games = 0;
                u64 secs = 0;
                for (const MenuItem &it : m_items) {
                    if (it.kind != ItemKind::Game) continue;
                    games++;
                    if (const play::PlayInfo *pi = Play(it.app_id)) secs += pi->seconds;
                }
                const SDL_Color cink = WithAlpha(sk.card && sk.has_list ? sk.list_text : Shade(base, 0.20f), A);
                char num[24];
                snprintf(num, sizeof(num), "%d", games);
                const std::string played = secs ? play::FormatPlaytime(secs) : std::string("-");
                const char *lab[3] = { T("Games"), T("Played"), T("Battery") };
                const char *val[3] = { num, played.c_str(), batt };
                for (int k = 0; k < 3; k++) {
                    MText(XF_Semi, 16, gx + 80, gy + 33 + k * 21, cink, lab[k]);
                    MText(XF_Semi, 16, gx + gw - 14 - MWidth(XF_Semi, 16, val[k]), gy + 33 + k * 21, cink, val[k]);
                }
            }

            // The list: a sheet of glass with a rule under every row, the selection lit.
            if (sk.panel) m_gfx->Draw9(sk.panel, left, kBlListY, pw, ph, sk.b_panel, kWhite, A);
            else card(nullptr, (float)left, (float)kBlListY, (float)pw, (float)ph, 8.0f, 0.0f,
                      SDL_Color{ 255, 255, 255, 24 }, A);
            const float top = BladeTop(m_xmb_item_scroll, count);
            const int y0 = kBlListY + 8, y1 = kBlListY + ph - 8;
            const float by = std::clamp(y0 + (m_xmb_item_scroll - top) * kBlRowH, (float)y0, (float)(y1 - kBlRowH));
            if (sk.highlight) m_gfx->Draw9(sk.highlight, left + 6, (int)by + 2, pw - 12, kBlRowH - 4, sk.b_highlight, kWhite, A);
            else {
                card(nullptr, left + 4.0f, by + 1.0f, pw - 8.0f, kBlRowH - 2.0f, 7.0f, 8.0f, WithAlpha(Gloss(base, 1.45f), 235), A);
                card(nullptr, left + 6.0f, by + 3.0f, pw - 12.0f, (kBlRowH - 6.0f) / 2.0f, 6.0f, 0.0f,
                     SDL_Color{ 255, 255, 255, 70 }, A);
            }
            for (int i = (int)top; i < count && i <= (int)top + kBlRows; i++) {
                const float ry = y0 + ((float)i - top) * kBlRowH;
                float vis = 1.0f;
                if (ry < y0) vis = 1.0f - (y0 - ry) / kBlRowH;
                if (ry + kBlRowH > y1) vis = std::min(vis, 1.0f - (ry + kBlRowH - y1) / kBlRowH);
                if (vis <= 0.03f) continue;
                const Uint8 a = (Uint8)(A * vis);
                const MenuItem &it = m_items[col.items[i]];
                const bool sel = i == m_xmb_item;
                if (!sk.panel && !sel)
                    m_gfx->FillRect(left + 10, (int)ry + kBlRowH - 1, pw - 20, 1, SDL_Color{ 255, 255, 255, (Uint8)(a * 70 / 255) });
                icon(it, left + 34, (int)ry + kBlRowH / 2, 36, a);
                MText(XF_Semi, 22, left + 64, (int)ry + (kBlRowH - 30) / 2,
                      WithAlpha(sel ? (sk.has_sel ? sk.list_sel : (sk.highlight ? kWhite : Shade(base, 0.22f)))
                                    : (sk.has_list ? sk.list_text : kWhite), a),
                      MFit(XF_Semi, 22, it.name, pw - 84).c_str());
            }

            // Beside it, the selected entry: its picture where the blade keeps its emblem,
            // then its name and what there is to say about it.
            const MenuItem &sel = m_items[col.items[std::clamp(m_xmb_item, 0, count - 1)]];
            const int dx = left + pw + 36, dw = right - dx;
            if (dw >= 200 && sel.kind == ItemKind::Game && sel.app_id != 0) {
                DrawBladesGameInfo(sel, BladesInfo(sel), dx - 12, 124, dw + 12, kBlListY + ph - 124, A);
            } else if (dw >= 200) {
                if (sk.detail) m_gfx->Draw9(sk.detail, dx - 12, 124, dw + 12, kBlListY + ph - 124, sk.b_panel, kWhite, A);
                const int as = std::min(180, dw - 20);
                icon(sel, dx + dw / 2, 132 + as / 2, as, A);
                int iy = 132 + as + 26;
                m_gfx->FillRect(dx, iy - 12, dw, 1, SDL_Color{ 255, 255, 255, (Uint8)(A * 80 / 255) });
                for (const std::string &ln : WrapText(FontSize::Small, sel.name, dw - 8)) {
                    if (iy > kBlListY + ph - 60) break;
                    MText(XF_Semi, 24, dx, iy, ink, ln.c_str());
                    iy += 32;
                }
                iy += 4;
                for (const std::string &ln : WrapText(FontSize::Small, XboxInfo(sel), dw - 8)) {
                    if (iy > kBlListY + ph - 26) break;
                    MText(XF_Semi, 18, dx, iy, soft, ln.c_str());
                    iy += 25;
                }
            }

            // The long button under everything, with its ring at the left end:
            // what is selected, and where that is in the list.
            if (m_show_counter) {
                const int py = kBlListY + ph + 10, pwid = right - left - 14;
                card(nullptr, left + 14.0f, (float)py, (float)pwid, 42.0f, 21.0f, 0.0f, SDL_Color{ 255, 255, 255, 60 }, A);
                card(nullptr, left + 15.5f, py + 1.5f, pwid - 3.0f, 39.0f, 19.5f, 0.0f, WithAlpha(Shade(base, 0.86f), 200), A);
                card(nullptr, left - 6.0f, py - 6.0f, 54.0f, 54.0f, 27.0f, 0.0f, SDL_Color{ 255, 255, 255, 110 }, A);
                card(nullptr, left - 2.0f, py - 2.0f, 46.0f, 46.0f, 23.0f, 0.0f, WithAlpha(Shade(base, 0.62f), 255), A);
                m_gfx->FillTriangle(left + 26, py + 13, left + 26, py + 29, left + 13, py + 21, WithAlpha(kWhite, (Uint8)(A * 200 / 255)));
                char pos[48];
                snprintf(pos, sizeof(pos), "%d %s %d", m_xmb_item + 1, T("of"), count);
                MText(XF_Semi, 19, left + 62, py + 7, ink, MFit(XF_Semi, 19, sel.name, pwid - 200).c_str());
                MText(XF_Semi, 18, right - 20 - MWidth(XF_Semi, 18, pos), py + 8, soft, pos);
            }
        }
        // The legend sits in the bottom band's corners, two to a side, the
        // right-hand pair with their buttons outermost.
        const int hy0 = 616, hy1 = 650;
        m_xb_hint_x = left; m_xb_hint_y = hy0;
        DrawStatusHint({ {{"y"}, "Search"} });
        m_xb_hint_y = hy1;
        DrawHint({ {{"x"}, "Options"} });
        m_xb_hint_flip = true; m_xb_hint_x = right;
        DrawHint({ {{"a"}, "Select"} });
        m_xb_hint_y = hy0;
        DrawHint({ {{"l","r"}, XmbShoulderHint()} });
        m_xb_hint_flip = false; m_xb_hint_y = 0;

        // Between the two pairs of prompts the legend band is free. The widgets
        // that are switched on sit there, as many as fit, each scaled to the band.
        if (m_deferred_joined && A > 8) {
            int x0 = left + 6, x1 = right - 6;
            if (m_show_hints) {
                x0 = left + 29 + std::max(MWidth(XF_Semi, 19, T("Search")), MWidth(XF_Semi, 19, T("Options"))) + 22;
                x1 = right - 6 - std::max(MWidth(XF_Semi, 19, T("Select")) + 22,
                                          MWidth(XF_Semi, 19, T(XmbShoulderHint())) + 47) - 22;
            }
            std::vector<int> on;
            for (int i = 0; i < m_widgets.Count(); i++)
                if (m_widgets.At(i) && m_widgets.IsEnabled(i)) on.push_back(i);
            constexpr int gap = 10, minW = 190, maxW = 300, bandY = 604, bandH = 84;
            const int fit = std::min((int)on.size(), (x1 - x0 + gap) / (minW + gap));
            if (fit > 0) {
                const int sw = std::min(maxW, (x1 - x0 - (fit - 1) * gap) / fit);
                int x = x0 + (x1 - x0 - fit * sw - (fit - 1) * gap) / 2;   // centred in what is free
                for (int k = 0; k < fit; k++, x += sw + gap)
                    DrawWidgetIn(m_widgets.At(on[k]), TileRect{ -1, x, bandY, sw, bandH }, A);
            }
        }
    }

    int Menu::BladesColAt(int x, int y) const {
        const int n = (int)m_xmb_cols.size();
        const BladeGeo g(n, m_xmb_col_scroll);
        for (int c = 0; c < n; c++) {
            if (c == m_xmb_col) continue;
            const float l = g.Edge(c, (float)y);
            if (x >= l && x < l + g.tab) return c;
        }
        return -1;
    }
    int Menu::BladesItemAt(int x, int y) const {
        if (m_xmb_col < 0 || m_xmb_col >= (int)m_xmb_cols.size()) return -1;
        const auto &items = m_xmb_cols[m_xmb_col].items;
        const BladeGeo g((int)m_xmb_cols.size(), m_xmb_col_scroll);
        const int left = g.Left(m_xmb_col), pw = std::min(kBlListW, g.Right(m_xmb_col) - left);
        const int y0 = kBlListY + 8;
        if (x < left || x >= left + pw || y < y0 || y >= y0 + kBlRows * kBlRowH) return -1;
        const int i = (int)(BladeTop(m_xmb_item_scroll, (int)items.size()) + (float)(y - y0) / kBlRowH);
        return (i >= 0 && i < (int)items.size()) ? items[i] : -1;
    }

    // ---------------------------------------------------------------------------
    // NXE
    // ---------------------------------------------------------------------------
    namespace {
#ifndef SL_SIMULATOR
        // A Switch Mii (CharInfo) as Mii Studio's 46 bytes: the same fields, in
        // the order the studio keeps them, then obfuscated the studio's way -
        // each byte 7 + (byte ^ the previous output), after a seed byte.
        std::string MiiStudioHex(const u8 *c) {
            const MiiCharInfo &m = *(const MiiCharInfo *)c;
            const u8 v[46] = {
                m.mii_facial_hair_color, m.mii_beard_style, m.mii_width, m.mii_eye_thickness, m.mii_eye_color,
                m.mii_eye_angle, m.mii_eye_size, m.mii_eye_style, m.mii_eye_pos_x, m.mii_eye_pos_y,
                m.mii_eyebrow_thickness, m.mii_eyebrow_color, m.mii_eyebrow_angle, m.mii_eyebrow_size,
                m.mii_eyebrow_style, m.mii_eyebrow_pos_x, m.mii_eyebrow_pos_y,
                m.mii_face_color, m.mii_makeup_style, m.mii_face_shape, m.mii_wrinkles_style,
                m.mii_color, m.mii_sex,
                m.mii_glasses_color, m.mii_glasses_size, m.mii_glasses_style, m.mii_glasses_pos,
                m.mii_hair_color, m.mii_has_hair_flipped, m.mii_hair_style, m.mii_height,
                m.mii_mole_size, m.mii_has_mole, m.mii_mole_pos_x, m.mii_mole_pos_y,
                m.mii_mouth_thickness, m.mii_mouth_color, m.mii_mouth_size, m.mii_mouth_style, m.mii_mouth_pos,
                m.mii_mustache_size, m.mii_mustache_style, m.mii_mustache_pos,
                m.mii_nose_size, m.mii_nose_style, m.mii_nose_pos };
            std::string hex = "00";
            u8 prev = 0;
            for (u8 b : v) {
                prev = (u8)(7 + (b ^ prev));
                char h[3];
                snprintf(h, sizeof(h), "%02x", prev);
                hex += h;
            }
            return hex;
        }
#endif

        // Every Mii in Mii Edit, as studio data with its name; returns the one
        // the account's picture was made from (-1: a character picture, or no
        // Mii service).
        int MiiReadList(AccountUid uid, std::vector<MiiEntry> &out) {
#ifdef SL_SIMULATOR
            (void)uid;
            static const char *kNames[] = { "Sim", "Ana", "Kenji", "Lea", "Omar", "Ines", "Yuki", "Bo" };
            for (int i = 0; i < 8; i++) {
                MiiEntry e;
                e.hex = "000d142a303f434b717a7b84939ba6b2bbbec5cbc9d0e2ea010d15252b3250535960736f726870757f8289a0a7aeb1";
                e.name = kNames[i];
                memset(e.cid, i, sizeof(e.cid));
                out.push_back(e);
            }
            return 0;
#else
            #define MIILOG(...) do { if (FILE *f_ = fopen("sdmc:/slaunch/boot.log", "a")) { fputs("mii: ", f_); fprintf(f_, __VA_ARGS__); fputc('\n', f_); fclose(f_); } } while (0)
            AccountUserData ud{};
            AccountProfile p;
            Result rc = accountUidIsValid(&uid) ? accountGetProfile(&p, uid) : MAKERESULT(Module_Libnx, LibnxError_BadInput);
            if (R_SUCCEEDED(rc)) { rc = accountProfileGet(&p, &ud, nullptr); accountProfileClose(&p); }
            if (R_FAILED(rc)) MIILOG("profile rc=0x%x", rc);
            rc = miiInitialize(MiiServiceType_System);
            if (R_FAILED(rc)) { MIILOG("init rc=0x%x", rc); return -1; }
            int hit = -1;
            MiiDatabase db;
            rc = miiOpenDatabase(&db, MiiSpecialKeyCode_Normal);
            if (R_SUCCEEDED(rc)) {
                std::vector<MiiCharInfo> all(100);
                s32 n = 0;
                rc = miiDatabaseGet1(&db, MiiSourceFlag_Database, all.data(), (s32)all.size(), &n);
                if (R_FAILED(rc)) n = 0;
                for (s32 i = 0; i < n && i < (s32)all.size(); i++) {
                    if (hit < 0 && !memcmp(&all[i].create_id, ud.miiID, 16)) hit = i;
                    MiiEntry e;
                    e.hex = MiiStudioHex((const u8 *)&all[i]);
                    memcpy(e.cid, &all[i].create_id, 16);
                    char name[48] = {};
                    utf16_to_utf8((uint8_t *)name, all[i].mii_name, sizeof(name) - 1);
                    e.name = name;
                    out.push_back(e);
                }
                MIILOG("get rc=0x%x count=%d account=%d", rc, n, hit);
                miiDatabaseClose(&db);
            } else {
                MIILOG("open rc=0x%x", rc);
            }
            miiExit();
            return hit;
            #undef MIILOG
#endif
        }

        // Debug: with sdmc:/slaunch/config/dump_miimodel present, copies the
        // MiiModel system archive (0100000000000802) to sdmc:/slaunch/miimodel/
        // once, to work out the local 3D renderer from the console's own data.
        void MiiDumpModel() {
#ifndef SL_SIMULATOR
            const char *flag = "sdmc:/slaunch/config/dump_miimodel";
            struct stat st {};
            if (stat(flag, &st) != 0) return;
            remove(flag);
            FILE *log = fopen("sdmc:/slaunch/boot.log", "a");
            FsStorage sto;
            Result rc = fsOpenDataStorageByDataId(&sto, 0x0100000000000802ull, NcmStorageId_BuiltInSystem);
            if (R_SUCCEEDED(rc)) rc = romfsMountFromStorage(sto, 0, "miimodel");
            if (log) fprintf(log, "mii: miimodel mount rc=0x%x\n", rc);
            if (R_FAILED(rc)) { if (log) fclose(log); return; }
            mkdir("sdmc:/slaunch/miimodel", 0777);
            if (DIR *d = opendir("miimodel:/")) {
                while (dirent *e = readdir(d)) {
                    const std::string src = std::string("miimodel:/") + e->d_name, dst = std::string("sdmc:/slaunch/miimodel/") + e->d_name;
                    FILE *i = fopen(src.c_str(), "rb"), *o = i ? fopen(dst.c_str(), "wb") : nullptr;
                    long n = 0;
                    if (i && o) { static char buf[64 * 1024]; size_t k; while ((k = fread(buf, 1, sizeof(buf), i)) > 0) n += (long)fwrite(buf, 1, k, o); }
                    if (o) fclose(o);
                    if (i) fclose(i);
                    if (log) fprintf(log, "mii: miimodel %s %ld bytes\n", e->d_name, n);
                }
                closedir(d);
            }
            romfsUnmount("miimodel");
            if (log) fclose(log);
#endif
        }

        // The system's own picture of the Mii (miiimg, 256x256 RGBA, drawn by
        // Mii Edit), saved as a PNG: works offline, and Nintendo's render
        // server is blocked on most modded consoles.
        bool MiiSystemImage(const u8 *cid, const char *png) {
#ifdef SL_SIMULATOR
            (void)cid; (void)png;
            return false;
#else
            Result rc = miiimgInitialize();
            s32 n = 0;
            if (R_SUCCEEDED(rc)) rc = miiimgGetCount(&n);
            bool ok = false;
            for (s32 i = 0; R_SUCCEEDED(rc) && i < n && !ok; i++) {
                MiiimgImageAttribute a{};
                if (R_FAILED(miiimgGetAttribute(i, &a)) || memcmp(&a.create_id, cid, 16)) continue;
                std::vector<u8> px(256 * 256 * 4);
                if (R_FAILED(miiimgLoadImage(a.image_id, px.data(), px.size()))) break;
                if (SDL_Surface *sf = SDL_CreateSurfaceFrom(256, 256, SDL_PIXELFORMAT_RGBA32, px.data(), 256 * 4)) {
                    ok = IMG_SavePNG(sf, png);
                    SDL_DestroySurface(sf);
                }
            }
            if (FILE *f = fopen("sdmc:/slaunch/boot.log", "a")) { fprintf(f, "mii: miiimg rc=0x%x count=%d local=%d\n", rc, n, ok); fclose(f); }
            if (R_SUCCEEDED(rc) || n) miiimgExit();
            return ok;
#endif
        }
    }

    void Menu::MiiTrampoline(void *self) {
        Menu *m = (Menu *)self;
        const bool got = net::Download(m->m_mii_url.c_str(), m->m_mii_path.c_str(), 15);
        const bool whole = got && net::ImageLooksWhole(m->m_mii_path.c_str());
        if (!whole) {
            remove(m->m_mii_path.c_str());
            if (FILE *f = fopen("sdmc:/slaunch/boot.log", "a")) {
                fprintf(f, "mii: render %s (%s)\n", got ? "not an image" : "download failed", m->m_mii_url.c_str());
                fclose(f);
            }
        }
        m->m_mii_done.store(true, std::memory_order_release);
    }

    SDL_Texture *Menu::MiiRender(int which) {
        static const char *const kType[2] = { "all_body", "face" };
        if (m_mii_running && m_mii_done.load(std::memory_order_acquire)) {
            threadWaitForExit(&m_mii_thread);
            threadClose(&m_mii_thread);
            m_mii_running = false;
            struct stat st {};
            if (stat(m_mii_path.c_str(), &st) == 0) m_mii_tex[m_mii_job] = m_gfx->LoadImage(m_mii_path.c_str());
            // The first try runs before the network is up on boot; without a
            // retry the Mii stayed blank for the whole session.
            if (!m_mii_tex[m_mii_job]) {
                m_mii_tried[m_mii_job] = false;
                m_mii_retry = armGetSystemTick() + 30 * armGetSystemTickFreq();
            }
        }
        if (m_mii_tried[which] || m_mii_running || armGetSystemTick() < m_mii_retry) return m_mii_tex[which];
        if (!m_mii_hex_read) MiiDumpModel();
        if (!m_mii_hex_read) { m_mii_hex_read = true; m_mii_hex = MiiChosenHex(m_mii_cid); }
        m_mii_tried[which] = true;
        if (m_mii_hex.empty()) return nullptr;
        // Named by the Mii's look, so an edit in Mii Edit renders it again.
        char name[48];
        snprintf(name, sizeof(name), "%016llx_%s.png", (unsigned long long)std::hash<std::string>{}(m_mii_hex), kType[which]);
        mkdir("sdmc:/slaunch/cache", 0777);
        mkdir("sdmc:/slaunch/cache/mii", 0777);
        m_mii_path = std::string("sdmc:/slaunch/cache/mii/") + name;
        struct stat st {};
        if (stat(m_mii_path.c_str(), &st) == 0 && st.st_size > 0 && net::ImageLooksWhole(m_mii_path.c_str()))
            m_mii_tex[which] = m_gfx->LoadImage(m_mii_path.c_str());
        if (m_mii_tex[which]) return m_mii_tex[which];
        remove(m_mii_path.c_str());   // an empty or broken cache file used to hide the Mii for good
        // The face is in the system's image database; only the body needs the server.
        if (which == 1 && MiiSystemImage(m_mii_cid, m_mii_path.c_str())) {
            m_mii_tex[1] = m_gfx->LoadImage(m_mii_path.c_str());
            if (m_mii_tex[1]) return m_mii_tex[1];
        }
        m_mii_url = "https://studio.mii.nintendo.com/miis/image.png?data=" + m_mii_hex + "&type=" + kType[which] +
                    "&width=512&expression=normal&bgColor=FFFFFF00";
        m_mii_job = which;
        m_mii_done.store(false, std::memory_order_release);
        if (R_SUCCEEDED(threadCreate(&m_mii_thread, &Menu::MiiTrampoline, this, nullptr, 0x10000, 0x3B, -2))) {
            threadStart(&m_mii_thread);
            m_mii_running = true;
        }
        return nullptr;
    }

    // The 3D Mii's files: the head from the console's MiiModel archive, the
    // body and animations copied to the card by the user (Nintendo's and
    // Microsoft's, never shipped; see scripts/mii/).
    namespace {
        // One Mii's scene from its studio data: the head from the console's
        // MiiModel archive (mounted as mount), else a dump on the card.
        // variant: the simulator's stand-ins, made to differ.
        bool LoadMiiScene(mii::Scene &sc, const std::string &hex, const char *mount, int variant) {
            mii::Parts p;
            if (!mii::PartsFromStudioHex(hex, p)) return false;
#ifdef SL_SIMULATOR
            if (variant > 0) {
                p.hair_type = (uint8_t)((p.hair_type + variant * 23) % 132);
                p.hair_color = (uint8_t)((p.hair_color + variant * 3) % 100);
                p.favorite_color = (uint8_t)((p.favorite_color + variant * 4) % 12);
                p.height = (uint8_t)(30 + (variant * 37) % 70);
                p.glass_type = 0;
            }
#else
            (void)variant;
#endif
            std::string dir = "sdmc:/slaunch/miimodel/";   // a dump (dump_miimodel), and the simulator's copy
#ifndef SL_SIMULATOR
            FsStorage sto;
            bool mounted = false;
            if (R_SUCCEEDED(fsOpenDataStorageByDataId(&sto, 0x0100000000000802ull, NcmStorageId_BuiltInSystem)) &&
                R_SUCCEEDED(romfsMountFromStorage(sto, 0, mount))) {
                mounted = true;
                dir = std::string(mount) + ":/";
            }
#else
            (void)mount;
#endif
            auto file = [](const std::string &a, const char *b) {
                struct stat st {};
                return stat(a.c_str(), &st) == 0 ? a : std::string("sdmc:/slaunch/miimodel/") + b;
            };
            const std::string shape = file(dir + "ShapeHigh.dat", "ShapeHigh.dat"), tex = file(dir + "NXTextureMidSRGB.dat", "NXTextureMidSRGB.dat");
            const bool ok = sc.Load(shape.c_str(), tex.c_str(), "sdmc:/slaunch/mii/MiiBody.dat", "sdmc:/slaunch/mii/anims.bin", p);
#ifndef SL_SIMULATOR
            if (mounted) romfsUnmount(mount);
#endif
            if (FILE *f = fopen("sdmc:/slaunch/boot.log", "a")) {
                fprintf(f, "mii: 3d %s (%s)\n", ok ? "ready" : "failed", shape.c_str());
                fclose(f);
            }
            return ok;
        }
    }

    // The 3D Mii's files: the head from the console's MiiModel archive, the
    // body and animations copied to the card by the user (Nintendo's and
    // Microsoft's, never shipped; see scripts/mii/).
    void Menu::Mii3DLoad(void *self) {
        Menu *m = (Menu *)self;
        const bool ok = LoadMiiScene(m->m_mii3d, m->m_mii_hex, "mii3d", m->m_mii_pick);   // the variant: the simulator's stand-ins only
        m->m_mii3d_state.store(ok ? 2 : 3, std::memory_order_release);
    }

    void Menu::MiiEnsureList() {
        if (m_mii_list_read) return;
        m_mii_list_read = true;
        m_mii_account = MiiReadList(m_user, m_mii_list);
    }
    std::string Menu::MiiChosenHex(u8 *cid) {
        MiiEnsureList();
        const int n = (int)m_mii_list.size();
        const int i = (m_mii_pick > 0 && m_mii_pick <= n) ? m_mii_pick - 1
                    : m_mii_account >= 0 ? m_mii_account : (n > 0 ? 0 : -1);
        if (i < 0) return "";
        memcpy(cid, m_mii_list[i].cid, 16);
        return m_mii_list[i].hex;
    }
    std::string Menu::MiiPickName(int pick) {
        MiiEnsureList();
        if (pick <= 0 || pick > (int)m_mii_list.size()) return T("Account's Mii");
        const std::string &n = m_mii_list[pick - 1].name;
        return n.empty() ? std::string("Mii ") + std::to_string(pick) : n;
    }
    void Menu::MiiReset() {
        if (m_mii_running) {
            threadWaitForExit(&m_mii_thread);
            threadClose(&m_mii_thread);
            m_mii_running = false;
        }
        if (m_mii3d_started) {
            threadWaitForExit(&m_mii3d_thread);
            threadClose(&m_mii3d_thread);
            m_mii3d_started = false;
        }
        for (SDL_Texture *&t : m_mii_tex) if (t) { m_gfx->FreeImage(t); t = nullptr; }
        if (m_mii3d_tex) { m_gfx->FreeImage(m_mii3d_tex); m_mii3d_tex = nullptr; }
        m_mii_tried[0] = m_mii_tried[1] = false;
        m_mii_hex_read = false;
        m_mii_hex.clear();
        m_mii_retry = 0;
        m_mii3d = mii::Scene{};
        m_mii3d_verts.clear();
        m_mii3d_state.store(0);
    }

    bool Menu::Mii3DReady() {
        int st = m_mii3d_state.load(std::memory_order_acquire);
        if (st == 0) {
            m_mii3d_state.store(3);
            struct stat fs {};
            if (stat("sdmc:/slaunch/mii/MiiBody.dat", &fs) != 0) return false;   // no body on the card: the studio picture
            if (!m_mii_hex_read) { m_mii_hex_read = true; m_mii_hex = MiiChosenHex(m_mii_cid); }
            if (m_mii_hex.empty()) return false;
            m_mii3d_state.store(1);
            if (R_FAILED(threadCreate(&m_mii3d_thread, &Menu::Mii3DLoad, this, nullptr, 0x20000, 0x3E, -2))) {
                m_mii3d_state.store(3);
                return false;
            }
            if (R_FAILED(threadStart(&m_mii3d_thread))) {
                threadClose(&m_mii3d_thread);
                m_mii3d_state.store(3);
                return false;
            }
            m_mii3d_started = true;
            return false;
        }
        if (st != 2) return false;
        if (!m_mii3d_tex) {
            if (m_mii3d_started) {
                threadWaitForExit(&m_mii3d_thread);
                threadClose(&m_mii3d_thread);
                m_mii3d_started = false;
            }
            const mii::Image &a = m_mii3d.Atlas();
            m_mii3d_tex = SDL_CreateTexture(m_gfx->Renderer(), SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, a.w, a.h);
            if (!m_mii3d_tex || !SDL_UpdateTexture(m_mii3d_tex, nullptr, a.px.data(), a.w * 4)) {
                if (m_mii3d_tex) { m_gfx->FreeImage(m_mii3d_tex); m_mii3d_tex = nullptr; }
                m_mii3d_state.store(3);
                return false;
            }
            m_mii3d.DropAtlas();   // 8 MB the GPU now holds
            SDL_SetTextureBlendMode(m_mii3d_tex, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(m_mii3d_tex, SDL_SCALEMODE_LINEAR);
        }
        return true;
    }

    namespace {
        // A changed face expression: the new mask over the atlas's mask rect.
        void MiiMaskUpload(mii::Scene &sc, SDL_Texture *tex, int res) {
            if (!tex) return;
            if (const mii::Image *m = sc.MaskUpdate(res)) {
                const SDL_Rect r{ 0, res, m->w, m->h };
                SDL_UpdateTexture(tex, &r, m->px.data(), m->w * 4);
            }
        }
    }

    bool Menu::MiiDraw(int x, int y, int size, Uint8 alpha) {
        if (Mii3DReady()) {
            static_assert(sizeof(mii::Vertex) == sizeof(SDL_Vertex), "mii::Vertex mirrors SDL_Vertex");
            const double t = (double)armGetSystemTick() / (double)armGetSystemTickFreq();
            m_mii3d.Build(t, (float)x, (float)y, (float)size, alpha / 255.0f, m_mii3d_verts);
            MiiMaskUpload(m_mii3d, m_mii3d_tex, 512);
            m_gfx->DrawGeometry(m_mii3d_tex, (const SDL_Vertex *)m_mii3d_verts.data(), (int)m_mii3d_verts.size());
            return true;
        }
        if (SDL_Texture *tex = m_mii3d_state.load() == 3 ? MiiBody() : nullptr) {   // no 3D Mii: the studio picture
            m_gfx->DrawImage(tex, x, y, size, size, alpha);
            return true;
        }
        return false;
    }

    // One panel of a channel's row. The library entry and anything without
    // pictures of its own is the skin's lime panel with its icon and name; a
    // game shows its art, cropped to the panel.
    void Menu::NxePanel(const MenuItem &it, float x, float y, float w, float h, float a, float lit, bool front) {
        const XboxSkin &sk = Skin();
        const Uint8 A = (Uint8)(255.0f * a);
        const float sc = h / kNxH[0];
        const bool prof = it.kind == ItemKind::UserPage;
        SDL_Texture *art = (it.kind == ItemKind::GameLibrary || prof) ? nullptr : ItemArt(it);
        // A panel's own face where the skin has one: the libraries'.
        SDL_Texture *face = nullptr;
        switch (it.kind) {
            case ItemKind::GameLibrary: face = sk.p_games; break;
            case ItemKind::VideoLibrary: face = sk.p_videos; break;
            case ItemKind::MusicPlayer: face = sk.p_music; break;
            case ItemKind::Album:       face = sk.p_photos; break;
            default: break;
        }
        if (!face) face = sk.tile;
        // The shadow it throws on the next one, drawn first so this panel covers its own edge.
        if (sk.shadow && sc > 0.2f) m_gfx->DrawImage(sk.shadow, (int)(x + w - 2), (int)y, (int)(32 * sc), (int)h, A);
        if (art) {
            float tw = 0, th = 0;
            SDL_GetTextureSize(art, &tw, &th);
            SDL_FRect src{ 0, 0, tw, th };
            if (tw / th > w / h) { src.w = th * w / h; src.x = (tw - src.w) / 2; }
            else                 { src.h = tw * h / w; src.y = (th - src.h) / 2; }
            m_gfx->DrawRegion(art, src, SDL_FRect{ x, y, w, h }, kWhite, A);
        } else if (face) {
            m_gfx->DrawImage(face, (int)x, (int)y, (int)w, (int)h, A);
        } else {
            m_gfx->FillRect((int)x, (int)y, (int)w, (int)h, SDL_Color{ 120, 180, 30, A });
        }
        if (prof && sc > 0.45f) {
            // The gamer card on the panel's face: who, how long they have played,
            // and what lately. The Mii stands in front of it (DrawMainNxe).
            const SDL_Color ink = WithAlpha(kWhite, A);
            const int px = (int)(x + 22 * sc);
            int ty = (int)(y + 16 * sc);
            if (!MiiHave()) {   // no Mii to stand there: the account picture, top right
                const int ps = (int)(76 * sc);
                if (SDL_Texture *av = MetroAvatar()) m_gfx->DrawImage(av, (int)(x + w - 18 * sc) - ps, ty, ps, ps, A);
            }
            MText(XF_Semi, (int)lroundf(30 * sc), px, ty, ink, MFit(XF_Semi, (int)lroundf(30 * sc), m_nickname, (int)(w * 0.6f)).c_str());
            ty += (int)(40 * sc);
            std::vector<std::pair<u64, const MenuItem *>> recent;
            u64 secs = 0;
            for (const MenuItem &g : m_items) {
                if (g.kind != ItemKind::Game) continue;
                if (const play::PlayInfo *pi = Play(g.app_id)) {
                    secs += pi->seconds;
                    if (pi->last_played) recent.push_back({ (u64)pi->last_played, &g });
                }
            }
            if (secs) MText(XF_Semi, (int)lroundf(21 * sc), px, ty, ink, play::FormatPlaytime(secs).c_str());
            // Latest Games: the last four played, newest first.
            const int n = std::min<int>(4, (int)recent.size());
            std::partial_sort(recent.begin(), recent.begin() + n, recent.end(),
                              [](const auto &l, const auto &r) { return l.first > r.first; });
            if (n && sc > 0.9f) {
                const int is = (int)(50 * sc), iy = (int)(y + h - 22 * sc) - is;
                MText(XF_Semi, (int)lroundf(17 * sc), px, iy - (int)(26 * sc), ink, T("Latest Games"));
                for (int k = 0; k < n; k++)
                    if (SDL_Texture *ic = ItemArt(*recent[k].second))
                        m_gfx->DrawImage(ic, px + k * (is + (int)(8 * sc)), iy, is, is, A);
            }
        }
        if (!art && !prof) {
            // The skin's own picture for the entries that have one, else the system glyph.
            const char *icn = nullptr;
            switch (it.kind) {
                case ItemKind::GameLibrary: case ItemKind::Game:
                case ItemKind::Collection:  icn = "games"; break;
                case ItemKind::RandomGame:  icn = "random"; break;
                case ItemKind::Album:       icn = "photos"; break;
                case ItemKind::VideoLibrary: icn = "videos"; break;
                case ItemKind::MusicPlayer: icn = "music"; break;
                case ItemKind::MiiEdit:     icn = "mii"; break;
                case ItemKind::UserPage:    icn = "profile"; break;
                case ItemKind::Wifi:        icn = "network"; break;
                case ItemKind::Bluetooth:   icn = "bluetooth"; break;
                case ItemKind::Power:       icn = "power"; break;
                case ItemKind::Controllers: icn = "games"; break;
                case ItemKind::Theming: case ItemKind::Themes: icn = "theming"; break;
                case ItemKind::Fonts:       icn = "fonts"; break;
                case ItemKind::HomebrewMenu: case ItemKind::Homebrew: icn = "homebrew"; break;
                case ItemKind::FileManager: icn = "files"; break;
                case ItemKind::WebBrowser:  icn = "web"; break;
                case ItemKind::Discord:     icn = "chat"; break;
                default:                    icn = "settings"; break;
            }
            SDL_Texture *ic = NxeIcon(icn);
            const float is = h * 0.56f;
            if (ic) {
                float iw = 0, ih = 0;
                SDL_GetTextureSize(ic, &iw, &ih);
                const float ww = is * iw / std::max(ih, 1.0f);
                m_gfx->DrawImage(ic, (int)(x + (w - ww) / 2), (int)(y + h * 0.40f - is / 2), (int)ww, (int)is, A);
            } else if (SDL_Texture *g = SystemIcon(it.kind == ItemKind::GameLibrary ? ItemKind::RandomGame : it.kind)) {
                const float gs = h * 0.36f;
                m_gfx->DrawImageTinted(g, (int)(x + (w - gs) / 2), (int)(y + h * 0.40f - gs / 2), (int)gs, (int)gs,
                                       SDL_Color{ 255, 255, 255, 255 }, (Uint8)(A * 235 / 255));
            }
        }
        // The name along the bottom: always on a plain panel, on art only up
        // front, over a dark fade so it reads on any picture.
        if (sc > 0.45f && !prof && (!art || front)) {
            const int pt = (int)lroundf(26.0f * sc);
            if (art)   // one smooth quad: stacked bands overlapped a row each and drew dark lines
                m_gfx->FillRectGradientV((int)x, (int)(y + h - 70 * sc), (int)w, (int)(70 * sc) + 1,
                                         SDL_Color{ 0, 0, 0, 0 }, SDL_Color{ 0, 0, 0, (Uint8)(A * 140 / 255) });
            std::string name = it.kind == ItemKind::GameLibrary ? std::string(T("Game Library"))
                             : it.kind == ItemKind::VideoLibrary ? std::string(T("Video Library"))
                             : it.kind == ItemKind::MusicPlayer ? std::string(T("Music Library"))
                             : it.kind == ItemKind::Album       ? std::string(T("Picture Library"))
                             : prof                             ? std::string(m_nickname) : it.name;
            MText(XF_Semi, pt, (int)(x + 22 * sc), (int)(y + h - 50 * sc), WithAlpha(kWhite, A),
                  MFit(XF_Semi, pt, name, (int)(w - 44 * sc)).c_str());
        }
        if (it.kind == ItemKind::Game && it.app_id == m_suspended && m_suspended != 0)
            m_gfx->FillRect((int)(x + w - 26 * sc), (int)(y + 12 * sc), (int)(12 * sc), (int)(12 * sc),
                            SDL_Color{ 160, 220, 40, A });
        if (lit < 0.99f)
            m_gfx->FillRect((int)x, (int)y, (int)w, (int)h, SDL_Color{ 0, 0, 0, (Uint8)(A * (1.0f - lit)) });
    }

    void Menu::DrawMainNxe() {
        if (!XboxStep()) return;
        const XboxSkin &sk = Skin();
        const Theme &t = m_theme.Current();
        const int ncols = (int)m_xmb_cols.size();

        // The account, top right: name over its play time, then its picture.
        {
            const int ps = 52, px = W - 36 - ps, py = 22;
            if (SDL_Texture *av = MetroAvatar()) m_gfx->DrawImage(av, px, py, ps, ps);
            else m_gfx->FillRect(px, py, ps, ps, SDL_Color{ 255, 255, 255, 60 });
            MText(XF_Semi, 20, px - 10 - MWidth(XF_Semi, 20, m_nickname), py, t.title, m_nickname);
            u64 secs = 0;
            for (const MenuItem &g : m_items)
                if (g.kind == ItemKind::Game)
                    if (const play::PlayInfo *pi = Play(g.app_id)) secs += pi->seconds;
            if (secs) {
                const std::string pt = play::FormatPlaytime(secs);
                MText(XF_Semi, 18, px - 10 - MWidth(XF_Semi, 18, pt.c_str()), py + 26, t.title, pt.c_str());
            }
        }

        // --- the rows: the open channel's, and the ones it is changing with ---
        for (int c = std::min(ncols - 1, m_xmb_col + 1); c >= std::max(0, m_xmb_col - 1); c--) {
            float yoff, ra;
            NxRow((float)c - m_xmb_col_scroll, yoff, ra);
            if (ra <= 0.02f) continue;
            const auto &items = m_xmb_cols[c].items;
            const int count = (int)items.size();
            const float at = c == m_xmb_col ? m_xmb_item_scroll : 0.0f;
            const int first = std::max(0, (int)floorf(at) - 1);
            const int last  = std::min(count - 1, (int)at + kNxN - 1);
            for (int i = last; i >= first; i--) {   // back to front
                float x, y, w, h, al, lit;
                NxPlace((float)i - at, x, y, w, h, al, lit);
                al *= ra;
                if (al < 0.02f || x > W) continue;
                NxePanel(m_items[items[i]], x, y + yoff, w, h, al, lit, c == m_xmb_col && i == m_xmb_item);
            }
            // The Mii stands on the floor in front of the profile panel's right
            // edge, taller than the row, over the panel after it.
            const bool mii = MiiHave();
            for (int i = first; mii && i <= last; i++) {
                if (m_items[items[i]].kind != ItemKind::UserPage) continue;
                float x, y, w, h, al, lit;
                NxPlace((float)i - at, x, y, w, h, al, lit);
                al *= ra;
                if (al < 0.02f) break;
                const Uint8 A = (Uint8)(255.0f * al);
                // The render is a square; the body is its lower ~58%, the feet near its foot.
                const float ms = h * 1.8f, cx = x + w * 0.86f, foot = y + yoff + h * 1.12f;
                if (sk.av_shadow) m_gfx->DrawImage(sk.av_shadow, (int)(cx - ms * 0.14f), (int)(foot - ms * 0.045f),
                                                   (int)(ms * 0.28f), (int)(ms * 0.08f), A);
                else m_gfx->FillRect((int)(cx - ms * 0.07f), (int)(foot - 3), (int)(ms * 0.14f), 5, SDL_Color{ 0, 0, 0, (Uint8)(A * 50 / 255) });
                MiiDraw((int)(cx - ms / 2), (int)(foot - ms * 0.975f), (int)ms, A);
                break;
            }
        }

        // --- the names: the open channel, and the ones before it above ---
        for (int c = 0; c < ncols; c++) {
            const float d = (float)c - m_xmb_col_scroll;
            const float al = NxNameAlpha(d);
            if (al <= 0.02f) continue;
            std::string name = !m_xmb_cols[c].label.empty() ? m_xmb_cols[c].label.c_str()
                             : m_xmb_cols[c].cat == XmbCat::User ? T("My Switch")   // the home channel
                             : XmbCatName(m_xmb_cols[c].cat);
            const float near = std::max(0.0f, 1.0f - std::abs(d));
            if (near > 0.5f && !m_search.empty()) name = std::string(T("Search")) + ": \"" + m_search + "\"";
            const int pt = NxNameSize(d);
            const int y = (int)NxNameY(d) - pt * 2 / 3;
            MText(XF_Semi, pt, kNxX, y, WithAlpha(near > 0.5f ? t.title : t.fg, (Uint8)(255 * al * (0.55f + 0.45f * near))),
                  name.c_str());
            if (near > 0.5f)   // the small square the open channel is marked with
                m_gfx->FillRect(kNxX - 14, y + pt / 2 - 2, 7, 7, WithAlpha(t.title, (Uint8)(255 * al)));
        }

        // Under the front panel: where it is in the row.
        const float slide = std::min(1.0f, std::abs((float)m_xmb_col - m_xmb_col_scroll) * 1.6f);
        const int count = (int)m_xmb_cols[m_xmb_col].items.size();
        if (slide < 0.97f && count > 0) {
            char pos[48];
            snprintf(pos, sizeof(pos), "%d %s %d", m_xmb_item + 1, T("of"), count);
            MText(XF_Semi, 19, kNxX, (int)(kNxCy + kNxH[0] / 2 + 12), WithAlpha(t.dim, (Uint8)(255 * (1.0f - slide))), pos);
        }
        m_xb_hint_x = kNxX;
        DrawStatusHint({ {{"a"}, "Select"}, {{"y"}, "Search"}, {{"x"}, "Options"} });
    }

    // The Game Library: every game as a list on the grey sheet, the selected
    // one's panel beside it, and what to show and in which order above.
    void Menu::DrawNxeLibrary() {
        static const char *kShow[] = { "All Games", "Favourites", "Recently Played", "Game Card", "Homebrew" };
        static const char *kSort[] = { "Recently Played", "Title", "Most Played" };
        const XboxSkin &sk = Skin();
        const Theme &t = m_theme.Current();
        m_icons.SetScale(0);
        std::vector<int> list;
        MetroLibList(list);
        const int n = (int)list.size();
        if (n) { m_metro_lib_cur = std::clamp(m_metro_lib_cur, 0, n - 1); m_cursor = list[m_metro_lib_cur]; }

        // The channel it was opened from, small, over its own name.
        MText(XF_Semi, 26, kNxX, 132, WithAlpha(t.fg, 150), T(m_search.empty() ? "My Switch" : "Games"));
        MText(XF_Semi, 40, kNxX, 170, t.title, T("Game Library"));
        m_gfx->FillRect(kNxX - 14, 170 + 18, 7, 7, t.title);

        // Show / Sort: up from the top of the list lands here.
        {
            int x = kNxX;
            for (int k = 0; k < 2; k++) {
                const std::string s = std::string(T(k ? "Sort" : "Show")) + ": " + T(k ? kSort[m_metro_lib_sort] : kShow[m_metro_lib_filter]);
                const int w = MWidth(XF_Semi, 19, s.c_str()) + 24;
                const bool on = m_metro_lib_hdr == k;
                if (on) {
                    if (sk.highlight) m_gfx->Draw9(sk.highlight, x - 12, 226, w, 32, std::min(sk.b_highlight, 14), kWhite);
                    else m_gfx->FillRect(x - 12, 226, w, 32, SDL_Color{ 120, 180, 30, 255 });
                }
                MText(XF_Semi, 19, x, 230, on ? kWhite : WithAlpha(t.fg, 210), s.c_str());
                x += w + 18;
            }
        }

        // The list.
        constexpr int lx = kNxX - 8, ly = 268, lw = 520, rh = 44, rows = 7;
        const int shown = std::max(1, std::min(n, rows));
        if (sk.panel) m_gfx->Draw9(sk.panel, lx, ly, lw, shown * rh + 16, sk.b_panel, kWhite);
        else m_gfx->FillRect(lx, ly, lw, shown * rh + 16, SDL_Color{ 60, 62, 66, 235 });
        m_metro_lib_scroll += ((float)m_metro_lib_cur - m_metro_lib_scroll) * 0.3f;
        const float top = std::clamp(m_metro_lib_scroll - (rows - 1) / 2.0f, 0.0f, (float)std::max(0, n - rows));
        const SDL_Color plain = sk.has_list ? sk.list_text : kNxInk;
        const SDL_Color lit = sk.has_sel ? sk.list_sel : kWhite;
        if (!n) MText(XF_Semi, 21, lx + 22, ly + 18, plain, T("No games here"));
        for (int i = std::max(0, (int)top); i < n && i <= (int)top + rows; i++) {
            const float ry = ly + 8 + ((float)i - top) * rh;
            if (ry < ly + 4 || ry + rh > ly + 12 + shown * rh) continue;
            const bool sel = i == m_metro_lib_cur && m_metro_lib_hdr < 0;
            if (sel) {
                if (sk.highlight) m_gfx->Draw9(sk.highlight, lx + 6, (int)ry + 2, lw - 12, rh - 4, sk.b_highlight, kWhite);
                else m_gfx->FillRect(lx + 6, (int)ry + 2, lw - 12, rh - 4, SDL_Color{ 120, 180, 30, 255 });
            } else if (i + 1 < n) {
                m_gfx->FillRect(lx + 14, (int)ry + rh - 1, lw - 28, 1, SDL_Color{ 255, 255, 255, 40 });
            }
            const MenuItem &it = m_items[list[i]];
            if (SDL_Texture *art = ItemArt(it)) m_gfx->DrawImage(art, lx + 16, (int)ry + 6, rh - 12, rh - 12);
            MText(XF_Semi, 20, lx + 16 + rh, (int)ry + (rh - 27) / 2, sel ? lit : plain,
                  MFit(XF_Semi, 20, it.name, lw - rh - 40).c_str());
        }
        if (n) {
            char pos[48];
            snprintf(pos, sizeof(pos), "%d %s %d", m_metro_lib_cur + 1, T("of"), n);
            MText(XF_Semi, 19, kNxX, ly + shown * rh + 26, WithAlpha(t.dim, 230), pos);
        }

        // The selected game's details, as Blades shows them.
        if (n) {
            const MenuItem &sel = m_items[list[m_metro_lib_cur]];
            DrawBladesGameInfo(sel, BladesInfo(sel), lx + lw + 24, ly, W - 88 - (lx + lw + 24), shown * rh + 16, 255);
        }
        DrawStatusHint({ {{"a"}, "Play"}, {{"b"}, "Back"}, {{"y"}, "Search"}, {{"x"}, "Options"} });
    }

    int Menu::NxeColAt(int x, int y) const {
        if (x < kNxX - 20 || x > kNxX + 520 || y > kNxTitleY + 24) return -1;
        for (int c = 0; c < m_xmb_col; c++) {
            const float d = (float)c - m_xmb_col_scroll;
            if (NxNameAlpha(d) > 0.02f && std::abs((float)y - NxNameY(d)) <= 14.0f) return c;
        }
        return -1;
    }
    int Menu::NxeItemAt(int x, int y) const {
        if (m_xmb_col < 0 || m_xmb_col >= (int)m_xmb_cols.size()) return -1;
        const auto &items = m_xmb_cols[m_xmb_col].items;
        const int count = (int)items.size();
        for (int i = std::max(0, (int)floorf(m_xmb_item_scroll)); i < count && i <= (int)m_xmb_item_scroll + kNxN; i++) {
            float px, py, w, h, al, lit;
            NxPlace((float)i - m_xmb_item_scroll, px, py, w, h, al, lit);
            if (al > 0.1f && x >= px && x < px + w && y >= py && y < py + h) return items[i];
        }
        return -1;
    }

    // ---------------------------------------------------------------------------
    // The Mii picker (Theming > Mii)
    // ---------------------------------------------------------------------------
    namespace {
        // Half size, colour weighted by alpha so transparent texels do not
        // darken the edges. The picker's Miis are small: 2 MB each, not 8.
        mii::Image Halve(const mii::Image &a) {
            mii::Image o;
            o.w = a.w / 2; o.h = a.h / 2;
            o.px.resize((size_t)o.w * o.h * 4);
            for (int y = 0; y < o.h; y++)
                for (int x = 0; x < o.w; x++) {
                    unsigned c[3] = {}, al = 0;
                    for (int k = 0; k < 4; k++) {
                        const uint8_t *p = &a.px[((size_t)(2 * y + (k >> 1)) * a.w + 2 * x + (k & 1)) * 4];
                        for (int i = 0; i < 3; i++) c[i] += p[i] * p[3];
                        al += p[3];
                    }
                    uint8_t *q = &o.px[((size_t)y * o.w + x) * 4];
                    for (int i = 0; i < 3; i++) q[i] = al ? (uint8_t)(c[i] / al) : 0;
                    q[3] = (uint8_t)(al / 4);
                }
            return o;
        }
    }

    void Menu::MiiPickLoad(void *self) {
        Menu *m = (Menu *)self;
        MiiPickSlot &sl = m->m_mp_slot[m->m_mp_job];
        sl.failed = !LoadMiiScene(sl.scene, m->m_mp_hex, "miipk", sl.idx);
        if (!sl.failed) { sl.small = Halve(sl.scene.Atlas()); sl.scene.DropAtlas(); }
        m->m_mp_state.store(2, std::memory_order_release);
    }

    void Menu::OpenMiiPicker() {
        MiiEnsureList();
        const int n = (int)m_mii_list.size() + 1;   // entry 0: the account's own
        m_mp_cursor = std::clamp(m_mii_pick, 0, n - 1);
        m_mp_pos = (float)(m_mp_cursor % kMiiPickPage);
        m_mp_open = true;
        if (m_screen != Screen::MiiPicker) m_mp_from = m_screen;
        m_screen = Screen::MiiPicker;
    }
    void Menu::MiiPickFree() {
        if (m_mp_state.load() != 0) {
            threadWaitForExit(&m_mp_thread);
            threadClose(&m_mp_thread);
            m_mp_state.store(0);
        }
        for (MiiPickSlot &sl : m_mp_slot) {
            if (sl.tex) m_gfx->FreeImage(sl.tex);
            sl = MiiPickSlot{};
        }
        m_mp_job = -1;
        m_mp_open = false;
    }
    void Menu::CloseMiiPicker() {
        MiiPickFree();
        m_screen = m_mp_from;
    }

    Menu::Action Menu::OnButtonMiiPicker(Btn b) {
        const int n = (int)m_mii_list.size() + 1;
        if (b == Btn::Left && m_mp_cursor > 0) m_mp_cursor--;
        else if (b == Btn::Right && m_mp_cursor < n - 1) m_mp_cursor++;
        else if (b == Btn::L && m_mp_cursor >= kMiiPickPage) m_mp_cursor -= kMiiPickPage;
        else if (b == Btn::R && m_mp_cursor / kMiiPickPage < (n - 1) / kMiiPickPage)
            m_mp_cursor = std::min(n - 1, m_mp_cursor + kMiiPickPage);
        else if (b == Btn::A) {
            if (m_mp_cursor != m_mii_pick) {
                m_mii_pick = m_mp_cursor;
                SaveSettings();
                MiiReset();
            }
            CloseMiiPicker();
        } else if (b == Btn::B) {
            CloseMiiPicker();
        }
        return Action::None;
    }

    namespace {
        // The picker's row: five places, and the silhouettes beyond them.
        constexpr int kMpX0 = 294, kMpStep = 173, kMpGround = 548, kMpSize = 420;
        inline int MpX(int slot) { return kMpX0 + slot * kMpStep; }
    }

    int Menu::MiiPickAt(int x) const {
        // -1 / kMiiPickPage: the silhouettes (the page before / after).
        const int i = (int)std::floor((x - (kMpX0 - kMpStep / 2)) / (float)kMpStep);
        return std::clamp(i, -1, kMiiPickPage);
    }

    void Menu::DrawMiiPicker() {
        const XboxSkin &sk = Skin();
        const int n = (int)m_mii_list.size() + 1;
        const int page = m_mp_cursor / kMiiPickPage, first = page * kMiiPickPage;
        const int last = std::min(n, first + kMiiPickPage) - 1, pages = (n + kMiiPickPage - 1) / kMiiPickPage;
        // Collect a finished load; start the next one of this page, nearest the cursor first.
        if (m_mp_state.load(std::memory_order_acquire) == 2) {
            threadWaitForExit(&m_mp_thread);
            threadClose(&m_mp_thread);
            MiiPickSlot &sl = m_mp_slot[m_mp_job];
            if (!sl.failed) {
                sl.tex = SDL_CreateTexture(m_gfx->Renderer(), SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, sl.small.w, sl.small.h);
                if (sl.tex && SDL_UpdateTexture(sl.tex, nullptr, sl.small.px.data(), sl.small.w * 4)) {
                    SDL_SetTextureBlendMode(sl.tex, SDL_BLENDMODE_BLEND);
                    SDL_SetTextureScaleMode(sl.tex, SDL_SCALEMODE_LINEAR);
                } else {
                    if (sl.tex) { m_gfx->FreeImage(sl.tex); sl.tex = nullptr; }
                    sl.failed = true;
                }
            }
            sl.small = mii::Image{};
            m_mp_job = -1;
            m_mp_state.store(0);
        }
        if (m_mp_state.load() == 0) {
            auto held = [&](int e) { for (const MiiPickSlot &sl : m_mp_slot) if (sl.idx == e) return true; return false; };
            int want = -1;
            for (int d = 0; d < kMiiPickPage * 2 && want < 0; d++) {
                const int e = m_mp_cursor + ((d & 1) ? -(d + 1) / 2 : d / 2);
                if (e >= first && e <= last && !held(e)) want = e;
            }
            int slot = -1;
            for (int i = 0; want >= 0 && i < kMiiPickSlots && slot < 0; i++)
                if (m_mp_slot[i].idx < first || m_mp_slot[i].idx > last) slot = i;   // free, or another page's
            if (slot >= 0) {
                MiiPickSlot &sl = m_mp_slot[slot];
                if (sl.tex) m_gfx->FreeImage(sl.tex);
                sl = MiiPickSlot{};
                sl.idx = want;
                const int li = want > 0 ? want - 1 : (m_mii_account >= 0 ? m_mii_account : 0);
                m_mp_hex = li < (int)m_mii_list.size() ? m_mii_list[li].hex : "";
                m_mp_job = slot;
                struct stat fs {};
                if (m_mp_hex.empty() || stat("sdmc:/slaunch/mii/MiiBody.dat", &fs) != 0) {
                    sl.failed = true;   // nothing to draw it with: the name alone
                    m_mp_job = -1;
                } else if (R_SUCCEEDED(threadCreate(&m_mp_thread, &Menu::MiiPickLoad, this, nullptr, 0x20000, 0x3E, -2))) {
                    m_mp_state.store(1);
                    if (R_FAILED(threadStart(&m_mp_thread))) { threadClose(&m_mp_thread); m_mp_state.store(0); sl.failed = true; m_mp_job = -1; }
                } else {
                    sl.failed = true;
                    m_mp_job = -1;
                }
            }
        }

        const SDL_Color ink = sk.has_text ? sk.text : kNxInk;
        const char *title = T("Choose your Mii");
        MText(XF_Semi, 20, 640 - MWidth(XF_Semi, 20, title) / 2, 74, WithAlpha(ink, 225), title);
        const double t = (double)armGetSystemTick() / (double)armGetSystemTickFreq();

        // The beam: a soft column of light from the top of the screen to the
        // floor, gliding to the selected place.
        m_mp_pos += ((m_mp_cursor - first) - m_mp_pos) * 0.22f;
        {
            const float bx = MpX(0) + m_mp_pos * kMpStep, hw = 62.0f, fy = kMpGround - 8.0f;
            const float x[4] = { bx - hw, bx - hw * 0.45f, bx + hw * 0.45f, bx + hw };
            const float a[4] = { 0.0f, 0.62f, 0.62f, 0.0f };
            SDL_Vertex v[18];
            int k = 0;
            for (int c = 0; c < 3; c++) {
                auto put = [&](float px, float py, float al) {
                    const float glow = 1.0f - py / fy * 0.55f;   // fades towards the floor
                    v[k++] = SDL_Vertex{ { px, py }, { 0.55f, 0.82f, 1.0f, al * glow }, { 0, 0 } };
                };
                put(x[c], 0, a[c]); put(x[c + 1], 0, a[c + 1]); put(x[c + 1], fy, a[c + 1]);
                put(x[c], 0, a[c]); put(x[c + 1], fy, a[c + 1]); put(x[c], fy, a[c]);
            }
            m_gfx->DrawGeometry(nullptr, v, k);
        }

        // The silhouettes: a Mii of this page in black, for the pages either side.
        const MiiPickSlot *anyMii = nullptr;
        for (const MiiPickSlot &sl : m_mp_slot) if (sl.tex && sl.idx >= first && sl.idx <= last) { anyMii = &sl; break; }
        auto silhouette = [&](int at, bool next) {
            if (!anyMii) return;
            MiiPickSlot &sl = const_cast<MiiPickSlot &>(*anyMii);
            std::vector<mii::Vertex> vs;
            const float ms = kMpSize * 0.86f, cx = (float)MpX(at);
            sl.scene.SetFloat(0);
            sl.scene.Build(t + sl.idx * 3.7, cx - ms / 2, kMpGround - ms * 0.975f, ms, 1.0f, vs);
            for (mii::Vertex &q : vs) { q.r = q.g = q.b = 0.04f; }
            m_gfx->DrawGeometry(sl.tex, (const SDL_Vertex *)vs.data(), (int)vs.size());
            // |< or >| on its chest.
            const float y = kMpGround - ms * 0.42f, s = 11.0f, d = next ? 1.0f : -1.0f;
            SDL_Vertex tri[9] = {
                { { cx - d * s, y - s }, { 1, 1, 1, 0.9f }, { 0, 0 } },
                { { cx - d * s, y + s }, { 1, 1, 1, 0.9f }, { 0, 0 } },
                { { cx + d * s * 0.7f, y }, { 1, 1, 1, 0.9f }, { 0, 0 } },
                { { cx + d * s * 0.8f, y - s }, { 1, 1, 1, 0.9f }, { 0, 0 } },
                { { cx + d * s * 1.15f, y - s }, { 1, 1, 1, 0.9f }, { 0, 0 } },
                { { cx + d * s * 1.15f, y + s }, { 1, 1, 1, 0.9f }, { 0, 0 } },
                { { cx + d * s * 0.8f, y - s }, { 1, 1, 1, 0.9f }, { 0, 0 } },
                { { cx + d * s * 1.15f, y + s }, { 1, 1, 1, 0.9f }, { 0, 0 } },
                { { cx + d * s * 0.8f, y + s }, { 1, 1, 1, 0.9f }, { 0, 0 } },
            };
            m_gfx->DrawGeometry(nullptr, tri, 9);
        };

        // The page: the selected one last, over its neighbours' hands.
        MiiPickSlot *order[kMiiPickSlots];
        int cnt = 0;
        for (MiiPickSlot &sl : m_mp_slot) if (sl.tex && sl.idx >= first && sl.idx <= last) order[cnt++] = &sl;
        std::sort(order, order + cnt, [&](MiiPickSlot *a, MiiPickSlot *b) {
            return (a->idx == m_mp_cursor) < (b->idx == m_mp_cursor); });
        for (int i = 0; i < cnt; i++) {
            MiiPickSlot &sl = *order[i];
            sl.fade = std::min(1.0f, sl.fade + 0.07f);
            sl.up += ((sl.idx == m_mp_cursor ? 1.0f : 0.0f) - sl.up) * 0.12f;
            const float up = sl.up, ms = (float)kMpSize, cx = (float)MpX(sl.idx - first);
            const float lift = up * (ms * 0.17f + 5.0f * (float)std::sin(t * 1.6));
            const Uint8 A = (Uint8)(255.0f * sl.fade);
            const float sw = ms * 0.27f * (1.0f - 0.25f * up);   // the shadow stays on the floor, smaller
            if (sk.av_shadow) m_gfx->DrawImage(sk.av_shadow, (int)(cx - sw / 2), (int)(kMpGround - ms * 0.04f), (int)sw, (int)(ms * 0.075f), (Uint8)(A * (1.0f - 0.35f * up)));
            else m_gfx->FillRect((int)(cx - sw / 4), kMpGround - 3, (int)(sw / 2), 5, SDL_Color{ 0, 0, 0, (Uint8)(A * 50 / 255) });
            sl.scene.SetFloat(up);
            sl.scene.Build(t + sl.idx * 3.7, cx - ms / 2, kMpGround - lift - ms * 0.975f, ms, sl.fade, sl.verts);
            MiiMaskUpload(sl.scene, sl.tex, 256);
            // In the beam the Mii takes its colour: lavender.
            const float tr = 1.0f - 0.22f * up, tg = 1.0f - 0.30f * up;
            for (mii::Vertex &v : sl.verts) { v.r = v.r * tr + 0.06f * up; v.g = v.g * tg + 0.04f * up; v.b = std::min(1.0f, v.b + 0.22f * up); }
            m_gfx->DrawGeometry(sl.tex, (const SDL_Vertex *)sl.verts.data(), (int)sl.verts.size());
        }
        if (page > 0) silhouette(-1, false);
        if (page < pages - 1) silhouette(kMiiPickPage, true);

        // The selected one's name, and the page.
        const std::string name = MiiPickName(m_mp_cursor);
        const char *sub = m_mp_cursor == m_mii_pick ? T("In use") : m_mp_cursor == 0 ? T("Follows the account's picture") : "";
        std::string line = name + (*sub ? std::string("  -  ") + sub : std::string());
        MText(XF_Light, 26, 640 - MWidth(XF_Light, 26, line.c_str()) / 2, 584, ink, line.c_str());
        char pg[48];
        snprintf(pg, sizeof(pg), T("Page %d of %d"), page + 1, pages);
        MText(XF_Semi, 16, 640 - MWidth(XF_Semi, 16, pg) / 2, 632, WithAlpha(ink, 170), pg);
        DrawStatusHint({ {{"a"}, "Select"}, {{"b"}, "Back"} });
    }

    int Menu::XboxItemPitch() const { return m_ui_mode == UiMode::NXE ? kNxPitch : kBlRowH; }

} // namespace sl::menu::ui
