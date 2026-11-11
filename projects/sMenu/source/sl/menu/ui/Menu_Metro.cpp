#include <sl/menu/ui/Menu.hpp>
#include <sl/menu/ui/Locale.hpp>
#include <sl/smi/Protocol.hpp>
#include <sys/stat.h>
#include <SDL3_image/SDL_image.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <ctime>
#include <algorithm>
#include "Menu_Internal.hpp"

// Metro: the Xbox 360 dashboard as it was from the fall 2012 update on. Pivot
// tabs in light lowercase over a strip of panels, one open at a time with the
// panels either side peeking in at the screen edges; each panel a fixed
// arrangement of tiles, green ones for places and art ones for games; a card
// view of the whole library ("my games"); and the Guide, the tabbed panel that
// HOME brings up over whatever is on screen. Sizes and colours are measured off
// 1280x720 captures of the real thing.
//
// The type is Selawik, Microsoft's open (OFL) stand-in for Segoe UI, shipped in
// sdmc:/slaunch/fonts/metro. Without it everything still draws, in the system
// font. Nothing of Microsoft's is drawn: no logos, no "Xbox" or "bing" labels.

namespace sl::menu::ui {

    namespace {
        const char *const kMtFace[3] = {
            "sdmc:/slaunch/fonts/metro/selawkl.ttf",    // light: tabs, titles
            "sdmc:/slaunch/fonts/metro/selawksl.ttf",   // semilight: everything else
            "sdmc:/slaunch/fonts/metro/selawksb.ttf",   // semibold: button letters
        };
        enum { MF_Light, MF_Semi, MF_Bold };

        constexpr SDL_Color kMtGreen   {  14, 150,  14, 255 };
        constexpr SDL_Color kMtWhite   { 255, 255, 255, 255 };
        constexpr SDL_Color kMtInk     {  40,  40,  42, 255 };

        // The open panel's top-left, one cell, and the gap between panels -
        // which is what leaves a strip of each neighbour on screen.
        constexpr int kMtGX = 231, kMtGY = 181;
        constexpr int kMtCW = 173, kMtRH = 130, kMtGap = 4;
        // Every panel gets a slot at least as wide as the home panel, so the
        // next one always starts just inside the right edge of the screen.
        constexpr int kMtPanelGap = 164, kMtSlotW = 820;
        constexpr int kMtTabsX = 78, kMtTabsY = 108, kMtTabPt = 34, kMtTabGap = 38;
        constexpr int kMtHintY = 652;
        // A settings list: its first row, row pitch and rows to a page.
        constexpr int kMtListY = 118, kMtListRH = 50, kMtListRows = 10;
        inline float MtListTop(float scroll, int n) {
            return std::clamp(scroll - kMtListRows / 2 + 1, 0.0f, (float)std::max(0, n - kMtListRows));
        }
        constexpr Uint8 kMtPeek = 105;   // neighbouring panels, faded

        // "my games"
        constexpr int kMtCardW = 222, kMtCardH = 400, kMtCardY = 185, kMtCardX = 68;
        constexpr int kMtCardPitch = kMtCardW + 9;

        inline SDL_Color Fade(SDL_Color c, Uint8 a) {
            return SDL_Color{ c.r, c.g, c.b, (Uint8)((int)c.a * a / 255) };
        }
        inline float Ease(float t) { t = std::clamp(t, 0.0f, 1.0f); return 1.0f - (1.0f - t) * (1.0f - t); }
        inline u64 MsSince(u64 tick) {
            return (armGetSystemTick() - tick) * 1000 / armGetSystemTickFreq();
        }
    }

    // ---- type ---------------------------------------------------------------
    void Menu::MText(int face, int pt, int x, int y, SDL_Color c, const char *s) {
        // Blades and NXE take the skin's font, when it has one.
        const char *path = (XboxOld() && !m_xb_font.empty()) ? m_xb_font.c_str() : kMtFace[face];
        if (TTF_Font *f = m_gfx->Face(path, pt)) { m_gfx->Text(f, x, y, c, s); return; }
        const FontSize fs = pt <= 18 ? FontSize::Small : pt <= 24 ? FontSize::Normal
                          : pt <= 36 ? FontSize::Large : FontSize::Title;
        m_gfx->Text(fs, x, y, c, s);
    }
    int Menu::MWidth(int face, int pt, const char *s) {
        const char *path = (XboxOld() && !m_xb_font.empty()) ? m_xb_font.c_str() : kMtFace[face];
        if (TTF_Font *f = m_gfx->Face(path, pt)) return m_gfx->TextWidth(f, s);
        const FontSize fs = pt <= 18 ? FontSize::Small : pt <= 24 ? FontSize::Normal
                          : pt <= 36 ? FontSize::Large : FontSize::Title;
        return m_gfx->TextWidth(fs, s);
    }
    std::string Menu::MFit(int face, int pt, const std::string &s, int maxw) {
        if (MWidth(face, pt, s.c_str()) <= maxw) return s;
        int lo = 0, hi = (int)s.size();
        while (lo < hi) {
            int mid = (lo + hi + 1) / 2;
            while (mid > lo && ((unsigned char)s[mid] & 0xC0) == 0x80) mid--;   // UTF-8
            if (mid == lo) break;
            if (MWidth(face, pt, (s.substr(0, mid) + "...").c_str()) <= maxw) lo = mid;
            else hi = mid - 1;
        }
        while (lo > 0 && ((unsigned char)s[lo] & 0xC0) == 0x80) lo--;
        return s.substr(0, lo) + "...";
    }

    // ---- what goes on the panels ---------------------------------------------
    void Menu::MetroRecent(std::vector<int> &out) const {
        out.clear();
        for (int i = 0; i < (int)m_items.size(); i++)
            if (m_items[i].kind == ItemKind::Game) out.push_back(i);
        std::stable_sort(out.begin(), out.end(), [&](int a, int b) {
            const play::PlayInfo *pa = Play(m_items[a].app_id);
            const play::PlayInfo *pb = Play(m_items[b].app_id);
            return (pa ? pa->last_played : 0) > (pb ? pb->last_played : 0);
        });
        // The game you are in comes first, played or not: it is what the
        // first tile offers to resume.
        if (m_suspended)
            for (size_t k = 0; k < out.size(); k++)
                if (m_items[out[k]].app_id == m_suspended) {
                    const int v = out[k];
                    out.erase(out.begin() + k);
                    out.insert(out.begin(), v);
                    break;
                }
    }
    int Menu::MetroFindKind(ItemKind k) const {
        for (int i = 0; i < (int)m_items.size(); i++)
            if (m_items[i].kind == k) return i;
        return -1;
    }

