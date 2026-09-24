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

    void Menu::DrawMainList() {
        const Theme &t = m_theme.Current();
        m_icons.SetScale(gfx::IconCache::GridScale); // small thumbnails: downscaled
        DrawTopBar(nullptr);

        if (m_items.empty()) { DrawMainEmpty(); return; }

        // Niagara-style vertical carousel: the item at the vertical centre is
        // enlarged with a '>' cursor; neighbours shrink and fade with distance.
        // The whole list slides smoothly because m_scroll_pos eases toward the
        // integer cursor rather than snapping to it.
        if (!ScrollBusy())   // a finger or a throw owns the scroll instead
            m_scroll_pos += (m_cursor - m_scroll_pos) * 0.30f; // ease toward target
        if (std::abs(m_cursor - m_scroll_pos) < 0.01f) m_scroll_pos = (float)m_cursor;

        const int margin   = kListX;      // left/right margin for text
        const int center_y = kListCenterY; // centre row's vertical centre
        const int spacing  = kListSpacing; // gap between adjacent items
        const int span     = 7;           // items drawn on each side of centre

        for (int off = -span; off <= span; off++) {
            int idx = (int)lroundf(m_scroll_pos) + off;
            if (idx < 0 || idx >= (int)m_items.size()) continue;
            const MenuItem &it = m_items[idx];

            const float vdist = std::abs((float)idx - m_scroll_pos); // distance from centre
            const bool  big   = vdist < 0.5f;                        // the centred row
            const FontSize fs = big ? FontSize::Large : FontSize::Normal;
            const Uint8 alpha = (Uint8)std::max(24.0f, 255.0f - vdist * 52.0f);
            const int   y     = center_y + (int)((idx - m_scroll_pos) * spacing) - m_gfx->LineHeight(fs) / 2;
            if (y < 90 || y > kHintY - 30) continue;

            const bool sel     = (idx == m_cursor);
            const bool running = (it.kind == ItemKind::Game &&
                                  it.app_id == m_suspended && m_suspended != 0);

            // Favourites get a leading star.
            std::string label = it.is_favourite ? (std::string("* ") + it.name)
                                                 : it.name;

            // Position the text per the chosen alignment; the '>' cursor always
            // sits just to the left of the text.
            const int lw = m_gfx->TextWidth(fs, label.c_str());
            int tx;
            switch (m_align) {
                case TextAlign::Center: tx = (gfx::Gfx::Width - lw) / 2; break;
                case TextAlign::Right:  tx = gfx::Gfx::Width - margin - lw; break;
                default:                tx = margin; break;
            }

            const int lh = m_gfx->LineHeight(fs);

            // Small icon in the left margin (Left alignment only, so it never
            // clashes with centred/right-aligned text). Games use their cached
            // app icon; system entries use their black/white icon.
            if (m_list_icons && m_align == TextAlign::Left) {
                const bool game = (it.kind == ItemKind::Game);
                const bool art  = game || it.kind == ItemKind::Homebrew;
                SDL_Texture *ic = game ? m_icons.Get(it.app_id)
                                 : it.kind == ItemKind::Homebrew ? m_hb_icons.Get(it.hb_icon)
                                 : SystemIcon(it.kind);
                if (ic) {
                    const int isz = std::min(lh, 44);
                    const Uint8 ia = game ? alpha : (Uint8)(alpha * 195 / 255);
                    // Real artwork is blitted as it is; a system glyph takes the
                    // theme's icon colour, the way the plate behind it does.
                    if (art) m_gfx->DrawImage(ic, 38, y + (lh - isz) / 2, isz, isz, ia);
                    else     m_gfx->DrawImageTinted(ic, 38, y + (lh - isz) / 2, isz, isz,
                                                    IconTint(t, ia));
                }
            }

            // The suspended game is highlighted with a faint accent pill and a
            // filled dot so it stands out even when it isn't the selected row.
            if (running) {
                m_gfx->FillRect(tx - 16, y - 4, lw + 150, lh + 8, WithAlpha(t.accent, 34));
                m_gfx->FillRect(tx - 8, y + lh / 2 - 5, 10, 10, WithAlpha(t.accent, alpha));
            }

            if (sel)
                m_gfx->Text(FontSize::Large, tx - 34, y, WithAlpha(t.accent, alpha), ">");
            const SDL_Color name_col = (running || big) ? t.accent : t.fg;
            m_gfx->Text(fs, tx, y, WithAlpha(name_col, alpha), label.c_str());

            if (running) {
                m_gfx->Text(FontSize::Small, tx + lw + 18,
                            y + lh - m_gfx->LineHeight(FontSize::Small) - 2,
                            WithAlpha(t.accent, alpha), "running");
            }
        }

        // Position indicator, right-aligned so the digits never crowd the edge.
        if ((int)m_items.size() > 1) {
            char pos[28];
            snprintf(pos, sizeof(pos), "%d / %d", m_cursor + 1, (int)m_items.size());
            // Position counters are optional; blanking the string here keeps
            // the layout arithmetic below untouched.
            if (!m_show_counter) pos[0] = '\0';
            int pw = m_gfx->TextWidth(FontSize::Small, pos);
            m_gfx->Text(FontSize::Small, gfx::Gfx::Width - pw - 40, 120, t.dim, pos);
        }

        // Play time / last played for the selected game, under the counter. Only
        // once pdm has answered, and only for games that have actually been played.
        if (const play::PlayInfo *pi = Play(m_items[m_cursor].app_id)) {
            if (pi->seconds > 0) {
                const std::string line = play::FormatPlaytime(pi->seconds) + "   " +
                                         play::FormatLastPlayed(pi->last_played);
                const int w = m_gfx->TextWidth(FontSize::Small, line.c_str());
                m_gfx->Text(FontSize::Small, gfx::Gfx::Width - w - 40, 150, t.dim, line.c_str());
            }
        }

        DrawStatusHint({ {{"a"}, "Select"}, {{"x"}, "Options"} });
    }
    // One item as a card: its artwork (or the themed plate with the glyph,
    // or a panel with the initial), rounded, shadowed, glowing when selected,
    // optionally standing on a reflection. Plain blits without the GPU path.
    void Menu::DrawAppTile(const MenuItem &it, int x, int y, int size,
                           bool selected, Uint8 alpha, bool reflect) {
        const Theme &t = m_theme.Current();

        const bool isGame = (it.kind == ItemKind::Game);
        const bool isHb   = (it.kind == ItemKind::Homebrew);
        SDL_Texture *icon = isGame ? m_icons.Get(it.app_id)
                          : isHb   ? m_hb_icons.Get(it.hb_icon)
                          : SystemIcon(it.kind);
        const bool art = icon && (isGame || isHb);

        gfx::Gfx::CardStyle cs;
        cs.radius   = std::max(6.0f, size / 9.0f);
        cs.shadow   = std::max(6.0f, size * 0.07f);
        cs.glow     = selected ? SelectionGlow() : 0.0f;
        cs.glow_col = t.accent;
        cs.reflect  = reflect;
        if (art) {
            cs.fill = t.bg_bottom;
        } else if (icon) {
            cs.fill  = IconPlate(t, 255);
            cs.glyph = true;
            cs.tint  = IconTint(t, 255);
        } else {
            cs.fill = WithAlpha(t.bg_bottom, 180);
        }
        const bool carded = m_gfx->Card(icon, (float)x, (float)y, (float)size, (float)size, cs, alpha);

        if (!carded) {
            if (art) {
                m_gfx->DrawImage(icon, x, y, size, size, alpha);   // real artwork
            } else if (icon) {
                // System entries: the themed plate, then the artwork on top of it.
                m_gfx->FillRect(x, y, size, size, IconPlate(t, alpha));
                m_gfx->DrawImageTinted(icon, x, y, size, size, IconTint(t, alpha));
            } else {
                m_gfx->FillRect(x, y, size, size, WithAlpha(t.bg_bottom, (Uint8)(alpha * 180 / 255)));
            }
            m_gfx->FillRect(x, y, size, 3, WithAlpha(t.accent, alpha)); // accent strip
        }
        if (!icon) {                                 // no icon file: big initial
            char initial[2] = { it.name.empty() ? '?' : it.name[0], '\0' };
            if (initial[0] >= 'a' && initial[0] <= 'z') initial[0] -= 32;
            const int iw = m_gfx->TextWidth(FontSize::Title, initial);
            m_gfx->Text(FontSize::Title, x + (size - iw) / 2, y + size / 2 - 26,
                        WithAlpha(t.dim, alpha), initial);
        }

        const bool running = (it.kind == ItemKind::Game &&
                              it.app_id == m_suspended && m_suspended != 0);
        if (running)
            m_gfx->FillRect(x + size - 20, y + 10, 10, 10, WithAlpha(t.accent, alpha));
        if (it.is_favourite)
            m_gfx->Text(FontSize::Small, x + 8, y + 6, WithAlpha(t.accent, alpha), "*");

        if (selected && !carded) {                  // selection frame - thin edges
            const SDL_Color a = t.accent;
            m_gfx->FillRect(x - 4,        y - 4,        size + 8, 4,        a);
            m_gfx->FillRect(x - 4,        y + size,     size + 8, 4,        a);
            m_gfx->FillRect(x - 4,        y - 4,        4,        size + 8, a);
            m_gfx->FillRect(x + size,     y - 4,        4,        size + 8, a);
        }
    }

    // The selected item's artwork, blurred to a wash of its colours behind the
    // whole layout. Built once the selection has rested for a moment (so a
    // fast scroll does not blur every icon it passes) and cross-faded in over
    // the previous one. System entries have no artwork, and fade it out.
    void Menu::DrawSelectionBackdrop() {
        if (m_items.empty()) return;
        const MenuItem &it = m_items[std::min(m_cursor, (int)m_items.size() - 1)];
        const u64 now = armGetSystemTick(), hz = armGetSystemTickFreq();
        const std::string key = ItemKey(it);
        if (key != m_bd_key) {
            m_bd_key = key;
            m_bd_moved = now;
            m_bd_pending = true;
        }
        if (m_bd_pending && (now - m_bd_moved) * 1000 / hz >= 180) {
            const bool artful = it.kind == ItemKind::Game || it.kind == ItemKind::Homebrew;
            SDL_Texture *src = !artful ? nullptr
                             : it.kind == ItemKind::Game ? m_icons.Get(it.app_id)
                                                         : m_hb_icons.Get(it.hb_icon);
            if (!src && it.kind == ItemKind::Game) src = FlowCover(it);   // box art will do
            if (src || !artful) {
                if (m_bd_old) m_gfx->FreeImage(m_bd_old);
                m_bd_old = m_bd_cur;
                m_bd_cur = src ? m_gfx->Blurred(src, 32) : nullptr;
                m_bd_tick = now;
                m_bd_pending = false;
            }
        }
        const float f = std::min(1.0f, (float)((now - m_bd_tick) * 1000 / hz) / 350.0f);
        const float strength = 120.0f;
        if (m_bd_old && f < 1.0f) m_gfx->DrawCover(m_bd_old, (Uint8)(strength * (1.0f - f)));
        if (m_bd_cur)             m_gfx->DrawCover(m_bd_cur, (Uint8)(strength * f));
        if (f >= 1.0f && m_bd_old) { m_gfx->FreeImage(m_bd_old); m_bd_old = nullptr; }
    }
    // Line mode: a horizontal cover carousel (EmulationStation style). The
    // selected cover sits centred and full-size; neighbours shrink and fade with
    // distance, and the whole strip eases toward the cursor.
    void Menu::DrawMainLine() {
        const Theme &t = m_theme.Current();
        m_icons.SetScale(0);   // few large covers -> original resolution (crisp)
        DrawSelectionBackdrop();
        DrawTopBar(nullptr);

        if (m_items.empty()) { DrawMainEmpty(); return; }

        if (!ScrollBusy())   // a finger or a throw owns the scroll instead
            m_scroll_pos += (m_cursor - m_scroll_pos) * 0.30f;
        if (std::abs(m_cursor - m_scroll_pos) < 0.01f) m_scroll_pos = (float)m_cursor;

        const int center_x = gfx::Gfx::Width / 2;
        const int center_y = 300;         // vertical centre of the covers
                                          // (room for the reflections below)
        const int bigSize  = 240;         // selected cover edge length
        const int spacing  = kLinePitch;  // horizontal gap between cover centres
        const int span     = 5;           // covers drawn each side of centre

        auto drawCover = [&](int off) {
            int idx = (int)lroundf(m_scroll_pos) + off;
            if (idx < 0 || idx >= (int)m_items.size()) return;

            const float hdist = std::abs((float)idx - m_scroll_pos);
            const float scale = std::max(0.55f, 1.0f - hdist * 0.18f);
            const int   size  = (int)(bigSize * scale);
            const Uint8 alpha = (Uint8)std::max(40.0f, 255.0f - hdist * 46.0f);
            const int   cx    = center_x + (int)((idx - m_scroll_pos) * spacing);
            const int   x     = cx - size / 2;
            const int   y     = center_y - size / 2;
            if (x + size < -40 || x > gfx::Gfx::Width + 40) return;

            DrawAppTile(m_items[idx], x, y, size, idx == m_cursor, alpha, true);
        };

        // Paint each side from the outside in, then the centre cover last, so the
        // enlarged selection always sits on top of its neighbours.
        for (int d = span; d >= 1; d--) { drawCover(-d); drawCover(+d); }
        drawCover(0);

        // Selected title name + position, centred beneath the strip.
        const MenuItem &sel = m_items[m_cursor];
        const int name_y = center_y + bigSize / 2 + 70;   // over the fading reflection
        m_gfx->TextCentered(FontSize::Large, center_x, name_y,
                            t.accent, Ellipsize(sel.name, gfx::Gfx::Width - 160, FontSize::Large).c_str());
        char pos[28];
        snprintf(pos, sizeof(pos), "%d / %d", m_cursor + 1, (int)m_items.size());
        // Position counters are optional; blanking the string here keeps
        // the layout arithmetic below untouched.
        if (!m_show_counter) pos[0] = '\0';
        m_gfx->TextCentered(FontSize::Small, center_x, name_y + 48, t.dim, pos);

        DrawStatusHint({ {{"a"}, "Select"}, {{"x"}, "Options"} });
    }
} // namespace sl::menu::ui
