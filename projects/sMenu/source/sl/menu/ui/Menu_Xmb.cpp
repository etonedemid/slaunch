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

    // ---------------------------------------------------------------------------
    // XMB (PSP cross-media bar)
    // ---------------------------------------------------------------------------
    namespace {

        // Homebrew that is really a way onto the internet, and so belongs in the
        // Network column next to the browser rather than in the pile of
        // everything else on the card. NetSurf is the one people actually have;
        // the list exists so adding the next one is a line, not a rewrite.
        //
        // Matched against both the title and the file name: a .nro carries
        // whatever title its author gave it (NetSurf's own builds have shipped
        // as "NetSurf" and as "NetSurf Browser"), and when the title is missing
        // the menu falls back to the file base name.
        constexpr const char *kNetHomebrew[] = { "netsurf" };

        // Case-insensitive substring search. Lowercase-ASCII only, which is all
        // the needles above need; a UTF-8 title passes through untouched and
        // simply does not match.
        bool ContainsCI(const std::string &hay, const char *needle) {
            const size_t n = strlen(needle);
            if (n == 0 || hay.size() < n) return false;
            for (size_t i = 0; i + n <= hay.size(); i++) {
                size_t k = 0;
                while (k < n && tolower((unsigned char)hay[i + k]) == needle[k]) k++;
                if (k == n) return true;
            }
            return false;
        }

        bool IsNetHomebrew(const MenuItem &it) {
            for (const char *needle : kNetHomebrew)
                if (ContainsCI(it.name, needle) || ContainsCI(it.hb_path, needle))
                    return true;
            return false;
        }

    } // namespace

    // Which column an entry belongs to. The six columns mirror the PSP's own
    // (Settings, Photo, Music, Video, Game, Network) mapped onto what a Switch
    // actually has, keeping Game in the same place along the bar.
    Menu::XmbCat Menu::XmbCatOf(const MenuItem &it) {
        switch (it.kind) {
            case ItemKind::Game:
            case ItemKind::RandomGame:   return XmbCat::Game;
            case ItemKind::Homebrew:     return IsNetHomebrew(it) ? XmbCat::Network
                                                                  : XmbCat::Homebrew;
            case ItemKind::HomebrewMenu:
            case ItemKind::FileManager:  return XmbCat::Homebrew;
            case ItemKind::Album:
            case ItemKind::MusicPlayer:  return XmbCat::Media;
            case ItemKind::UserPage:
            case ItemKind::MiiEdit:      return XmbCat::User;
            case ItemKind::WebBrowser:   return XmbCat::Network;
            default:                     return XmbCat::Settings;   // Theming, Controllers, Settings, Power...
        }
    }
    const char *Menu::XmbCatName(XmbCat c) {
        switch (c) {
            case XmbCat::Settings: return T("Settings");
            case XmbCat::Media:    return T("Media");
            case XmbCat::User:     return T("User");
            case XmbCat::Network:  return T("Network");
            case XmbCat::Game:     return T("Games");
            case XmbCat::Homebrew: return T("Homebrew");
            default:               return "";
        }
    }
    // Column headline icon, borrowed from the system entry that best represents it.
    ItemKind Menu::XmbCatIconKind(XmbCat c) {
        switch (c) {
            case XmbCat::Settings: return ItemKind::Settings;
            case XmbCat::Media:    return ItemKind::MediaCat;
            case XmbCat::User:     return ItemKind::UserPage;
            case XmbCat::Network:  return ItemKind::WebBrowser;
            case XmbCat::Game:     return ItemKind::Game;
            case XmbCat::Homebrew: return ItemKind::HomebrewMenu;
            default:               return ItemKind::Settings;
        }
    }
    // Regroup m_items into the bar. Called from RebuildItems, so the per-frame
    // work is only ever "walk the visible slice of one column".
    void Menu::XmbRebuild() {
        m_xmb_cols.clear();
        std::vector<int> bucket[(int)XmbCat::Count];
        // Shortcut categories, in the order they are first seen. m_items has
        // them already grouped and sorted (ScanShortcuts sorts by category, and
        // RebuildItems appends them in that order), so first-seen order is
        // alphabetical without sorting anything again here.
        std::vector<std::pair<std::string, std::vector<int>>> cats;
        for (int i = 0; i < (int)m_items.size(); i++) {
            // The Homebrew column lists every scanned .nro directly, so the
            // entry that only opens the browser has nothing left to offer there
            // and is dropped. It stays in m_items for the other layouts, where
            // it is still their only route to the browser.
            if (m_items[i].kind == ItemKind::HomebrewMenu) continue;
            const std::string &cat = m_items[i].category;
            if (!cat.empty()) {
                auto it = std::find_if(cats.begin(), cats.end(),
                                       [&](const auto &c){ return c.first == cat; });
                if (it == cats.end()) { cats.push_back({ cat, { i } }); continue; }
                it->second.push_back(i);
                continue;
            }
            bucket[(int)XmbCatOf(m_items[i])].push_back(i);
        }

        // Emulated libraries sit immediately after the console's own games,
        // which is where you look for them - and it keeps them off the far end
        // of a bar that would otherwise put Homebrew between the two. With no
        // games installed there is no Games column to sit after, so they are
        // appended instead of being dropped.
        bool cats_placed = cats.empty();
        for (int c = 0; c < (int)XmbCat::Count; c++) {
            if (bucket[c].empty()) continue;          // empty columns are not shown
            m_xmb_cols.push_back({ (XmbCat)c, std::string(), std::move(bucket[c]) });
            if ((XmbCat)c != XmbCat::Game || cats_placed) continue;
            for (auto &kv : cats)
                m_xmb_cols.push_back({ XmbCat::Game, kv.first, std::move(kv.second) });
            cats_placed = true;
        }
        if (!cats_placed)
            for (auto &kv : cats)
                m_xmb_cols.push_back({ XmbCat::Game, kv.first, std::move(kv.second) });
        if (m_xmb_cols.empty()) { m_xmb_col = -1; m_xmb_item = 0; return; }

        // Every entry lands in exactly one column, so following m_cursor keeps
        // the bar on whatever was selected across a rebuild (a favourite being
        // toggled, the full app list replacing the cached one, ...). m_cursor is
        // deliberately not written back here: the other layouts share it, and a
        // rebuild must not move their selection.
        m_xmb_col = -1;
        XmbSyncFromCursor();
        if (m_xmb_col < 0) { m_xmb_col = 0; m_xmb_item = 0; }
    }
    // Put the bar on the Games column. Used the first time XMB is actually shown:
    // the flat cursor starts at the top of the list, which is a system entry, and
    // opening the bar anywhere but Games would be odd on a games console (the
    // handheld starts there too).
    void Menu::XmbOpenDefaultColumn() {
        m_xmb_placed = true;
        for (int i = 0; i < (int)m_xmb_cols.size(); i++) {
            if (m_xmb_cols[i].cat != XmbCat::Game) continue;
            m_xmb_col = i; m_xmb_item = 0;
            m_xmb_col_scroll = (float)i; m_xmb_item_scroll = 0.0f;
            XmbApplyCursor();
            return;
        }
    }
    // What L/R do in the open column - the hint has to say which, since the
    // shoulders page by five in most columns and jump by initial in a shortcut
    // category. Returns a T() key, like every other hint label.
    const char *Menu::XmbShoulderHint() const {
        if (m_xmb_col < 0 || m_xmb_col >= (int)m_xmb_cols.size()) return "Jump";
        return m_xmb_cols[m_xmb_col].label.empty() ? "Jump" : "Letter";
    }

    // The character a name is filed under: first letter, upper-cased, with
    // everything that is not a letter collapsed to one '#' bucket so the
    // leading-digit and bracketed-prefix names a ROM set is full of do not each
    // become a stop of their own.
    char Menu::XmbInitial(const std::string &name) {
        if (name.empty()) return '#';
        const unsigned char c = (unsigned char)name[0];
        if (c >= 'a' && c <= 'z') return (char)(c - 32);
        if (c >= 'A' && c <= 'Z') return (char)c;
        return '#';   // digits, punctuation, and any UTF-8 lead byte
    }

    // Index of the first entry of the next (or previous) initial, clamped at the
    // ends. Always moves at least one entry so an unsorted column still steps.
    int Menu::XmbLetterJump(int dir) const {
        if (m_xmb_col < 0 || m_xmb_col >= (int)m_xmb_cols.size()) return m_xmb_item;
        const auto &items = m_xmb_cols[m_xmb_col].items;
        const int n = (int)items.size();
        if (n == 0) return 0;

        const int cur = std::min(std::max(0, m_xmb_item), n - 1);
        const char here = XmbInitial(m_items[items[cur]].name);

        if (dir > 0) {
            for (int i = cur + 1; i < n; i++)
                if (XmbInitial(m_items[items[i]].name) != here) return i;
            return n - 1;
        }
        // Backwards behaves like a "previous track" button: from inside a letter
        // it goes to that letter's first entry, and only from there on to the
        // previous letter's first. Jumping straight to the previous letter
        // skips the start of the one you are standing in, which is usually
        // where you were trying to get back to.
        int start = cur;
        while (start > 0 && XmbInitial(m_items[items[start - 1]].name) == here) start--;
        if (start < cur) return start;      // inside a letter: go to its start
        if (start == 0) return 0;           // already at the very top
        int j = start - 1;                  // at a letter's start: previous letter
        const char prev = XmbInitial(m_items[items[j]].name);
        while (j > 0 && XmbInitial(m_items[items[j - 1]].name) == prev) j--;
        return j;
    }

    // Point the bar at whatever m_cursor currently selects, so switching into XMB
    // from another layout (or back from a submenu) keeps your place.
    void Menu::XmbSyncFromCursor() {
        for (int c = 0; c < (int)m_xmb_cols.size(); c++) {
            const auto &items = m_xmb_cols[c].items;
            for (int i = 0; i < (int)items.size(); i++) {
                if (items[i] != m_cursor) continue;
                m_xmb_col  = c;
                m_xmb_item = i;
                m_xmb_col_scroll  = (float)c;
                m_xmb_item_scroll = (float)i;
                return;
            }
        }
    }
    void Menu::XmbApplyCursor() {
        if (m_xmb_col < 0 || m_xmb_col >= (int)m_xmb_cols.size()) return;
        const auto &items = m_xmb_cols[m_xmb_col].items;
        if (m_xmb_item >= 0 && m_xmb_item < (int)items.size())
            m_cursor = items[m_xmb_item];
    }
    // XMB mode: the PSP's cross-media bar. A horizontal row of category icons
    // crosses the screen; the selected one is anchored at a fixed point and its
    // entries hang below it in a vertical column, the selected entry anchored in
    // turn. Moving sideways slides the bar, moving down slides the column under
    // the crossing point, and entries that pass above it fade out behind the bar.
    //
    // Switch-side departures from the handheld, all in the name of usability:
    // columns clamp at the ends (there are only six, all visible at once) while
    // the column wraps on a fresh press, since a library can be hundreds long;
    // the selected entry carries a play-time/state line; and everything is
    // touchable.
    // One entry in the cross-media bar: real cover art for games and homebrew,
    // a theme-coloured glyph for system entries. Deliberately not DrawAppTile,
    // which frames every tile with an accent strip and a selection box - that
    // reads as a grid of cards, and the bar wants bare icons floating on the
    // background with nothing but size and brightness separating them.
    void Menu::DrawXmbIcon(const MenuItem &it, int cx, int cy, int size, Uint8 alpha) {
        const Theme &t = m_theme.Current();
        const bool isGame = (it.kind == ItemKind::Game);
        const bool isHb   = (it.kind == ItemKind::Homebrew);
        const int  x = cx - size / 2, y = cy - size / 2;

        SDL_Texture *icon = isGame ? m_icons.Get(it.app_id)
                          : isHb   ? m_hb_icons.Get(it.hb_icon)
                                   : SystemIcon(it.kind);
        if (icon && (isGame || isHb)) {
            m_gfx->DrawImage(icon, x, y, size, size, alpha);
        } else if (icon) {
            m_gfx->FillRect(x, y, size, size, IconPlate(t, alpha));
            m_gfx->DrawImageTinted(icon, x, y, size, size, IconTint(t, alpha));
        } else {
            // No artwork cached yet: a plain plate with the initial, sized to
            // match, so the column never gains or loses a row while icons load.
            m_gfx->FillRect(x, y, size, size, WithAlpha(t.bg_bottom, (Uint8)(alpha * 170 / 255)));
            char initial[2] = { it.name.empty() ? '?' : it.name[0], '\0' };
            if (initial[0] >= 'a' && initial[0] <= 'z') initial[0] -= 32;
            const int iw = m_gfx->TextWidth(FontSize::Normal, initial);
            const int ih = m_gfx->LineHeight(FontSize::Normal);
            m_gfx->Text(FontSize::Normal, cx - iw / 2, cy - ih / 2,
                        WithAlpha(t.dim, alpha), initial);
        }

        // Running badge, kept tiny so it reads as a status dot, not decoration.
        if (isGame && it.app_id == m_suspended && m_suspended != 0)
            m_gfx->FillRect(x + size - 9, y - 3, 8, 8, WithAlpha(t.accent, alpha));
    }
    // XMB mode, laid out the way RetroArch's XMB driver lays it out.
    //
    // Two axes crossing at one anchor: a horizontal row of category icons near
    // the top, and the open category's entries hanging below it in a column.
    // The active category is parked directly above that column rather than in
    // the middle of the screen, which is what makes the whole thing read as a
    // cross instead of as two stacked lists.
    //
    // Selection is carried by size and brightness only. There are no panels,
    // frames, rules or selection boxes anywhere in this layout, and the row
    // spacing is deliberately uneven - see the kXmb* offsets - because the
    // cursor row has to clear the category bar above it and leave a band for
    // its own sublabel below.
    //
    // Presentation only: selection, input and the item model are shared with the
    // other five layouts and are untouched here.
    void Menu::DrawMainXmb() {
        const Theme &t = m_theme.Current();
        const int   W  = gfx::Gfx::Width;
        const int   H  = gfx::Gfx::Height;
        m_icons.SetScale(gfx::IconCache::GridScale);

        if (m_items.empty() || m_xmb_cols.empty()) {
            DrawTopBar(nullptr);
            DrawMainEmpty();
            return;
        }
        if (!m_xmb_placed) XmbOpenDefaultColumn();
        if (m_xmb_col < 0) { m_xmb_col = 0; m_xmb_item = 0; }

        m_xmb_col_scroll  += ((float)m_xmb_col  - m_xmb_col_scroll)  * 0.20f;
        if (!ScrollBusy())   // a finger or a throw owns the scroll instead
            m_xmb_item_scroll += ((float)m_xmb_item - m_xmb_item_scroll) * 0.26f;
        if (std::abs((float)m_xmb_col  - m_xmb_col_scroll)  < 0.004f) m_xmb_col_scroll  = (float)m_xmb_col;
        if (std::abs((float)m_xmb_item - m_xmb_item_scroll) < 0.004f) m_xmb_item_scroll = (float)m_xmb_item;

        const XmbColumn &col   = m_xmb_cols[m_xmb_col];
        const int        count = (int)col.items.size();

        // The category name doubles as the screen title, as it does in XMB.
        const char *colname = m_xmb_cols[m_xmb_col].label.empty()
                                  ? XmbCatName(m_xmb_cols[m_xmb_col].cat)
                                  : m_xmb_cols[m_xmb_col].label.c_str();
        // A filtered bar looks exactly like an unfiltered one with fewer games
        // in it, so the query has to be on screen the whole time it is active.
        std::string header = colname ? colname : "";
        if (!m_search.empty()) header = std::string(T("Search")) + ": \"" + m_search + "\"";
        DrawXmbHeader(header.c_str());

        // --- category row ----------------------------------------------------
        // RetroArch tweens every tab between a passive and an active zoom and
        // alpha as the row slides; distance from the anchor stands in for that
        // tween here, which gives the same result without a tween queue.
        for (int c = 0; c < (int)m_xmb_cols.size(); c++) {
            const float d  = (float)c - m_xmb_col_scroll;
            const int   cx = kXmbAnchorX + (int)(d * kXmbSpacingH);
            if (cx < -kXmbIcon || cx > W + kXmbIcon) continue;

            const float prox = std::max(0.0f, 1.0f - std::abs(d));
            const float zoom = kXmbZoomPassive + (kXmbZoomActive - kXmbZoomPassive) * prox;
            const int   sz   = (int)(kXmbIcon * zoom);
            const Uint8 a    = (Uint8)(255.0f * (0.75f + 0.25f * prox));

            if (SDL_Texture *icon = SystemIcon(XmbCatIconKind(m_xmb_cols[c].cat))) {
                const int ix = cx - sz / 2, iy = kXmbTabY - sz / 2;
                m_gfx->FillRect(ix, iy, sz, sz, IconPlate(t, a));
                m_gfx->DrawImageTinted(icon, ix, iy, sz, sz, IconTint(t, a));
            }
        }

        // --- entry column -----------------------------------------------------
        // The whole column fades out and back while the category row slides, so
        // the two axes never look like two independent lists.
        const float slide = std::min(1.0f, std::abs((float)m_xmb_col - m_xmb_col_scroll));
        const Uint8 listA = (Uint8)(255.0f * (1.0f - slide));
        if (listA <= 8 || count == 0) {
            DrawStatusHint({ {{"a"}, "Select"}, {{"y"}, "Search"},
                         {{"l","r"}, XmbShoulderHint()}, {{"x"}, "Options"} });
            return;
        }

        const int firstv = std::max(0, (int)m_xmb_item_scroll - kXmbAbove - 1);
        const int lastv  = std::min(count - 1, (int)m_xmb_item_scroll + kXmbBelow + 1);
        const int textX  = kXmbAnchorX + kXmbIcon / 2 + kXmbLabelLeft;

        for (int i = firstv; i <= lastv; i++) {
            const float d  = (float)i - m_xmb_item_scroll;
            const int   cy = kXmbMarginTop + kXmbIcon / 2 + (int)XmbRowOffset(d);
            if (cy >= kXmbFadeBotEnd) break;
            if (cy < kXmbFadeEnd)     continue;

            // Rows drifting up into the category row fade out behind it instead
            // of clipping against it - RetroArch's vertical fade factor - and
            // rows sliding off the bottom fade the same way rather than popping.
            float fade = 1.0f;
            if (cy < kXmbFadeStart)
                fade = (float)(cy - kXmbFadeEnd) / (float)(kXmbFadeStart - kXmbFadeEnd);
            else if (cy > kXmbFadeBotStart)
                fade = (float)(kXmbFadeBotEnd - cy)
                     / (float)(kXmbFadeBotEnd - kXmbFadeBotStart);

            const bool  sel  = (i == m_xmb_item);
            const float prox = std::max(0.0f, 1.0f - std::abs(d));
            const float zoom = kXmbZoomPassive + (kXmbZoomActive - kXmbZoomPassive) * prox;
            const float al   = kXmbAlphaPassive + (kXmbAlphaActive - kXmbAlphaPassive) * prox;
            const int   sz   = (int)(kXmbIcon * zoom);
            const Uint8 a    = (Uint8)(listA * fade * al);

            DrawXmbIcon(m_items[col.items[i]], kXmbAnchorX, cy, sz, a);

            // Labels share one left edge whatever their icon's current size, so
            // the text column stays straight while the icons breathe around the
            // cursor. Row height, not icon height, centres them.
            const FontSize fs = sel ? FontSize::Normal : FontSize::Small;
            const int      th = m_gfx->LineHeight(fs);
            const std::string label =
                  Ellipsize(m_items[col.items[i]].name, W - 120 - textX, fs);
            m_gfx->Text(fs, textX, cy - th / 2,
                        WithAlpha(sel ? t.title : t.fg, a), label.c_str());
        }

        // --- sublabel, cursor row only ----------------------------------------
        // Sits in the band the placement curve leaves open under the cursor,
        // which is the whole reason that band exists.
        const MenuItem &sel  = m_items[col.items[m_xmb_item]];
        const int       selY = kXmbMarginTop + kXmbIcon / 2
                             + (int)XmbRowOffset((float)m_xmb_item - m_xmb_item_scroll);

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
        if (!info.empty())
            m_gfx->Text(FontSize::Small, textX,
                        selY + m_gfx->LineHeight(FontSize::Normal) / 2 + 6,
                        WithAlpha(t.dim, listA),
                        Ellipsize(info, W - 120 - textX, FontSize::Small).c_str());

        // Position within the category, bottom right, as RetroArch places its
        // entry index.
        char pos[32];
        snprintf(pos, sizeof(pos), "%d/%d", m_xmb_item + 1, count);
        // Position counters are optional; blanking the string here keeps
        // the layout arithmetic below untouched.
        if (!m_show_counter) pos[0] = '\0';
        const int pw = m_gfx->TextWidth(FontSize::Small, pos);
        const int ph = m_gfx->LineHeight(FontSize::Small);
        m_gfx->Text(FontSize::Small, W - 8 - pw, H - 8 - ph,
                    WithAlpha(t.dim, listA), pos);

        DrawStatusHint({ {{"a"}, "Select"}, {{"y"}, "Search"},
                         {{"l","r"}, XmbShoulderHint()}, {{"x"}, "Options"} });
    }
} // namespace sl::menu::ui