    // Each panel in the same cell units as the dashboard: 173x130 cells on a
    // 4px gap, three rows high.
    void Menu::MetroBuild(int panel, std::vector<MetroTile> &out) const {
        out.clear();
        constexpr int C = kMtCW + kMtGap, R = kMtRH + kMtGap;
        auto add = [&](int x, int y, int w, int h, int item, int special,
                       ItemKind kind, std::string cap) {
            MetroTile t;
            t.x = x; t.y = y; t.w = w; t.h = h;
            t.item = item; t.special = special; t.kind = kind;
            t.caption = std::move(cap);
            out.push_back(std::move(t));
        };
        auto kindTile = [&](int x, int y, int w, int h, ItemKind k, const char *cap) {
            add(x, y, w, h, -1, MS_Kind, k, T(cap));
        };
        auto gameTile = [&](int x, int y, int w, int h, int idx) {
            add(x, y, w, h, idx, MS_None, ItemKind::Game, m_items[idx].name);
        };

        std::vector<int> rec;
        MetroRecent(rec);
        size_t next = 0;
        auto game = [&]() { return next < rec.size() ? rec[next++] : -1; };

        switch (panel) {
            case MP_Search:
                // At the right of its slot, so it is what peeks in from the
                // left of home.
                add(kMtSlotW - (C + kMtCW), R / 2, C + kMtCW, 2 * kMtRH + kMtGap, -1, MS_Search,
                    ItemKind::Game, T("Search"));
                break;

            case MP_Home: {
                // Column one: the game to go back to, then pins and recent.
                const int g0 = game();
                if (g0 >= 0) {
                    add(0, 0, kMtCW, kMtRH, g0, MS_None, ItemKind::Game,
                        std::string(m_items[g0].app_id == m_suspended && m_suspended
                                    ? T("Resume") : T("Play")) + " " + m_items[g0].name);
                } else {
                    add(0, 0, kMtCW, kMtRH, -1, MS_MyGames, ItemKind::Game, T("My Games"));
                }
                add(0, R,     kMtCW, kMtRH, -1, MS_Pins,   ItemKind::Game, T("My Pins"));
                add(0, 2 * R, kMtCW, kMtRH, -1, MS_Recent, ItemKind::Game, T("Recent"));

                // The big one: the dashboard's spotlight, here a slow turn
                // through the next few games you played.
                std::vector<int> spot;
                for (int k = 0; k < 4; k++) { const int g = game(); if (g >= 0) spot.push_back(g); }
                if (!spot.empty()) {
                    const int g = spot[m_metro_spot % (int)spot.size()];
                    add(C, 0, 466, 2 * kMtRH + kMtGap, g, MS_Spotlight, ItemKind::Game,
                        m_items[g].name);
                    out.back().kind = (ItemKind)(int)spot.size();   // dot count, see the draw
                } else {
                    kindTile(C, 0, 466, 2 * kMtRH + kMtGap, ItemKind::Theming, "Preferences");
                }

                // Around it: more games, then places once the games run out.
                static const struct { ItemKind k; const char *cap; } fill[] = {
                    { ItemKind::Album, "Album" }, { ItemKind::MusicPlayer, "Music" },
                    { ItemKind::WebBrowser, "Web Browser" }, { ItemKind::Themes, "Themes" },
                    { ItemKind::Power, "Turn Off" },
                };
                int f = 0;
                const int slots[5][3] = { { C + 470, 0, kMtCW }, { C + 470, R, kMtCW },
                                          { C, 2 * R, 231 }, { C + 235, 2 * R, 231 },
                                          { C + 470, 2 * R, kMtCW } };
                for (const auto &s : slots) {
                    const int g = game();
                    if (g >= 0) gameTile(s[0], s[1], s[2], kMtRH, g);
                    else        { kindTile(s[0], s[1], s[2], kMtRH, fill[f].k, fill[f].cap); f++; }
                }
                break;
            }

            case MP_Social:
                add(0, 0, 350, 3 * kMtRH + 2 * kMtGap, -1, MS_Gamer, ItemKind::UserPage, m_nickname);
                kindTile(354, 0,     kMtCW, kMtRH, ItemKind::UserPage,    "Friends");
                kindTile(354, R,     kMtCW, kMtRH, ItemKind::MiiEdit,     "Avatar");
                kindTile(354, 2 * R, kMtCW, kMtRH, ItemKind::Controllers, "Controllers");
#ifdef SL_QEXT
                kindTile(354 + C, 0, kMtCW, 2 * kMtRH + kMtGap, ItemKind::Discord, "Discord");
#endif
                break;

            case MP_Games: {
                add(0, 0, kMtCW, 2 * kMtRH + kMtGap, -1, MS_MyGames, ItemKind::Game, T("My Games"));
                kindTile(0, 2 * R, kMtCW, kMtRH, ItemKind::RandomGame, "Random Game");
                const int g0 = game();
                if (g0 >= 0) gameTile(C, 0, 466, 2 * kMtRH + kMtGap, g0);
                const int slots[5][3] = { { C, 2 * R, 231 }, { C + 235, 2 * R, 231 },
                                          { C + 470, 0, kMtCW }, { C + 470, R, kMtCW },
                                          { C + 470, 2 * R, kMtCW } };
                for (const auto &s : slots) {
                    const int g = game();
                    if (g < 0) break;
                    gameTile(s[0], s[1], s[2], kMtRH, g);
                }
                break;
            }

            case MP_Media:
                kindTile(0, 0,     kMtCW, kMtRH, ItemKind::Album,       "Album");
                kindTile(0, R,     kMtCW, kMtRH, ItemKind::MusicPlayer, "Music");
                kindTile(0, 2 * R, kMtCW, kMtRH, ItemKind::FileManager, "Files");
                kindTile(C, 0, 350, 2 * kMtRH + kMtGap, ItemKind::WebBrowser, "Web Browser");
                kindTile(C, 2 * R, 350, kMtRH, ItemKind::Themes, "Themes");
                break;

            case MP_Apps: {
                kindTile(0, 0, kMtCW, 2 * kMtRH + kMtGap, ItemKind::HomebrewMenu, "My Apps");
                kindTile(0, 2 * R, kMtCW, kMtRH, ItemKind::FileManager, "File Manager");
                // Pinned homebrew, three to a column like the tiles beside it.
                int k = 0;
                for (int i = 0; i < (int)m_items.size(); i++) {
                    if (m_items[i].kind != ItemKind::Homebrew) continue;
                    add(C + (k / 3) * C, (k % 3) * R, kMtCW, kMtRH, i, MS_None,
                        ItemKind::Homebrew, m_items[i].name);
                    k++;
                }
                break;
            }

            case MP_Settings: {
                // Two rows of four, centred in the three-row band.
                const int y0 = R / 2;
                add(0, y0, kMtCW, kMtRH, -1, MS_System, ItemKind::Settings, T("System"));
                kindTile(C,     y0, kMtCW, kMtRH, ItemKind::Theming,     "Preferences");
                kindTile(2 * C, y0, kMtCW, kMtRH, ItemKind::UserPage,    "Profile");
                kindTile(3 * C, y0, kMtCW, kMtRH, ItemKind::Wifi,        "Network");
                kindTile(0,     y0 + R, kMtCW, kMtRH, ItemKind::Controllers, "Controllers");
                kindTile(C,     y0 + R, kMtCW, kMtRH, ItemKind::Bluetooth,   "Bluetooth");
                add(2 * C, y0 + R, kMtCW, kMtRH, -1, MS_About, ItemKind::Settings, T("About"));
                kindTile(3 * C, y0 + R, kMtCW, kMtRH, ItemKind::Power,       "Turn Off");
                break;
            }
        }
    }
    // Left edge of a panel's slot along the strip.
    int Menu::MetroPanelX(int panel) const {
        int x = 0;
        std::vector<MetroTile> tiles;
        for (int p = 0; p <= panel; p++) {
            MetroBuild(p, tiles);
            int w = 0;
            for (const MetroTile &t : tiles) w = std::max(w, t.x + t.w);
            const int slot = std::max(w, kMtSlotW);
            if (p == panel) return x;
            x += slot + kMtPanelGap;
        }
        return x;
    }

    // ---- drawing ------------------------------------------------------------
    // The 2012 backdrop: charcoal at the top lightening to a pale band about
    // where the tiles stand, as if on a lit floor, then dimming again. One
    // texture a pixel wide, stretched - a gradient the GPU filters for free.
    void Menu::MetroDrawBackdrop() {
        if (!m_metro_bg) {
            struct Stop { int y; SDL_Color c; };
            static const Stop stops[] = {
                {   0, {  80,  81,  86, 255 } }, { 160, { 104, 105, 110, 255 } },
                { 300, { 132, 133, 137, 255 } }, { 450, { 164, 164, 166, 255 } },
                { 575, { 182, 182, 182, 255 } }, { 625, { 176, 176, 177, 255 } },
                { 720, { 142, 143, 147, 255 } },
            };
            constexpr int H = 256;
            if (SDL_Surface *s = SDL_CreateSurface(1, H, SDL_PIXELFORMAT_RGBA32)) {
                Uint8 *px = (Uint8 *)s->pixels;
                for (int y = 0; y < H; y++) {
                    const float fy = y * 720.0f / (H - 1);
                    int k = 0;
                    while (k < 5 && stops[k + 1].y < fy) k++;
                    const float u = (fy - stops[k].y) / (float)(stops[k + 1].y - stops[k].y);
                    const SDL_Color a = stops[k].c, b = stops[k + 1].c;
                    Uint8 *p = px + y * s->pitch;
                    p[0] = (Uint8)(a.r + (b.r - a.r) * u);
                    p[1] = (Uint8)(a.g + (b.g - a.g) * u);
                    p[2] = (Uint8)(a.b + (b.b - a.b) * u);
                    p[3] = 255;
                }
                m_metro_bg = SDL_CreateTextureFromSurface(m_gfx->Renderer(), s);
                SDL_DestroySurface(s);
                if (m_metro_bg) SDL_SetTextureScaleMode(m_metro_bg, SDL_SCALEMODE_LINEAR);
            }
        }
        if (m_metro_bg) m_gfx->DrawImage(m_metro_bg, 0, 0, gfx::Gfx::Width, gfx::Gfx::Height);
        else            m_gfx->FillRect(0, 0, gfx::Gfx::Width, gfx::Gfx::Height, SDL_Color{ 128, 129, 133, 255 });
    }

    // The signed-in account's picture, once.
    SDL_Texture *Menu::MetroAvatar() {
        if (m_metro_avatar_tried) return m_metro_avatar;
        m_metro_avatar_tried = true;
#ifdef SL_QEXT
        const int size = accountUidIsValid(&m_user) ? qext_account_image_size(m_user) : -1;
        if (size > 0 && size <= 0x40000) {
            std::vector<u8> buf(size);
            const int got = qext_account_image(m_user, buf.data(), size);
            if (got == size) m_metro_avatar = m_gfx->LoadImageScaled(buf.data(), got, 256, 256);
        }
#else
        AccountProfile p;
        if (!accountUidIsValid(&m_user) || R_FAILED(accountGetProfile(&p, m_user))) return nullptr;
        u32 size = 0;
        if (R_SUCCEEDED(accountProfileGetImageSize(&p, &size)) && size > 0 && size <= 0x40000) {
            std::vector<u8> buf(size);
            u32 got = 0;
            if (R_SUCCEEDED(accountProfileLoadImage(&p, buf.data(), size, &got)) && got == size)
                m_metro_avatar = m_gfx->LoadImageScaled(buf.data(), got, 256, 256);
        }
        accountProfileClose(&p);
#endif
        return m_metro_avatar;
    }

    // A game with no wide art: its icon over a blurred copy of itself, the
    // way the dashboard filled a tile it had no art for. Kept for a handful.
    SDL_Texture *Menu::MetroFill(const MenuItem &it, SDL_Texture *src) {
        const u64 key = it.app_id ? it.app_id : std::hash<std::string>{}(it.hb_path);
        auto f = m_metro_fill.find(key);
        if (f != m_metro_fill.end()) return f->second;
        if (m_metro_fill.size() >= 8) {
            for (auto &kv : m_metro_fill) if (kv.second) m_gfx->FreeImage(kv.second);
            m_metro_fill.clear();
        }
        SDL_Texture *t = m_gfx->Blurred(src, 24);
        m_metro_fill[key] = t;
        return t;
    }

