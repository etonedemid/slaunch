#include <sl/menu/ui/Menu.hpp>
#include <sl/menu/ui/Locale.hpp>
#include <SDL3_image/SDL_image.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <ctime>
#include <algorithm>
#include <sys/stat.h>
#include <dirent.h>
#include <map>
#include <vector>
#include "Menu_Internal.hpp"

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
        constexpr int   kNxX      = 110;
        constexpr int   kNxSize   = 290;
        constexpr int   kNxTop    = 208;
        constexpr int   kNxTitleY = 150;    // centre line of the open channel's name
        constexpr int   kNxNextY  = 606;    // ...and of the one below
        constexpr int   kNxPeek   = 428;    // how far down the next channel's row waits
        constexpr int   kNxPitch  = 162;    // finger travel per panel

        // Panel d places from the front one: where it sits, how big, how lit.
        void NxPlace(float d, float &x, float &cy, float &size, float &a, float &lit) {
            if (d >= 0.0f) {
                const float sc = powf(0.8f, d);
                size = kNxSize * sc;
                x    = kNxX + 2.8f * kNxSize * (1.0f - sc);
                cy   = kNxTop + kNxSize / 2.0f - 46.0f * (1.0f - sc);
                a    = std::clamp((9.0f - d) / 2.0f, 0.0f, 1.0f);
                lit  = std::clamp(1.0f - 0.12f * d, 0.3f, 1.0f);
            } else {   // on its way out, toward the viewer
                size = kNxSize * (1.0f - 0.25f * d);
                x    = kNxX + d * (kNxSize + 120.0f);
                cy   = kNxTop + kNxSize / 2.0f - 20.0f * d;
                a    = std::clamp(1.0f + d * 1.4f, 0.0f, 1.0f);
                lit  = 1.0f;
            }
        }
        // A channel's row, rd channels below the open one: how far down, how lit, how solid.
        void NxRow(float rd, float &yoff, float &lit, float &a) {
            if (rd <= 0.0f)      { yoff = rd * 240.0f; lit = 1.0f; a = std::clamp(1.0f + rd * 1.5f, 0.0f, 1.0f); }
            else if (rd <= 1.0f) { yoff = kNxPeek * rd; lit = 1.0f - 0.6f * rd; a = 1.0f; }
            else                 { yoff = kNxPeek + (rd - 1.0f) * 180.0f; lit = 0.4f; a = std::clamp(2.0f - rd, 0.0f, 1.0f); }
        }
        float NxNameY(float d) {
            if (d <= 0.0f) return kNxTitleY + d * 40.0f - (d < 0.0f ? std::min(1.0f, -d) * 12.0f : 0.0f);
            if (d <= 1.0f) return kNxTitleY + (kNxNextY - kNxTitleY) * d;
            return kNxNextY + (d - 1.0f) * 60.0f;
        }
        float NxNameAlpha(float d) {
            if (d <= 0.0f) return std::clamp(1.0f + d * 0.38f, 0.0f, 1.0f);
            return std::clamp(1.0f - 0.45f * d, 0.0f, 1.0f) * std::clamp(2.0f - d, 0.0f, 1.0f);
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
        const Pick kPickNxe[]    = { { "panel", { "panel_bg.png" } } };
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
                for (const Pick &pk : kPickNxe) take(pk);
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
        SDL_Texture *bg = nullptr;                          // NXE: the whole screen
        SDL_Texture *face = nullptr;                        // Blades: any open blade, tinted its colour
        SDL_Texture *spine = nullptr, *spine_r = nullptr;   // Blades: one blade's tile, left / right stack
        SDL_Texture *light = nullptr;                       // Blades: the lighting over an open blade
        SDL_Texture *light_w = nullptr;                     // ...and over one that fills the screen (no bands)
        SDL_Texture *cat_face[6] = {}, *cat_spine[6] = {};  // Blades: one category's, drawn as is
        SDL_Texture *panel = nullptr, *highlight = nullptr, *card = nullptr, *detail = nullptr;
        SDL_Texture *frame = nullptr, *front = nullptr;     // NXE: over every panel / the front one
        SDL_Texture *btn[4] = {};                           // a b x y
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
        if (m_xskin && m_xskin->dir == dir) return *m_xskin;
        XboxSkinDrop();
        m_xskin = new XboxSkin;
        XboxSkin &s = *m_xskin;
        s.dir = dir;
        // What sLaunch ships, for whatever the skin leaves alone.
        auto builtin = [&] {
            if (m_ui_mode != UiMode::Blades) return;
            auto own = [&](const char *file) -> SDL_Texture * {
                const std::string p = std::string(kXbArt) + file;
                return Exists(p) ? m_gfx->LoadImage(p.c_str()) : nullptr;
            };
            bool theirs = s.spine != nullptr;
            for (SDL_Texture *t : s.cat_spine) theirs = theirs || t;
            if (!theirs) { s.spine = own("spine.png"); s.spine_r = own("spine_right.png"); }
            if (!s.light) s.light = own("light.png");
            if (!s.light_w) s.light_w = own("light_wide.png");
        };
        if (dir.empty()) { builtin(); return s; }
        auto img = [&](const std::string &base) -> SDL_Texture * {
            for (const char *ext : { ".png", ".jpg" }) {
                const std::string p = dir + "/" + base + ext;
                struct stat st {};
                if (stat(p.c_str(), &st) == 0)
                    if (SDL_Texture *t = m_gfx->LoadImage(p.c_str())) return t;
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
        s.panel = img("panel");
        s.highlight = img("highlight");
        s.card = img("gamercard");
        s.detail = img("detail");
        s.frame = img("frame");
        s.front = img("front");
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
        return s;
    }

    void Menu::XboxSkinDrop() {
        if (!m_xskin) return;
        XboxSkin &s = *m_xskin;
        std::vector<SDL_Texture *> all = { s.bg, s.face, s.spine, s.spine_r, s.light, s.light_w, s.panel, s.highlight, s.card,
                                           s.detail, s.frame, s.front };
        all.insert(all.end(), std::begin(s.cat_face), std::end(s.cat_face));
        all.insert(all.end(), std::begin(s.cat_spine), std::end(s.cat_spine));
        all.insert(all.end(), std::begin(s.btn), std::end(s.btn));
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

    // Pop-ups: the screen dimmed, and a pale card in the blade's colour with a
    // darker bar for its title, as the gamer card has. Returns the card's left edge.
    int Menu::BladesPopup(int cw, int ch, const std::string &title) {
        const SDL_Color base = BladesColorOf(m_xmb_col);
        int left, right;
        BladesSpan(left, right);
        const int x = std::clamp((left + right - cw) / 2, 8, W - cw - 8), y = std::max(12, (H - ch) / 2);
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
        const int x = BladesPopup(cw, ch, (m_cursor >= 0 && m_cursor < (int)m_items.size())
                                          ? m_items[m_cursor].name : std::string(T("Options")));
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
    // Blades: the main screen
    // ---------------------------------------------------------------------------
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
                if (SDL_Texture *av = MetroAvatar())
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
            if (dw >= 200) {
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
    void Menu::DrawMainNxe() {
        if (!XboxStep()) return;
        const XboxSkin &sk = Skin();
        Theme t = m_theme.Current();   // a copy: the skin's colours win over the theme's here
        if (sk.has_text) t.title = sk.text;
        if (sk.has_dim)  { t.dim = sk.dim; t.fg = sk.dim; }
        if (sk.bg) m_gfx->DrawCover(sk.bg);
        const int ncols = (int)m_xmb_cols.size();
        DrawTopBar(nullptr);

        // --- the rows of panels: the open channel's, the one leaving above it
        // and the next one waiting at the bottom. Solid, back to front, and
        // darker the further off they are - never see-through. ---
        for (int c = std::min(ncols - 1, m_xmb_col + 2); c >= std::max(0, m_xmb_col - 2); c--) {
            float yoff, rlit, ra;
            NxRow((float)c - m_xmb_col_scroll, yoff, rlit, ra);
            if (ra <= 0.02f || kNxTop + yoff > H) continue;
            const auto &items = m_xmb_cols[c].items;
            const int count = (int)items.size();
            const float at = c == m_xmb_col ? m_xmb_item_scroll : 0.0f;
            const int first = std::max(0, (int)floorf(at) - 1);
            const int last  = std::min(count - 1, (int)at + 9);
            for (int i = last; i >= first; i--) {
                float x, cy, size, al, lit;
                NxPlace((float)i - at, x, cy, size, al, lit);
                const Uint8 a = (Uint8)(255.0f * al * ra);
                if (a < 6) continue;
                cy += yoff;
                lit *= rlit;
                const MenuItem &it = m_items[items[i]];
                const float y = cy - size / 2.0f, sc = size / kNxSize;
                const bool front = c == m_xmb_col && i == m_xmb_item;
                SDL_Texture *art = ItemArt(it);
                gfx::Gfx::CardStyle cs;
                cs.radius = 9.0f * sc;
                cs.shadow = 22.0f * sc;
                cs.glow   = (front && !sk.front) ? 0.5f : 0.0f;
                if (!art) cs.fill = SDL_Color{ t.icon_bg.r, t.icon_bg.g, t.icon_bg.b, 255 };
                const bool gpu = m_gfx->Card(art, x, y, size, size, cs, a);
                if (!gpu) {
                    if (art) m_gfx->DrawImage(art, (int)x, (int)y, (int)size, (int)size, a);
                    else     m_gfx->FillRect((int)x, (int)y, (int)size, (int)size, WithAlpha(cs.fill, a));
                }
                if (!art) {
                    if (SDL_Texture *icon = SystemIcon(it.kind)) {
                        const int is = (int)(size * 0.5f);
                        m_gfx->DrawImageTinted(icon, (int)(x + size / 2) - is / 2, (int)cy - is / 2, is, is,
                                               IconTint(t, a));
                    } else if (!it.name.empty()) {
                        char initial[2] = { (char)toupper((unsigned char)it.name[0]), '\0' };
                        m_gfx->TextCentered(FontSize::Title, (int)(x + size / 2),
                                            (int)cy - m_gfx->LineHeight(FontSize::Title) / 2,
                                            WithAlpha(t.dim, a), initial);
                    }
                }
                if (it.kind == ItemKind::Game && it.app_id == m_suspended && m_suspended != 0)
                    m_gfx->FillRect((int)(x + size) - 22, (int)y + 10, 12, 12, WithAlpha(t.accent, a));
                // The skin's frame over every panel, or its own for the front one.
                if (SDL_Texture *fr = front && sk.front ? sk.front : sk.frame)
                    m_gfx->Draw9(fr, (int)x, (int)y, (int)size, (int)size, (int)(sk.b_frame * sc + 0.5f),
                                 kWhite, a);
                if (lit < 0.99f) {   // distance, as shade
                    gfx::Gfx::CardStyle shade;
                    shade.radius = cs.radius; shade.shadow = 0.0f;
                    shade.fill = SDL_Color{ 0, 0, 0, (Uint8)(255.0f * (1.0f - lit)) };
                    if (!gpu || !m_gfx->Card(nullptr, x, y, size, size, shade, a))
                        m_gfx->FillRect((int)x, (int)y, (int)size, (int)size, WithAlpha(shade.fill, (Uint8)(shade.fill.a * a / 255)));
                }
            }
        }

        // --- the channel names: the open one large, the ones above it fading
        // off up the screen, the next one down by its waiting row ---
        for (int c = 0; c < ncols; c++) {
            const float d = (float)c - m_xmb_col_scroll;
            const float al = NxNameAlpha(d);
            if (al <= 0.02f) continue;
            std::string name = m_xmb_cols[c].label.empty() ? XmbCatName(m_xmb_cols[c].cat)
                                                            : m_xmb_cols[c].label.c_str();
            const float near = std::max(0.0f, 1.0f - std::abs(d));
            if (near > 0.5f && !m_search.empty()) name = std::string(T("Search")) + ": \"" + m_search + "\"";
            const int pt = (int)lroundf(24.0f + 26.0f * near);
            MText(XF_Light, pt, kNxX, (int)NxNameY(d) - pt * 2 / 3,
                  WithAlpha(near > 0.5f ? t.title : t.fg, (Uint8)(255 * al)), name.c_str());
        }

        // Under the front panel: what it is.
        const float slide = std::min(1.0f, std::abs((float)m_xmb_col - m_xmb_col_scroll) * 1.6f);
        const XmbColumn &col = m_xmb_cols[m_xmb_col];
        const int count = (int)col.items.size();
        if (slide < 0.97f && count > 0) {
            const MenuItem &sel = m_items[col.items[std::clamp(m_xmb_item, 0, count - 1)]];
            const Uint8 a = (Uint8)(255.0f * (1.0f - slide));
            const int ty = kNxTop + kNxSize + 12;
            MText(XF_Semi, 30, kNxX, ty, WithAlpha(t.title, a), MFit(XF_Semi, 30, sel.name, W - 2 * kNxX).c_str());
            const std::string info = XboxInfo(sel);
            if (!info.empty())
                MText(XF_Semi, 18, kNxX, ty + 42, WithAlpha(t.dim, a), MFit(XF_Semi, 18, info, W - 2 * kNxX).c_str());
            if (m_show_counter) {
                char pos[48];
                snprintf(pos, sizeof(pos), "%d %s %d", m_xmb_item + 1, T("of"), count);
                MText(XF_Semi, 17, W - 60 - MWidth(XF_Semi, 17, pos), kNxTitleY - 8, WithAlpha(t.dim, a), pos);
            }
        }
        m_xb_hint_x = 760;   // clear of the row waiting at the bottom left
        DrawStatusHint({ {{"a"}, "Select"}, {{"y"}, "Search"},
                         {{"l","r"}, XmbShoulderHint()}, {{"x"}, "Options"} });
    }

    int Menu::NxeColAt(int x, int y) const {
        if (x < kNxX - 20 || x > kNxX + 460) return -1;
        // The row waiting at the bottom is the next channel, whatever part of it is touched.
        if (y >= kNxNextY - 20 && m_xmb_col + 1 < (int)m_xmb_cols.size()) return m_xmb_col + 1;
        for (int c = 0; c < m_xmb_col; c++) {
            const float d = (float)c - m_xmb_col_scroll;
            if (NxNameAlpha(d) > 0.02f && std::abs((float)y - NxNameY(d)) <= 18.0f) return c;
        }
        return -1;
    }
    int Menu::NxeItemAt(int x, int y) const {
        if (m_xmb_col < 0 || m_xmb_col >= (int)m_xmb_cols.size()) return -1;
        const auto &items = m_xmb_cols[m_xmb_col].items;
        const int count = (int)items.size();
        // Front to back, as they overlap.
        for (int i = std::max(0, (int)floorf(m_xmb_item_scroll)); i < count && i <= (int)m_xmb_item_scroll + 9; i++) {
            float cx, cy, size, al, lit;
            NxPlace((float)i - m_xmb_item_scroll, cx, cy, size, al, lit);
            if (al > 0.1f && x >= cx && x < cx + size && y >= cy - size / 2 && y < cy + size / 2) return items[i];
        }
        return -1;
    }

    int Menu::XboxItemPitch() const { return m_ui_mode == UiMode::NXE ? kNxPitch : kBlRowH; }

} // namespace sl::menu::ui
