#include <sl/menu/ui/Menu.hpp>
#include <unordered_set>
#include <sl/menu/ui/Locale.hpp>
#include <sl/menu/net/Http.hpp>
#include <sl/smi/Protocol.hpp>
#include <SDL2/SDL_image.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <ctime>
#include <algorithm>
#include <dirent.h>
#include <sys/stat.h>
#include "Menu_Internal.hpp"

namespace sl::menu::ui {

    // The row: the game you last played first - the one the wide tile is for -
    // then everything else in whatever order the menu is already sorted in.
    // Homebrew is in here too, because on this layout it launches the same way.
    void Menu::DeckRow(std::vector<int> &out) const {
        out = m_flow_items;
        if (out.size() < 2) return;
        int   best   = -1;
        u64   best_t = 0;
        for (int i = 0; i < (int)out.size(); i++) {
            const MenuItem &it = m_items[out[i]];
            if (it.kind != ItemKind::Game) continue;
            const play::PlayInfo *pi = Play(it.app_id);
            if (pi && pi->last_played > best_t) { best_t = pi->last_played; best = i; }
        }
        // Nothing has ever been played (a fresh card, or pdm has no record):
        // the row simply starts where the list does.
        if (best > 0) {
            const int v = out[best];
            out.erase(out.begin() + best);
            out.insert(out.begin(), v);
        }
    }
    // The shared cursor is an index into m_items, which holds the settings
    // entries too - and this layout's row does not. Coming from another layout
    // (or from a hidden entry being unhidden) it can be pointing at something
    // the row has no tile for, which drew a nameless row with a settings entry
    // captioned under it. Snap it to the first tile in that case.
    void Menu::DeckSyncCursor() {
        std::vector<int> row;
        DeckRow(row);
        if (row.empty()) return;
        for (int i : row) if (i == m_cursor) return;
        m_cursor = row[0];
    }
    // Left edge of a slot before scrolling. Slot 0 is the wide one, so this is
    // not a multiplication - every caller has to agree on that, which is why
    // there is only one of these.
    int Menu::DeckSlotX(int slot) const {
        if (slot <= 0) return kDeckRowX;
        return kDeckRowX + kDeckHeroW + kDeckRowGap +
               (slot - 1) * (kDeckTileW + kDeckRowGap);
    }
    const std::vector<news::Item> &Menu::DeckCards() const {
        return m_news.Get(m_deck_tab == DeckTab_Nintendo ? news::Source::Nintendo
                                                         : news::Source::Steam);
    }
    SDL_Texture *Menu::DeckArt(const std::string &path) {
        if (path.empty()) return nullptr;
        auto f = m_news_art.find(path);
        if (f != m_news_art.end()) return f->second;   // nullptr is cached too
        // Decoded to the card's art size rather than whatever the source was:
        // a Nintendo asset is 480x270 and the store header 460x215, and holding
        // either at full size for eight cards is memory for nothing.
        //
        // Cropped to fill the box, not stretched: a Nintendo image is already
        // 16:9 so this is a no-op for it, but a Steam header (460x215, wider
        // than 16:9) used to get squashed taller to fill the same box exactly.
        // Biased down slightly rather than centred - these are mostly banner
        // art with the subject sitting low in the frame, and a dead-centre
        // crop was cutting into it while keeping empty sky/background.
        SDL_Texture *tex = m_gfx->LoadImageCropped(path.c_str(),
                                                    kDeckCardW, kDeckCardArtH, 0.62f);
        m_news_art[path] = tex;
        return tex;
    }
    // Hero art for the wide first tile: SteamGridDB wide key art (1920x620 at
    // source, see the fetch worker's own comment for why this and the
    // back-of-case screenshots are separate features now), cached to disk the
    // same way FlowCover caches a cover - a decode is a JPEG inflate of an
    // image four times the screen's own width, so it is worth never doing
    // twice. Cropped rather than stretched (DecodeCoverSurfaceCropped): the
    // source is 3.1:1 and the tile is 16:9, nowhere close enough to just blit.
    SDL_Texture *Menu::HeroArt(const MenuItem &it) {
        if (it.kind != ItemKind::Game || it.app_id == 0) return nullptr;
        auto f = m_hero_art.find(it.app_id);
        if (f != m_hero_art.end()) return f->second;   // nullptr is cached too
        QueueArt(Art_Hero, it.app_id);                 // see PollArt, Menu_Flow.cpp
        return nullptr;
    }
    void Menu::DeckFreeArt() {
        for (auto &kv : m_news_art)
            if (kv.second) m_gfx->FreeImage(kv.second);
        m_news_art.clear();
    }
    // Ask the feed for what the current tab wants, and reap anything that has
    // landed. Called once a frame from the Deck renderer: every request the feed
    // has already served (or has cached and fresh) is dropped inside Request.
    void Menu::DeckPollNews() {
        if (m_news.Poll()) DeckFreeArt();   // paths may be reused at new content

        if (m_deck_tab == DeckTab_Nintendo) {
            m_news.Request(news::Source::Nintendo);
        } else if (m_deck_tab == DeckTab_Game && m_cursor < (int)m_items.size()) {
            const MenuItem &it = m_items[m_cursor];
            if (it.kind == ItemKind::Game && it.app_id)
                m_news.Request(news::Source::Steam, it.app_id, it.name.c_str());
        }
    }
    void Menu::DrawMainDeck() {
        const Theme &t = m_theme.Current();
        m_icons.SetScale(0);
        DrawTopBar(nullptr);

        if (m_items.empty()) { DrawMainEmpty(); return; }
        DeckSyncCursor();
        DeckPollNews();

        const bool row_games = (m_deck_row == 0);

        // The row, and where the selection sits in it.
        std::vector<int> row;
        DeckRow(row);
        const int rn = (int)row.size();
        int at = 0;
        for (int i = 0; i < rn; i++)
            if (row[i] == m_cursor) { at = i; break; }

        // Scroll is in pixels here rather than in slots: the first tile is a
        // different width from the rest, so there is no single pitch to count
        // in. Enough is scrolled to bring the selection fully on screen, and no
        // more, which leaves the wide tile in place until the selection has
        // actually walked past it.
        if (rn > 0) {
            const int selx = DeckSlotX(at);
            const int selw = at == 0 ? kDeckHeroW : kDeckTileW;
            float want = m_deck_scroll;
            if ((float)selx - want < (float)kDeckRowX)
                want = (float)(selx - kDeckRowX);
            if ((float)(selx + selw) - want > (float)(gfx::Gfx::Width - kDeckRowRight))
                want = (float)(selx + selw - gfx::Gfx::Width + kDeckRowRight);
            if (want < 0.0f) want = 0.0f;
            if (!ScrollBusy()) m_deck_scroll += (want - m_deck_scroll) * 0.30f;
            if (std::abs(want - m_deck_scroll) < 0.5f) m_deck_scroll = want;
        }

        // ---- the row --------------------------------------------------------
        // A trailing unselected tile can otherwise land flush against, or past,
        // the true screen edge - the scroll target above only keeps the
        // *selected* tile inside kDeckRowRight, it does not stop whatever else
        // is drawn from bleeding past it. Clipping to the safe width turns
        // that into the deliberate "next tile peeks in" look instead of a
        // hard, margin-less cut.
        //
        // Full screen height, not just the row's: the selected tile's frame
        // is drawn a few px above/below the row itself (see the FillRects
        // below), and a rect matching the row exactly would clip that border
        // off top and bottom. Only the right edge needs holding back.
        const SDL_Rect deckRowClip{0, 0, gfx::Gfx::Width - kDeckRowRight, gfx::Gfx::Height};
        m_gfx->FxClose();   // SDL is used directly below
        SDL_RenderSetClipRect(m_gfx->Renderer(), &deckRowClip);
        for (int i = 0; i < rn; i++) {
            const int w = (i == 0) ? kDeckHeroW : kDeckTileW;
            const int x = DeckSlotX(i) - (int)m_deck_scroll;
            if (x > gfx::Gfx::Width) break;
            if (x + w < 0) continue;

            const MenuItem &it  = m_items[row[i]];
            const bool      sel = (row[i] == m_cursor);

            m_gfx->FillRect(x, kDeckRowY, w, kDeckRowH, WithAlpha(t.bg_bottom, 235));

            if (i == 0) {
                // Wide art for the wide tile: hero key art first - it is a
                // banner, and this is the one spot in the whole menu that
                // reads as one - then the back-of-case screenshots as a
                // second choice, then the upright cover (centred, not
                // stretched to a shape it was never drawn for), then the
                // plain icon.
                SDL_Texture *wide = (it.kind == ItemKind::Game) ? HeroArt(it) : nullptr;
                if (!wide && it.kind == ItemKind::Game) {
                    const FlowShots &fs = FlowBackShots(it);
                    wide = fs.a ? fs.a : fs.b;
                }
                if (wide) {
                    m_gfx->DrawImage(wide, x, kDeckRowY, w, kDeckRowH, 255);
                } else if (SDL_Texture *cov = FlowCover(it)) {
                    const int cw = (kDeckRowH * 2) / 3;
                    m_gfx->DrawImage(cov, x + (w - cw) / 2, kDeckRowY, cw, kDeckRowH, 255);
                } else {
                    const int sq = kDeckRowH - 96;
                    DrawAppTile(it, x + (w - sq) / 2, kDeckRowY + 48, sq, false, 255);
                }
            } else if (SDL_Texture *cov = FlowCover(it)) {
                m_gfx->DrawImage(cov, x, kDeckRowY, w, kDeckRowH, sel ? 255 : 235);
            } else {
                // No box art: the square icon, centred on the tile at a size it
                // was drawn for. Blown up to the full tile it reads as a broken
                // image rather than as a title waiting for its cover.
                const int sq = kDeckTileW - 44;
                const Uint8 a = sel ? 255 : 235;
                DrawAppTile(it, x + (w - sq) / 2, kDeckRowY + (kDeckRowH - sq) / 2 - 14,
                            sq, false, a);
                // ...with its name under it, which is the only thing left to
                // tell one coverless tile from another.
                const std::string nm = Ellipsize(it.name, w - 16, FontSize::Small);
                m_gfx->TextCentered(FontSize::Small, x + w / 2,
                                    kDeckRowY + (kDeckRowH + sq) / 2 + 2,
                                    WithAlpha(t.fg, a), nm.c_str());
            }

            // Suspended game: the same "still running" tag the other layouts use.
            if (it.kind == ItemKind::Game && m_suspended != 0 && it.app_id == m_suspended) {
                const int tw = m_gfx->TextWidth(FontSize::Small, T("Running")) + 16;
                m_gfx->FillRect(x + 8, kDeckRowY + 8, tw, 24, WithAlpha(t.accent, 220));
                m_gfx->Text(FontSize::Small, x + 16, kDeckRowY + 10, t.bg_bottom,
                            T("Running"));
            }

            if (sel) {
                // The frame is what says the row has focus: when the cards or
                // the tabs have it, the selection is still marked but quietly.
                const SDL_Color fr = row_games ? t.accent : WithAlpha(t.fg, 90);
                // A halo under the frame while this row has focus, so the
                // selected tile lifts off the row instead of being marked only
                // by a hairline. Additive, so it reads as light rather than as
                // a second border.
                if (row_games)
                    m_gfx->GlowRect(x, kDeckRowY, w, kDeckRowH, t.accent, 14);
                const int fw = row_games ? 3 : 2;
                m_gfx->FillRect(x - fw, kDeckRowY - fw, w + fw * 2, fw, fr);
                m_gfx->FillRect(x - fw, kDeckRowY + kDeckRowH, w + fw * 2, fw, fr);
                m_gfx->FillRect(x - fw, kDeckRowY, fw, kDeckRowH, fr);
                m_gfx->FillRect(x + w, kDeckRowY, fw, kDeckRowH, fr);
            }
        }
        m_gfx->FxClose();   // SDL is used directly below
        SDL_RenderSetClipRect(m_gfx->Renderer(), nullptr);

        // ---- name and play line, for whatever is selected --------------------
        if (rn > 0 && m_cursor < (int)m_items.size()) {
            const MenuItem &sel = m_items[m_cursor];
            const std::string nm = Ellipsize(sel.name,
                                             gfx::Gfx::Width - kDeckRowX - 40,
                                             FontSize::Large);
            m_gfx->Text(FontSize::Large, kDeckRowX, kDeckRowY + kDeckRowH + 10,
                        t.title, nm.c_str());

            std::string sub;
            if (sel.kind == ItemKind::Game) {
                if (const play::PlayInfo *pi = Play(sel.app_id)) {
                    if (pi->seconds > 0) {
                        sub = std::string(T("Played")) + ": " +
                              play::FormatPlaytime(pi->seconds);
                        if (pi->last_played)
                            sub += "   " + play::FormatLastPlayed(pi->last_played);
                    }
                }
                if (sub.empty()) sub = T("Never played");
            } else {
                sub = T("Homebrew");
            }
            // The little play triangle, in the accent: this is the line that
            // says "this is the thing A launches".
            const int ty = kDeckRowY + kDeckRowH + 50;
            m_gfx->FillTriangle(kDeckRowX + 1, ty, kDeckRowX + 1, ty + 14,
                                kDeckRowX + 12, ty + 7, t.accent);
            m_gfx->Text(FontSize::Small, kDeckRowX + 22, ty - 1, t.dim, sub.c_str());
        }

        // ---- tabs ------------------------------------------------------------
        {
            const char *labels[DeckTab_Count] = { T("What's new"), T("Nintendo"),
                                                  T("Widgets") };
            int w[DeckTab_Count], total = 0;
            for (int i = 0; i < DeckTab_Count; i++) {
                w[i] = m_gfx->TextWidth(FontSize::Normal, labels[i]) + 44;
                total += w[i];
            }
            int x = (gfx::Gfx::Width - total) / 2;
            for (int i = 0; i < DeckTab_Count; i++) {
                const bool on  = (i == m_deck_tab);
                const bool foc = on && m_deck_row == 1;
                if (on)
                    m_gfx->FillRect(x, kDeckTabY, w[i], kDeckTabH,
                                    foc ? WithAlpha(t.accent, 210) : WithAlpha(t.fg, 40));
                m_gfx->TextCentered(FontSize::Normal, x + w[i] / 2, kDeckTabY + 2,
                                    on ? (foc ? t.bg_bottom : t.fg) : t.dim, labels[i]);
                x += w[i];
            }
        }

        // ---- cards ------------------------------------------------------------
        if (m_deck_tab == DeckTab_Widgets) {
            // The widgets, one per card, drawn at the card's size. A script that
            // honours the height it is handed fills its card; one that does not
            // draws its natural height and is clipped to the card rather than
            // being allowed to run across the row below.
            SDL_Renderer *ren = m_gfx->Renderer();
            int slot = 0;
            for (int i = 0; i < m_widgets.Count() && slot < kDeckCardsVisible; i++) {
                widgets::IWidget *wd = m_widgets.At(i);
                if (!wd || !m_widgets.IsEnabled(i)) continue;
                const int x = kDeckCardX + slot * (kDeckCardW + kDeckCardGap);
                const bool foc = (m_deck_row == 2 && slot == m_deck_card);
                m_gfx->FillRect(x, kDeckCardY, kDeckCardW, kDeckCardH,
                                WithAlpha(t.bg_bottom, 225));
                if (foc) {
                    m_gfx->FillRect(x - 2, kDeckCardY - 2, kDeckCardW + 4, 2, t.accent);
                    m_gfx->FillRect(x - 2, kDeckCardY + kDeckCardH, kDeckCardW + 4, 2, t.accent);
                    m_gfx->FillRect(x - 2, kDeckCardY, 2, kDeckCardH, t.accent);
                    m_gfx->FillRect(x + kDeckCardW, kDeckCardY, 2, kDeckCardH, t.accent);
                }
                SDL_Rect clip { x + 10, kDeckCardY + 8, kDeckCardW - 20, kDeckCardH - 16 };
                m_gfx->FxClose();   // SDL is used directly below
                if (ren) SDL_RenderSetClipRect(ren, &clip);
                wd->Render(m_gfx, t, x + 12, kDeckCardY + 10, kDeckCardW - 24,
                           kDeckCardH - 20);
                m_gfx->FxClose();   // SDL is used directly below
                if (ren) SDL_RenderSetClipRect(ren, nullptr);
                slot++;
            }
            if (slot == 0)
                m_gfx->TextCentered(FontSize::Normal, gfx::Gfx::Width / 2,
                                    kDeckCardY + 60, t.dim,
                                    T("No widgets enabled"));
        } else {
            const std::vector<news::Item> &cards = DeckCards();
            if (cards.empty()) {
                const bool waiting = m_news.Busy();
                m_gfx->TextCentered(FontSize::Normal, gfx::Gfx::Width / 2,
                                    kDeckCardY + 60, t.dim,
                                    waiting ? T("Loading...") : T("No news"));
            }
            // Scrolled so the selected card is always on screen, without moving
            // while the row still fits.
            int first = 0;
            if (m_deck_row == 2 && m_deck_card >= kDeckCardsVisible)
                first = m_deck_card - kDeckCardsVisible + 1;
            for (int slot = 0; slot < kDeckCardsVisible; slot++) {
                const int idx = first + slot;
                if (idx >= (int)cards.size()) break;
                const news::Item &n = cards[idx];
                const int x = kDeckCardX + slot * (kDeckCardW + kDeckCardGap);
                const bool foc = (m_deck_row == 2 && idx == m_deck_card);

                m_gfx->FillRect(x, kDeckCardY, kDeckCardW, kDeckCardH,
                                WithAlpha(t.bg_bottom, 235));
                if (SDL_Texture *art = DeckArt(n.img))
                    m_gfx->DrawImage(art, x, kDeckCardY, kDeckCardW, kDeckCardArtH, 255);
                else
                    m_gfx->FillRect(x, kDeckCardY, kDeckCardW, kDeckCardArtH,
                                    WithAlpha(t.fg, 22));

                if (foc) {
                    m_gfx->FillRect(x - 2, kDeckCardY - 2, kDeckCardW + 4, 2, t.accent);
                    m_gfx->FillRect(x - 2, kDeckCardY + kDeckCardH, kDeckCardW + 4, 2, t.accent);
                    m_gfx->FillRect(x - 2, kDeckCardY, 2, kDeckCardH, t.accent);
                    m_gfx->FillRect(x + kDeckCardW, kDeckCardY, 2, kDeckCardH, t.accent);
                }

                // Kind and date on one line, the headline over two under it.
                std::string kind = n.kind;
                for (char &c : kind) c = (char)toupper((unsigned char)c);
                m_gfx->Text(FontSize::Small, x + 12, kDeckCardY + kDeckCardArtH + 8,
                            t.accent, Ellipsize(kind, kDeckCardW - 90,
                                                FontSize::Small).c_str());
                if (!n.date.empty()) {
                    const int dw = m_gfx->TextWidth(FontSize::Small, n.date.c_str());
                    m_gfx->Text(FontSize::Small, x + kDeckCardW - 12 - dw,
                                kDeckCardY + kDeckCardArtH + 8, t.dim, n.date.c_str());
                }
                DeckWrapText(n.title, x + 12, kDeckCardY + kDeckCardArtH + 32,
                             kDeckCardW - 24, FontSize::Normal, t.fg, 2);
            }
        }

        // Hint line: what the two buttons this layout adds actually do. Skipped
        // when this is being drawn as the side menu's backdrop, which prints a
        // hint of its own over the top.
        if (!m_items.empty() && !ScrollBusy()) FetchArtFor(m_items[m_cursor]);
        if (!m_deck_backdrop) {
            DrawFetchStatus();
            DrawStatusHint({ {{"a"}, "Open"}, {{"y"}, "Library"}, {{"minus"}, "Menu"}, {{"x"}, "Options"} });
        }
    }
    // Draw `s` word-wrapped into `w`, at most `max_lines` lines, ellipsising the
    // last one. Returns the number of lines drawn.
    int Menu::DeckWrapText(const std::string &s, int x, int y, int w,
                           gfx::FontSize fs, SDL_Color c, int max_lines) {
        const int lh = (fs == FontSize::Small) ? 20 : (fs == FontSize::Normal) ? 26 : 32;
        std::string line;
        int drawn = 0;
        size_t i = 0;
        while (i <= s.size() && drawn < max_lines) {
            const size_t sp = s.find(' ', i);
            const std::string word = s.substr(i, (sp == std::string::npos)
                                                 ? std::string::npos : sp - i);
            const std::string cand = line.empty() ? word : line + " " + word;
            if (m_gfx->TextWidth(fs, cand.c_str()) <= w) {
                line = cand;
            } else {
                if (line.empty()) line = cand;      // one word longer than the box
                const bool last = (drawn == max_lines - 1);
                m_gfx->Text(fs, x, y + drawn * lh, c,
                            last ? Ellipsize(line, w, fs).c_str() : line.c_str());
                drawn++;
                line = word;
            }
            if (sp == std::string::npos) break;
            i = sp + 1;
        }
        if (!line.empty() && drawn < max_lines) {
            m_gfx->Text(fs, x, y + drawn * lh, c, Ellipsize(line, w, fs).c_str());
            drawn++;
        }
        return drawn;
    }
    bool Menu::DeckNav(Btn b, Action &out, u64 &out_app_id) {
        (void)out_app_id;
        out = Action::None;
        DeckSyncCursor();

        if (b == Btn::Y) { m_screen = Screen::DeckLibrary; m_deck_lib_cursor = 0;
                           m_deck_lib_scroll = 0.0f; return true; }
        if (b == Btn::Minus) { m_screen = Screen::DeckMenu; m_deck_menu_cursor = 0;
                               m_sub_scroll = 0; return true; }

        const std::vector<news::Item> &cards = DeckCards();
        const int ncards = (m_deck_tab == DeckTab_Widgets)
                         ? std::min(kDeckCardsVisible, EnabledWidgetCount())
                         : (int)cards.size();

        if (b == Btn::Down) {
            if (m_deck_row < 2) m_deck_row++;
            if (m_deck_row == 2 && ncards == 0) m_deck_row = 1;   // nothing to land on
            if (m_deck_card >= ncards) m_deck_card = ncards > 0 ? ncards - 1 : 0;
            return true;
        }
        if (b == Btn::Up) {
            if (m_deck_row > 0) m_deck_row--;
            return true;
        }

        if (m_deck_row == 1) {
            if (b == Btn::Left)  { m_deck_tab = (m_deck_tab + DeckTab_Count - 1) % DeckTab_Count;
                                   m_deck_card = 0; return true; }
            if (b == Btn::Right) { m_deck_tab = (m_deck_tab + 1) % DeckTab_Count;
                                   m_deck_card = 0; return true; }
            if (b == Btn::A)     { m_deck_row = 2; return true; }
            return b == Btn::L || b == Btn::R;
        }

        if (m_deck_row == 2) {
            if (b == Btn::Left)  { if (m_deck_card > 0) m_deck_card--; return true; }
            if (b == Btn::Right) { if (m_deck_card + 1 < ncards) m_deck_card++; return true; }
            if (b == Btn::A) {
                if (m_deck_tab != DeckTab_Widgets && m_deck_card < (int)cards.size()) {
                    m_deck_reading = m_deck_card;
                    m_screen = Screen::DeckNews;
                    m_sub_scroll = 0;
                }
                return true;
            }
            return b == Btn::L || b == Btn::R;
        }

        // Row 0: the tile row. Left/right and the shoulders walk it and write
        // the result back to m_cursor, so everything downstream - launching,
        // options, favourites - keeps seeing one selection.
        std::vector<int> row;
        DeckRow(row);
        const int rn = (int)row.size();
        if (rn > 0) {
            int at = 0;
            for (int i = 0; i < rn; i++)
                if (row[i] == m_cursor) { at = i; break; }
            int delta = 0;
            if (b == Btn::Right) delta = +1;
            if (b == Btn::Left)  delta = -1;
            if (b == Btn::R)     delta = +5;
            if (b == Btn::L)     delta = -5;
            if (delta) {
                int nx = at + delta;
                if (nx < 0)   nx = m_wrap_nav ? rn - 1 : 0;
                if (nx >= rn) nx = m_wrap_nav ? 0 : rn - 1;
                m_cursor = row[nx];
                return true;
            }
        }
        // A on the cover row is not ours: the shared handler launches it.
        return false;
    }
    int Menu::EnabledWidgetCount() {
        int n = 0;
        for (int i = 0; i < m_widgets.Count(); i++)
            if (m_widgets.At(i) && m_widgets.IsEnabled(i)) n++;
        return n;
    }
    // ---- Deck: the whole library, as a grid ---------------------------------
    void Menu::DeckLibraryList(std::vector<int> &out) const {
        out.clear();
        for (int i = 0; i < (int)m_items.size(); i++) {
            const MenuItem &it = m_items[i];
            const bool game = (it.kind == ItemKind::Game);
            const bool hb   = (it.kind == ItemKind::Homebrew);
            if (!game && !hb) continue;
            switch (m_deck_lib_tab) {
                case DeckLib_Favourites: if (!it.is_favourite) continue; break;
                case DeckLib_Gamecard:   if (!it.is_gamecard)  continue; break;
                case DeckLib_Homebrew:   if (!hb)              continue; break;
                case DeckLib_Recent: {
                    if (!game) continue;
                    const play::PlayInfo *pi = Play(it.app_id);
                    if (!pi || pi->last_played == 0) continue;
                    break;
                }
                default: break;   // All: games and homebrew both
            }
            out.push_back(i);
        }
        // Recently played is the one tab with an order of its own; the rest
        // inherit whatever sort the menu is already using.
        if (m_deck_lib_tab == DeckLib_Recent) {
            std::stable_sort(out.begin(), out.end(), [&](int a, int b) {
                const play::PlayInfo *pa = Play(m_items[a].app_id);
                const play::PlayInfo *pb = Play(m_items[b].app_id);
                return (pa ? pa->last_played : 0) > (pb ? pb->last_played : 0);
            });
        }
    }
    void Menu::DrawDeckLibrary() {
        const Theme &t = m_theme.Current();
        m_icons.SetScale(0);
        DrawTopBar(nullptr);

        std::vector<int> list;
        DeckLibraryList(list);
        const int n = (int)list.size();
        if (m_deck_lib_cursor >= n) m_deck_lib_cursor = n > 0 ? n - 1 : 0;

        // ---- tabs, with their counts, and the shoulder chips that page them --
        {
            const char *labels[DeckLib_Count] = { T("All games"), T("Favourites"),
                                                  T("Recent"), T("Game card"),
                                                  T("Homebrew") };
            // Every tab's count, so the row reads the same whichever is open.
            const int save = m_deck_lib_tab;
            int counts[DeckLib_Count];
            std::vector<int> tmp;
            for (int i = 0; i < DeckLib_Count; i++) {
                const_cast<Menu *>(this)->m_deck_lib_tab = i;
                DeckLibraryList(tmp);
                counts[i] = (int)tmp.size();
            }
            m_deck_lib_tab = save;

            int w[DeckLib_Count], total = 0;
            char buf[64];
            for (int i = 0; i < DeckLib_Count; i++) {
                snprintf(buf, sizeof(buf), "%s  %d", labels[i], counts[i]);
                w[i] = m_gfx->TextWidth(FontSize::Normal, buf) + 36;
                total += w[i];
            }
            int x = (gfx::Gfx::Width - total) / 2;
            for (int i = 0; i < DeckLib_Count; i++) {
                snprintf(buf, sizeof(buf), "%s  %d", labels[i], counts[i]);
                const bool on = (i == m_deck_lib_tab);
                if (on) m_gfx->FillRect(x, 78, w[i], kDeckTabH, WithAlpha(t.fg, 42));
                m_gfx->TextCentered(FontSize::Normal, x + w[i] / 2, 80,
                                    on ? t.fg : t.dim, buf);
                x += w[i];
            }
            // L / R chips at the ends of the row, as the console prints them.
            m_gfx->FillRect(26, 78, 44, kDeckTabH, WithAlpha(t.fg, 28));
            m_gfx->TextCentered(FontSize::Normal, 48, 84, t.fg, "L");
            m_gfx->FillRect(gfx::Gfx::Width - 70, 78, 44, kDeckTabH, WithAlpha(t.fg, 28));
            m_gfx->TextCentered(FontSize::Normal, gfx::Gfx::Width - 48, 84, t.fg, "R");
        }

        if (n == 0) {
            m_gfx->TextCentered(FontSize::Normal, gfx::Gfx::Width / 2, 330, t.dim,
                                T("Nothing here"));
            DrawHint({ {{"l","r"}, "Tab"}, {{"b"}, "Back"} });
            return;
        }

        // The selected title, on the band between the tabs and the grid: under
        // the grid it would have to share the hint line with the button prompts.
        {
            char pos[24];
            snprintf(pos, sizeof(pos), "%d / %d", m_deck_lib_cursor + 1, n);
            const std::string nm = Ellipsize(m_items[list[m_deck_lib_cursor]].name,
                                             gfx::Gfx::Width - 240, FontSize::Normal);
            m_gfx->Text(FontSize::Normal, kDeckLibX, 116, t.fg, nm.c_str());
            if (m_show_counter) {
                const int pw = m_gfx->TextWidth(FontSize::Small, pos);
                m_gfx->Text(FontSize::Small, gfx::Gfx::Width - kDeckLibX - pw, 120,
                            t.dim, pos);
            }
        }

        // Keep the selected row on screen, then ease toward it.
        const int rows    = (n + kDeckLibCols - 1) / kDeckLibCols;
        const int cur_row = m_deck_lib_cursor / kDeckLibCols;
        const int per     = kDeckLibH + kDeckLibGapY;
        const int visible = (kDeckLibBot - kDeckLibTop + kDeckLibGapY) / per;
        float want = m_deck_lib_scroll;
        if (cur_row < (int)want)                 want = (float)cur_row;
        if (cur_row > (int)want + visible - 1)   want = (float)(cur_row - visible + 1);
        if (want > (float)std::max(0, rows - visible)) want = (float)std::max(0, rows - visible);
        if (want < 0) want = 0;
        m_deck_lib_scroll += (want - m_deck_lib_scroll) * 0.35f;
        if (std::abs(want - m_deck_lib_scroll) < 0.01f) m_deck_lib_scroll = want;

        SDL_Renderer *ren = m_gfx->Renderer();
        SDL_Rect clip { 0, kDeckLibTop - 6, gfx::Gfx::Width,
                        kDeckLibBot - kDeckLibTop + 12 };
        m_gfx->FxClose();   // SDL is used directly below
        if (ren) SDL_RenderSetClipRect(ren, &clip);

        for (int i = 0; i < n; i++) {
            const int r = i / kDeckLibCols, c = i % kDeckLibCols;
            const int x = kDeckLibX + c * (kDeckLibW + kDeckLibGapX);
            const int y = kDeckLibTop + (int)((r - m_deck_lib_scroll) * per);
            if (y > kDeckLibBot + kDeckLibH || y + kDeckLibH < kDeckLibTop - per) continue;

            const MenuItem &it = m_items[list[i]];
            const bool sel = (i == m_deck_lib_cursor);

            m_gfx->FillRect(x, y, kDeckLibW, kDeckLibH, WithAlpha(t.bg_bottom, 230));
            if (SDL_Texture *cov = FlowCover(it))
                m_gfx->DrawImage(cov, x, y, kDeckLibW, kDeckLibH, 255);
            else
                DrawAppTile(it, x, y + (kDeckLibH - kDeckLibW) / 2, kDeckLibW,
                            false, 255);

            if (sel) {
                const int fw = 3;
                m_gfx->FillRect(x - fw, y - fw, kDeckLibW + fw * 2, fw, t.accent);
                m_gfx->FillRect(x - fw, y + kDeckLibH, kDeckLibW + fw * 2, fw, t.accent);
                m_gfx->FillRect(x - fw, y, fw, kDeckLibH, t.accent);
                m_gfx->FillRect(x + kDeckLibW, y, fw, kDeckLibH, t.accent);
            }
            if (it.is_favourite)
                m_gfx->Text(FontSize::Small, x + 8, y + 6, t.accent, "*");
        }
        m_gfx->FxClose();   // SDL is used directly below
        if (ren) SDL_RenderSetClipRect(ren, nullptr);

        DrawHint({ {{"dpad"}, "Move"}, {{"a"}, "Launch"}, {{"l","r"}, "Tab"}, {{"b"}, "Back"} });
    }
    Menu::Action Menu::OnButtonDeckLibrary(Btn b, u64 &out_app_id) {
        std::vector<int> list;
        DeckLibraryList(list);
        const int n = (int)list.size();

        if (b == Btn::B) { m_screen = Screen::Main; return Action::None; }
        if (b == Btn::L || b == Btn::R) {
            m_deck_lib_tab = (m_deck_lib_tab + (b == Btn::R ? 1 : DeckLib_Count - 1))
                             % DeckLib_Count;
            m_deck_lib_cursor = 0;
            m_deck_lib_scroll = 0.0f;
            m_sfx.Play(b == Btn::R ? audio::Sfx::PageRight : audio::Sfx::PageLeft);
            return Action::None;
        }
        if (n == 0) return Action::None;
        if (m_deck_lib_cursor >= n) m_deck_lib_cursor = n - 1;

        switch (b) {
            case Btn::Left:  if (m_deck_lib_cursor > 0) m_deck_lib_cursor--; break;
            case Btn::Right: if (m_deck_lib_cursor + 1 < n) m_deck_lib_cursor++; break;
            case Btn::Up:    if (m_deck_lib_cursor >= kDeckLibCols)
                                 m_deck_lib_cursor -= kDeckLibCols;
                             break;
            case Btn::Down:  if (m_deck_lib_cursor + kDeckLibCols < n)
                                 m_deck_lib_cursor += kDeckLibCols;
                             else m_deck_lib_cursor = n - 1;
                             break;
            case Btn::A: {
                // Hand off to the shared handler: it already knows what every
                // kind does, including the "close the running game first"
                // dialog, so this screen only decides which entry.
                m_cursor = list[m_deck_lib_cursor];
                m_screen = Screen::Main;
                return ActivateSelected(out_app_id);
            }
            case Btn::X: {
                // Options for the entry under the cursor, same as the home screen.
                m_cursor = list[m_deck_lib_cursor];
                m_options_sub = Sub_None;
                BuildOptions();
                m_options_cursor = 0;
                if (!m_options.empty()) m_options_open = true;
                break;
            }
            default: break;
        }
        return Action::None;
    }
    // ---- Deck: the side menu -------------------------------------------------
    // Everything that is not a game, in a panel down the left, over a dimmed
    // copy of the home screen - the shape SteamOS uses for its own menu. The
    // entries are the same m_items every other layout shows, so hiding one under
    // Theming > Menu entries hides it here too, and "Library" is prepended
    // because on this layout that is a place rather than an entry.
    void Menu::DrawDeckMenu() {
        m_deck_backdrop = true;
        DrawMainDeck();                      // the screen it slid over
        m_deck_backdrop = false;

        // The panel slides in from the edge it's anchored to rather than just
        // cutting in at full size - the screen-transition fade every other
        // screen change gets (Render()) is suppressed for this one (see
        // m_suppress_screen_fade) because a full-screen darken barely reads
        // as motion here: the backdrop above is already dark, and this panel
        // is the one thing on screen actually worth animating.
        int xoff = 0;
        if (m_screen_trans_tick != 0) {
            constexpr u64 kSlideMs = 150;
            const u64 ms = (armGetSystemTick() - m_screen_trans_tick) * 1000
                         / armGetSystemTickFreq();
            if (ms < kSlideMs) {
                const float t2 = (float)ms / (float)kSlideMs;
                const float e  = 1.0f - (1.0f - t2) * (1.0f - t2);  // ease-out
                xoff = -(int)((1.0f - e) * (float)kDeckMenuW);
            }
        }
        DrawDeckMenuPanel(xoff, 150);
    }
    // The tail end of closing back to the Deck home screen: called from
    // Render() *after* that screen has already drawn itself this frame (see
    // m_deck_menu_close_tick there), so this owns only the dim and the panel,
    // sliding the other way instead of the panel just vanishing. u in [0,1],
    // 0 = still fully open, 1 = fully closed.
    void Menu::DrawDeckMenuClosing(float u) {
        const int xoff = -(int)(u * (float)kDeckMenuW);
        const Uint8 dim = (Uint8)(150.0f * (1.0f - u));
        DrawDeckMenuPanel(xoff, dim);
    }
    // Shared by both: the dim, the panel and everything on it, offset by
    // xoff so a single set of draw calls serves opening and closing alike.
    void Menu::DrawDeckMenuPanel(int xoff, Uint8 dimAlpha) {
        const Theme &t = m_theme.Current();
        m_gfx->FillRect(0, 0, gfx::Gfx::Width, gfx::Gfx::Height,
                        SDL_Color{ 0, 0, 0, dimAlpha });
        // Frosted: the panel samples the menu drawn behind it this frame, so it
        // blurs whatever is actually back there - including the covers moving
        // under it - rather than sitting on a flat block of colour. The tint on
        // top is what keeps the text readable over it.
        m_gfx->DrawSceneBlurred(xoff, 0, kDeckMenuW, gfx::Gfx::Height, 10);
        m_gfx->FillRect(xoff, 0, kDeckMenuW, gfx::Gfx::Height, WithAlpha(t.bg_top, 205));
        m_gfx->FillRect(xoff + kDeckMenuW, 0, 2, gfx::Gfx::Height, WithAlpha(t.accent, 120));

        m_gfx->Text(FontSize::Large, 34 + xoff, 34, t.title, T("Menu"));

        std::vector<int> rows;                  // -1 = the Library entry
        rows.push_back(-1);
        for (int i = 0; i < (int)m_items.size(); i++)
            if (FlowMenuItem(m_items[i])) rows.push_back(i);

        const int n = (int)rows.size();
        if (m_deck_menu_cursor >= n) m_deck_menu_cursor = n - 1;

        const int top = 106, rh = 54;
        const int visible = (gfx::Gfx::Height - top - 60) / rh;
        int first = 0;
        if (m_deck_menu_cursor >= visible) first = m_deck_menu_cursor - visible + 1;

        for (int slot = 0; slot < visible; slot++) {
            const int i = first + slot;
            if (i >= n) break;
            const int y = top + slot * rh;
            const bool sel = (i == m_deck_menu_cursor);
            if (sel) {
                m_gfx->FillRect(xoff, y - 6, kDeckMenuW, rh - 4, WithAlpha(t.accent, 60));
                m_gfx->FillRect(xoff, y - 6, 4, rh - 4, t.accent);
            }
            const char *label;
            if (rows[i] < 0) {
                label = T("Library");
                // No item to take an icon from, so it draws its own: four tiles,
                // which is what the screen it opens looks like. Drawn in the
                // icon colour rather than the text colour, so it matches the
                // real glyphs sitting above and below it in this list.
                m_gfx->FillRect(24 + xoff, y - 2, 36, 36, IconPlate(t, 255));
                for (int q = 0; q < 4; q++)
                    m_gfx->FillRect(30 + xoff + (q % 2) * 13, y + 4 + (q / 2) * 13,
                                    10, 10, sel ? t.accent : t.icon_fg);
            } else {
                label = m_items[rows[i]].name.c_str();
                DrawAppTile(m_items[rows[i]], 24 + xoff, y - 2, 36, false, 255);
            }
            m_gfx->Text(FontSize::Normal, 76 + xoff, y + 2, sel ? t.accent : t.fg,
                        Ellipsize(label, kDeckMenuW - 100, FontSize::Normal).c_str());
        }
        DrawHint({ {{"up","down"}, "Select"}, {{"a"}, "Open"}, {{"b"}, "Close"} });
    }
    Menu::Action Menu::OnButtonDeckMenu(Btn b, u64 &out_app_id) {
        std::vector<int> rows;
        rows.push_back(-1);
        for (int i = 0; i < (int)m_items.size(); i++)
            if (FlowMenuItem(m_items[i])) rows.push_back(i);

        const int n = (int)rows.size();
        if (b == Btn::B || b == Btn::Minus) {
            m_screen = Screen::Main;
            m_deck_menu_close_tick = armGetSystemTick();   // slide back out; see Render()
            return Action::None;
        }
        if (n == 0) return Action::None;
        if (m_deck_menu_cursor >= n) m_deck_menu_cursor = n - 1;

        if (b == Btn::Down) m_deck_menu_cursor = (m_deck_menu_cursor + 1) % n;
        if (b == Btn::Up)   m_deck_menu_cursor = (m_deck_menu_cursor + n - 1) % n;

        if (b == Btn::A) {
            if (rows[m_deck_menu_cursor] < 0) {
                m_screen = Screen::DeckLibrary;
                m_deck_lib_cursor = 0;
                m_deck_lib_scroll = 0.0f;
                return Action::None;
            }
            m_cursor = rows[m_deck_menu_cursor];
            m_screen = Screen::Main;
            const Action a = ActivateSelected(out_app_id);
            // Opened a screen rather than launching something: remember where
            // from, so B comes back to the menu instead of the home screen.
            m_from_deck_menu = (m_screen != Screen::Main);
            return a;
        }
        return Action::None;
    }
    // ---- Deck: the reader ----------------------------------------------------
    void Menu::DrawDeckNews() {
        const Theme &t = m_theme.Current();
        DrawTopBar(nullptr);

        const std::vector<news::Item> &cards = DeckCards();
        if (m_deck_reading >= (int)cards.size()) {
            m_gfx->TextCentered(FontSize::Normal, gfx::Gfx::Width / 2, 340, t.dim,
                                T("No news"));
            DrawHint({ {{"b"}, "Back"} });
            return;
        }
        const news::Item &n = cards[m_deck_reading];

        // Art across the top. Kept to the same height the panel always budgeted
        // for it (the summary below needs the rest), but true 16:9 now instead
        // of stretched to the full panel width - so it is narrower than the
        // panel and centred, rather than spanning it.
        const int px = 150, pw = gfx::Gfx::Width - px * 2;
        int y = 92;
        if (SDL_Texture *art = DeckArt(n.img)) {
            const int ah = 270, aw = ah * 16 / 9;
            const int ax = px + (pw - aw) / 2;
            m_gfx->FillRect(ax, y, aw, ah, WithAlpha(t.bg_bottom, 235));
            m_gfx->DrawImage(art, ax, y, aw, ah, 255);
            y += ah + 18;
        }

        std::string head = n.kind;
        for (char &c : head) c = (char)toupper((unsigned char)c);
        if (!n.date.empty()) head += "   " + n.date;
        m_gfx->Text(FontSize::Small, px, y, t.accent, head.c_str());
        y += 26;

        y += DeckWrapText(n.title, px, y, pw, FontSize::Large, t.title, 2) * 32 + 10;
        DeckWrapText(n.summary, px, y, pw, FontSize::Normal, t.fg, 5);

        if (!n.link.empty())
            m_gfx->Text(FontSize::Small, px, gfx::Gfx::Height - 96, t.dim,
                        Ellipsize(n.link, pw, FontSize::Small).c_str());
        DrawHint({ {{"b"}, "Back"} });
    }
    Menu::Action Menu::OnButtonDeckNews(Btn b) {
        const std::vector<news::Item> &cards = DeckCards();
        const int n = (int)cards.size();
        if (b == Btn::B) { m_screen = Screen::Main; return Action::None; }
        if (n == 0) return Action::None;
        if (b == Btn::Right && m_deck_reading + 1 < n) { m_deck_reading++; m_deck_card = m_deck_reading; }
        if (b == Btn::Left  && m_deck_reading > 0)     { m_deck_reading--; m_deck_card = m_deck_reading; }
        return Action::None;
    }
    // ---- Deck: touch ---------------------------------------------------------
    // The cover row follows the cursor rather than owning a scroll of its own, so
    // there is nothing here to drag - a tap picks, and a tap on what is already
    // picked opens it, which is how every other layout behaves.
    int Menu::DeckItemAt(int px, int py) const {
        if (py < kDeckRowY || py > kDeckRowY + kDeckRowH) return -1;
        std::vector<int> row;
        DeckRow(row);
        for (int i = 0; i < (int)row.size(); i++) {
            const int w = (i == 0) ? kDeckHeroW : kDeckTileW;
            const int x = DeckSlotX(i) - (int)m_deck_scroll;
            if (x > gfx::Gfx::Width) break;
            if (px >= x && px < x + w) return row[i];
        }
        return -1;
    }
    bool Menu::DeckTap(int px, int py, u64 &out_app_id, Action &out) {
        (void)out_app_id;
        out = Action::None;

        // Tabs.
        if (py >= kDeckTabY - 6 && py <= kDeckTabY + kDeckTabH + 6) {
            const char *labels[DeckTab_Count] = { T("What's new"), T("Nintendo"),
                                                  T("Widgets") };
            int w[DeckTab_Count], total = 0;
            for (int i = 0; i < DeckTab_Count; i++) {
                w[i] = m_gfx->TextWidth(FontSize::Normal, labels[i]) + 44;
                total += w[i];
            }
            int x = (gfx::Gfx::Width - total) / 2;
            for (int i = 0; i < DeckTab_Count; i++) {
                if (px >= x && px < x + w[i]) {
                    if (i != m_deck_tab) { m_deck_tab = i; m_deck_card = 0; }
                    m_deck_row = 1;
                    return true;
                }
                x += w[i];
            }
            return false;
        }

        // Cards.
        if (py >= kDeckCardY && py <= kDeckCardY + kDeckCardH) {
            const std::vector<news::Item> &cards = DeckCards();
            const int ncards = (m_deck_tab == DeckTab_Widgets)
                             ? std::min(kDeckCardsVisible, EnabledWidgetCount())
                             : (int)cards.size();
            if (ncards == 0) return false;
            int first = 0;
            if (m_deck_row == 2 && m_deck_card >= kDeckCardsVisible)
                first = m_deck_card - kDeckCardsVisible + 1;
            for (int slot = 0; slot < kDeckCardsVisible; slot++) {
                const int x = kDeckCardX + slot * (kDeckCardW + kDeckCardGap);
                if (px < x || px >= x + kDeckCardW) continue;
                const int idx = first + slot;
                if (idx >= ncards) return false;
                // Tapping the card already under the cursor opens it, exactly as
                // A does; the first tap only moves the selection there.
                const bool again = (m_deck_row == 2 && idx == m_deck_card);
                m_deck_row  = 2;
                m_deck_card = idx;
                if (again && m_deck_tab != DeckTab_Widgets) {
                    m_deck_reading = idx;
                    m_screen = Screen::DeckNews;
                }
                return true;
            }
        }
        return false;
    }
    int Menu::DeckLibraryAt(int px, int py) const {
        if (py < kDeckLibTop - 6 || py > kDeckLibBot) return -1;
        std::vector<int> list;
        DeckLibraryList(list);
        const int n = (int)list.size();
        const int per = kDeckLibH + kDeckLibGapY;
        for (int i = 0; i < n; i++) {
            const int r = i / kDeckLibCols, c = i % kDeckLibCols;
            const int x = kDeckLibX + c * (kDeckLibW + kDeckLibGapX);
            const int y = kDeckLibTop + (int)((r - m_deck_lib_scroll) * per);
            if (px >= x && px < x + kDeckLibW && py >= y && py < y + kDeckLibH)
                return i;
        }
        return -1;
    }
} // namespace sl::menu::ui