    void Menu::MetroDrawTile(const MetroTile &t, int px, int py, float scale, Uint8 a, bool focus) {
        const int w = (int)lroundf(t.w * scale), h = (int)lroundf(t.h * scale);
        const int x = px + t.x - (w - t.w) / 2, y = py + t.y - (h - t.h) / 2;
        const MenuItem *it = (t.item >= 0 && t.item < (int)m_items.size()) ? &m_items[t.item] : nullptr;

        bool dark_caption = false;
        if (t.special == MS_Gamer) {
            // The gamer card: the account picture over a pale plate, as the
            // social hub stood your avatar on one.
            m_gfx->FillRect(x, y, w, h, Fade(SDL_Color{ 232, 233, 235, 255 }, a));
            const int s = std::min(w - 40, h - 90);
            if (SDL_Texture *av = MetroAvatar()) m_gfx->DrawImage(av, x + (w - s) / 2, y + 26, s, s, a);
            else {
                m_gfx->FillRect(x + (w - s) / 2, y + 26, s, s, Fade(SDL_Color{ 200, 202, 205, 255 }, a));
                if (SDL_Texture *ic = SystemIcon(ItemKind::UserPage))
                    m_gfx->DrawImageTinted(ic, x + (w - s / 2) / 2, y + 26 + s / 4, s / 2, s / 2,
                                           SDL_Color{ 255, 255, 255, 255 }, a);
            }
            dark_caption = true;
        } else if (it) {
            // Art: wide key art where the tile is wide enough to want it, else
            // the icon cropped to the tile; for the big tiles with no key art,
            // the icon over its own blur.
            SDL_Texture *wide = (it->kind == ItemKind::Game) ? HeroArt(*it) : nullptr;
            SDL_Texture *icon = ItemArt(*it);
            m_gfx->FillRect(x, y, w, h, Fade(SDL_Color{ 30, 30, 32, 255 }, a));
            if (wide) {
                m_gfx->DrawImageCrop(wide, x, y, w, h, a);
            } else if (icon && t.h > kMtRH + 10) {
                if (SDL_Texture *fill = MetroFill(*it, icon)) {
                    m_gfx->DrawImageCrop(fill, x, y, w, h, a);
                    m_gfx->FillRect(x, y, w, h, Fade(SDL_Color{ 0, 0, 0, 70 }, a));
                }
                const int s = h - 40;
                m_gfx->DrawImage(icon, x + (w - s) / 2, y + 12, s, s, a);
            } else if (icon) {
                m_gfx->DrawImageCrop(icon, x, y, w, h, a);
            }
            // The dark band captions on art sit on.
            m_gfx->FillRect(x, y + h - 34, w, 34, Fade(SDL_Color{ 0, 0, 0, 140 }, a));
        } else {
            // A place: flat green with a white glyph.
            m_gfx->FillRect(x, y, w, h, Fade(m_mt_accent, a));
            const int s = std::clamp((int)(std::min(w, h) * 0.42f), 40, 84);
            const int cx = x + w / 2, cy = y + (h - 30) / 2 + 2;
            const SDL_Color wc = Fade(m_mt_on, a);
            SDL_Texture *ic = nullptr;
            switch (t.special) {
                case MS_Kind:    ic = SystemIcon(t.kind); break;
                case MS_MyGames: ic = SystemIcon(ItemKind::Game); break;
                default: break;
            }
            // Network wears the radio-and-cog icon the old Settings entry had.
            if (t.special == MS_Kind && t.kind == ItemKind::Wifi) ic = SystemIcon(ItemKind::Settings);
            if (ic) {
                m_gfx->DrawImageTinted(ic, cx - s / 2, cy - s / 2, s, s, wc, a);
            } else {
                // A ring is a white disc with a green one on it, both drawn as
                // round cards so the edges are smooth.
                auto disc = [&](float rx, float ry, float r, SDL_Color c) {
                    gfx::Gfx::CardStyle cs;
                    cs.radius = r; cs.shadow = 0.0f; cs.fill = c;
                    return m_gfx->Card(nullptr, rx - r, ry - r, 2 * r, 2 * r, cs, 255);
                };
                auto ring = [&](float rx, float ry, float r, float wdt) {
                    if (disc(rx, ry, r + wdt / 2, wc) && disc(rx, ry, r - wdt / 2, Fade(m_mt_accent, a)))
                        return;
                    constexpr int N = 40;
                    for (int i = 0; i < N; i++) {
                        const float a0 = i * 6.2831853f / N, a1 = (i + 1) * 6.2831853f / N;
                        m_gfx->LineAA(rx + r * cosf(a0), ry + r * sinf(a0),
                                      rx + r * cosf(a1), ry + r * sinf(a1), wc, wdt);
                    }
                };
                const float r = s * 0.40f;
                switch (t.special) {
                    case MS_Recent:   // a clock at ten past the hour
                        ring((float)cx, (float)cy, r, 4.0f);
                        m_gfx->FillRect(cx - 2, cy - (int)(r * 0.62f), 4, (int)(r * 0.62f) + 2, wc);
                        m_gfx->FillRect(cx - 2, cy - 2, (int)(r * 0.5f) + 2, 4, wc);
                        break;
                    case MS_Search:   // magnifier
                        ring(cx - r * 0.25f, cy - r * 0.25f, r * 0.8f, 4.5f);
                        m_gfx->LineAA(cx + r * 0.32f, cy + r * 0.32f, cx + r, cy + r, wc, 7.0f);
                        break;
                    case MS_Pins: {
                        // A pushpin leaning right: needle, plate, neck, cap,
                        // each a thick stroke across or along its axis.
                        const float d = r * 1.1f, ux = 0.7071f, uy = -0.7071f;
                        auto at = [&](float along, float across, float &ox, float &oy) {
                            ox = cx + ux * along - uy * across;
                            oy = cy + uy * along + ux * across;
                        };
                        // LineAA is for hairlines; the wide strokes are quads.
                        auto bar = [&](float a0, float a1, float wdt) {
                            float x0, y0, x1, y1;
                            at(a0, -wdt / 2, x0, y0); at(a1, -wdt / 2, x1, y1);
                            float x2, y2, x3, y3;
                            at(a1, wdt / 2, x2, y2); at(a0, wdt / 2, x3, y3);
                            m_gfx->FillTriangle((int)x0, (int)y0, (int)x1, (int)y1, (int)x2, (int)y2, wc);
                            m_gfx->FillTriangle((int)x0, (int)y0, (int)x2, (int)y2, (int)x3, (int)y3, wc);
                        };
                        float x0, y0, x1, y1;
                        at(-0.95f * d, 0, x0, y0); at(-0.25f * d, 0, x1, y1);
                        m_gfx->LineAA(x0, y0, x1, y1, wc, 3.0f);   // needle
                        bar(-0.3f * d, -0.14f * d, 1.0f * d);       // plate
                        bar(-0.14f * d, 0.42f * d, 0.36f * d);      // neck
                        bar(0.42f * d, 0.72f * d, 0.66f * d);       // cap
                        break;
                    }
                    case MS_System: {   // the console, joy-cons on
                        const int bw = (int)(s * 0.9f), bh = (int)(s * 0.52f);
                        const int bx = cx - bw / 2, by = cy - bh / 2, jw = bw / 6;
                        m_gfx->FillRect(bx, by, jw, bh, wc);
                        m_gfx->FillRect(bx + bw - jw, by, jw, bh, wc);
                        m_gfx->FillRect(bx + jw + 2, by, bw - 2 * jw - 4, 3, wc);
                        m_gfx->FillRect(bx + jw + 2, by + bh - 3, bw - 2 * jw - 4, 3, wc);
                        m_gfx->FillRect(bx + jw + 2, by, 3, bh, wc);
                        m_gfx->FillRect(bx + bw - jw - 5, by, 3, bh, wc);
                        break;
                    }
                    case MS_About:    // an i in a ring
                        ring((float)cx, (float)cy, r, 4.0f);
                        m_gfx->FillRect(cx - 2, cy - (int)(r * 0.15f), 5, (int)(r * 0.6f), wc);
                        m_gfx->FillRect(cx - 2, cy - (int)(r * 0.55f), 5, 5, wc);
                        break;
                    default: break;
                }
            }
        }

        const int pt = t.h > kMtRH + 10 && t.w > 300 ? 21 : 19;
        const bool running = it && it->kind == ItemKind::Game && m_suspended && it->app_id == m_suspended;
        // Who is online in a game, at the right of its caption as a live tile
        // counted things, clear of the running badge.
        int cw = 0;
        if (const int on = it ? OnlineCount(*it) : -1; on >= 0) {
            char n[12];
            snprintf(n, sizeof(n), "%d", on);
            const int nw = MWidth(MF_Semi, pt, n), nx = x + w - 10 - (running ? 26 : 0) - nw;
            MText(MF_Semi, pt, nx, y + h - (pt + 11), Fade(kMtWhite, a), n);
            OnlineDot(nx - 15, y + h - (pt + 11) + pt * 6 / 10 - 5, 10, on, a);
            cw = nw + 25 + (running ? 26 : 0);
        }
        const std::string cap = MFit(MF_Semi, pt, t.caption, w - 20 - cw);
        MText(MF_Semi, pt, x + 10, y + h - (pt + 11),
              Fade(dark_caption ? kMtInk : it ? kMtWhite : m_mt_on, a), cap.c_str());

        if (running) {
            // Running: the disc badge the dashboard put on the game in the tray.
            const int bx = x + w - 22, by = y + h - 19;
            m_gfx->FillRect(bx - 7, by - 7, 14, 14, Fade(kMtWhite, a));
            m_gfx->FillRect(bx - 2, by - 2, 4, 4, Fade(SDL_Color{ 30, 30, 32, 255 }, a));
        }
        if (t.special == MS_Spotlight) {
            // Which of the spotlight's games this is, under the grid below
            // its right edge.
            const int n = (int)t.kind;
            const int dy = py + 3 * kMtRH + 2 * kMtGap + 10;
            for (int i = 0; i < n; i++) {
                const bool on = i == m_metro_spot % std::max(1, n);
                m_gfx->FillRect(x + w - (n - i) * 13, dy, 10, 4,
                                Fade(on ? SDL_Color{ 60, 60, 62, 255 } : SDL_Color{ 150, 151, 154, 255 }, a));
            }
        }
        if (focus) {
            // Focus: a hairline of white round the tile, which has grown a
            // little towards you.
            const SDL_Color fc = Fade(m_mt_fg, a);
            m_gfx->FillRect(x - 2, y - 2, w + 4, 2, fc);
            m_gfx->FillRect(x - 2, y + h, w + 4, 2, fc);
            m_gfx->FillRect(x - 2, y, 2, h, fc);
            m_gfx->FillRect(x + w, y, 2, h, fc);
        }
    }

