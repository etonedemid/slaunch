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

    void Menu::DrawMainCover() {
        const Theme &t = m_theme.Current();
        m_icons.SetScale(0);   // one large cover -> original resolution
        DrawSelectionBackdrop();
        DrawTopBar(nullptr);

        if (m_items.empty()) { DrawMainEmpty(); return; }

        if (!ScrollBusy())   // a finger or a throw owns the scroll instead
            m_scroll_pos += (m_cursor - m_scroll_pos) * 0.28f;
        if (std::abs(m_cursor - m_scroll_pos) < 0.01f) m_scroll_pos = (float)m_cursor;

        const int cx = gfx::Gfx::Width / 2;
        const int cy = 290;
        const int size = 360;
        const int pageW = gfx::Gfx::Width;   // one cover per screen width
        const int total = (int)m_items.size();

        // The centred cover plus its immediate neighbours (only visible mid-slide).
        for (int off = -1; off <= 1; off++) {
            int idx = (int)lroundf(m_scroll_pos) + off;
            if (idx < 0 || idx >= total) continue;
            const int x = cx + (int)((idx - m_scroll_pos) * pageW);
            if (x < -size || x > gfx::Gfx::Width + size) continue;
            const float d = std::abs((float)idx - m_scroll_pos);
            const Uint8 a = (Uint8)std::max(60.0f, 255.0f - d * 160.0f);
            DrawAppTile(m_items[idx], x - size / 2, cy - size / 2, size, false, a, true);
        }

        // Left/right hint chevrons.
        if (total > 1) {
            m_gfx->Text(FontSize::Title, 44, cy - 26, WithAlpha(t.dim, 150), "<");
            const int rw = m_gfx->TextWidth(FontSize::Title, ">");
            m_gfx->Text(FontSize::Title, gfx::Gfx::Width - 44 - rw, cy - 26, WithAlpha(t.dim, 150), ">");
        }

        // Name + position of the centred item.
        const MenuItem &sel = m_items[m_cursor];
        m_gfx->TextCentered(FontSize::Large, cx, cy + size / 2 + 40, t.accent,
                            Ellipsize(sel.name, gfx::Gfx::Width - 200, FontSize::Large).c_str());
        char pos[28];
        snprintf(pos, sizeof(pos), "%d / %d", m_cursor + 1, total);
        // Position counters are optional; blanking the string here keeps
        // the layout arithmetic below untouched.
        if (!m_show_counter) pos[0] = '\0';
        m_gfx->TextCentered(FontSize::Small, cx, cy + size / 2 + 86, t.dim, pos);

        DrawStatusHint({ {{"a"}, "Launch"}, {{"x"}, "Options"}, {{"left","right"}, "Browse"} });
    }
} // namespace sl::menu::ui
