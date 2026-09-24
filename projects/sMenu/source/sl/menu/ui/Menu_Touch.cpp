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

    // Mirror of DrawKeyboard's layout for touch hit-testing.
    bool Menu::KbKeyAt(int x, int y, int &row, int &col) const {
        const int cx = gfx::Gfx::Width / 2;
        const int top = 250, rowH = 66, keyW = 66;

        // Character rows.
        for (int r = 0; r < 4; r++) {
            const int n    = (int)strlen(kKbRows[r]);
            const int rowW = n * keyW;
            const int x0   = cx - rowW / 2;
            const int ry   = top + r * rowH;
            if (y >= ry - 8 && y < ry + rowH - 8 && x >= x0 && x < x0 + rowW) {
                row = r; col = (x - x0) / keyW;
                if (col >= n) col = n - 1;
                return true;
            }
        }

        // Special row (Shift / Space / Back / Clear / Done).
        const int sy = top + 4 * rowH;
        const int sw = 176, gap = 12;
        const int totW = kKbSpecialCols * sw + (kKbSpecialCols - 1) * gap, sx0 = cx - totW / 2;
        if (y >= sy - 8 && y < sy + rowH - 8) {
            for (int c = 0; c < kKbSpecialCols; c++) {
                const int kx = sx0 + c * (sw + gap);
                if (x >= kx && x < kx + sw) { row = kKbSpecialRow; col = c; return true; }
            }
        }
        return false;
    }
    // Touch input.
    // - Keyboard: tap a key.
    // - Colour picker: tap/drag an R/G/B slider.
    // - Home screen (Main):
    //     * touch on widget -> grab & drag to move, release to drop
    //     * touch elsewhere + short tap -> select item under finger (or launch if already selected)
    //     * touch elsewhere + vertical/horizontal drag beyond threshold -> scroll the list/grid/line/shelf
    // - Submenus (Theming, Themes, etc.): tap on a row selects it.
    Menu::Action Menu::OnTouch(int phase, int x, int y, u64 &out_app_id) {
        out_app_id = 0;
        if (m_sd_removed) return Action::None;   // frozen: awaiting reboot

        // ---- universal "go back" corner, ahead of every screen-specific
        // branch below so it works the same on all of them, including the
        // ones (CoverPicker, Network) whose own touch handling - or lack of
        // one - has no other way out. Mirrors DrawBackTap's box exactly. ----
        if (m_screen != Screen::Main && !m_options_open && m_dialog == Dialog::None) {
            if (phase == 0 && x >= kBackTapX && x < kBackTapX + kBackTapW &&
                y >= kBackTapY && y < kBackTapY + kBackTapH) {
                return DispatchButton(Btn::B, out_app_id);
            }
        }

        // ---- on-screen keyboard: tap a key ----
        if (m_screen == Screen::Keyboard) {
            if (phase != 0) return Action::None;  // act on touch-down only
            int r, c;
            if (KbKeyAt(x, y, r, c)) {
                m_kb_row = r; m_kb_col = c;
                DispatchButton(Btn::A, out_app_id);   // press the key under the finger
            }
            return Action::None;
        }

        // ---- colour picker: tap / drag an R/G/B slider ----
        if (m_screen == Screen::ColorPicker) {
            if (phase == 2 || !m_pick_target) return Action::None;  // act on down + drag
            const int cx = gfx::Gfx::Width / 2;
            const int tx = cx - 300, tw = 520, sy = 360, rh = 68; // mirror DrawColorPicker
            Uint8 *ch[3] = { &m_pick_target->r, &m_pick_target->g, &m_pick_target->b };
            for (int i = 0; i < 3; i++) {
                const int ry = sy + i * rh;
                if (y >= ry - 20 && y < ry + 34 && x >= tx - 12 && x <= tx + tw + 12) {
                    m_pick_channel = i;
                    int v = (x - tx) * 255 / tw;
                    *ch[i] = (Uint8)(v < 0 ? 0 : v > 255 ? 255 : v);
                    // Only when the colour being edited IS a theme colour. The
                    // picker also edits tile colours now, and re-selecting the
                    // theme there switched the user's active theme to whichever
                    // one the editor last had open - the button path was gated
                    // for this and the touch path was not.
                    if (m_pick_preview) m_theme.Select(m_editing_theme);
                    break;
                }
            }
            return Action::None;
        }

        // ---- Deck: its screens are a grid, a panel and a reader, none of them
        // the row carousel the submenu path below hit-tests ----
        if ((m_screen == Screen::DeckLibrary || m_screen == Screen::DeckMenu ||
             m_screen == Screen::DeckNews) &&
            !m_options_open && m_dialog == Dialog::None) {
            if (phase != 0) return Action::None;      // act on touch-down only

            if (m_screen == Screen::DeckLibrary) {
                const int cell = DeckLibraryAt(x, y);
                if (cell < 0) return Action::None;
                if (cell == m_deck_lib_cursor)
                    return DispatchButton(Btn::A, out_app_id);
                m_deck_lib_cursor = cell;
                return Action::None;
            }
            if (m_screen == Screen::DeckMenu) {
                if (x > kDeckMenuW) { m_screen = Screen::Main; return Action::None; }
                std::vector<int> rows;
                rows.push_back(-1);
                for (int i = 0; i < (int)m_items.size(); i++)
                    if (FlowMenuItem(m_items[i])) rows.push_back(i);
                const int n = (int)rows.size();
                const int top = 106, rh = 54;
                const int visible = (gfx::Gfx::Height - top - 60) / rh;
                int first = 0;
                if (m_deck_menu_cursor >= visible) first = m_deck_menu_cursor - visible + 1;
                const int slot = (y - top + 6) / rh;
                const int idx  = first + slot;
                if (slot < 0 || slot >= visible || idx < 0 || idx >= n)
                    return Action::None;
                if (idx == m_deck_menu_cursor)
                    return DispatchButton(Btn::A, out_app_id);
                m_deck_menu_cursor = idx;
                return Action::None;
            }
            // Reader: the outer thirds page through the stories, as the Cover
            // layout's do.
            if (x < gfx::Gfx::Width / 3)      DispatchButton(Btn::Left, out_app_id);
            else if (x > gfx::Gfx::Width * 2 / 3) DispatchButton(Btn::Right, out_app_id);
            return Action::None;
        }

        // ---- submenus: tap to select a row ----
        if (m_screen != Screen::Main && !m_options_open && m_dialog == Dialog::None) {
            if (phase == 0 && m_screen == Screen::Music) { OnTouchMusic(x, y); return Action::None; }
            if (phase == 0 && m_screen == Screen::Album) { OnTouchAlbum(x, y); return Action::None; }
            if (phase == 0 && m_screen == Screen::Files) { OnTouchFiles(x, y); return Action::None; }
            if (phase == 0) {
                // Which cursor this screen drives, how many rows it has, and the
                // scroll value its carousel is actually animating. All three have
                // to come from the same place: a reference cannot be rebound, so
                // the old switch assigned each screen's cursor *into* the Theming
                // one and every submenu tap moved the Theming cursor instead.
                int *cursor = nullptr;
                int  rows   = 0;
                float scroll = m_sub_scroll;   // what DrawCarousel eases

                switch (m_screen) {
                    case Screen::Theming:
                        cursor = &m_theming_cursor;   rows = TH_Count; break;
                    case Screen::Themes:
                        cursor = &m_theme_cursor;     rows = m_theme.Count() + 1; break;
                    case Screen::ThemeEditor:
                        cursor = &m_edit_cursor;      rows = EF_Count;
                        scroll = m_edit_scroll;       break;
                    case Screen::Fonts:
                        cursor = &m_font_cursor;      rows = (int)m_font_names.size(); break;
                    case Screen::Widgets:
                        cursor = &m_widget_cursor;    rows = m_widgets.Count(); break;
                    case Screen::WidgetOptions:
                        cursor = &m_widgetopt_cursor;
                        if (widgets::IWidget *w = m_widgets.At(m_widget_sel)) rows = w->OptionCount();
                        break;
                    case Screen::Homebrew:
                        cursor = &m_hb_cursor;        rows = (int)m_hb.size(); break;
                    case Screen::SysEntries:
                        cursor = &m_sys_cursor;       rows = kSysEntryN; break;
                    case Screen::Network:
                        cursor = &m_net_cursor;       rows = NET_Count; break;
                    case Screen::Power:
                        cursor = &m_power_cursor;     rows = (int)m_power_rows.size(); break;
                    case Screen::Payloads:
                        cursor = &m_payload_cursor;   rows = (int)m_payloads.size() + 1; break;
                    default: break;   // About/Keyboard/Welcome: not a row list
                }
                if (!cursor || rows <= 0) return Action::None;

                // The editor hides rows (ribbon settings, blur radius) so screen
                // position maps to the visible list, not to the raw row ids.
                const float off = (float)(y - kListCenterY) / (float)kListSpacing;
                int target = (int)lroundf(scroll + off);
                if (m_screen == Screen::ThemeEditor) {
                    const Theme *c = m_theme.IsCustom(m_editing_theme)
                                   ? &m_theme.CustomAt(m_editing_theme) : nullptr;
                    if (!c) return Action::None;
                    int vis[EF_Count], n = 0;
                    for (int i = 0; i < EF_Count; i++) {
                        if ((IsRibbonRow(i) || IsFxColourRow(i)) && !StyleHasParams(c->background_style)) continue;
                        if (IsBlurRadiusRow(i) && !c->wallpaper_blur) continue;
                        vis[n++] = i;
                    }
                    if (n == 0) return Action::None;
                    if (target < 0) target = 0;
                    if (target >= n) target = n - 1;
                    m_edit_cursor = vis[target];
                    return Action::None;
                }

                if (target < 0) target = 0;
                if (target >= rows) target = rows - 1;
                *cursor = target;
            }
            return Action::None;
        }

        // ---- home screen ----
        if (m_screen != Screen::Main || m_options_open || m_dialog != Dialog::None) {
            m_touching = false; m_drag_active = false;
            m_touch_scroll_active = false;
            return Action::None;
        }

        // move: either drag a widget or scroll the list/grid/line/shelf
        if (phase == 1) {
            if (m_drag_active) {
                m_widgets.MoveBy(m_drag_widget, x - m_touch_lx, y - m_touch_ly);
                m_touch_lx = x; m_touch_ly = y;
                return Action::None;
            }

            // If not yet scrolling, check if the finger moved enough to start scrolling.
            const int dx_move = x - m_touch_start_x;
            const int dy_move = y - m_touch_start_y;
            const int move_dist_sq = dx_move * dx_move + dy_move * dy_move;
            constexpr int kScrollThresholdSq = 100; // ~10px threshold

            // Deck has no drag-scroll of its own - the cover row follows the
            // cursor and the tab/card strips are tapped, not thrown - so a
            // finger that merely jitters between down and up must never be
            // reclassified as a scroll here, or the phase==2 tap handling
            // below (which is what actually calls DeckTap) gets skipped.
            if (m_ui_mode != UiMode::Deck &&
                !m_touch_scroll_active && move_dist_sq >= kScrollThresholdSq) {
                m_touch_scroll_active = true;
            }

            // Touch scrolling: adjust scroll_pos based on drag distance.
            if (m_touch_scroll_active) {
                const int dx = x - m_touch_start_x;
                const int dy = y - m_touch_start_y;

                // A drag moves the content, not the cursor: the row (or column)
                // sticks to the finger and the selection rides along with it.
                //
                // Every layout places its entries at
                //     position = anchor + (index - scroll) * pitch
                // so the scroll value has to move *against* the drag for the
                // entries to follow it, and dividing by that same pitch is what
                // makes the movement track the finger one-to-one. Getting the
                // pitch wrong is as wrong as getting the sign wrong: the content
                // slides faster or slower than the finger holding it.
                const int last = (int)m_items.size() - 1;
                auto dragged = [&](int delta_px, int pitch) {
                    return m_touch_scroll_start - (float)delta_px / (float)pitch;
                };
                auto clamped = [](float p, int hi) {
                    if (p < 0) p = 0;
                    if (hi >= 0 && p > (float)hi) p = (float)hi;
                    return p;
                };

                // Speed is measured on whichever axis this layout scrolls, so
                // letting go can throw it. Blended rather than taken raw: the
                // last sample before a finger leaves the screen is often a
                // stutter or a tiny jitter, and handing that straight to the
                // fling either kills a genuine throw or launches the row off a
                // twitch.
                ScrollAxis ax = ActiveAxis();
                const float before = ax.pos ? *ax.pos : 0.0f;

                switch (m_ui_mode) {
                    case UiMode::Line:      // horizontal cover carousel
                        m_scroll_pos = clamped(dragged(dx, kLinePitch), last);
                        m_cursor = (int)lroundf(m_scroll_pos);
                        break;
                    case UiMode::Shelf:     // horizontal cover row
                        m_scroll_pos = clamped(dragged(dx, ShelfPitch()), last);
                        m_cursor = (int)lroundf(m_scroll_pos);
                        break;
                    case UiMode::Flow: {    // 3D coverflow
                        // Pitch is the on-screen distance between the centre
                        // cover and its neighbour. The row is perspective
                        // projected so that spacing shrinks toward the edges,
                        // but the finger is almost always over the middle, and
                        // matching it there is what makes the drag track.
                        // No clamp when the row is endless - dragging past the
                        // last game simply carries on into the first.
                        m_flow_scroll = m_wrap_nav
                                ? dragged(dx, kFlowPitchPx)
                                : clamped(dragged(dx, kFlowPitchPx),
                                          (int)m_flow_items.size() - 1);
                        SyncCursorFromScroll();
                        break;
                    }
                    case UiMode::XMB:
                        // The open column only. Categories are changed by tapping
                        // the bar, so a diagonal drag can never fight the column.
                        if (m_xmb_col >= 0 && m_xmb_col < (int)m_xmb_cols.size()) {
                            const int lastc = (int)m_xmb_cols[m_xmb_col].items.size() - 1;
                            m_xmb_item_scroll = clamped(dragged(dy, kXmbItemPitch), lastc);
                            m_xmb_item = (int)lroundf(m_xmb_item_scroll);
                            XmbApplyCursor();
                        }
                        break;
                    case UiMode::Grid:      // scrolls by whole rows
                        m_grid_scroll = clamped(dragged(dy, TilePitch()),
                                                TileMaxScroll());
                        break;
                    default:                // List and Cover
                        m_scroll_pos = clamped(dragged(dy, kListSpacing), last);
                        m_cursor = (int)lroundf(m_scroll_pos);
                        break;
                }

                if (ax.pos) {
                    const u64 t = armGetSystemTick();
                    if (m_drag_tick) {
                        const float sdt = (float)(t - m_drag_tick) /
                                          (float)armGetSystemTickFreq();
                        if (sdt > 0.001f) {
                            const float v = (*ax.pos - before) / sdt;
                            m_fling_vel = m_fling_vel * 0.65f + v * 0.35f;
                        }
                    }
                    m_drag_tick = t;
                }
            }

            m_touch_lx = x; m_touch_ly = y;
            return Action::None;
        }

        // up: drop widget or end touch scroll, possibly launching on tap
        if (phase == 2) {
            bool was_widget_drag = false;
            if (m_drag_active) {
                m_drag_active = false;
                m_widgets.SavePositions();
                was_widget_drag = true;
            }

            // If we were scrolling, commit the cursor position.
            bool was_scroll = m_touch_scroll_active;
            m_touch_scroll_active = false;
            m_drag_tick = 0;

            // Below this the finger was placing the row, not throwing it, and it
            // should settle where it was left instead of creeping on.
            if (std::abs(m_fling_vel) < kFlowFlingMin) m_fling_vel = 0.0f;

            if (!was_widget_drag && !was_scroll) {
                // XMB: a tap on the bar opens that category outright.
                if (m_ui_mode == UiMode::XMB) {
                    const int c = XmbColAt(x, y);
                    if (c >= 0) {
                        if (c != m_xmb_col) {
                            m_xmb_col = c; m_xmb_item = 0; m_xmb_item_scroll = 0.0f;
                            XmbApplyCursor();
                        }
                        m_touching = false; m_touch_widget = -1;
                        return Action::None;
                    }
                }
                // Deck: the tab row and the cards are its own, and are not
                // items in the list the shared hit-test walks.
                if (m_ui_mode == UiMode::Deck) {
                    Action da = Action::None;
                    if (DeckTap(x, y, out_app_id, da)) {
                        m_touching = false; m_touch_widget = -1;
                        return da;
                    }
                }
                // Short tap: select or launch item under finger.
                const int idx = MainItemAt(x, y);
                if (idx >= 0 && idx < (int)m_items.size()) {
                    if (idx == m_cursor) return DispatchButton(Btn::A, out_app_id);
                    m_cursor = idx;
                    if (m_ui_mode == UiMode::XMB) XmbSyncFromCursor();
                }
            }

            m_touching = false; m_touch_widget = -1;
            return Action::None;
        }

        // down: start widget drag or prepare for touch scroll / tap
        //
        // Catching a coasting row stops it where it is, the way a finger on a
        // spinning wheel does.
        m_fling_vel = 0.0f;
        m_drag_tick = 0;

        m_touching = true;
        m_touch_lx = x; m_touch_ly = y;
        m_touch_start_x = x; m_touch_start_y = y;
        // Each layout keeps its scroll position in its own member, and the drag
        // has to start from the one actually on screen. Flow was missing here
        // and fell through to m_scroll_pos, which belongs to the flat layouts -
        // so touching the shelf yanked it to wherever List or Shelf had last
        // been left, almost always the first cover.
        m_touch_scroll_start = (m_ui_mode == UiMode::Grid) ? m_grid_scroll
                             : (m_ui_mode == UiMode::XMB)  ? m_xmb_item_scroll
                             : (m_ui_mode == UiMode::Flow) ? m_flow_scroll
                                                           : m_scroll_pos;

        // Check for widget hit first.
        m_touch_widget = (m_deferred_joined && m_widgets.AnyEnabled())
                       ? m_widgets.HitTest(x, y) : -1;
        if (m_touch_widget >= 0) {
            m_drag_active = true; m_drag_widget = m_touch_widget;
            return Action::None;
        }

        // Not on a widget: start in "tap" mode. If the finger moves beyond a
        // threshold before lift, we switch to scrolling.
        m_touch_scroll_active = false;
        return Action::None;
    }
    int Menu::CoverPickAt(int x, int y) const {
        const int have = m_pick_have.load(std::memory_order_acquire);
        if (have <= 0) return -1;
        const int top = kPickTop - (int)(m_pick_scroll * (kPickCellH + kPickGapY));
        for (int i = 0; i < have; i++) {
            const int cx = PickCellX(i % kPickCols);
            const int cy = top + (i / kPickCols) * (kPickCellH + kPickGapY);
            if (x >= cx && x < cx + kPickCellW && y >= cy && y < cy + kPickCellH)
                return i;
        }
        return -1;
    }
    // Item index under a touch point in Grid mode (mirrors DrawMainGrid), or -1.
    // Entry under a touch point on the tile wall, or -1. Answered from the same
    // packed layout the renderer draws, so a tap always lands on what you see -
    // including the wide tiles, which no row/column arithmetic would cover.
    int Menu::GridItemAt(int px, int py) const {
        std::vector<TileRect> tiles;
        BuildTiles(tiles);
        const int scrollPx = (int)lroundf(m_grid_scroll * TilePitch());
        for (const TileRect &r : tiles) {
            const int y = r.y - scrollPx;
            if (px >= r.x && px < r.x + r.w && py >= y && py < y + r.h)
                return r.item;
        }
        return -1;   // in a gap, or off the wall
    }
    // Item index under a touch point in List mode (inverts the carousel), or -1.
    int Menu::ListItemAt(int /*px*/, int py) const {
        const int center_y = kListCenterY, spacing = kListSpacing;
        const int idx = (int)lroundf(m_scroll_pos + (float)(py - center_y) / spacing);
        return (idx >= 0 && idx < (int)m_items.size()) ? idx : -1;
    }
    // XMB: which category icon is under a touch point, or -1. Only the bar row
    // answers, so a tap on the column below never jumps categories.
    int Menu::XmbColAt(int x, int y) const {
        if (y < kXmbTabY - kXmbIcon || y > kXmbTabY + kXmbIcon) return -1;
        for (int c = 0; c < (int)m_xmb_cols.size(); c++) {
            const int cx = kXmbAnchorX + (int)((float)(c - m_xmb_col_scroll) * kXmbSpacingH);
            if (std::abs(x - cx) <= kXmbSpacingH / 2) return c;
        }
        return -1;
    }
    // XMB: which entry of the open column is under a touch point, or -1. Mirrors
    // the row placement in DrawMainXmb, and the whole row width is tappable.
    int Menu::XmbItemAt(int x, int y) const {
        (void)x;
        if (m_xmb_col < 0 || m_xmb_col >= (int)m_xmb_cols.size()) return -1;
        if (y < kXmbFadeEnd || y >= kXmbFadeBotEnd) return -1;
        const auto &items = m_xmb_cols[m_xmb_col].items;

        // Rows are not evenly spaced - the cursor row carries a wide band above
        // and below it - so the placement curve is walked rather than inverted,
        // taking the nearest centre. This is the same loop the renderer runs, so
        // a tap always lands on the row it visually hit.
        const int first = std::max(0, (int)m_xmb_item_scroll - kXmbAbove - 1);
        const int last  = std::min((int)items.size() - 1,
                                   (int)m_xmb_item_scroll + kXmbBelow + 1);
        int best = -1, best_d = 0;
        for (int i = first; i <= last; i++) {
            const int cy = kXmbMarginTop + kXmbIcon / 2
                         + (int)XmbRowOffset((float)i - m_xmb_item_scroll);
            // The band is a whole icon rather than a row pitch: the cursor row
            // is three pitches clear of the row under it, so a tighter band
            // would leave that gap dead to touch.
            const int d = std::abs(y - cy);
            if (d > kXmbIcon) continue;
            if (best < 0 || d < best_d) { best = i; best_d = d; }
        }
        if (best < 0) return -1;
        return items[best];
    }
    // Item index under a touch point, dispatched by the active layout.
    int Menu::MainItemAt(int x, int y) const {
        const int last = (int)m_items.size() - 1;
        if (m_ui_mode == UiMode::Grid) return GridItemAt(x, y);
        if (m_ui_mode == UiMode::XMB)  return XmbItemAt(x, y);
        if (m_ui_mode == UiMode::Flow) return FlowItemAt(x, y);
        if (m_ui_mode == UiMode::Deck) return DeckItemAt(x, y);
        if (m_ui_mode == UiMode::Line) {
            const int idx = (int)lroundf(m_scroll_pos +
                                         (float)(x - gfx::Gfx::Width / 2) / (float)kLinePitch);
            return (idx >= 0 && idx <= last) ? idx : -1;
        }
        if (m_ui_mode == UiMode::Shelf) {   // left-anchored uniform row (see DrawMainShelf)
            const int idx = (int)lroundf(m_scroll_pos + (float)(x - kShelfAnchorX) / ShelfPitch());
            return (idx >= 0 && idx <= last) ? idx : -1;
        }
        if (m_ui_mode == UiMode::Cover) {          // left/right thirds browse
            if (x < gfx::Gfx::Width / 3)       return (m_cursor > 0)    ? m_cursor - 1 : m_cursor;
            if (x > gfx::Gfx::Width * 2 / 3)   return (m_cursor < last) ? m_cursor + 1 : m_cursor;
            return m_cursor;                       // centre -> tap launches
        }
        return ListItemAt(x, y);
    }
} // namespace sl::menu::ui