    // Button prompts the dashboard way: the face button's colour in a disc,
    // its letter on it, the word beside.
    void Menu::MetroDrawHints(std::initializer_list<std::pair<char, const char *>> hints, int y) {
        if (!m_show_hints) return;
        int x = 120;
        for (const auto &h : hints) {
            SDL_Color c;
            switch (h.first) {
                case 'A': c = {  96, 181,  46, 255 }; break;
                case 'B': c = { 222,  44,  56, 255 }; break;
                case 'X': c = {  38, 110, 215, 255 }; break;
                default:  c = { 238, 186,  18, 255 }; break;
            }
            constexpr int r = 11;
            gfx::Gfx::CardStyle cs;
            cs.radius = (float)r; cs.shadow = 0.0f; cs.fill = c;
            if (!m_gfx->Card(nullptr, (float)x, (float)(y + 2), 2.0f * r, 2.0f * r, cs, 255))
                m_gfx->FillRect(x + 2, y + 4, 2 * r - 4, 2 * r - 4, c);
            const char letter[2] = { h.first, 0 };
            const int lw = MWidth(MF_Bold, 15, letter);
            MText(MF_Bold, 15, x + r - lw / 2, y + 3, kMtWhite, letter);
            MText(MF_Semi, 19, x + 2 * r + 7, y, m_mt_fg, T(h.second));
            x += 2 * r + 7 + MWidth(MF_Semi, 19, T(h.second)) + 22;
        }
        // A fresh status line goes on the same row, out of the way.
        if (m_status[0] && (armGetSystemTick() - m_status_tick) <
                           m_status_secs * armGetSystemTickFreq()) {
            const int sw = MWidth(MF_Semi, 19, m_status);
            MText(MF_Semi, 19, gfx::Gfx::Width - 100 - sw, y, m_mt_fg, m_status);
        }
    }

    // Top right: name, then the clock and battery where the dashboard kept
    // friends and messages, and the account picture beside them.
    void Menu::MetroDrawUser() {
        const int ax = 1128, ay = 30, as = 56;
        if (SDL_Texture *av = MetroAvatar()) m_gfx->DrawImage(av, ax, ay, as, as, 255);
        else {
            m_gfx->FillRect(ax, ay, as, as, m_mt_accent);
            if (SDL_Texture *ic = SystemIcon(ItemKind::UserPage))
                m_gfx->DrawImageTinted(ic, ax + 10, ay + 10, as - 20, as - 20, m_mt_on, 255);
        }
        const int nw = MWidth(MF_Semi, 21, m_nickname);
        MText(MF_Semi, 21, ax - 12 - nw, ay + 2, Fade(m_mt_fg, 235), m_nickname);

        time_t now = time(nullptr);
        struct tm tm_now;
        localtime_r(&now, &tm_now);
        char line[48];
        const u32 charge = sys::BatteryPercent();
        char clock[16];
        strftime(clock, sizeof(clock), "%H:%M", &tm_now);
        snprintf(line, sizeof(line), "%s   %lu%%", clock, (unsigned long)charge);
        const int lw = MWidth(MF_Semi, 17, line);
        MText(MF_Semi, 17, ax - 12 - lw, ay + 31, Fade(m_mt_fg, 190), line);
    }

    void Menu::MetroSync() {
        std::vector<MetroTile> tiles;
        MetroBuild(m_metro_panel, tiles);
        int &cur = m_metro_last[m_metro_panel];
        if (tiles.empty()) { cur = 0; return; }
        cur = std::clamp(cur, 0, (int)tiles.size() - 1);
        if (!m_metro_hdr && tiles[cur].item >= 0) m_cursor = tiles[cur].item;
    }

    // The dashboard's own grey and green, or the theme's colours over the
    // theme's background (Theming > Metro colours). Text on a tile is white
    // or near-black, whichever the tile colour can carry.
    void Menu::MetroPalette() {
        if (m_metro_themed) {
            const Theme &t = m_theme.Current();
            m_mt_accent = t.accent;
            m_mt_fg     = t.fg;
        } else {
            m_mt_accent = kMtGreen;
            m_mt_fg     = kMtWhite;
        }
        const int luma = (m_mt_accent.r * 299 + m_mt_accent.g * 587 + m_mt_accent.b * 114) / 1000;
        m_mt_on = luma > 165 ? SDL_Color{ 25, 25, 27, 255 } : kMtWhite;
    }

    void Menu::DrawMainMetro() {
        m_icons.SetScale(0);
        MetroPalette();
        // Opened from a game with HOME: the Guide comes up first, over it.
        if (!m_metro_booted) {
            m_metro_booted = true;
            if (m_suspended) MetroOpenGuide(true);
            struct stat st;   // the flag predates nothing: make sure it is there
            if (stat(sl::smi::HomeFrameFlag, &st) != 0)
                if (FILE *ff = fopen(sl::smi::HomeFrameFlag, "w")) fclose(ff);
        }
        // Over a game the Guide sits on the game itself, as the daemon caught
        // it when HOME was pressed. Without a capture, on the dashboard.
        if (m_metro_guide && m_metro_guide_game) {
            if (!m_metro_frame_tried) {
                m_metro_frame_tried = true;
                struct stat st;
                if (stat(sl::smi::HomeFramePath, &st) == 0 && st.st_size > 0)
                    m_metro_frame = m_gfx->LoadImage(sl::smi::HomeFramePath);
            }
            if (m_metro_frame) {
                m_gfx->DrawImage(m_metro_frame, 0, 0, gfx::Gfx::Width, gfx::Gfx::Height);
                DrawMetroGuide();
                return;
            }
        }
        // A search that has just been typed shows its results as cards.
        if (m_search != m_metro_search_seen) {
            m_metro_search_seen = m_search;
            if (!m_search.empty()) { MetroOpenLib(0); return; }
        }

        MetroDrawUser();

        // Spotlight: on to the next game every seven seconds, unless you are
        // looking at it.
        if (m_metro_spot_tick == 0) m_metro_spot_tick = armGetSystemTick();
        std::vector<MetroTile> tiles;
        MetroBuild(m_metro_panel, tiles);
        int &cur = m_metro_last[m_metro_panel];
        if (!tiles.empty()) cur = std::clamp(cur, 0, (int)tiles.size() - 1);
        const bool spot_focused = !tiles.empty() && !m_metro_hdr &&
                                  tiles[cur].special == MS_Spotlight;
        if (!spot_focused && MsSince(m_metro_spot_tick) > 7000) {
            m_metro_spot++;
            m_metro_spot_tick = armGetSystemTick();
        }

        // ---- pivot header ---------------------------------------------------
        {
            int xs[MP_Count], ws[MP_Count], x = 0;
            const char *labels[MP_Count] = { T("search"), T("home"), T("social"), T("games"),
                                             T("media"), T("apps"), T("settings") };
            for (int i = 0; i < MP_Count; i++) {
                ws[i] = MWidth(MF_Light, kMtTabPt, labels[i]);
                xs[i] = x;
                x += ws[i] + kMtTabGap;
            }
            // Slides left only as far as it must to keep the open tab on screen.
            const float want = (float)std::max(0, kMtTabsX + xs[m_metro_panel] + ws[m_metro_panel] - 1150);
            m_metro_hdr_x += (want - m_metro_hdr_x) * 0.25f;
            for (int i = 0; i < MP_Count; i++) {
                const bool on = (i == m_metro_panel);
                const Uint8 al = on ? 255 : 112;
                MText(MF_Light, kMtTabPt, kMtTabsX + xs[i] - (int)m_metro_hdr_x, kMtTabsY,
                      Fade(m_mt_fg, al), labels[i]);
                if (on && m_metro_hdr)   // the header has the focus: say which tab
                    m_gfx->FillRect(kMtTabsX + xs[i] - (int)m_metro_hdr_x, kMtTabsY + 52,
                                    ws[i], 2, m_mt_fg);
            }
        }

        // ---- the panel strip --------------------------------------------------
        const float target = (float)(MetroPanelX(m_metro_panel) - kMtGX);
        if (m_metro_cam < 0.0f) m_metro_cam = target;
        m_metro_cam += (target - m_metro_cam) * 0.22f;
        if (std::abs(target - m_metro_cam) < 0.5f) m_metro_cam = target;

        const float zoom = m_metro_hdr ? 0.0f : Ease(MsSince(m_metro_focus_tick) / 140.0f);
        int px = 0;
        std::vector<MetroTile> pt;
        const MetroTile *focus_t = nullptr;
        int focus_x = 0;
        for (int p = 0; p < MP_Count; p++) {
            MetroBuild(p, pt);
            int w = 0;
            for (const MetroTile &t : pt) w = std::max(w, t.x + t.w);
            const int slot = std::max(w, kMtSlotW);
            const int sx = px - (int)m_metro_cam;
            if (sx < gfx::Gfx::Width && sx + w > 0) {
                const Uint8 a = (p == m_metro_panel) ? 255 : kMtPeek;
                for (int i = 0; i < (int)pt.size(); i++) {
                    if (p == m_metro_panel && i == cur && !m_metro_hdr) continue;   // drawn last
                    MetroDrawTile(pt[i], sx, kMtGY, 1.0f, a, false);
                }
                if (p == m_metro_panel) focus_x = sx;
            }
            px += slot + kMtPanelGap;
        }
        if (!tiles.empty() && !m_metro_hdr) {
            focus_t = &tiles[cur];
            const float grow = 1.0f + 0.05f * zoom * std::min(1.0f, 260.0f / focus_t->w);
            MetroDrawTile(*focus_t, focus_x, kMtGY, grow, 255, true);
        }

        if (!m_items.empty() && !ScrollBusy() && m_cursor < (int)m_items.size())
            FetchArtFor(m_items[m_cursor]);

        if (m_metro_guide) {
            DrawMetroGuide();
            return;
        }
        const bool item = focus_t && focus_t->item >= 0;
        if (item) MetroDrawHints({ { 'A', "Select" }, { 'X', "Options" }, { 'Y', "Search" } }, kMtHintY);
        else      MetroDrawHints({ { 'A', "Select" }, { 'Y', "Search" } }, kMtHintY);
    }

    // ---- the other screens, in Metro's look -----------------------------------
    // Title top left in the pivot's light lowercase, small enough to stay
    // clear of what each screen puts under it; the account block top right.
    void Menu::DrawMetroHeader(const char *title) {
        MetroPalette();
        if (title && *title) {
            std::string s = title;
            for (char &c : s) c = (char)tolower((unsigned char)c);
            MText(MF_Light, 32, kMtTabsX, 22, m_mt_fg, s.c_str());
        }
        MetroDrawUser();
    }

