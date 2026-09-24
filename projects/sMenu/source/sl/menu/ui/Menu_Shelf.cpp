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

    // Shelf mode: the Xbox 360 dashboard's game library (the 2008 "New Xbox
    // Experience"). The selected game stands large at the front left; the rest
    // of the row runs away into the distance to the right, each one further
    // back, smaller and dimmer, and the ones already passed slide out to the
    // left. Every tile stands on a glossy floor that reflects it.
    //
    // Drawn as 3D quads (Gfx::DrawQuad3D - on the GPU, perspective-correct)
    // with the camera Flow uses. Box art in vertical mode, the square icon
    // otherwise; system entries are their glyph on the theme's plate.
    void Menu::DrawMainShelf() {
        const Theme &t = m_theme.Current();
        m_icons.SetScale(0);
        DrawSelectionBackdrop();
        DrawTopBar(nullptr);

        if (m_items.empty()) { DrawMainEmpty(); return; }

        if (!ScrollBusy())   // a finger or a throw owns the scroll instead
            m_scroll_pos += (m_cursor - m_scroll_pos) * 0.22f;
        if (std::abs(m_cursor - m_scroll_pos) < 0.005f) m_scroll_pos = (float)m_cursor;

        const int total = (int)m_items.size();
        const bool tall = m_shelf_vertical;
        // Half extents in world units: a 2:3 case, or a square.
        const float hh = tall ? 0.66f : 0.52f;
        const float hw = tall ? hh * 2.0f / 3.0f : hh;

        // Where row position d (0 = selected, negative = already passed) sits.
        auto place = [&](float d, float &x, float &y, float &z, float &a) {
            if (d >= 0.0f) {
                x = -1.45f + d * 0.78f;
                y =  0.30f + d * 0.12f;
                z =  3.60f + d * 1.00f;
                a = std::max(0.0f, 1.0f - d * 0.16f);
            } else {                                   // sliding out to the left
                x = -1.45f + d * 1.90f;
                y =  0.30f;
                z =  3.60f + d * 0.30f;
                a = std::max(0.0f, 1.0f + d * 1.2f);
            }
        };

        const int centre = (int)lroundf(m_scroll_pos);
        int first = std::max(0, centre - 2), last = std::min(total - 1, centre + 7);
        // Far to near: the row recedes to the right, so the last is farthest.
        for (int idx = last; idx >= first; idx--) {
            const MenuItem &it = m_items[idx];
            const float d = (float)idx - m_scroll_pos;
            float x, y, z, a;
            place(d, x, y, z, a);
            if (a <= 0.01f) continue;
            const bool sel = (idx == m_cursor);

            const bool game = it.kind == ItemKind::Game, hb = it.kind == ItemKind::Homebrew;
            SDL_Texture *cov  = (tall && game) ? FlowCover(it) : nullptr;
            SDL_Texture *icon = game ? m_icons.Get(it.app_id)
                              : hb   ? m_hb_icons.Get(it.hb_icon)
                              : SystemIcon(it.kind);
            SDL_Texture *art  = cov ? cov : icon;
            const bool glyph  = !cov && icon && !game && !hb;

            const float quad[4][3] = { { x - hw, y + hh, z }, { x + hw, y + hh, z },
                                       { x + hw, y - hh, z }, { x - hw, y - hh, z } };
            // The floor under each tile, and the tile mirrored in it.
            const float floor_y = y - hh - 0.04f;
            const float refl[4][3] = { { x - hw, floor_y, z }, { x + hw, floor_y, z },
                                       { x + hw, floor_y - 2 * hh, z }, { x - hw, floor_y - 2 * hh, z } };

            const Uint8 lit = (Uint8)(150 + 105 * std::max(0.0f, 1.0f - std::abs(d)));
            const SDL_Color tint{ lit, lit, lit, 255 };
            const Uint8 A = (Uint8)(255 * a);

            auto face = [&](const float q[4][3], bool mirror, Uint8 top, Uint8 bottom) {
                if (!art || glyph) {                   // the plate behind a glyph
                    const SDL_Color plate = glyph ? IconPlate(t, 255) : WithAlpha(t.bg_bottom, 255);
                    SDL_Color pc = plate;
                    pc.a = 255;
                    m_gfx->DrawQuad3D(nullptr, q, pc, glyph ? (Uint8)(top * t.icon_bg_alpha / 255) : top,
                                      glyph ? (Uint8)(bottom * t.icon_bg_alpha / 255) : bottom, mirror, 4);
                }
                if (art) {
                    const SDL_Color c = glyph ? IconTint(t, 255) : tint;
                    m_gfx->DrawQuad3D(art, q, c, top, bottom, mirror, 12);
                }
            };
            face(refl, true, (Uint8)(A * 0.22f), 0);
            face(quad, false, A, A);

            if (!art) {                                // no picture at all: its initial
                float sx, sy;
                const float c[3] = { x, y, z };
                m_gfx->Project3D(c, sx, sy);
                char initial[2] = { it.name.empty() ? '?' : (char)toupper((unsigned char)it.name[0]), 0 };
                m_gfx->TextCentered(FontSize::Title, (int)sx, (int)sy - 26, WithAlpha(t.dim, A), initial);
            }
            if (sel) {                                 // the 360's bright frame
                float x0, y0, x1, y1;
                m_gfx->Project3D(quad[0], x0, y0);
                m_gfx->Project3D(quad[2], x1, y1);
                const SDL_Color f = WithAlpha(t.accent, (Uint8)(A * SelectionGlow()));
                const int ix0 = (int)x0 - 4, iy0 = (int)y0 - 4, iw = (int)(x1 - x0) + 8, ih = (int)(y1 - y0) + 8;
                m_gfx->FillRect(ix0, iy0, iw, 3, f);
                m_gfx->FillRect(ix0, iy0 + ih - 3, iw, 3, f);
                m_gfx->FillRect(ix0, iy0, 3, ih, f);
                m_gfx->FillRect(ix0 + iw - 3, iy0, 3, ih, f);
            }
        }

        // Release covers well outside the visible run. The shelf shares Flow's
        // cover cache, and leaving it unbounded is what previously starved the
        // rest of the menu of memory.
        if (tall) {
            const int keep_lo = std::max(0, first - 4);
            const int keep_hi = std::min(total - 1, last + 4);
            for (auto it2 = m_covers.begin(); it2 != m_covers.end(); ) {
                bool keep = false;
                for (int i = keep_lo; i <= keep_hi && !keep; i++)
                    keep = (m_items[i].app_id == it2->first);
                if (keep) { ++it2; continue; }
                if (it2->second) m_gfx->FreeImage(it2->second);
                it2 = m_covers.erase(it2);
            }
        }

        // Title and details under the selected game, where the 360 put them.
        const MenuItem &sel = m_items[m_cursor];
        float sx, sy;
        {
            const float foot[3] = { -1.45f - hw, 0.30f - hh, 3.60f };
            m_gfx->Project3D(foot, sx, sy);
        }
        const int tx = std::max(40, (int)sx);
        const int ty = (int)sy + (int)(hh * 2 * 900.0f / 3.6f * 0.30f) + 14;
        m_gfx->Text(FontSize::Large, tx, ty, t.title,
                    Ellipsize(sel.name, gfx::Gfx::Width - tx - 60, FontSize::Large).c_str());
        const int lh = m_gfx->LineHeight(FontSize::Large);
        std::string sub = sel.is_gamecard ? T("Game card")
                        : (sel.kind == ItemKind::Game ? T("Nintendo Switch") : "");
        if (sel.app_id == m_suspended && m_suspended != 0) {
            sub = T("Running");
        } else if (const play::PlayInfo *pi = Play(sel.app_id)) {
            if (pi->seconds > 0)
                sub += (sub.empty() ? "" : "   ") + play::FormatPlaytime(pi->seconds) + "   " +
                       play::FormatLastPlayed(pi->last_played);
        }
        if (!sub.empty())
            m_gfx->Text(FontSize::Small, tx, ty + lh + 2, t.dim, sub.c_str());

        // Position, top right, as the 360 counted its library.
        if (m_show_counter) {
            char cnt[32];
            snprintf(cnt, sizeof(cnt), "%d / %d", m_cursor + 1, total);
            const int cw = m_gfx->TextWidth(FontSize::Normal, cnt);
            m_gfx->Text(FontSize::Normal, gfx::Gfx::Width - 44 - cw, 70, t.dim, cnt);
        }
        m_gfx->Text(FontSize::Small,  44, 64, t.dim, T("sort"));
        m_gfx->Text(FontSize::Normal, 44, 82, t.fg,  SortLabel());

        if (tall && m_scroll_pos == (float)m_cursor) FetchArtFor(m_items[m_cursor]);
        DrawFetchStatus();
        DrawStatusHint({ {{"a"}, "Launch"}, {{"x"}, "Options"} });
    }
} // namespace sl::menu::ui