    // A settings list as the dashboard's settings pages drew one: rows down
    // the left, the focused one a bar of the tile colour, values on the right.
    void Menu::DrawCarouselMetro(const std::vector<std::string> &labels,
                                 const std::vector<std::string> &values,
                                 int cursor, float &scroll_pos) {
        MetroPalette();
        const int n = (int)labels.size();
        scroll_pos += (cursor - scroll_pos) * 0.30f;
        if (std::abs(cursor - scroll_pos) < 0.01f) scroll_pos = (float)cursor;

        constexpr int X = kMtGX, W = 818, Y0 = kMtListY, RH = kMtListRH, Rows = kMtListRows;
        const float top = MtListTop(scroll_pos, n);
        for (int i = std::max(0, (int)top - 1); i < n && i <= (int)top + Rows; i++) {
            const int y = Y0 + (int)lroundf((i - top) * RH);
            if (y < Y0 - RH / 2 || y > Y0 + (Rows - 1) * RH + RH / 2) continue;
            const bool sel = (i == cursor);
            if (sel) m_gfx->FillRect(X, y, W, RH - 4, m_mt_accent);
            const SDL_Color c = sel ? m_mt_on : m_mt_fg;
            const std::string &v = i < (int)values.size() ? values[i] : std::string();
            const int vw = v.empty() ? 0 : MWidth(MF_Semi, 21, v.c_str());
            const std::string l = MFit(MF_Semi, 25, labels[i], W - 40 - (vw ? vw + 24 : 0));
            MText(MF_Semi, 25, X + 16, y + 7, c, l.c_str());
            if (vw) MText(MF_Semi, 21, X + W - 16 - vw, y + 11, Fade(c, sel ? 230 : 170), v.c_str());
        }
        // Where you are in a list longer than the page.
        if (n > Rows) {
            const int bh = Rows * RH - 4, th = std::max(24, bh * Rows / n);
            const int ty = Y0 + (int)((bh - th) * (top / std::max(1, n - Rows)));
            m_gfx->FillRect(X + W + 10, Y0, 3, bh, Fade(m_mt_fg, 50));
            m_gfx->FillRect(X + W + 10, ty, 3, th, Fade(m_mt_fg, 200));
        }
    }

    // The sub-screens' prompts, the dashboard way: A/B/X/Y as coloured
    // discs, anything else as its usual glyph in the text colour.
    void Menu::MetroHintSegs(std::initializer_list<HintSeg> segs) {
        MetroPalette();
        int x = 120, y = kMtHintY;
        bool flip = false;
        if (XboxOld()) {   // the 360's own legend, on Blades' colours or the theme's
            const bool main = m_screen == Screen::Main && !m_options_open && m_dialog == Dialog::None;
            m_mt_fg = (main && m_ui_mode == UiMode::Blades) ? kMtWhite : m_theme.Current().fg;
            if (main) x = m_xb_hint_x;
            if (m_xb_hint_y) {   // a legend its screen places by hand, pop-up over it or not
                x = m_xb_hint_x; y = m_xb_hint_y; flip = m_xb_hint_flip;
            } else if (BladesUi()) {   // any other Blades screen: one row in the legend band
                int left, right;
                BladesSpan(left, right);
                x = left; y = BladesHintY();
            }
        }
        constexpr int r = 11;
        for (const HintSeg &seg : segs) {
            if (flip) {   // right-aligned: the word, then its buttons
                const char *label = T(seg.label);
                x -= MWidth(MF_Semi, 19, label) + 6 + (int)seg.icons.size() * (2 * r + 3) - 3;
                MText(MF_Semi, 19, x, y, m_mt_fg, label);
                x += MWidth(MF_Semi, 19, label) + 6;
            }
            for (const char *icon : seg.icons) {
                SDL_Color c{};
                const char k = (icon[0] && !icon[1]) ? icon[0] : 0;
                switch (k) {
                    case 'a': c = {  96, 181,  46, 255 }; break;
                    case 'b': c = { 222,  44,  56, 255 }; break;
                    case 'x': c = {  38, 110, 215, 255 }; break;
                    case 'y': c = { 238, 186,  18, 255 }; break;
                    default: break;
                }
                SDL_Texture *glyph = (c.a && XboxOld()) ? SkinButton(k) : nullptr;
                if (glyph) {
                    m_gfx->DrawImage(glyph, x, y + 2, 2 * r, 2 * r);   // the skin's own button
                } else if (c.a) {
                    gfx::Gfx::CardStyle cs;
                    cs.radius = (float)r; cs.shadow = 0.0f; cs.fill = c;
                    if (!m_gfx->Card(nullptr, (float)x, (float)(y + 2), 2.0f * r, 2.0f * r, cs, 255))
                        m_gfx->FillRect(x + 2, y + 4, 2 * r - 4, 2 * r - 4, c);
                    const char letter[2] = { (char)toupper((unsigned char)k), 0 };
                    MText(MF_Bold, 15, x + r - MWidth(MF_Bold, 15, letter) / 2, y + 3, kMtWhite, letter);
                } else {
                    // Everything else: a grey disc with a white mark - an
                    // arrow for a direction, a cross for the D-pad, the
                    // letter or sign otherwise.
                    gfx::Gfx::CardStyle cs;
                    cs.radius = (float)r; cs.shadow = 0.0f; cs.fill = { 88, 90, 96, 255 };
                    if (!m_gfx->Card(nullptr, (float)x, (float)(y + 2), 2.0f * r, 2.0f * r, cs, 255))
                        m_gfx->FillRect(x + 2, y + 4, 2 * r - 4, 2 * r - 4, cs.fill);
                    const int cx = x + r, cy = y + 2 + r;
                    const std::string nm = icon;
                    if (nm == "up")         m_gfx->FillTriangle(cx - 5, cy + 3, cx + 5, cy + 3, cx, cy - 4, kMtWhite);
                    else if (nm == "down")  m_gfx->FillTriangle(cx - 5, cy - 3, cx + 5, cy - 3, cx, cy + 4, kMtWhite);
                    else if (nm == "left")  m_gfx->FillTriangle(cx + 3, cy - 5, cx + 3, cy + 5, cx - 4, cy, kMtWhite);
                    else if (nm == "right") m_gfx->FillTriangle(cx - 3, cy - 5, cx - 3, cy + 5, cx + 4, cy, kMtWhite);
                    else if (nm == "dpad") {
                        m_gfx->FillRect(cx - 6, cy - 2, 12, 4, kMtWhite);
                        m_gfx->FillRect(cx - 2, cy - 6, 4, 12, kMtWhite);
                    } else {
                        const char *mark = nm == "plus" ? "+" : nm == "minus" ? "-"
                                         : nm == "l" ? "L" : nm == "r" ? "R"
                                         : nm == "zl" ? "ZL" : nm == "zr" ? "ZR" : "RS";
                        const int pt = mark[1] ? 11 : 15;
                        MText(MF_Bold, pt, cx - MWidth(MF_Bold, pt, mark) / 2, cy - pt / 2 - 3,
                              kMtWhite, mark);
                    }
                }
                x += 2 * r + 3;
            }
            if (flip) continue;
            const char *label = T(seg.label);
            MText(MF_Semi, 19, x + 4, y, m_mt_fg, label);
            x += 4 + MWidth(MF_Semi, 19, label) + 22;
        }
    }

    int Menu::MetroListRowAt(int y, float scroll, int rows) const {
        return (int)floorf(MtListTop(scroll, rows) + (float)(y - kMtListY) / (float)kMtListRH);
    }

    // Metro in the 360's own colours, on a screen that takes them (setup and
    // the splash keep the theme's).
    bool Menu::MetroUi() const {
        return m_ui_mode == UiMode::Metro && !m_metro_themed &&
               m_screen != Screen::Oobe && m_screen != Screen::Welcome;
    }
    // What the screens Metro does not draw itself use for colours while it is
    // up: white on charcoal with the dashboard's green, in place of a theme
    // that was picked against some other background.
    void Menu::MetroTheme(Theme &t) const {
        t.bg_top    = SDL_Color{ 52, 53, 57, 255 };
        t.bg_bottom = SDL_Color{ 34, 35, 38, 255 };
        t.fg = t.title = t.icon_fg = kMtWhite;
        t.dim    = SDL_Color{ 200, 202, 207, 255 };
        t.accent = SDL_Color{ 136, 214, 88, 255 };   // the tile green, lightened to read as text
    }

    // Pop-ups as the dashboard drew them: a dark band right across the screen,
    // its content lined up with the tiles, choices as rows with the focus bar.
    void Menu::DrawMetroOptions() {
        MetroPalette();
        const int n = (int)m_options.size();
        constexpr int RH = 48;
        const int bh = 76 + n * RH + 22;
        const int by = std::max(20, (gfx::Gfx::Height - bh) / 2);
        m_gfx->FillRect(0, 0, gfx::Gfx::Width, gfx::Gfx::Height, SDL_Color{ 0, 0, 0, 120 });
        m_gfx->FillRect(0, by, gfx::Gfx::Width, bh, SDL_Color{ 34, 35, 38, 245 });
        const std::string title = (m_cursor >= 0 && m_cursor < (int)m_items.size())
                                ? MFit(MF_Light, 32, m_items[m_cursor].name, 820) : std::string();
        MText(MF_Light, 32, kMtGX, by + 18, kMtWhite, title.c_str());
        for (int i = 0; i < n; i++) {
            const int y = by + 76 + i * RH;
            const bool sel = (i == m_options_cursor);
            if (sel) m_gfx->FillRect(kMtGX, y, 460, RH - 4, m_mt_accent);
            MText(MF_Semi, 23, kMtGX + 16, y + 8, sel ? m_mt_on : kMtWhite,
                  MFit(MF_Semi, 23, m_options[i].label, 428).c_str());
        }
    }

    void Menu::DrawMetroDialog() {
        MetroPalette();
        // The note, wrapped to the band.
        std::vector<std::string> lines;
        {
            std::string line, word;
            auto flush = [&]() {
                if (word.empty()) return;
                const std::string t = line.empty() ? word : line + " " + word;
                if (!line.empty() && MWidth(MF_Semi, 20, t.c_str()) > 800) { lines.push_back(line); line = word; }
                else line = t;
                word.clear();
            };
            for (char c : m_dialog_note) { if (c == ' ') flush(); else word += c; }
            flush();
            if (!line.empty()) lines.push_back(line);
        }
        const int nopts = m_dialog == Dialog::CrashReport ? 1 : 2;
        constexpr int RH = 48;
        const int bh = 80 + (int)lines.size() * 28 + 16 + nopts * RH + 22;
        const int by = (gfx::Gfx::Height - bh) / 2;
        m_gfx->FillRect(0, 0, gfx::Gfx::Width, gfx::Gfx::Height, SDL_Color{ 0, 0, 0, 120 });
        m_gfx->FillRect(0, by, gfx::Gfx::Width, bh, SDL_Color{ 34, 35, 38, 245 });
        MText(MF_Light, 32, kMtGX, by + 20, kMtWhite,
              m_dialog_title.empty() ? T("Close running application?") : m_dialog_title.c_str());
        int y = by + 80;
        for (const std::string &l : lines) { MText(MF_Semi, 20, kMtGX, y, Fade(kMtWhite, 200), l.c_str()); y += 28; }
        y += 16;
        const char *opts[2] = { T("Yes"), T("No") };
        if (nopts == 1) opts[0] = T("OK");
        for (int i = 0; i < nopts; i++, y += RH) {
            const bool sel = (i == m_dialog_cursor);
            if (sel) m_gfx->FillRect(kMtGX, y, 360, RH - 4, m_mt_accent);
            MText(MF_Semi, 23, kMtGX + 16, y + 8, sel ? m_mt_on : kMtWhite, opts[i]);
        }
    }

    // ---- input --------------------------------------------------------------
    Menu::Action Menu::MetroActivate(const MetroTile &t, u64 &out_app_id) {
        switch (t.special) {
            case MS_Pins:    MetroOpenLib(1); return Action::None;
            case MS_Recent:  MetroOpenLib(2); return Action::None;
            case MS_MyGames: MetroOpenLib(0); return Action::None;
            case MS_Search:  OpenSearch();    return Action::None;
            case MS_System:  return Action::OpenSystemSettings;
            case MS_About:   m_screen = Screen::About; m_about_scroll = 0; return Action::None;
            case MS_Gamer:   return Action::OpenUserPage;
            case MS_Kind: {
                const int i = MetroFindKind(t.kind);
                if (i >= 0) { m_cursor = i; return ActivateSelected(out_app_id); }
                MenuItem it{};
                it.kind = t.kind;
                it.name = t.caption;
                return ActivateItem(it, out_app_id);
            }
            default: break;
        }
        if (t.item >= 0 && t.item < (int)m_items.size()) {
            m_cursor = t.item;
            return ActivateSelected(out_app_id);
        }
        return Action::None;
    }

    bool Menu::MetroNav(Btn b, Action &out, u64 &out_app_id) {
        out = Action::None;
        if (m_metro_guide) return MetroGuideNav(b, out, out_app_id);

        std::vector<MetroTile> tiles;
        MetroBuild(m_metro_panel, tiles);
        int &cur = m_metro_last[m_metro_panel];
        if (!tiles.empty()) cur = std::clamp(cur, 0, (int)tiles.size() - 1);

        auto toPanel = [&](int p, int edge, float from_cy) {
            if (p < 0 || p >= MP_Count) return false;
            m_sfx.Play(p > m_metro_panel ? audio::Sfx::PageRight : audio::Sfx::PageLeft);
            m_metro_panel = p;
            m_metro_focus_tick = armGetSystemTick();
            // Coming in from the side: land on the tile at that edge nearest
            // the height you left from. From the bumpers: where you were last.
            if (edge != 0) {
                std::vector<MetroTile> nt;
                MetroBuild(p, nt);
                int best = 0;
                float bd = 1e9f;
                for (int i = 0; i < (int)nt.size(); i++) {
                    const float ex = edge < 0 ? (float)nt[i].x : (float)(nt[i].x + nt[i].w);
                    const float d = (edge < 0 ? ex : -ex) * 4.0f +
                                    std::abs(nt[i].y + nt[i].h * 0.5f - from_cy);
                    if (d < bd) { bd = d; best = i; }
                }
                m_metro_last[p] = best;
            }
            MetroSync();
            return true;
        };

        if (b == Btn::L || b == Btn::R) {
            toPanel(m_metro_panel + (b == Btn::R ? 1 : -1), 0, 0.0f);
            return true;
        }
        if (b == Btn::B) {
            if (!m_search.empty()) return false;   // the shared handler clears it
            if (m_metro_hdr) { m_metro_hdr = false; MetroSync(); return true; }
            // B on any other hub goes home, as it did on the console.
            if (m_metro_panel != MP_Home) toPanel(MP_Home, 0, 0.0f);
            return true;
        }

        if (m_metro_hdr) {
            if (b == Btn::Left)  { toPanel(m_metro_panel - 1, 0, 0.0f); return true; }
            if (b == Btn::Right) { toPanel(m_metro_panel + 1, 0, 0.0f); return true; }
            if (b == Btn::Down || b == Btn::A) {
                m_metro_hdr = false;
                m_metro_focus_tick = armGetSystemTick();
                MetroSync();
                return true;
            }
            return b == Btn::Up || b == Btn::X;
        }
        if (tiles.empty()) {
            if (b == Btn::Up) { m_metro_hdr = true; return true; }
            if (b == Btn::Left)  return toPanel(m_metro_panel - 1, +1, kMtRH * 1.5f), true;
            if (b == Btn::Right) return toPanel(m_metro_panel + 1, -1, kMtRH * 1.5f), true;
            return false;
        }

        const MetroTile &f = tiles[cur];
        const float fcx = f.x + f.w * 0.5f, fcy = f.y + f.h * 0.5f;
        if (b == Btn::Up || b == Btn::Down || b == Btn::Left || b == Btn::Right) {
            // The nearest tile that way: overlap across the move counts for
            // more than distance along it, so a wide tile hands you to the one
            // under the part of it you came from.
            int best = -1;
            float bd = 1e9f;
            for (int i = 0; i < (int)tiles.size(); i++) {
                if (i == cur) continue;
                const MetroTile &c = tiles[i];
                float along, across;
                if (b == Btn::Right || b == Btn::Left) {
                    along = (b == Btn::Right) ? c.x - (f.x + f.w) : f.x - (c.x + c.w);
                    const float ov = std::min(f.y + f.h, c.y + c.h) - std::max(f.y, c.y);
                    across = ov > 0 ? 0.0f : std::abs(c.y + c.h * 0.5f - fcy);
                } else {
                    along = (b == Btn::Down) ? c.y - (f.y + f.h) : f.y - (c.y + c.h);
                    const float ov = std::min(f.x + f.w, c.x + c.w) - std::max(f.x, c.x);
                    across = ov > 0 ? 0.0f : std::abs(c.x + c.w * 0.5f - fcx);
                }
                if (along < -2.0f) continue;
                const float d = along + across * 3.0f;
                if (d < bd) { bd = d; best = i; }
            }
            if (best >= 0) {
                cur = best;
                m_metro_focus_tick = armGetSystemTick();
                MetroSync();
                return true;
            }
            if (b == Btn::Up)    { m_metro_hdr = true; return true; }
            if (b == Btn::Left)  { toPanel(m_metro_panel - 1, +1, fcy); return true; }
            if (b == Btn::Right) { toPanel(m_metro_panel + 1, -1, fcy); return true; }
            return true;
        }
        if (b == Btn::A) { out = MetroActivate(f, out_app_id); return true; }
        if (b == Btn::X) {
            if (f.item < 0) return true;   // options are for games and apps
            m_cursor = f.item;
            return false;                  // the shared handler opens them
        }
        return false;   // Y searches, + is power: the shared handling
    }

    // ---- the Guide ------------------------------------------------------------
    void Menu::MetroGuideRows(int tab, std::vector<MetroGuideRow> &out) const {
        out.clear();
        std::string running;
        int running_i = -1;
        if (m_suspended)
            for (int i = 0; i < (int)m_items.size(); i++)
                if (m_items[i].kind == ItemKind::Game && m_items[i].app_id == m_suspended) {
                    running = m_items[i].name; running_i = i; break;
                }
        switch (tab) {
            case MG_Games: {
                if (m_suspended) {
                    out.push_back({ std::string(T("Resume")) + " " + running, GA_Resume, 0 });
                    out.push_back({ std::string(T("Close")) + " " + running, GA_Close, 0 });
                }
                out.push_back({ T("My Games"), GA_MyGames, 0 });
                std::vector<int> rec;
                MetroRecent(rec);
                int n = 0;
                for (int i : rec) {
                    if (i == running_i) continue;
                    out.push_back({ m_items[i].name, GA_Launch, i });
                    if (++n == 4) break;
                }
                break;
            }
            case MG_User:
                out.push_back({ T("Home"), GA_Home, 0 });
                out.push_back({ T("Friends"), GA_Kind, (int)ItemKind::UserPage });
                out.push_back({ T("Avatar"), GA_Kind, (int)ItemKind::MiiEdit });
                out.push_back({ T("Album"), GA_Kind, (int)ItemKind::Album });
                if (m_suspended)
                    out.push_back({ std::string(T("Resume")) + " " + running, GA_Resume, 0 });
                break;
            case MG_Media:
                out.push_back({ T("Album"), GA_Kind, (int)ItemKind::Album });
                out.push_back({ T("Music"), GA_Kind, (int)ItemKind::MusicPlayer });
                out.push_back({ T("Web Browser"), GA_Kind, (int)ItemKind::WebBrowser });
#ifdef SL_QEXT
                out.push_back({ T("Discord"), GA_Kind, (int)ItemKind::Discord });
#endif
                out.push_back({ T("Files"), GA_Kind, (int)ItemKind::FileManager });
                break;
            default:
                out.push_back({ T("System Settings"), GA_System, 0 });
                out.push_back({ T("Preferences"), GA_Kind, (int)ItemKind::Theming });
                out.push_back({ T("Network"), GA_Kind, (int)ItemKind::Wifi });
                out.push_back({ T("Controllers"), GA_Kind, (int)ItemKind::Controllers });
                out.push_back({ T("Turn Off"), GA_Kind, (int)ItemKind::Power });
                break;
        }
    }

    void Menu::MetroOpenGuide(bool over_game) {
        m_metro_guide      = true;
        m_metro_guide_game = over_game;
        m_metro_gtab       = MG_User;
        m_metro_grow       = 0;
        m_metro_guide_tick = armGetSystemTick();
        m_sfx.Play(audio::Sfx::PageRight);
    }

    void Menu::DrawMetroGuide() {
        const float e = Ease(MsSince(m_metro_guide_tick) / 170.0f);
        // Lighter over the game's own picture than over the dashboard.
        const float dim = (m_metro_guide_game && m_metro_frame) ? 120.0f : 185.0f;
        m_gfx->FillRect(0, 0, gfx::Gfx::Width, gfx::Gfx::Height,
                        SDL_Color{ 0, 0, 0, (Uint8)(dim * e) });

        constexpr int PX = 170, PW = 940, PH = 380, SP = 62, RH = 60;
        const int PY = 160 + (int)((1.0f - e) * 24.0f);
        const Uint8 a = (Uint8)(255.0f * e);

        // Above the panel: the title, the account picture, the time.
        MText(MF_Semi, 30, PX + 30, PY - 62, Fade(kMtWhite, a), T("Guide"));
        if (SDL_Texture *av = MetroAvatar()) m_gfx->DrawImage(av, PX + PW / 2 - 32, PY - 76, 64, 64, a);
        {
            time_t now = time(nullptr);
            struct tm tm_now;
            localtime_r(&now, &tm_now);
            char clock[16];
            strftime(clock, sizeof(clock), "%H:%M", &tm_now);
            const int cw = MWidth(MF_Semi, 30, clock);
            MText(MF_Semi, 30, PX + PW - 24 - cw, PY - 62, Fade(kMtWhite, a), clock);
        }

        // Tabs are spines with their names running down them: the ones before
        // the open tab stack on the left, the ones after on the right.
        const char *names[MG_Count] = { T("Games & Apps"), m_nickname, T("Media"), T("Settings") };
        const int lx = PX + (m_metro_gtab + 1) * SP;
        const int rx = PX + PW - (MG_Count - 1 - m_metro_gtab) * SP;
        for (int i = 0; i < MG_Count; i++) {
            int sx;
            if (i <= m_metro_gtab) sx = PX + i * SP;
            else                   sx = rx + (i - m_metro_gtab - 1) * SP;
            const bool open = (i == m_metro_gtab);
            m_gfx->FillRect(sx, PY, SP, PH, Fade(open ? SDL_Color{ 206, 208, 211, 255 }
                                                       : SDL_Color{ 96, 101, 106, 255 }, a));
            m_gfx->FillRect(sx + SP - 1, PY, 1, PH, Fade(SDL_Color{ 70, 74, 78, 255 }, a));
            if (TTF_Font *f = m_gfx->Face(kMtFace[MF_Semi], 24))
                m_gfx->TextDown(f, sx + (SP - m_gfx->LineHeight(f)) / 2 + 2, PY + 22,
                                Fade(open ? SDL_Color{ 64, 64, 66, 255 }
                                          : SDL_Color{ 228, 230, 232, 255 }, a), names[i]);
        }

        // The open tab's list.
        m_gfx->FillRect(lx, PY, rx - lx, PH, Fade(SDL_Color{ 238, 239, 240, 255 }, a));
        std::vector<MetroGuideRow> rows;
        MetroGuideRows(m_metro_gtab, rows);
        if (!rows.empty()) m_metro_grow = std::clamp(m_metro_grow, 0, (int)rows.size() - 1);
        // The highlight is the accent, unless that is too pale to show on the
        // white list; then a dark bar.
        const int luma = (m_mt_accent.r * 299 + m_mt_accent.g * 587 + m_mt_accent.b * 114) / 1000;
        const SDL_Color hl  = luma > 200 ? SDL_Color{ 60, 60, 64, 255 } : m_mt_accent;
        const SDL_Color hlt = luma > 200 ? kMtWhite : m_mt_on;
        for (int i = 0; i < (int)rows.size() && (i + 1) * RH <= PH; i++) {
            const int y = PY + i * RH;
            const bool sel = (i == m_metro_grow);
            if (sel) m_gfx->FillRect(lx, y, rx - lx, RH, Fade(hl, a));
            else     m_gfx->FillRect(lx + 12, y + RH - 1, rx - lx - 24, 1,
                                     Fade(SDL_Color{ 212, 214, 216, 255 }, a));
            const std::string s = MFit(MF_Semi, 25, rows[i].label, rx - lx - 52);
            MText(MF_Semi, 25, lx + 26, y + (RH - 32) / 2, Fade(sel ? hlt : kMtInk, a), s.c_str());
        }

        if (m_show_hints) {
            const int hy = PY + PH + 16;
            MetroDrawHints({ { 'A', "Select" }, { 'B', "Back" }, { 'Y', "Home" } }, hy);
        }
    }

    bool Menu::MetroGuideNav(Btn b, Action &out, u64 &out_app_id) {
        out = Action::None;
        std::vector<MetroGuideRow> rows;
        MetroGuideRows(m_metro_gtab, rows);
        const int n = (int)rows.size();
        auto close = [&]() { m_metro_guide = false; m_metro_guide_game = false; };

        switch (b) {
            case Btn::Left: case Btn::L:
                if (m_metro_gtab > 0) { m_metro_gtab--; m_metro_grow = 0;
                                        m_sfx.Play(audio::Sfx::PageLeft); }
                return true;
            case Btn::Right: case Btn::R:
                if (m_metro_gtab < MG_Count - 1) { m_metro_gtab++; m_metro_grow = 0;
                                                   m_sfx.Play(audio::Sfx::PageRight); }
                return true;
            case Btn::Up:   if (n) m_metro_grow = (m_metro_grow + n - 1) % n; return true;
            case Btn::Down: if (n) m_metro_grow = (m_metro_grow + 1) % n;     return true;
            case Btn::B:
                if (m_metro_guide_game && m_suspended) { out = Action::ResumeApp; return true; }
                close();
                return true;
            case Btn::Y:
                close();
                m_metro_panel = MP_Home;
                m_metro_hdr = false;
                MetroSync();
                return true;
            case Btn::A: break;
            default: return true;   // nothing else reaches through the Guide
        }
        if (n == 0) return true;
        const MetroGuideRow r = rows[std::clamp(m_metro_grow, 0, n - 1)];
        switch (r.act) {
            case GA_Home:
                close();
                m_metro_panel = MP_Home;
                m_metro_hdr = false;
                MetroSync();
                return true;
            case GA_Resume:
                out = Action::ResumeApp;
                return true;
            case GA_Close:
                close();
                m_dialog_title.clear();
                m_dialog_note.clear();
                m_dialog_cursor = 1;   // default to No, as everywhere else
                m_dialog = Dialog::ConfirmCloseGame;
                return true;
            case GA_MyGames:
                close();
                MetroOpenLib(0);
                return true;
            case GA_Launch:
                close();
                m_cursor = r.arg;
                out = ActivateSelected(out_app_id);
                return true;
            case GA_System:
                close();
                out = Action::OpenSystemSettings;
                return true;
            default: {
                close();
                MetroTile t;
                t.special = MS_Kind;
                t.kind = (ItemKind)r.arg;
                t.caption = r.label;
                out = MetroActivate(t, out_app_id);
                return true;
            }
        }
    }

    // ---- my games -----------------------------------------------------------
    void Menu::MetroLibList(std::vector<int> &out) const {
        out.clear();
        for (int i = 0; i < (int)m_items.size(); i++) {
            const MenuItem &it = m_items[i];
            const bool game = it.kind == ItemKind::Game, hb = it.kind == ItemKind::Homebrew;
            if (!game && !hb) continue;
            switch (m_metro_lib_filter) {
                case 1: if (!it.is_favourite) continue; break;
                case 2: {
                    if (!game) continue;
                    const play::PlayInfo *pi = Play(it.app_id);
                    if (!pi || !pi->last_played) continue;
                    break;
                }
                case 3: if (!it.is_gamecard) continue; break;
                case 4: if (!hb) continue; break;
                default: break;
            }
            out.push_back(i);
        }
        std::stable_sort(out.begin(), out.end(), [&](int a, int b) {
            const MenuItem &ia = m_items[a], &ib = m_items[b];
            const play::PlayInfo *pa = Play(ia.app_id), *pb = Play(ib.app_id);
            switch (m_metro_lib_sort) {
                case 1:  return strcasecmp(ia.name.c_str(), ib.name.c_str()) < 0;
                case 2:  return (pa ? pa->seconds : 0) > (pb ? pb->seconds : 0);
                default: return (pa ? pa->last_played : 0) > (pb ? pb->last_played : 0);
            }
        });
    }
    void Menu::MetroOpenLib(int filter) {
        m_metro_lib_filter = filter;
        m_metro_lib_sort   = 0;
        m_metro_lib_cur    = 0;
        m_metro_lib_hdr    = -1;
        m_metro_lib_scroll = 0.0f;
        m_screen = Screen::MetroGames;
    }

    void Menu::DrawMetroGames() {
        static const char *kShow[] = { "all games", "my pins", "recent", "game card", "homebrew" };
        static const char *kSort[] = { "recent", "title", "most played" };
        m_icons.SetScale(0);
        MetroPalette();

        std::vector<int> list;
        MetroLibList(list);
        const int n = (int)list.size();
        if (n) m_metro_lib_cur = std::clamp(m_metro_lib_cur, 0, n - 1);
        if (n) m_cursor = list[m_metro_lib_cur];

        // A deep blue room, and the selected game's colours washed through it.
        if (!m_metro_themed) m_gfx->FillRect(0, 0, gfx::Gfx::Width, gfx::Gfx::Height, SDL_Color{ 22, 34, 58, 255 });
        if (n) {
            const MenuItem &sel = m_items[list[m_metro_lib_cur]];
            if (SDL_Texture *icon = ItemArt(sel))
                if (SDL_Texture *fill = MetroFill(sel, icon))
                    m_gfx->DrawImageCrop(fill, 0, 0, gfx::Gfx::Width, gfx::Gfx::Height, 70);
        }
        for (int i = 0; i < 12; i++)
            if (!m_metro_themed) m_gfx->FillRect(0, i * 60, gfx::Gfx::Width, 60, SDL_Color{ 10, 18, 34, (Uint8)(i * 9) });

        // ---- header ---------------------------------------------------------
        const struct { int x; const char *label, *value; } picks[2] = {
            { 100, T("show me"), T(kShow[m_metro_lib_filter]) },
            { 360, T("sort"),    T(kSort[m_metro_lib_sort]) },
        };
        for (int i = 0; i < 2; i++) {
            const bool foc = (m_metro_lib_hdr == i);
            const SDL_Color c = Fade(m_mt_fg, foc ? 255 : 200);
            const int x = picks[i].x, y = 88;
            m_gfx->LineAA(x + 1, y + 7, x + 6, y + 12, c, 2.0f);   // the drop-down chevron
            m_gfx->LineAA(x + 6, y + 12, x + 11, y + 7, c, 2.0f);
            MText(MF_Semi, 18, x + 20, y, c, picks[i].label);
            MText(MF_Light, 27, x, y + 22, c, picks[i].value);
            if (foc) m_gfx->FillRect(x, y + 60, MWidth(MF_Light, 27, picks[i].value), 2, m_mt_fg);
        }
        {
            const char *title = !m_search.empty() ? T("search")
                              : m_metro_lib_filter == 1 ? T("my pins") : T("my games");
            const int tw = MWidth(MF_Light, 40, title);
            MText(MF_Light, 40, 1180 - tw, 82, m_mt_fg, title);
            char pos[48];
            if (!m_search.empty()) snprintf(pos, sizeof(pos), "\"%s\"", m_search.c_str());
            else snprintf(pos, sizeof(pos), "%d %s %d", n ? m_metro_lib_cur + 1 : 0, T("of"), n);
            const int pw = MWidth(MF_Semi, 19, pos);
            MText(MF_Semi, 19, 1180 - pw, 132, Fade(m_mt_fg, 220), pos);
        }

        if (n == 0) {
            const char *s = T("Nothing here");
            MText(MF_Light, 30, (gfx::Gfx::Width - MWidth(MF_Light, 30, s)) / 2, 340, Fade(m_mt_fg, 200), s);
            MetroDrawHints({ { 'B', "Back" } }, kMtHintY);
            return;
        }

        // ---- cards ------------------------------------------------------------
        {
            const int sel_x = kMtCardX + m_metro_lib_cur * kMtCardPitch;
            float want = m_metro_lib_scroll;
            if (sel_x - want < kMtCardX + 0.0f) want = (float)(sel_x - kMtCardX);
            if (sel_x + kMtCardW - want > gfx::Gfx::Width - kMtCardX)
                want = (float)(sel_x + kMtCardW - gfx::Gfx::Width + kMtCardX);
            m_metro_lib_scroll += (want - m_metro_lib_scroll) * 0.25f;
            if (std::abs(want - m_metro_lib_scroll) < 0.5f) m_metro_lib_scroll = want;
        }
        int sel_drawn = -1;
        auto card = [&](int i, bool sel) {
            const MenuItem &it = m_items[list[i]];
            const float s = sel ? 1.05f : 1.0f;
            const int w = (int)(kMtCardW * s), h = (int)(kMtCardH * s);
            const int x = kMtCardX + i * kMtCardPitch - (int)m_metro_lib_scroll - (w - kMtCardW) / 2;
            const int y = kMtCardY - (h - kMtCardH) / 2 - (sel ? 6 : 0);
            m_gfx->FillRect(x, y, w, h, sel ? SDL_Color{ 250, 250, 252, 255 } : SDL_Color{ 232, 232, 238, 255 });

            // The band naming what it is, over the art.
            const bool hb = it.kind == ItemKind::Homebrew;
            const SDL_Color band = hb ? SDL_Color{ 104, 33, 122, 255 }
                                 : it.is_gamecard ? SDL_Color{ 243, 124, 0, 255 }
                                 : SDL_Color{ 252, 252, 252, 255 };
            const SDL_Color btxt = (!hb && !it.is_gamecard) ? SDL_Color{ 110, 110, 114, 255 } : kMtWhite;
            const int ix = x + 8, iw = w - 16;
            m_gfx->FillRect(ix, y + 8, iw, 34, band);
            MText(MF_Semi, 17, ix + 7, y + 14, btxt,
                  hb ? T("HOMEBREW") : it.is_gamecard ? T("GAME CARD") : T("NINTENDO SWITCH"));

            const int ay = y + 42, ah = (int)(246 * s);
            SDL_Texture *art = FlowCover(it);
            if (!art) art = ItemArt(it);
            m_gfx->FillRect(ix, ay, iw, ah, SDL_Color{ 40, 40, 44, 255 });
            if (art) m_gfx->DrawImageCrop(art, ix, ay, iw, ah, 255);

            const std::string nm = MFit(MF_Semi, 20, it.name, iw - 4);
            MText(MF_Semi, 20, ix, ay + ah + 8, kMtInk, nm.c_str());
            std::string sub;
            if (hb) sub = T("Homebrew");
            else if (const play::PlayInfo *pi = Play(it.app_id); pi && pi->seconds > 0)
                sub = play::FormatPlaytime(pi->seconds);
            else sub = T("Never played");
            MText(MF_Semi, 18, ix, ay + ah + 36, SDL_Color{ 90, 90, 94, 255 }, sub.c_str());
            if (const int on = OnlineCount(it); on >= 0) {
                char n[32];
                snprintf(n, sizeof(n), "%d %s", on, T("online"));
                const int ox = ix + MWidth(MF_Semi, 18, sub.c_str()) + MWidth(MF_Semi, 18, "   ");
                OnlineDot(ox, ay + ah + 36 + 8, 9, on, 255);
                MText(MF_Semi, 18, ox + 15, ay + ah + 36, SDL_Color{ 90, 90, 94, 255 }, n);
            }

            // A little pad in the corner, as each card had.
            const int gx = x + w - 38, gy = y + h - 28;
            const SDL_Color gc { 90, 90, 94, 255 };
            m_gfx->FillRect(gx + 4, gy, 20, 11, gc);
            m_gfx->FillRect(gx, gy + 3, 28, 9, gc);
            m_gfx->FillRect(gx + 1, gy + 12, 7, 5, gc);
            m_gfx->FillRect(gx + 20, gy + 12, 7, 5, gc);
            if (it.is_favourite) m_gfx->FillRect(x + w - 22, y + 14, 10, 10, m_mt_accent);
            if (it.kind == ItemKind::Game && m_suspended && it.app_id == m_suspended)
                MText(MF_Semi, 16, x + w - 92, y + h - 28, m_mt_accent, T("Running"));
        };
        for (int i = 0; i < n; i++) {
            const int x = kMtCardX + i * kMtCardPitch - (int)m_metro_lib_scroll;
            if (x > gfx::Gfx::Width) break;
            if (x + kMtCardW < 0) continue;
            if (i == m_metro_lib_cur && m_metro_lib_hdr < 0) { sel_drawn = i; continue; }
            card(i, false);
        }
        if (sel_drawn >= 0) card(sel_drawn, true);

        if (!ScrollBusy()) FetchArtFor(m_items[list[m_metro_lib_cur]]);
        MetroDrawHints({ { 'A', "Launch" }, { 'B', "Back" }, { 'X', "Game Details" } }, kMtHintY);
    }

    Menu::Action Menu::OnButtonMetroGames(Btn b, u64 &out_app_id) {
        std::vector<int> list;
        MetroLibList(list);
        const int n = (int)list.size();

        if (b == Btn::B) {
            if (m_metro_lib_hdr >= 0) { m_metro_lib_hdr = -1; return Action::None; }
            if (!m_search.empty()) {
                m_search.clear();
                m_metro_search_seen.clear();
                RebuildItems();
            }
            m_screen = Screen::Main;
            return Action::None;
        }
        if (b == Btn::Y) { OpenSearch(); return Action::None; }

        if (m_metro_lib_hdr >= 0) {
            if (b == Btn::Left || b == Btn::Right) m_metro_lib_hdr ^= 1;
            if (b == Btn::Down) m_metro_lib_hdr = -1;
            if (b == Btn::A) {
                if (m_metro_lib_hdr == 0) m_metro_lib_filter = (m_metro_lib_filter + 1) % 5;
                else                      m_metro_lib_sort   = (m_metro_lib_sort + 1) % 3;
                m_metro_lib_cur = 0;
                m_metro_lib_scroll = 0.0f;
            }
            return Action::None;
        }
        if (b == Btn::Up) { m_metro_lib_hdr = 0; return Action::None; }
        if (n == 0) return Action::None;
        m_metro_lib_cur = std::clamp(m_metro_lib_cur, 0, n - 1);
        switch (b) {
            case Btn::Left:  if (m_metro_lib_cur > 0) m_metro_lib_cur--; break;
            case Btn::Right: if (m_metro_lib_cur + 1 < n) m_metro_lib_cur++; break;
            case Btn::L:     m_metro_lib_cur = std::max(0, m_metro_lib_cur - 5); break;
            case Btn::R:     m_metro_lib_cur = std::min(n - 1, m_metro_lib_cur + 5); break;
            case Btn::A:
                m_cursor = list[m_metro_lib_cur];
                m_screen = Screen::Main;
                return ActivateSelected(out_app_id);
            case Btn::X:
                m_cursor = list[m_metro_lib_cur];
                m_options_sub = Sub_None;
                BuildOptions();
                m_options_cursor = 0;
                if (!m_options.empty()) m_options_open = true;
                break;
            default: break;
        }
        return Action::None;
    }

} // namespace sl::menu::ui
