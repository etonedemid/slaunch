#include <sl/menu/ui/Menu.hpp>
#include <sl/menu/ui/Locale.hpp>
#include <sl/smi/Protocol.hpp>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <ctime>
#include <algorithm>
#include <dirent.h>
#include <sys/stat.h>

namespace sl::menu::ui {

    using gfx::FontSize;

    // Layout (1280x720)
    static constexpr int kTopBarH   = 56;
    static constexpr int kListX      = 120;
    static constexpr int kListTop    = 150;
    static constexpr int kRowH       = 54;
    static constexpr int kListW       = 1040;
    static constexpr int kHintY       = 682;

    static SDL_Color WithAlpha(SDL_Color c, Uint8 a) { return SDL_Color{ c.r, c.g, c.b, a }; }


    // Editor palette.
    static const SDL_Color kPalette[] = {
        {255,255,255,255},{200,200,200,255},{120,120,120,255},{  0,  0,  0,255},
        { 90,170,255,255},{ 60,120,220,255},{200,120,255,255},{160, 90,220,255},
        {255,190, 90,255},{255,140, 60,255},{255, 90, 90,255},{180,240,120,255},
        { 90,220,140,255},{ 90,210,210,255},{240,220,120,255},{ 30, 40, 70,255},
    };
    static constexpr int kPaletteCount = (int)(sizeof(kPalette) / sizeof(kPalette[0]));

    // =========================================================================
    void Menu::Init(gfx::Gfx *gfx, AccountUid user, u64 suspended_app_id, bool start_oobe) {
        m_gfx       = gfx;
        m_user      = user;
        m_suspended = suspended_app_id;
        m_icons.Init(gfx);
        LocaleInit();   // load the system-language locale (English is the fallback)

        // Resolve the user's nickname; if the launcher handed us an invalid uid,
        // fall back to the first account so the top bar isn't stuck on "Player".
        if (!accountUidIsValid(&m_user)) {
            s32 count = 0;
            AccountUid uids[ACC_USER_LIST_SIZE];
            if (R_SUCCEEDED(accountListAllUsers(uids, ACC_USER_LIST_SIZE, &count)) && count > 0)
                m_user = uids[0];
        }
        AccountProfile profile;
        AccountProfileBase base = {};
        if (accountUidIsValid(&m_user) && R_SUCCEEDED(accountGetProfile(&profile, m_user))) {
            accountProfileGet(&profile, nullptr, &base);
            accountProfileClose(&profile);
            if (base.nickname[0]) {
                strncpy(m_nickname, base.nickname, 32);
                m_nickname[32] = '\0';
            }
        }

        m_theme.Load();
        m_theme_cursor = m_theme.CurrentIndex();

        ScanFonts();
        LoadFontConfig();   // applies the saved font (or default)

        LoadFavourites();
        LoadSort();
        LoadSettings();
        LoadNames();
        // Widget loading (Lua parse + curl init) is deferred to InitDeferred so it
        // doesn't sit on the suspend->first-frame critical path.

        m_screen = start_oobe ? Screen::Oobe : Screen::Main;
        RebuildItems();
    }

    void Menu::InitDeferred() {
        if (m_deferred_done) return;
        m_deferred_done = true;
        m_widgets.Init();   // loads widget config, starts the network thread
    }

    Menu::~Menu() {
        // Stop the network thread before curl/sockets tear down.
        m_widgets.Exit();
        // Free textures while the renderer is still alive (main() runs gfx.Exit()
        // only after the Menu is destroyed).
        m_icons.Exit();
        for (auto &kv : m_sys_icons)
            if (kv.second) m_gfx->FreeImage(kv.second);
        m_sys_icons.clear();
    }

    void Menu::RebuildItems() {
        m_items.clear();

        // Split games into favourites and the rest, each ordered by sort mode.
        std::vector<MenuItem> favs, rest;
        for (auto &a : m_apps) {
            MenuItem it;
            it.kind = ItemKind::Game;
            it.app_id = a.app_id;
            const std::string *custom = CustomName(a.app_id);
            it.name = custom ? *custom : a.name;
            it.is_gamecard  = a.is_gamecard;
            it.is_favourite = IsFavourite(a.app_id);
            (it.is_favourite ? favs : rest).push_back(std::move(it));
        }
        auto sort_group = [this](std::vector<MenuItem> &v) {
            auto title = [](const MenuItem &x, const MenuItem &y) {
                return strcasecmp(x.name.c_str(), y.name.c_str());
            };
            switch (m_sort) {
                case SortMode::TitleAsc:
                    std::sort(v.begin(), v.end(),
                              [&](const MenuItem &x, const MenuItem &y){ return title(x, y) < 0; });
                    break;
                case SortMode::TitleDesc:
                    std::sort(v.begin(), v.end(),
                              [&](const MenuItem &x, const MenuItem &y){ return title(x, y) > 0; });
                    break;
                case SortMode::GamecardFirst:
                    // Physical carts grouped on top, each group A-Z.
                    std::sort(v.begin(), v.end(),
                              [&](const MenuItem &x, const MenuItem &y){
                                  if (x.is_gamecard != y.is_gamecard) return x.is_gamecard;
                                  return title(x, y) < 0;
                              });
                    break;
                default: break;   // Default: keep enumeration order
            }
        };
        sort_group(favs);
        sort_group(rest);

        // Favourites pinned at the very top for quick access.
        for (auto &it : favs) m_items.push_back(std::move(it));

        // System shortcuts.
        auto add = [&](ItemKind k, const char *name) {
            MenuItem it; it.kind = k; it.name = name; m_items.push_back(std::move(it));
        };
        add(ItemKind::Theming,      T("Theming"));
        add(ItemKind::Controllers,  T("Controllers"));
        add(ItemKind::Album,        T("Album"));
        add(ItemKind::UserPage,     T("User Page"));
        add(ItemKind::WebBrowser,   T("Web Browser"));
        add(ItemKind::MiiEdit,      T("Mii Edit"));
        add(ItemKind::Settings,     T("Settings"));
        add(ItemKind::Power,        T("Power"));
        add(ItemKind::HomebrewMenu, T("Homebrew menu"));

        // The remaining (non-favourite) games.
        for (auto &it : rest) m_items.push_back(std::move(it));

        if (m_cursor >= (int)m_items.size())
            m_cursor = m_items.empty() ? 0 : (int)m_items.size() - 1;
    }

    // ---- Favourites + sorting ----------------------------------------------
    bool Menu::IsFavourite(u64 app_id) const {
        return std::find(m_favourites.begin(), m_favourites.end(), app_id) != m_favourites.end();
    }

    void Menu::ToggleFavourite(u64 app_id) {
        auto it = std::find(m_favourites.begin(), m_favourites.end(), app_id);
        if (it != m_favourites.end()) m_favourites.erase(it);
        else                          m_favourites.push_back(app_id);
        SaveFavourites();
    }

    void Menu::LoadFavourites() {
        m_favourites.clear();
        FILE *fp = fopen("sdmc:/slaunch/config/favourites.txt", "r");
        if (!fp) return;
        char line[32];
        while (fgets(line, sizeof(line), fp)) {
            u64 id = strtoull(line, nullptr, 16);
            if (id) m_favourites.push_back(id);
        }
        fclose(fp);
    }

    void Menu::SaveFavourites() {
        mkdir("sdmc:/slaunch", 0777);
        mkdir("sdmc:/slaunch/config", 0777);
        FILE *fp = fopen("sdmc:/slaunch/config/favourites.txt", "w");
        if (!fp) return;
        for (u64 id : m_favourites) fprintf(fp, "%016llX\n", (unsigned long long)id);
        fclose(fp);
    }

    void Menu::LoadSort() {
        FILE *fp = fopen("sdmc:/slaunch/config/sort.txt", "r");
        if (!fp) return;
        int v = 0;
        if (fscanf(fp, "%d", &v) == 1 && v >= 0 && v < (int)SortMode::Count)
            m_sort = (SortMode)v;
        fclose(fp);
    }

    void Menu::SaveSort() {
        mkdir("sdmc:/slaunch", 0777);
        mkdir("sdmc:/slaunch/config", 0777);
        FILE *fp = fopen("sdmc:/slaunch/config/sort.txt", "w");
        if (!fp) return;
        fprintf(fp, "%d\n", (int)m_sort);
        fclose(fp);
    }

    bool Menu::SelectApp(u64 app_id) {
        for (int i = 0; i < (int)m_items.size(); i++) {
            if (m_items[i].kind == ItemKind::Game && m_items[i].app_id == app_id) {
                m_cursor = i;
                m_scroll_pos = (float)i;
                m_grid_scroll = (float)(i / GridColumns());
                return true;
            }
        }
        return false;
    }

    void Menu::SetApps(std::vector<AppEntry> apps) {
        m_apps = std::move(apps);
        RebuildItems();
        // Drop the cursor onto the suspended game so it's one button away. Only
        // mark it done once the jump actually lands - the game may not be in the
        // first (cached) list yet (e.g. a gamecard just inserted), and we want to
        // retry on the next SetApps once it shows up.
        if (m_suspended != 0 && !m_jumped_to_suspended && SelectApp(m_suspended))
            m_jumped_to_suspended = true;
    }
    void Menu::SetSuspendedApp(u64 app_id) { m_suspended = app_id; }
    void Menu::ClearSuspendedApp()          { m_suspended = 0; }

    // ---- Settings (text alignment) + custom names --------------------------
    void Menu::LoadSettings() {
        FILE *fp = fopen("sdmc:/slaunch/config/settings.txt", "r");
        if (!fp) return;
        char line[64];
        while (fgets(line, sizeof(line), fp)) {
            int v = 0;
            if (sscanf(line, "align=%d", &v) == 1 && v >= 0 && v <= 2)
                m_align = (TextAlign)v;
            else if (sscanf(line, "ui_mode=%d", &v) == 1 &&
                     v >= 0 && v < (int)UiMode::Count)
                m_ui_mode = (UiMode)v;
            else if (sscanf(line, "list_icons=%d", &v) == 1)
                m_list_icons = (v != 0);
        }
        fclose(fp);
    }

    void Menu::SaveSettings() {
        mkdir("sdmc:/slaunch", 0777);
        mkdir("sdmc:/slaunch/config", 0777);
        FILE *fp = fopen("sdmc:/slaunch/config/settings.txt", "w");
        if (!fp) return;
        fprintf(fp, "align=%d\n", (int)m_align);
        fprintf(fp, "ui_mode=%d\n", (int)m_ui_mode);
        fprintf(fp, "list_icons=%d\n", m_list_icons ? 1 : 0);
        fclose(fp);
    }

    void Menu::LoadNames() {
        m_names.clear();
        FILE *fp = fopen("sdmc:/slaunch/config/names.txt", "r");
        if (!fp) return;
        char line[160];
        while (fgets(line, sizeof(line), fp)) {
            line[strcspn(line, "\r\n")] = '\0';
            char *eq = strchr(line, '=');
            if (!eq) continue;
            *eq = '\0';
            u64 id = strtoull(line, nullptr, 16);
            if (id && eq[1]) m_names.emplace_back(id, std::string(eq + 1));
        }
        fclose(fp);
    }

    void Menu::SaveNames() {
        mkdir("sdmc:/slaunch", 0777);
        mkdir("sdmc:/slaunch/config", 0777);
        FILE *fp = fopen("sdmc:/slaunch/config/names.txt", "w");
        if (!fp) return;
        for (auto &n : m_names)
            fprintf(fp, "%016llX=%s\n", (unsigned long long)n.first, n.second.c_str());
        fclose(fp);
    }

    const std::string *Menu::CustomName(u64 app_id) const {
        for (auto &n : m_names)
            if (n.first == app_id) return &n.second;
        return nullptr;
    }

    void Menu::SetCustomName(u64 app_id, const char *name) {
        for (auto it = m_names.begin(); it != m_names.end(); ++it) {
            if (it->first == app_id) {
                if (name && name[0]) it->second = name;
                else                 m_names.erase(it); // empty clears the rename
                SaveNames();
                return;
            }
        }
        if (name && name[0]) m_names.emplace_back(app_id, std::string(name));
        SaveNames();
    }

    void Menu::RenameSelected() {
        if (m_items.empty()) return;
        const MenuItem &it = m_items[m_cursor];
        if (it.kind != ItemKind::Game) return;
        m_kb_purpose = sl::smi::Kb_RenameGame;
        m_kb_app = it.app_id;
        m_kb_text = it.name;
        m_kb_row = 0;
        m_kb_col = 0;
        m_kb_upper = false;
        m_screen = Screen::Keyboard;
    }

    // ---- On-screen keyboard (rename) ---------------------------------------
    namespace {
        // 4 character rows + a special bottom row (Shift/Space/Back/Done).
        const char *kKbRows[4] = {
            "1234567890",
            "qwertyuiop",
            "asdfghjkl",
            "zxcvbnm",
        };
        const char *kKbSpecial[4] = { "Shift", "Space", "Back", "Done" };
        constexpr int kKbSpecialRow = 4;

        int KbRowLen(int row) {
            if (row < 4) return (int)strlen(kKbRows[row]);
            return 4; // special row
        }
    }

    Menu::Action Menu::OnButtonKeyboard(Btn b) {
        auto commit = [&]() {
            switch (m_kb_purpose) {
                case sl::smi::Kb_WidgetOption:
                    if (widgets::IWidget *w = m_widgets.At((int)m_kb_app))
                        w->SetOption(m_kb_opt, m_kb_text);
                    SetStatus("Saved");
                    m_screen = Screen::WidgetOptions;
                    break;
                case sl::smi::Kb_ThemeName:
                    if (m_theme.IsCustom((int)m_kb_app) && !m_kb_text.empty()) {
                        Theme &c = m_theme.CustomAt((int)m_kb_app);
                        strncpy(c.name, m_kb_text.c_str(), sizeof(c.name) - 1);
                        c.name[sizeof(c.name) - 1] = '\0';
                        m_theme.Save();
                    }
                    SetStatus("Theme renamed");
                    m_screen = Screen::ThemeEditor;
                    break;
                default: // Kb_RenameGame
                    SetCustomName(m_kb_app, m_kb_text.c_str());
                    RebuildItems();
                    SelectApp(m_kb_app);
                    SetStatus("Renamed");
                    m_screen = Screen::Main;
                    break;
            }
        };
        auto backspace = [&]() {
            if (!m_kb_text.empty()) m_kb_text.pop_back();
        };

        if (b == Btn::Up)   { m_kb_row = (m_kb_row + 4) % 5; m_kb_col = std::min(m_kb_col, KbRowLen(m_kb_row) - 1); }
        if (b == Btn::Down) { m_kb_row = (m_kb_row + 1) % 5; m_kb_col = std::min(m_kb_col, KbRowLen(m_kb_row) - 1); }
        if (b == Btn::Left)  { int n = KbRowLen(m_kb_row); m_kb_col = (m_kb_col + n - 1) % n; }
        if (b == Btn::Right) { int n = KbRowLen(m_kb_row); m_kb_col = (m_kb_col + 1) % n; }

        if (b == Btn::A) {
            if (m_kb_row < 4) {
                char c = kKbRows[m_kb_row][m_kb_col];
                if (m_kb_upper && c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
                if (m_kb_text.size() < 60) m_kb_text.push_back(c);
            } else {
                switch (m_kb_col) {
                    case 0: m_kb_upper = !m_kb_upper; break;         // Shift
                    case 1: if (m_kb_text.size() < 60) m_kb_text.push_back(' '); break; // Space
                    case 2: backspace(); break;                     // Back
                    case 3: commit(); break;                        // Done
                }
            }
        }
        if (b == Btn::Y)    backspace();          // quick backspace
        if (b == Btn::X)    m_kb_upper = !m_kb_upper;
        if (b == Btn::Plus) commit();
        if (b == Btn::B) {
            m_screen = (m_kb_purpose == sl::smi::Kb_WidgetOption)
                       ? Screen::WidgetOptions :
                       (m_kb_purpose == sl::smi::Kb_ThemeName)
                       ? Screen::ThemeEditor : Screen::Main;
        }
        return Action::None;
    }

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

        // Special row (Shift / Space / Back / Done).
        const int sy = top + 4 * rowH;
        const int sw = 200, gap = 12, totW = 4 * sw + 3 * gap, sx0 = cx - totW / 2;
        if (y >= sy - 8 && y < sy + rowH - 8) {
            for (int c = 0; c < 4; c++) {
                const int kx = sx0 + c * (sw + gap);
                if (x >= kx && x < kx + sw) { row = kKbSpecialRow; col = c; return true; }
            }
        }
        return false;
    }

    // Touch input. On the keyboard a tap presses a key; in the colour picker a
    // tap/drag moves a slider; on the home screen a long-press (1.5s) on a widget
    // grabs it for dragging (drop on release) and a double-tap elsewhere launches
    // the item under the finger.
    Menu::Action Menu::OnTouch(int phase, int x, int y, u64 &out_app_id) {
        out_app_id = 0;

        // ---- on-screen keyboard: tap a key ----
        if (m_screen == Screen::Keyboard) {
            if (phase != 0) return Action::None;  // act on touch-down only
            int r, c;
            if (KbKeyAt(x, y, r, c)) {
                m_kb_row = r; m_kb_col = c;
                OnButtonKeyboard(Btn::A);         // press the key under the finger
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
                    m_theme.Select(m_editing_theme); // live preview
                    break;
                }
            }
            return Action::None;
        }

        // home screen: a touch on a widget grabs it (never the UI); a touch
        // elsewhere selects the item, or launches it if already selected; a touch
        // on nothing is ignored.
        if (m_screen != Screen::Main || m_options_open || m_dialog != Dialog::None) {
            m_touching = false; m_drag_active = false;
            return Action::None;
        }

        if (phase == 1) {                          // move: drag the held widget
            if (m_drag_active)
                m_widgets.MoveBy(m_drag_widget, x - m_touch_lx, y - m_touch_ly);
            m_touch_lx = x; m_touch_ly = y;
            return Action::None;
        }

        if (phase == 2) {                          // up: drop
            if (m_drag_active) {
                m_drag_active = false;
                m_widgets.SavePositions();
            }
            m_touching = false; m_touch_widget = -1;
            return Action::None;
        }

        // down
        m_touching = true;
        m_touch_lx = x; m_touch_ly = y;
        m_touch_widget = m_widgets.AnyEnabled() ? m_widgets.HitTest(x, y) : -1;
        if (m_touch_widget >= 0) {                  // grab it - drag to move, release to drop
            m_drag_active = true; m_drag_widget = m_touch_widget;
            return Action::None;
        }

        const int idx = MainItemAt(x, y);           // select, or launch if already selected
        if (idx >= 0 && idx < (int)m_items.size()) {
            if (idx == m_cursor) return OnButtonMain(Btn::A, out_app_id);
            m_cursor = idx;
        }
        return Action::None;
    }


    void Menu::SetUser(AccountUid uid, const char *nickname) {
        m_user = uid;
        strncpy(m_nickname, nickname, 32);
        m_nickname[32] = '\0';
    }

    void Menu::SetStatus(const char *msg) {
        strncpy(m_status, T(msg), 127);   // localized; unknown messages pass through
        m_status[127] = '\0';
        m_status_tick = armGetSystemTick();
    }

    // Ask the daemon to show the keyboard: write a request file, then flag the
    // applet to exit so qlaunch can display swkbd and hand the text back.
    // =========================================================================
    // Input
    Menu::Action Menu::OnButton(Btn b, u64 &out_app_id) {
        out_app_id = 0;
        if (m_options_open)          return OnButtonOptions(b, out_app_id);
        if (m_dialog != Dialog::None) return OnButtonDialog(b, out_app_id);
        switch (m_screen) {
            case Screen::Oobe:        return OnButtonOobe(b);
            case Screen::Main:        return OnButtonMain(b, out_app_id);
            case Screen::Theming:     return OnButtonTheming(b);
            case Screen::Themes:      return OnButtonThemes(b);
            case Screen::ThemeEditor: return OnButtonEditor(b);
            case Screen::ColorPicker: return OnButtonColorPicker(b);
            case Screen::Fonts:       return OnButtonFonts(b);
            case Screen::Widgets:       return OnButtonWidgets(b);
            case Screen::WidgetOptions: return OnButtonWidgetOptions(b);
            case Screen::Keyboard:    return OnButtonKeyboard(b);
        }
        return Action::None;
    }

    Menu::Action Menu::OnButtonOobe(Btn b) {
        constexpr int LastStep = 2;
        if (m_oobe_step == 1) {
            const int n = m_theme.Count();
            if (b == Btn::Down) { m_theme_cursor = (m_theme_cursor + 1) % n; m_theme.Select(m_theme_cursor); }
            if (b == Btn::Up)   { m_theme_cursor = (m_theme_cursor + n - 1) % n; m_theme.Select(m_theme_cursor); }
        }
        if (b == Btn::A) {
            if (m_oobe_step < LastStep) { m_oobe_step++; }
            else { m_theme.Save(); m_screen = Screen::Main; return Action::FinishSetup; }
        }
        if (b == Btn::B && m_oobe_step > 0) m_oobe_step--;
        return Action::None;
    }

    Menu::Action Menu::OnButtonMain(Btn b, u64 &out_app_id) {
        if (m_items.empty()) {
            if (b == Btn::Plus) return Action::OpenPower;
            return Action::None;
        }
        // Carousel navigation: the selected item is always centred, so we only
        // move the cursor. Scrolling stops at the ends; a *fresh* press (not an
        // auto-repeat) at an end wraps around, so holding the stick can't spin
        // the list endlessly.
        const int last = (int)m_items.size() - 1;
        auto step     = [&](int d) { m_cursor = std::min(std::max(0, m_cursor + d), last); };
        auto wrapNext = [&]() {
            if (m_cursor < last)    m_cursor++;
            else if (m_nav_fresh) { m_cursor = 0;    m_scroll_pos = 0.0f; }
        };
        auto wrapPrev = [&]() {
            if (m_cursor > 0)       m_cursor--;
            else if (m_nav_fresh) { m_cursor = last; m_scroll_pos = (float)last; }
        };

        // Navigation depends on the layout: the text List and the Line cover
        // carousel are 1-D; the Grid moves in two dimensions by whole rows.
        if (m_ui_mode == UiMode::Grid) {
            const int cols = GridColumns();
            if (b == Btn::Right) { wrapNext(); return Action::None; }
            if (b == Btn::Left)  { wrapPrev(); return Action::None; }
            if (b == Btn::Down)  { m_cursor = std::min(m_cursor + cols, last); return Action::None; }
            if (b == Btn::Up)    { if (m_cursor - cols >= 0) m_cursor -= cols; return Action::None; }
            if (b == Btn::R) { step(cols * 3);  return Action::None; }
            if (b == Btn::L) { step(-cols * 3); return Action::None; }
        } else if (m_ui_mode == UiMode::Line || m_ui_mode == UiMode::Cover ||
                   m_ui_mode == UiMode::Shelf) {
            if (b == Btn::Right || b == Btn::Down) { wrapNext(); return Action::None; }
            if (b == Btn::Left  || b == Btn::Up)   { wrapPrev(); return Action::None; }
            if (b == Btn::R) { step(5);  return Action::None; }
            if (b == Btn::L) { step(-5); return Action::None; }
        } else { // List (text carousel)
            if (b == Btn::Down) { wrapNext(); return Action::None; }
            if (b == Btn::Up)   { wrapPrev(); return Action::None; }
            if (b == Btn::R) { step(5);  return Action::None; }
            if (b == Btn::L) { step(-5); return Action::None; }
        }

        const MenuItem &it = m_items[m_cursor];

        if (b == Btn::A) {
            switch (it.kind) {
                case ItemKind::Game:
                    if (m_suspended != 0 && it.app_id == m_suspended) return Action::ResumeApp;
                    if (m_suspended != 0) {
                        m_pending_launch = it.app_id;
                        m_dialog_cursor  = 1;
                        m_dialog = Dialog::ConfirmCloseForLaunch;
                        return Action::None;
                    }
                    out_app_id = it.app_id;
                    return Action::LaunchApp;
                case ItemKind::Theming:
                    m_screen = Screen::Theming;
                    m_theming_cursor = 0;
                    m_sub_scroll = 0;
                    return Action::None;
                case ItemKind::Themes:
                    m_screen = Screen::Themes;
                    m_theme_cursor = m_theme.CurrentIndex();
                    return Action::None;
                case ItemKind::Fonts:
                    m_screen = Screen::Fonts;
                    m_font_cursor = m_font_applied;
                    return Action::None;
                case ItemKind::Album:        return Action::OpenAlbum;
                case ItemKind::UserPage:     return Action::OpenUserPage;
                case ItemKind::WebBrowser:   return Action::OpenWebBrowser;
                case ItemKind::MiiEdit:      return Action::OpenMiiEdit;
                case ItemKind::Controllers:  return Action::OpenControllers;
                case ItemKind::HomebrewMenu: return Action::OpenHomebrewMenu;
                case ItemKind::Settings:     return Action::OpenNetConnect;
                case ItemKind::Power:        return Action::OpenPower;
            }
        }
        if (b == Btn::X) {
            BuildOptions();
            m_options_cursor = 0;
            if (!m_options.empty()) m_options_open = true;
            return Action::None;
        }
        if (b == Btn::Plus) return Action::OpenPower;
        return Action::None;
    }

    // ---- X "Options" overlay ------------------------------------------------
    namespace { enum { OptFav = 0, OptRename, OptSort, OptCloseGame, OptDismiss }; }

    void Menu::BuildOptions() {
        m_options.clear();
        if (m_items.empty()) return;
        const MenuItem &it = m_items[m_cursor];
        if (it.kind == ItemKind::Game) {
            m_options.push_back({ IsFavourite(it.app_id) ? T("Remove from Favourites")
                                                         : T("Add to Favourites"), OptFav });
            m_options.push_back({ T("Rename"), OptRename });
            if (m_suspended != 0 && it.app_id == m_suspended)
                m_options.push_back({ T("Close game"), OptCloseGame });
        }
        m_options.push_back({ std::string(T("Sort: ")) + SortLabel(), OptSort });
        m_options.push_back({ T("Cancel"), OptDismiss });
    }

    const char *Menu::SortLabel() const {
        switch (m_sort) {
            case SortMode::TitleAsc:      return T("Title A - Z");
            case SortMode::TitleDesc:     return T("Title Z - A");
            case SortMode::GamecardFirst: return T("Game card first");
            default:                      return T("Default");
        }
    }

    Menu::Action Menu::OnButtonOptions(Btn b, u64 &out_app_id) {
        (void)out_app_id;
        int n = (int)m_options.size();
        if (n == 0) { m_options_open = false; return Action::None; }
        if (b == Btn::Down) { m_options_cursor = (m_options_cursor + 1) % n; return Action::None; }
        if (b == Btn::Up)   { m_options_cursor = (m_options_cursor + n - 1) % n; return Action::None; }
        if (b == Btn::B || b == Btn::X) { m_options_open = false; return Action::None; }
        if (b != Btn::A) return Action::None;

        // Capture the selection before RebuildItems can invalidate references.
        const MenuItem &sel = m_items[m_cursor];
        const bool sel_is_game = (sel.kind == ItemKind::Game);
        const u64  sel_id      = sel.app_id;

        switch (m_options[m_options_cursor].action) {
            case OptFav: {
                bool now_fav = !IsFavourite(sel_id);
                ToggleFavourite(sel_id);
                RebuildItems();
                SelectApp(sel_id);
                SetStatus(now_fav ? "Added to Favourites" : "Removed from Favourites");
                m_options_open = false;
                return Action::None;
            }
            case OptRename:
                m_options_open = false;
                RenameSelected();   // shows the software keyboard, then rebuilds
                return Action::None;
            case OptSort:
                m_sort = (SortMode)(((int)m_sort + 1) % (int)SortMode::Count);
                SaveSort();
                RebuildItems();
                if (sel_is_game) SelectApp(sel_id);
                BuildOptions(); // refresh the sort label, keep the menu open
                if (m_options_cursor >= (int)m_options.size())
                    m_options_cursor = (int)m_options.size() - 1;
                return Action::None;
            case OptCloseGame:
                m_options_open = false;
                return Action::TerminateApp;
            case OptDismiss:
            default:
                m_options_open = false;
                return Action::None;
        }
    }

    // ---- Theming submenu (appearance + widgets) ----------------------------
    namespace { enum { TH_Themes = 0, TH_Fonts, TH_TextPos, TH_UiMode, TH_ListIcons,
                       TH_Widgets, TH_Back, TH_Count }; }

    Menu::Action Menu::OnButtonTheming(Btn b) {
        auto cycleAlign = [&](int dir) {
            m_align = (TextAlign)(((int)m_align + dir + 3) % 3);
            SaveSettings();
        };
        auto cycleUiMode = [&](int dir) {
            const int n = (int)UiMode::Count;
            m_ui_mode = (UiMode)(((int)m_ui_mode + dir + n) % n);
            m_grid_hl_x = -1.0f; // re-prime the grid animation on (re)entry
            SaveSettings();
        };
        auto openWidgets = [&]() {
            m_screen = Screen::Widgets;
            m_widget_cursor = 0;
            m_sub_scroll = 0;
        };
        if (b == Btn::Down) m_theming_cursor = (m_theming_cursor + 1) % TH_Count;
        if (b == Btn::Up)   m_theming_cursor = (m_theming_cursor + TH_Count - 1) % TH_Count;
        if (m_theming_cursor == TH_TextPos) {
            if (b == Btn::Right) cycleAlign(+1);
            if (b == Btn::Left)  cycleAlign(-1);
        }
        if (m_theming_cursor == TH_UiMode) {
            if (b == Btn::Right) cycleUiMode(+1);
            if (b == Btn::Left)  cycleUiMode(-1);
        }
        auto toggleListIcons = [&]() { m_list_icons = !m_list_icons; SaveSettings(); };
        if (m_theming_cursor == TH_ListIcons && (b == Btn::Left || b == Btn::Right))
            toggleListIcons();
        if (b == Btn::A) {
            switch (m_theming_cursor) {
                case TH_Themes:  m_screen = Screen::Themes; m_theme_cursor = m_theme.CurrentIndex(); m_sub_scroll = m_theme_cursor; break;
                case TH_Fonts:   m_screen = Screen::Fonts;  m_font_cursor = m_font_applied; m_sub_scroll = m_font_cursor; break;
                case TH_TextPos: cycleAlign(+1); break;
                case TH_UiMode:  cycleUiMode(+1); break;
                case TH_ListIcons: toggleListIcons(); break;
                case TH_Widgets: openWidgets(); break;
                case TH_Back:    m_screen = Screen::Main; break;
            }
        }
        if (b == Btn::B) m_screen = Screen::Main;
        return Action::None;
    }

    // ---- Widgets submenu: list detected Lua widgets ------------------------
    Menu::Action Menu::OnButtonWidgets(Btn b) {
        const int n = m_widgets.Count();
        if (b == Btn::B) { m_screen = Screen::Theming; m_sub_scroll = TH_Widgets; return Action::None; }
        if (n == 0) return Action::None;
        if (b == Btn::Down) m_widget_cursor = (m_widget_cursor + 1) % n;
        if (b == Btn::Up)   m_widget_cursor = (m_widget_cursor + n - 1) % n;
        // Left/Right toggles whether the menu loads/shows this widget.
        if (b == Btn::Left || b == Btn::Right)
            m_widgets.SetEnabled(m_widget_cursor, !m_widgets.IsEnabled(m_widget_cursor));
        if (b == Btn::A) {   // A opens the widget's own options
            m_widget_sel = m_widget_cursor;
            m_widgetopt_cursor = 0;
            m_sub_scroll = 0;
            m_screen = Screen::WidgetOptions;
        }
        return Action::None;
    }

    // ---- Widget options: edit one widget's exposed variables ---------------
    Menu::Action Menu::OnButtonWidgetOptions(Btn b) {
        widgets::IWidget *w = m_widgets.At(m_widget_sel);
        const int n = w ? w->OptionCount() : 0;
        if (b == Btn::B) {
            m_screen = Screen::Widgets;
            m_sub_scroll = m_widget_cursor;
            return Action::None;
        }
        if (!w || n == 0) return Action::None;
        if (b == Btn::Down) m_widgetopt_cursor = (m_widgetopt_cursor + 1) % n;
        if (b == Btn::Up)   m_widgetopt_cursor = (m_widgetopt_cursor + n - 1) % n;

        const int oi = m_widgetopt_cursor;
        const bool isBool = (w->OptionType(oi) == "bool");

        auto toggleBool = [&]() {
            w->SetOption(oi, w->OptionValue(oi) == "1" ? "0" : "1");
        };

        if (isBool && (b == Btn::Left || b == Btn::Right)) toggleBool();
        if (b == Btn::A) {
            if (isBool) {
                toggleBool();
            } else {
                // Edit a string/int value on the software keyboard.
                m_kb_purpose = sl::smi::Kb_WidgetOption;
                m_kb_app  = (u64)m_widget_sel;
                m_kb_opt  = oi;
                m_kb_text = w->OptionValue(oi);
                m_kb_row = 0; m_kb_col = 0; m_kb_upper = false;
                m_screen = Screen::Keyboard;
            }
        }
        return Action::None;
    }

    Menu::Action Menu::OnButtonThemes(Btn b) {
        const int nThemes = m_theme.Count();
        const int listN   = nThemes + 1;
        const int newIdx  = nThemes; 

        auto openEditor = [&](int theme_idx) {
            m_editing_theme = theme_idx;
            m_theme.Select(theme_idx);
            m_theme_cursor = theme_idx;
            m_edit_cursor  = 0;
            ScanWallpapers();
            m_screen = Screen::ThemeEditor;
        };

        if (b == Btn::Down) { m_theme_cursor = (m_theme_cursor + 1) % listN; if (m_theme_cursor < nThemes) m_theme.Select(m_theme_cursor); }
        if (b == Btn::Up)   { m_theme_cursor = (m_theme_cursor + listN - 1) % listN; if (m_theme_cursor < nThemes) m_theme.Select(m_theme_cursor); }
        if (b == Btn::A) {
            if (m_theme_cursor == newIdx) {
                openEditor(m_theme.AddCustom());   
            } else {
                m_theme.Select(m_theme_cursor);
                m_theme.Save();
                SetStatus("Theme applied");
            }
        }
        if (b == Btn::Y && m_theme_cursor < nThemes && m_theme.IsCustom(m_theme_cursor))
            openEditor(m_theme_cursor);
        if (b == Btn::B) {
            m_theme.Load();
            m_theme_cursor = m_theme.CurrentIndex();
            m_screen = Screen::Theming;
            m_sub_scroll = m_theming_cursor;
        }
        return Action::None;
    }

    // Theme-editor rows: a background selector, six Color slots, then rename /
    // save / delete.
    namespace {
        enum { EF_Background = 0, EF_Top, EF_Bottom, EF_Text,
               EF_Accent, EF_Secondary, EF_Title, EF_Rename, EF_Save, EF_Delete, EF_Count };
    }

    static SDL_Color *EditorColor(Theme &c, int row) {
        switch (row) {
            case EF_Top:       return &c.bg_top;
            case EF_Bottom:    return &c.bg_bottom;
            case EF_Text:      return &c.fg;
            case EF_Accent:    return &c.accent;
            case EF_Secondary: return &c.dim;
            case EF_Title:     return &c.title;
            default:           return nullptr;
        }
    }

    void Menu::ScanWallpapers() {
        m_wallpapers.clear();
        // Accept images from either the documented themes folder or the slaunch
        // root, so wherever the user drops them works.
        const char *dirs[2] = { "sdmc:/slaunch/themes", "sdmc:/slaunch" };
        for (const char *dir : dirs) {
            DIR *d = opendir(dir);
            if (!d) continue;
            struct dirent *e;
            while ((e = readdir(d)) != nullptr) {
                const char *name = e->d_name;
                size_t len = strlen(name);
                if (len < 5) continue;
                const char *e4 = name + len - 4;
                const char *e5 = len >= 5 ? name + len - 5 : "";
                if (strcasecmp(e4, ".jpg") == 0 || strcasecmp(e4, ".png") == 0 ||
                    strcasecmp(e4, ".bmp") == 0 || strcasecmp(e5, ".jpeg") == 0)
                    m_wallpapers.push_back(std::string(dir) + "/" + name);
            }
            closedir(d);
        }
    }

    void Menu::CycleBackground(int dir) {
        if (!m_theme.IsCustom(m_editing_theme)) return;
        Theme &c = m_theme.CustomAt(m_editing_theme);
        const int n = (int)m_wallpapers.size();       // option 0 = Gradient
        int cur = 0;
        for (int i = 0; i < n; i++)
            if (m_wallpapers[i] == c.wallpaper) { cur = i + 1; break; }
        cur = (cur + dir + (n + 1)) % (n + 1);
        if (cur == 0) c.wallpaper[0] = '\0';
        else {
            strncpy(c.wallpaper, m_wallpapers[cur - 1].c_str(), sizeof(c.wallpaper) - 1);
            c.wallpaper[sizeof(c.wallpaper) - 1] = '\0';
        }
        m_theme.Select(m_editing_theme);
        m_theme_cursor    = m_editing_theme;
        m_wallpaper_theme = -1; // force the wallpaper cache to reload
    }

    void Menu::OpenColorPicker(SDL_Color *target) {
        m_pick_target   = target;
        m_pick_original = *target;
        m_pick_channel  = 0;
        m_screen        = Screen::ColorPicker;
    }

    Menu::Action Menu::OnButtonEditor(Btn b) {
        if (!m_theme.IsCustom(m_editing_theme)) { m_screen = Screen::Themes; return Action::None; }
        Theme &c = m_theme.CustomAt(m_editing_theme);

        if (b == Btn::Down) m_edit_cursor = (m_edit_cursor + 1) % EF_Count;
        if (b == Btn::Up)   m_edit_cursor = (m_edit_cursor + EF_Count - 1) % EF_Count;

        if (m_edit_cursor == EF_Background) {
            if (b == Btn::Right) CycleBackground(+1);
            if (b == Btn::Left)  CycleBackground(-1);
        }

        if (b == Btn::A) {
            if (m_edit_cursor == EF_Save) {
                m_theme.Select(m_editing_theme);
                m_theme.Save();
                SetStatus("Theme saved");
                m_theme_cursor = m_editing_theme;
                m_screen = Screen::Themes;
            } else if (m_edit_cursor == EF_Rename) {
                m_kb_purpose = sl::smi::Kb_ThemeName;
                m_kb_app = (u64)m_editing_theme;
                m_kb_text = c.name;
                m_kb_row = 0; m_kb_col = 0; m_kb_upper = false;
                m_screen = Screen::Keyboard;
            } else if (m_edit_cursor == EF_Delete) {
                m_theme.DeleteCustom(m_editing_theme);
                m_theme.Save();
                m_editing_theme = -1;
                m_theme_cursor = m_theme.CurrentIndex();
                SetStatus("Theme deleted");
                m_screen = Screen::Themes;
            } else if (SDL_Color *col = EditorColor(c, m_edit_cursor)) {
                OpenColorPicker(col);
            }
        }
        if (b == Btn::B) m_screen = Screen::Themes;
        return Action::None;
    }

    Menu::Action Menu::OnButtonColorPicker(Btn b) {
        if (!m_pick_target) { m_screen = Screen::ThemeEditor; return Action::None; }
        Uint8 *ch[3] = { &m_pick_target->r, &m_pick_target->g, &m_pick_target->b };

        if (b == Btn::Down) m_pick_channel = (m_pick_channel + 1) % 3;
        if (b == Btn::Up)   m_pick_channel = (m_pick_channel + 2) % 3;

        auto adjust = [&](int d) {
            int v = (int)*ch[m_pick_channel] + d;
            *ch[m_pick_channel] = (Uint8)(v < 0 ? 0 : v > 255 ? 255 : v);
            m_theme.Select(m_editing_theme); // live preview
        };
        if (b == Btn::Right) adjust(+1);
        if (b == Btn::Left)  adjust(-1);
        if (b == Btn::R)     adjust(+16); // shoulder buttons = coarse steps
        if (b == Btn::L)     adjust(-16);

        if (b == Btn::A) { m_pick_target = nullptr; m_screen = Screen::ThemeEditor; }
        if (b == Btn::B) { *m_pick_target = m_pick_original; m_pick_target = nullptr; m_screen = Screen::ThemeEditor; }
        return Action::None;
    }

    // ---- Fonts screen -------------------------------------------------------
    Menu::Action Menu::OnButtonFonts(Btn b) {
        int n = (int)m_font_names.size();
        if (n == 0) { if (b == Btn::B) m_screen = Screen::Theming; return Action::None; }

        if (b == Btn::Down) m_font_cursor = (m_font_cursor + 1) % n;
        if (b == Btn::Up)   m_font_cursor = (m_font_cursor + n - 1) % n;
        if (b == Btn::A) {
            ApplyFont(m_font_cursor);
            m_font_applied = m_font_cursor;
            SaveFontConfig();
            SetStatus("Font applied");
        }
        if (b == Btn::B) {
            // Revert any live preview back to the applied font, then leave.
            ApplyFont(m_font_applied);
            m_screen = Screen::Theming;
            m_sub_scroll = m_theming_cursor;
        }
        return Action::None;
    }

    // ---- Font management ----------------------------------------------------
    void Menu::ScanFonts() {
        m_font_names.clear();
        m_font_paths.clear();
        m_font_names.push_back("Default (System)");
        m_font_paths.push_back("");
        if (!g_sd_ok) return; // no SD -> system font only

        DIR *d = opendir("sdmc:/slaunch/fonts");
        if (d) {
            struct dirent *e;
            while ((e = readdir(d)) != nullptr) {
                const char *name = e->d_name;
                size_t len = strlen(name);
                if (len < 5) continue;
                const char *ext = name + len - 4;
                if (strcasecmp(ext, ".ttf") != 0 && strcasecmp(ext, ".otf") != 0) continue;

                std::string display(name, len - 4); // strip extension
                std::string path = std::string("sdmc:/slaunch/fonts/") + name;
                m_font_names.push_back(std::move(display));
                m_font_paths.push_back(std::move(path));
            }
            closedir(d);
        }
    }

    void Menu::ApplyFont(int index) {
        if (index <= 0 || index >= (int)m_font_paths.size()) {
            m_gfx->ClearContentFont();
            m_font_preview = 0;
            return;
        }
        if (!m_gfx->LoadContentFont(m_font_paths[index].c_str())) {
            // Failed to load - fall back to the system font.
            m_gfx->ClearContentFont();
            m_font_preview = 0;
            return;
        }
        m_font_preview = index;
    }

    void Menu::EnsurePreviewFont(int index) {
        if (index == m_font_preview) return;
        ApplyFont(index);
    }

    void Menu::LoadFontConfig() {
        m_font_applied = 0;
        if (!g_sd_ok) { ApplyFont(0); return; }
        FILE *fp = fopen("sdmc:/slaunch/config/font.cfg", "r");
        if (fp) {
            char line[160];
            if (fgets(line, sizeof(line), fp)) {
                line[strcspn(line, "\r\n")] = '\0';
                const char *val = line;
                if (strncmp(line, "font=", 5) == 0) val = line + 5;
                // Match saved value against a known display name.
                for (int i = 0; i < (int)m_font_names.size(); i++) {
                    if (m_font_names[i] == val) { m_font_applied = i; break; }
                }
            }
            fclose(fp);
        }
        ApplyFont(m_font_applied);
    }

    void Menu::SaveFontConfig() {
        if (!g_sd_ok) return;
        mkdir("sdmc:/slaunch", 0777);
        mkdir("sdmc:/slaunch/config", 0777);
        FILE *fp = fopen("sdmc:/slaunch/config/font.cfg", "w");
        if (!fp) return;
        const char *name = (m_font_applied >= 0 && m_font_applied < (int)m_font_names.size())
                           ? m_font_names[m_font_applied].c_str() : "Default (System)";
        fprintf(fp, "font=%s\n", name);
        fclose(fp);
    }

    Menu::Action Menu::OnButtonDialog(Btn b, u64 &out_app_id) {
        if (b == Btn::Up || b == Btn::Down) m_dialog_cursor ^= 1;
        if (b == Btn::B) { m_dialog = Dialog::None; m_pending_launch = 0; return Action::None; }
        if (b == Btn::A) {
            bool yes = (m_dialog_cursor == 0);
            Dialog which = m_dialog;
            m_dialog = Dialog::None;
            if (!yes) { m_pending_launch = 0; return Action::None; }
            if (which == Dialog::ConfirmCloseForLaunch) {
                out_app_id = m_pending_launch;
                m_pending_launch = 0;
                return Action::LaunchApp;
            }
            if (which == Dialog::ConfirmCloseGame) return Action::TerminateApp;
        }
        return Action::None;
    }

    // =========================================================================
    // Rendering
    void Menu::EnsureWallpaper() {
        int idx = m_theme.CurrentIndex();
        if (idx == m_wallpaper_theme) return;
        if (m_wallpaper) { m_gfx->FreeImage(m_wallpaper); m_wallpaper = nullptr; }
        const Theme &t = m_theme.Current();
        if (g_sd_ok && t.wallpaper[0])
            m_wallpaper = m_gfx->LoadImage(t.wallpaper); // nullptr if file missing
        m_wallpaper_theme = idx;
    }

    void Menu::DrawBackground() {
        const Theme &t = m_theme.Current();
        m_gfx->GradientV(t.bg_top, t.bg_bottom);
        EnsureWallpaper();
        if (m_wallpaper) {
            m_gfx->DrawCover(m_wallpaper, 255);
            // Darkening scrim so text stays legible over photos.
            m_gfx->FillRect(0, 0, gfx::Gfx::Width, gfx::Gfx::Height, SDL_Color{0,0,0,90});
        }
    }

    void Menu::DrawTopBar(const char *center_title) {
        const Theme &t = m_theme.Current();

        time_t now = time(nullptr);
        struct tm tm_now;
        localtime_r(&now, &tm_now);
        char clock[32];
        strftime(clock, sizeof(clock), "%H:%M   %a %b %d", &tm_now);
        m_gfx->Text(FontSize::Small, 40, 16, t.dim, clock);

        // Right side: nickname then battery, laid out by measured width so the
        // gap is even and the '%' never crowds the digits or the screen edge.
        u32 charge = 0;
        psmGetBatteryChargePercentage(&charge);
        char batt[16];
        snprintf(batt, sizeof(batt), "%lu%%", (unsigned long)charge);
        char name[24];
        snprintf(name, sizeof(name), "%.20s", m_nickname);

        int bw = m_gfx->TextWidth(FontSize::Small, batt);
        int nw = m_gfx->TextWidth(FontSize::Small, name);
        const int edge = 40, gap = 24;
        m_gfx->Text(FontSize::Small, gfx::Gfx::Width - edge - bw, 16, t.fg, batt);
        m_gfx->Text(FontSize::Small, gfx::Gfx::Width - edge - bw - gap - nw, 16, t.dim, name);

        // Localized when it's a known UI title; user data (theme/widget names)
        // passes through T() unchanged.
        if (center_title && center_title[0])
            m_gfx->TextCentered(FontSize::Title, gfx::Gfx::Width / 2, 26, t.title, T(center_title));

        (void)kTopBarH;
    }

    void Menu::DrawHint(const char *hint) {
        const Theme &t = m_theme.Current();
        m_gfx->TextCentered(FontSize::Small, gfx::Gfx::Width / 2, kHintY, t.dim, T(hint));
    }

    void Menu::DrawCarousel(const std::vector<std::string> &labels,
                            const std::vector<std::string> &values,
                            int cursor, float &scroll_pos) {
        const Theme &t = m_theme.Current();
        if (labels.empty()) return;

        scroll_pos += (cursor - scroll_pos) * 0.30f;
        if (std::abs(cursor - scroll_pos) < 0.01f) scroll_pos = (float)cursor;

        const int margin = kListX, center_y = 360, spacing = 48, span = 7;
        for (int off = -span; off <= span; off++) {
            const int idx = (int)lroundf(scroll_pos) + off;
            if (idx < 0 || idx >= (int)labels.size()) continue;

            const float vdist = std::abs((float)idx - scroll_pos);
            const bool  big   = vdist < 0.5f;
            const FontSize fs = big ? FontSize::Large : FontSize::Normal;
            const Uint8 alpha = (Uint8)std::max(24.0f, 255.0f - vdist * 52.0f);
            const int   lh    = m_gfx->LineHeight(fs);
            const int   y     = center_y + (int)((idx - scroll_pos) * spacing) - lh / 2;
            if (y < 90 || y > kHintY - 30) continue;

            const bool sel = (idx == cursor);
            const std::string &label = labels[idx];
            const int lw = m_gfx->TextWidth(fs, label.c_str());
            int tx;
            switch (m_align) {
                case TextAlign::Center: tx = (gfx::Gfx::Width - lw) / 2; break;
                case TextAlign::Right:  tx = gfx::Gfx::Width - margin - lw; break;
                default:                tx = margin; break;
            }

            if (sel)
                m_gfx->Text(FontSize::Large, tx - 34, y, WithAlpha(t.accent, alpha), ">");
            m_gfx->Text(fs, tx, y, WithAlpha(big ? t.accent : t.fg, alpha), label.c_str());

            if (idx < (int)values.size() && !values[idx].empty()) {
                const std::string &v = values[idx];
                const int vy = y + lh - m_gfx->LineHeight(FontSize::Small) - 2;
                const int vw = m_gfx->TextWidth(FontSize::Small, v.c_str());
                const int vx = (m_align == TextAlign::Right) ? (tx - vw - 18)
                                                             : (tx + lw + 18);
                m_gfx->Text(FontSize::Small, vx, vy, WithAlpha(t.accent, alpha), v.c_str());
            }
        }
    }

    void Menu::Render() {
        // The Fonts and Color-picker screens always render their chrome in the
        // default system font so they can never make themselves unreadable.
        m_gfx->UseDefaultFont(m_screen == Screen::Fonts || m_screen == Screen::ColorPicker ||
                              m_screen == Screen::Keyboard);

        DrawBackground();
        switch (m_screen) {
            case Screen::Oobe:        DrawOobe();   break;
            case Screen::Main:        DrawMain();   break;
            case Screen::Theming:     DrawTheming(); break;
            case Screen::Themes:      DrawThemes(); break;
            case Screen::ThemeEditor: DrawEditor(); break;
            case Screen::ColorPicker: DrawColorPicker(); break;
            case Screen::Fonts:       DrawFonts();  break;
            case Screen::Widgets:       DrawWidgets(); break;
            case Screen::WidgetOptions: DrawWidgetOptions(); break;
            case Screen::Keyboard:    DrawKeyboard(); break;
        }
        if (m_options_open) DrawOptions();
        if (m_dialog != Dialog::None) DrawDialog();
        m_gfx->Present();
    }

    void Menu::DrawOobe() {
        const Theme &t = m_theme.Current();
        int cx = gfx::Gfx::Width / 2;
        if (m_oobe_step == 0) {
            m_gfx->TextCentered(FontSize::Title, cx, 240, t.title, T("Welcome to sLaunch"));
            m_gfx->TextCentered(FontSize::Small, cx, 380, t.dim, T("Let's get you set up."));
            DrawHint("A: Continue");
        } else if (m_oobe_step == 1) {
            m_gfx->TextCentered(FontSize::Large, cx, 120, t.title, T("Choose a Theme"));
            int top = 260;
            for (int i = 0; i < m_theme.Count(); i++) {
                bool sel = (i == m_theme_cursor);
                int y = top + i * 52;
                if (sel) m_gfx->FillRect(cx - 200, y - 4, 400, 46, WithAlpha(t.accent, 40));
                m_gfx->TextCentered(FontSize::Normal, cx, y, sel ? t.accent : t.fg, m_theme.At(i).name);
            }
            DrawHint("A: Continue    B: Back");
        } else {
            m_gfx->TextCentered(FontSize::Title, cx, 260, t.title, T("All set!"));
            m_gfx->TextCentered(FontSize::Normal, cx, 350, t.fg, T("You're ready to go."));
            DrawHint("A: Finish");
        }
    }

    void Menu::DrawMainEmpty() {
        const Theme &t = m_theme.Current();
        m_gfx->TextCentered(FontSize::Normal, gfx::Gfx::Width / 2, 340, t.dim,
                            m_loading ? T("Loading games...") : T("No apps found"));
        DrawHint("+: Power");
    }

    void Menu::DrawMain() {
        switch (m_ui_mode) {
            case UiMode::Line:    DrawMainLine();    break;
            case UiMode::Grid:    DrawMainGrid();    break;
            case UiMode::Cover:   DrawMainCover();   break;
            case UiMode::Shelf:   DrawMainShelf();   break;
            default:              DrawMainList();    break;
        }

        // Widgets overlay every layout, drawn at their own (draggable) positions.
        if (m_widgets.AnyEnabled()) {
            const Theme &t = m_theme.Current();
            m_widgets.Render(m_gfx, t);

            // Accent outline around the widget being dragged, so it reads as "held".
            int bx, by, bw, bh;
            if (m_drag_active && m_widgets.GetBox(m_drag_widget, bx, by, bw, bh) && bh > 0) {
                const SDL_Color a = t.accent;
                m_gfx->FillRect(bx - 3, by - 3,  bw + 6, 3,      a);
                m_gfx->FillRect(bx - 3, by + bh, bw + 6, 3,      a);
                m_gfx->FillRect(bx - 3, by - 3,  3,      bh + 6, a);
                m_gfx->FillRect(bx + bw, by - 3, 3,      bh + 6, a);
            }
        }
    }

    void Menu::DrawMainList() {
        const Theme &t = m_theme.Current();
        m_icons.SetScale(gfx::IconCache::GridScale); // small thumbnails: downscaled
        DrawTopBar(nullptr);

        if (m_items.empty()) { DrawMainEmpty(); return; }

        // Niagara-style vertical carousel: the item at the vertical centre is
        // enlarged with a '>' cursor; neighbours shrink and fade with distance.
        // The whole list slides smoothly because m_scroll_pos eases toward the
        // integer cursor rather than snapping to it.
        m_scroll_pos += (m_cursor - m_scroll_pos) * 0.30f; // ease toward target
        if (std::abs(m_cursor - m_scroll_pos) < 0.01f) m_scroll_pos = (float)m_cursor;

        const int margin   = kListX;      // left/right margin for text
        const int center_y = 360;         // centre row's vertical centre
        const int spacing  = 48;          // gap between adjacent items
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
                SDL_Texture *ic = game ? m_icons.Get(it.app_id) : SystemIcon(it.kind);
                if (ic) {
                    const int isz = std::min(lh, 44);
                    const Uint8 ia = game ? alpha : (Uint8)(alpha * 195 / 255);
                    m_gfx->DrawImage(ic, 38, y + (lh - isz) / 2, isz, isz, ia);
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
            int pw = m_gfx->TextWidth(FontSize::Small, pos);
            m_gfx->Text(FontSize::Small, gfx::Gfx::Width - pw - 40, 120, t.dim, pos);
        }

        DrawStatusHint("A: Select    X: Options");
    }

    void Menu::DrawStatusHint(const char *hint) {
        const Theme &t = m_theme.Current();
        // Fresh status (< 3s) shows above the control hint, then fades out.
        if (m_status[0] != '\0') {
            u64 nowt = armGetSystemTick(), freq = armGetSystemTickFreq();
            if ((nowt - m_status_tick) < 3 * freq)
                m_gfx->TextCentered(FontSize::Normal, gfx::Gfx::Width / 2, kHintY - 44, t.accent, m_status);
            else
                m_status[0] = '\0';
        }
        DrawHint(hint);
    }

    // One square app/entry tile: the cached icon when present, otherwise a
    // themed placeholder card carrying the item's initial + name so system
    // entries (Theming, Album, ...) and icon-less titles still read clearly.
    // Load (and cache) the black/white icon drawn for a non-game menu entry.
    // Files live at sdmc:/slaunch/icons/<name>.png; missing files -> nullptr and
    // the tile falls back to its lettered placeholder.
    SDL_Texture *Menu::SystemIcon(ItemKind kind) {
        auto it = m_sys_icons.find((int)kind);
        if (it != m_sys_icons.end()) return it->second;

        const char *file = nullptr;
        switch (kind) {
            case ItemKind::Theming:      file = "theming";      break;
            case ItemKind::Controllers:  file = "controllers";  break;
            case ItemKind::Album:        file = "album";        break;
            case ItemKind::UserPage:     file = "user";         break;
            case ItemKind::WebBrowser:   file = "browser";      break;
            case ItemKind::MiiEdit:      file = "mii";          break;
            case ItemKind::Settings:     file = "settings";     break;
            case ItemKind::Power:        file = "power";        break;
            case ItemKind::HomebrewMenu: file = "homebrewmenu"; break;
            default: break;
        }
        SDL_Texture *tex = nullptr;
        if (file) {
            char path[64];
            snprintf(path, sizeof(path), "sdmc:/slaunch/icons/%s.png", file);
            tex = m_gfx->LoadImage(path);
            // Force alpha blending: an icon PNG with no alpha channel loads with
            // blend mode NONE, so our alpha-mod (the grey/transparency) is ignored.
            if (tex) SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
        }
        m_sys_icons[(int)kind] = tex; // cache even nullptr so we don't re-stat
        return tex;
    }

    // Item index under a touch point in Grid mode (mirrors DrawMainGrid), or -1.
    int Menu::GridItemAt(int px, int py) const {
        const int cols = GridColumns();
        const int tile = 120, gapX = 18, gapY = 18, pitch = tile + gapY, topBase = 80;
        const int gridW = cols * tile + (cols - 1) * gapX;
        const int startX = (gfx::Gfx::Width - gridW) / 2;
        const float scrollPx = m_grid_scroll * pitch;

        const int relx = px - startX;
        if (relx < 0) return -1;
        const int c = relx / (tile + gapX);
        if (c >= cols || (relx - c * (tile + gapX)) >= tile) return -1; // in the gap

        const float fy = (float)py - topBase + scrollPx;
        if (fy < 0) return -1;
        const int r = (int)(fy / pitch);
        if ((fy - r * pitch) >= tile) return -1;                       // in the gap

        const int idx = r * cols + c;
        return (idx >= 0 && idx < (int)m_items.size()) ? idx : -1;
    }

    // Item index under a touch point in List mode (inverts the carousel), or -1.
    int Menu::ListItemAt(int /*px*/, int py) const {
        const int center_y = 360, spacing = 48;
        const int idx = (int)lroundf(m_scroll_pos + (float)(py - center_y) / spacing);
        return (idx >= 0 && idx < (int)m_items.size()) ? idx : -1;
    }

    // Shelf mode geometry (Xbox-360 "My Games" style): uniform covers in a row,
    // the selected one anchored near the left. Shared by the draw + hit-test.
    namespace {
        constexpr int kShelfTile    = 208;   // cover edge
        constexpr int kShelfGap     = 20;
        constexpr int kShelfPitch   = kShelfTile + kShelfGap;
        constexpr int kShelfAnchorX = 88;    // left edge of the selected cover
        constexpr int kShelfTop     = 150;   // top edge of the cover row
    }

    // Item index under a touch point, dispatched by the active layout.
    int Menu::MainItemAt(int x, int y) const {
        const int last = (int)m_items.size() - 1;
        if (m_ui_mode == UiMode::Grid) return GridItemAt(x, y);
        if (m_ui_mode == UiMode::Line) {
            const int idx = (int)lroundf(m_scroll_pos + (float)(x - gfx::Gfx::Width / 2) / 210.0f);
            return (idx >= 0 && idx <= last) ? idx : -1;
        }
        if (m_ui_mode == UiMode::Shelf) {   // left-anchored uniform row (see DrawMainShelf)
            const int idx = (int)lroundf(m_scroll_pos + (float)(x - kShelfAnchorX) / kShelfPitch);
            return (idx >= 0 && idx <= last) ? idx : -1;
        }
        if (m_ui_mode == UiMode::Cover) {          // left/right thirds browse
            if (x < gfx::Gfx::Width / 3)       return (m_cursor > 0)    ? m_cursor - 1 : m_cursor;
            if (x > gfx::Gfx::Width * 2 / 3)   return (m_cursor < last) ? m_cursor + 1 : m_cursor;
            return m_cursor;                       // centre -> tap launches
        }
        return ListItemAt(x, y);
    }

    void Menu::DrawAppTile(const MenuItem &it, int x, int y, int size,
                           bool selected, Uint8 alpha) {
        const Theme &t = m_theme.Current();

        const bool isGame = (it.kind == ItemKind::Game);
        SDL_Texture *icon = isGame ? m_icons.Get(it.app_id) : SystemIcon(it.kind);

        if (icon) {
            // icon fills the tile; game icons opaque, system icons dimmed to grey
            const Uint8 ia = isGame ? alpha : (Uint8)(alpha * 195 / 255);
            m_gfx->DrawImage(icon, x, y, size, size, ia);
        } else {
            // no icon file: themed panel + big initial
            m_gfx->FillRect(x, y, size, size, WithAlpha(t.bg_bottom, (Uint8)(alpha * 180 / 255)));
            char initial[2] = { it.name.empty() ? '?' : it.name[0], '\0' };
            if (initial[0] >= 'a' && initial[0] <= 'z') initial[0] -= 32;
            const int iw = m_gfx->TextWidth(FontSize::Title, initial);
            m_gfx->Text(FontSize::Title, x + (size - iw) / 2, y + size / 2 - 26,
                        WithAlpha(t.dim, alpha), initial);
        }

        m_gfx->FillRect(x, y, size, 3, WithAlpha(t.accent, alpha)); // accent strip, same on every tile

        const bool running = (it.kind == ItemKind::Game &&
                              it.app_id == m_suspended && m_suspended != 0);
        if (running)
            m_gfx->FillRect(x + size - 20, y + 10, 10, 10, WithAlpha(t.accent, alpha));
        if (it.is_favourite)
            m_gfx->Text(FontSize::Small, x + 8, y + 6, WithAlpha(t.accent, alpha), "*");

        if (selected) {                             // selection frame - thin edges, don't cover the icon
            const SDL_Color a = t.accent;
            m_gfx->FillRect(x - 4,        y - 4,        size + 8, 4,        a);
            m_gfx->FillRect(x - 4,        y + size,     size + 8, 4,        a);
            m_gfx->FillRect(x - 4,        y - 4,        4,        size + 8, a);
            m_gfx->FillRect(x + size,     y - 4,        4,        size + 8, a);
        }
    }

    // Line mode: a horizontal cover carousel (EmulationStation style). The
    // selected cover sits centred and full-size; neighbours shrink and fade with
    // distance, and the whole strip eases toward the cursor.
    void Menu::DrawMainLine() {
        const Theme &t = m_theme.Current();
        m_icons.SetScale(0);   // few large covers -> original resolution (crisp)
        DrawTopBar(nullptr);

        if (m_items.empty()) { DrawMainEmpty(); return; }

        m_scroll_pos += (m_cursor - m_scroll_pos) * 0.30f;
        if (std::abs(m_cursor - m_scroll_pos) < 0.01f) m_scroll_pos = (float)m_cursor;

        const int center_x = gfx::Gfx::Width / 2;
        const int center_y = 348;         // vertical centre of the covers
        const int bigSize  = 240;         // selected cover edge length
        const int spacing  = 210;         // horizontal gap between cover centres
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

            DrawAppTile(m_items[idx], x, y, size, idx == m_cursor, alpha);
        };

        // Paint each side from the outside in, then the centre cover last, so the
        // enlarged selection always sits on top of its neighbours.
        for (int d = span; d >= 1; d--) { drawCover(-d); drawCover(+d); }
        drawCover(0);

        // Selected title name + position, centred beneath the strip.
        const MenuItem &sel = m_items[m_cursor];
        m_gfx->TextCentered(FontSize::Large, center_x, center_y + bigSize / 2 + 26,
                            t.accent, sel.name.c_str());
        char pos[28];
        snprintf(pos, sizeof(pos), "%d / %d", m_cursor + 1, (int)m_items.size());
        m_gfx->TextCentered(FontSize::Small, center_x, center_y + bigSize / 2 + 74, t.dim, pos);

        DrawStatusHint("A: Select    X: Options");
    }

    // Grid mode: a page of icon tiles. The page scrolls smoothly (an eased row
    // offset) and the selection is a highlight frame that glides to the cursor,
    // so both axes animate instead of snapping. Tiles fade as they cross the top
    // and bottom edges of the viewport.
    void Menu::DrawMainGrid() {
        const Theme &t = m_theme.Current();
        m_icons.SetScale(gfx::IconCache::GridScale); // many tiles: downscaled for perf
        DrawTopBar(nullptr);

        if (m_items.empty()) { DrawMainEmpty(); return; }

        // 8 columns x 4 visible rows. The name text is hidden, so tiles reach
        // further down. Keep these in sync with GridItemAt (touch hit-testing).
        const int cols     = GridColumns();               // 8
        const int tile     = 120;
        const int gapX     = 18;
        const int gapY     = 18;
        const int pitch    = tile + gapY;                 // vertical distance between rows
        const int rowsVis  = 4;                           // rows fully on screen
        const int gridW    = cols * tile + (cols - 1) * gapX;
        const int startX   = (gfx::Gfx::Width - gridW) / 2;
        const int topBase  = 80;                          // y of row 0 at scroll 0
        const int total    = (int)m_items.size();
        const int rows     = (total + cols - 1) / cols;

        const int cur_row = m_cursor / cols;
        const int cur_col = m_cursor % cols;

        // --- ease the vertical scroll (keep the selected row one row from the top
        // when possible, clamped so we never scroll past the last page). ---
        const float maxScroll = (float)std::max(0, rows - rowsVis);
        float target = (float)std::min(std::max(0, cur_row - 1), (int)maxScroll);
        m_grid_scroll += (target - m_grid_scroll) * 0.30f;
        if (std::abs(target - m_grid_scroll) < 0.01f) m_grid_scroll = target;
        const float scrollPx = m_grid_scroll * pitch;

        // Viewport band used for edge fading (tiles above/below fade out).
        const int bandTop = topBase;
        const int bandBot = topBase + (rowsVis - 1) * pitch + tile;

        // Draw a couple of extra rows beyond the visible band so scrolling reveals
        // tiles gradually rather than popping them in.
        const int firstR = std::max(0, (int)std::floor(m_grid_scroll) - 1);
        const int lastR  = std::min(rows - 1, firstR + rowsVis + 2);
        for (int r = firstR; r <= lastR; r++) {
            const int y = topBase + (int)lroundf(r * pitch - scrollPx);
            Uint8 alpha = 255;
            if (y < bandTop)               alpha = (Uint8)std::max(0, 255 - (bandTop - y) * 4);
            else if (y + tile > bandBot)   alpha = (Uint8)std::max(0, 255 - (y + tile - bandBot) * 4);
            if (alpha <= 8) continue;
            for (int c = 0; c < cols; c++) {
                const int idx = r * cols + c;
                if (idx >= total) break;
                const int x = startX + c * (tile + gapX);
                DrawAppTile(m_items[idx], x, y, tile, /*selected*/ false, alpha);
            }
        }

        // --- glide the selection highlight toward the selected tile. ---
        const float tHLx = (float)(startX + cur_col * (tile + gapX));
        const float tHLy = (float)topBase + (float)cur_row * pitch - scrollPx;
        if (m_grid_hl_x < 0.0f) {                 // first frame: snap into place
            m_grid_hl_x = tHLx; m_grid_hl_y = tHLy;
        } else {
            m_grid_hl_x += (tHLx - m_grid_hl_x) * 0.35f;
            m_grid_hl_y += (tHLy - m_grid_hl_y) * 0.35f;
        }
        const int hx = (int)lroundf(m_grid_hl_x), hy = (int)lroundf(m_grid_hl_y);
        const SDL_Color a = t.accent;
        m_gfx->FillRect(hx - 4,        hy - 4,    tile + 8, 4,        a);
        m_gfx->FillRect(hx - 4,        hy + tile, tile + 8, 4,        a);
        m_gfx->FillRect(hx - 4,        hy - 4,    4,        tile + 8, a);
        m_gfx->FillRect(hx + tile,     hy - 4,    4,        tile + 8, a);

        // Position indicator only (name text hidden by request).
        char pos[28];
        snprintf(pos, sizeof(pos), "%d / %d", m_cursor + 1, total);
        int pw = m_gfx->TextWidth(FontSize::Small, pos);
        m_gfx->Text(FontSize::Small, gfx::Gfx::Width - pw - 40, 120, t.dim, pos);

        DrawStatusHint("A: Select    X: Options");
    }

    // Cover mode: a fullscreen single-cover pager. One large cover is centred;
    // browsing slides it out one screen-width while the next slides in.
    void Menu::DrawMainCover() {
        const Theme &t = m_theme.Current();
        m_icons.SetScale(0);   // one large cover -> original resolution
        DrawTopBar(nullptr);

        if (m_items.empty()) { DrawMainEmpty(); return; }

        m_scroll_pos += (m_cursor - m_scroll_pos) * 0.28f;
        if (std::abs(m_cursor - m_scroll_pos) < 0.01f) m_scroll_pos = (float)m_cursor;

        const int cx = gfx::Gfx::Width / 2;
        const int cy = 344;
        const int size = 420;
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
            DrawAppTile(m_items[idx], x - size / 2, cy - size / 2, size, false, a);
        }

        // Left/right hint chevrons.
        if (total > 1) {
            m_gfx->Text(FontSize::Title, 44, cy - 26, WithAlpha(t.dim, 150), "<");
            const int rw = m_gfx->TextWidth(FontSize::Title, ">");
            m_gfx->Text(FontSize::Title, gfx::Gfx::Width - 44 - rw, cy - 26, WithAlpha(t.dim, 150), ">");
        }

        // Name + position of the centred item.
        const MenuItem &sel = m_items[m_cursor];
        m_gfx->TextCentered(FontSize::Large, cx, cy + size / 2 + 22, t.accent, sel.name.c_str());
        char pos[28];
        snprintf(pos, sizeof(pos), "%d / %d", m_cursor + 1, total);
        m_gfx->TextCentered(FontSize::Small, cx, cy + size / 2 + 68, t.dim, pos);

        DrawStatusHint("A: Launch    X: Options    Left/Right: Browse");
    }

    // Shelf mode: an Xbox-360 "My Games" style row of uniform covers. The selected
    // cover is anchored near the left inside a highlight card that shows its name
    // and platform; the rest of the row scrolls behind it. Unselected covers carry
    // a small caption underneath.
    void Menu::DrawMainShelf() {
        const Theme &t = m_theme.Current();
        m_icons.SetScale(0);
        DrawTopBar(nullptr);

        if (m_items.empty()) { DrawMainEmpty(); return; }

        m_scroll_pos += (m_cursor - m_scroll_pos) * 0.30f;
        if (std::abs(m_cursor - m_scroll_pos) < 0.01f) m_scroll_pos = (float)m_cursor;

        const int total = (int)m_items.size();
        const int tile  = kShelfTile;
        const int top   = kShelfTop;
        const int pitch = kShelfPitch;

        // Fit text into a pixel width, trailing "..." if it overflows.
        auto ellipsize = [&](std::string s, int maxw, gfx::FontSize fs) {
            if (m_gfx->TextWidth(fs, s.c_str()) <= maxw) return s;
            while (!s.empty() && m_gfx->TextWidth(fs, (s + "...").c_str()) > maxw)
                s.pop_back();
            return s + "...";
        };

        // Header row below the top bar: sort on the left, position on the right.
        m_gfx->Text(FontSize::Small,  kShelfAnchorX, 64, t.dim, T("sort"));
        m_gfx->Text(FontSize::Normal, kShelfAnchorX, 82, t.fg,  SortLabel());
        {
            char cnt[32];
            snprintf(cnt, sizeof(cnt), "%d / %d", m_cursor + 1, total);
            const int cw = m_gfx->TextWidth(FontSize::Large, cnt);
            m_gfx->Text(FontSize::Large, gfx::Gfx::Width - 44 - cw, 70, t.dim, cnt);
        }

        // Highlight card behind the anchored (selected) cover.
        const int pad   = 14;
        const int infoH = 104;
        m_gfx->FillRect(kShelfAnchorX - pad, top - pad,
                        tile + pad * 2, tile + pad + infoH, WithAlpha(t.fg, 20));

        // Covers, painted right-to-left so the selected one lands on top of its
        // neighbours during a slide.
        int firstv = (int)m_scroll_pos - 1;
        if (firstv < 0) firstv = 0;
        int lastv = (int)m_scroll_pos + (gfx::Gfx::Width - kShelfAnchorX) / pitch + 2;
        if (lastv > total - 1) lastv = total - 1;
        for (int idx = lastv; idx >= firstv; idx--) {
            const int x = kShelfAnchorX + (int)((idx - m_scroll_pos) * pitch);
            if (x + tile < 0 || x > gfx::Gfx::Width) continue;
            const bool selg = (idx == m_cursor);
            DrawAppTile(m_items[idx], x, top, tile, selg, selg ? 255 : 225);
            if (!selg)
                m_gfx->Text(FontSize::Small, x, top + tile + 12, t.dim,
                            ellipsize(m_items[idx].name, tile, FontSize::Small).c_str());
        }

        // Selected item's info block inside the card.
        const MenuItem &sel = m_items[m_cursor];
        m_gfx->Text(FontSize::Normal, kShelfAnchorX, top + tile + 12, t.title,
                    ellipsize(sel.name, tile, FontSize::Normal).c_str());
        const char *sub = sel.is_gamecard ? T("Game card")
                        : (sel.kind == ItemKind::Game ? T("Nintendo Switch") : "");
        if (sub[0])
            m_gfx->Text(FontSize::Small, kShelfAnchorX, top + tile + 54, t.dim, sub);
        if (sel.app_id == m_suspended && m_suspended != 0)
            m_gfx->Text(FontSize::Small, kShelfAnchorX, top + tile + 76, t.accent, T("Running"));

        DrawStatusHint("A: Launch    X: Options    Left/Right: Browse");
    }

    void Menu::DrawOptions() {
        const Theme &t = m_theme.Current();
        const int n = (int)m_options.size();
        if (n == 0) return;

        // Dim the menu behind the panel.
        m_gfx->FillRect(0, 0, gfx::Gfx::Width, gfx::Gfx::Height, SDL_Color{0, 0, 0, 150});

        const int rowH   = 62;
        const int panelW = 560;
        const int panelH = rowH * n + 40;
        const int px = (gfx::Gfx::Width  - panelW) / 2;
        const int py = (gfx::Gfx::Height - panelH) / 2;

        m_gfx->FillRect(px, py, panelW, panelH, WithAlpha(t.bg_bottom, 245));
        m_gfx->FillRect(px, py, panelW, 4, t.accent);

        for (int i = 0; i < n; i++) {
            const bool sel = (i == m_options_cursor);
            const int  ry  = py + 26 + i * rowH;
            if (sel) m_gfx->FillRect(px + 14, ry - 8, panelW - 28, rowH - 10, WithAlpha(t.accent, 60));
            m_gfx->Text(FontSize::Normal, px + 36, ry, sel ? t.accent : t.fg,
                        m_options[i].label.c_str());
        }
    }

    void Menu::DrawTheming() {
        DrawTopBar("Theming");
        const char *aligns[3] = { T("Left"), T("Center"), T("Right") };
        const char *modes[5]  = { T("List"), T("Line"), T("Grid"), T("Cover"), T("Shelf") };
        std::vector<std::string> labels = {
            T("Themes"), T("Fonts"), T("Text position"), T("UI mode"), T("List icons"),
            T("Widgets"), T("Back")
        };
        std::vector<std::string> values(labels.size());
        values[TH_TextPos]     = aligns[(int)m_align];
        values[TH_UiMode]      = modes[(int)m_ui_mode];
        values[TH_ListIcons]   = m_list_icons ? T("On") : T("Off");
        {
            char c[32];
            snprintf(c, sizeof(c), "%d %s", m_widgets.Count(), T("found"));
            values[TH_Widgets] = c;
        }

        DrawCarousel(labels, values, m_theming_cursor, m_sub_scroll);
        DrawHint("Up/Down: Select   A: Open/Toggle   Left/Right: Change   B: Back");
    }

    // ---- Widgets submenu drawing -------------------------------------------
    void Menu::DrawWidgets() {
        const Theme &t = m_theme.Current();
        DrawTopBar("Widgets");

        const int n = m_widgets.Count();
        if (n == 0) {
            m_gfx->TextCentered(FontSize::Normal, gfx::Gfx::Width / 2, 320, t.dim,
                                T("No widgets found"));
            m_gfx->TextCentered(FontSize::Small, gfx::Gfx::Width / 2, 372, t.dim,
                                T("Drop .lua widgets in sdmc:/slaunch/widgets/"));
            DrawHint("B: Back");
            return;
        }

        std::vector<std::string> labels, values;
        for (int i = 0; i < n; i++) {
            widgets::IWidget *w = m_widgets.At(i);
            labels.push_back(w ? w->Name() : "Widget");
            values.push_back(m_widgets.IsEnabled(i) ? T("On") : T("Off"));
        }
        DrawCarousel(labels, values, m_widget_cursor, m_sub_scroll);
        DrawHint("Left/Right: On/Off   A: Configure   B: Back");
    }

    void Menu::DrawWidgetOptions() {
        const Theme &t = m_theme.Current();
        widgets::IWidget *w = m_widgets.At(m_widget_sel);
        DrawTopBar(w ? w->Name().c_str() : "Widget");

        const int n = w ? w->OptionCount() : 0;
        if (n == 0) {
            m_gfx->TextCentered(FontSize::Normal, gfx::Gfx::Width / 2, 340, t.dim,
                                T("This widget has no options"));
            DrawHint("B: Back");
            return;
        }

        std::vector<std::string> labels, values;
        for (int i = 0; i < n; i++) {
            labels.push_back(w->OptionLabel(i));
            if (w->OptionType(i) == "bool") {
                values.push_back(w->OptionValue(i) == "1" ? T("On") : T("Off"));
            } else {
                std::string v = w->OptionValue(i);
                values.push_back(v.empty() ? T("(not set)") : v);
            }
        }
        DrawCarousel(labels, values, m_widgetopt_cursor, m_sub_scroll);
        DrawHint("A: Edit/Toggle   Left/Right: Toggle   B: Back");
    }

    void Menu::DrawThemes() {
        DrawTopBar("Themes");
        std::vector<std::string> labels, values;
        for (int i = 0; i < m_theme.Count(); i++) {
            labels.push_back(m_theme.At(i).name);
            values.push_back(i == m_theme.CurrentIndex() ? T("current") : "");
        }
        labels.push_back(T("+ New custom theme"));
        values.push_back("");
        DrawCarousel(labels, values, m_theme_cursor, m_sub_scroll);

        if (m_theme_cursor == m_theme.Count())
            DrawHint("A: Create new theme    B: Back");
        else if (m_theme.IsCustom(m_theme_cursor))
            DrawHint("A: Apply    Y: Edit    B: Back");
        else
            DrawHint("A: Apply    B: Back");
    }

    void Menu::DrawEditor() {
        const Theme &t = m_theme.Current(); // the edited theme, shown live
        if (!m_theme.IsCustom(m_editing_theme)) return;
        Theme &c = m_theme.CustomAt(m_editing_theme);
        DrawTopBar(c.name);

        const char *labels[EF_Count] = {
            T("Background"), T("Gradient top"), T("Gradient bottom"), T("Text"),
            T("Accent"), T("Secondary"), T("Title"), T("Rename theme"),
            T("Save & Apply"), T("Delete theme")
        };

        const int lx = 200, vx = 720, rowH = 50, top = 132;
        for (int i = 0; i < EF_Count; i++) {
            const bool sel = (i == m_edit_cursor);
            const int  y   = top + i * rowH;
            const SDL_Color rc = (i == EF_Delete) ? SDL_Color{235, 90, 90, 255}
                                                  : (sel ? t.accent : t.fg);
            if (sel) m_gfx->FillRect(lx - 30, y - 6, 900, rowH - 6, WithAlpha(t.accent, 46));
            m_gfx->Text(FontSize::Normal, lx, y, rc, labels[i]);

            if (SDL_Color *col = EditorColor(c, i)) {
                m_gfx->FillRect(vx, y, 60, 32, *col);
                char hex[16];
                snprintf(hex, sizeof(hex), "#%02X%02X%02X", col->r, col->g, col->b);
                m_gfx->Text(FontSize::Small, vx + 78, y + 4, t.dim, hex);
            } else if (i == EF_Background) {
                const char *slash = c.wallpaper[0] ? strrchr(c.wallpaper, '/') : nullptr;
                const char *bg = (c.wallpaper[0] == '\0') ? T("Gradient")
                                : (slash ? slash + 1 : c.wallpaper);
                m_gfx->Text(FontSize::Small, vx - 40, y + 4, t.dim, "<");
                m_gfx->Text(FontSize::Normal, vx, y, t.fg, bg);
            }
        }

        // Contextual hint for the current row.
        if (m_edit_cursor == EF_Background)
            DrawHint("Up/Down: Row    Left/Right: Change background    B: Back");
        else if (m_edit_cursor == EF_Save)
            DrawHint("Up/Down: Row    A: Save & apply    B: Back");
        else if (m_edit_cursor == EF_Rename)
            DrawHint("Up/Down: Row    A: Rename    B: Back");
        else if (m_edit_cursor == EF_Delete)
            DrawHint("Up/Down: Row    A: Delete this theme    B: Back");
        else
            DrawHint("Up/Down: Row    A: Edit color    B: Back");
    }

    void Menu::DrawColorPicker() {
        if (!m_pick_target) return;
        const int cx = gfx::Gfx::Width / 2;

        // Fixed, always-readable chrome (independent of the Color being edited).
        const SDL_Color white{240, 240, 240, 255}, dim{150, 150, 155, 255},
                        accent{90, 200, 255, 255}, track{50, 50, 58, 255};
        m_gfx->FillRect(0, 0, gfx::Gfx::Width, gfx::Gfx::Height, SDL_Color{12, 12, 16, 255});

        m_gfx->TextCentered(FontSize::Title, cx, 70, white, T("Color"));

        // Live swatch + hex.
        m_gfx->FillRect(cx - 130, 150, 260, 110, *m_pick_target);
        char hex[16];
        snprintf(hex, sizeof(hex), "#%02X%02X%02X",
                 m_pick_target->r, m_pick_target->g, m_pick_target->b);
        m_gfx->TextCentered(FontSize::Large, cx, 280, white, hex);

        const char *names[3] = { "R", "G", "B" };
        const Uint8 vals[3]  = { m_pick_target->r, m_pick_target->g, m_pick_target->b };
        const SDL_Color chc[3] = {{255,90,90,255},{90,220,110,255},{100,150,255,255}};

        const int tx = cx - 300, tw = 520, sy = 360, rh = 68;
        for (int i = 0; i < 3; i++) {
            const bool sel = (i == m_pick_channel);
            const int  y   = sy + i * rh;
            m_gfx->Text(FontSize::Large, tx - 60, y - 8, sel ? accent : white, names[i]);
            m_gfx->FillRect(tx, y, tw, 10, track);
            m_gfx->FillRect(tx, y, tw * vals[i] / 255, 10, chc[i]);
            m_gfx->FillRect(tx + tw * vals[i] / 255 - 5, y - 8, 10, 26, sel ? accent : white);
            char vb[8]; snprintf(vb, sizeof(vb), "%d", vals[i]);
            m_gfx->Text(FontSize::Normal, tx + tw + 30, y - 10, sel ? accent : white, vb);
        }

        m_gfx->TextCentered(FontSize::Small, cx, kHintY, dim,
            "A: Done   B: Cancel");
    }

    void Menu::DrawFonts() {
        const Theme &t = m_theme.Current();
        DrawTopBar("Fonts");

        std::vector<std::string> labels, values;
        for (int i = 0; i < (int)m_font_names.size(); i++) {
            labels.push_back(m_font_names[i]);
            values.push_back(i == m_font_applied ? T("applied") : "");
        }
        DrawCarousel(labels, values, m_font_cursor, m_sub_scroll);

        // Live preview of the highlighted font, drawn IN that font at the bottom.
        EnsurePreviewFont(m_font_cursor);
        m_gfx->UseDefaultFont(false);
        m_gfx->TextCentered(FontSize::Large, gfx::Gfx::Width / 2, kHintY - 66, t.fg,
                            "The quick brown fox 0123");
        m_gfx->UseDefaultFont(true);

        DrawHint("Up/Down: Preview    A: Apply    B: Back");
    }

    void Menu::DrawKeyboard() {
        const Theme &t = m_theme.Current();
        const int cx = gfx::Gfx::Width / 2;
        DrawTopBar("Rename");

        // Current text in an input box.
        m_gfx->FillRect(cx - 400, 130, 800, 60, WithAlpha(t.fg, 24));
        m_gfx->FillRect(cx - 400, 186, 800, 3, t.accent);
        std::string shown = m_kb_text.empty() ? T("(empty)") : m_kb_text;
        m_gfx->Text(FontSize::Large, cx - 384, 142,
                    m_kb_text.empty() ? t.dim : t.fg, shown.c_str());

        // Key grid.
        const int top = 250, rowH = 66, keyW = 66, cxKeys = cx;
        for (int r = 0; r < 4; r++) {
            const int n = (int)strlen(kKbRows[r]);
            const int rowW = n * keyW;
            const int x0 = cxKeys - rowW / 2;
            const int y  = top + r * rowH;
            for (int c = 0; c < n; c++) {
                const bool sel = (m_kb_row == r && m_kb_col == c);
                const int kx = x0 + c * keyW;
                if (sel) m_gfx->FillRect(kx + 4, y - 4, keyW - 8, rowH - 10, WithAlpha(t.accent, 70));
                char ch = kKbRows[r][c];
                if (m_kb_upper && ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
                char s[2] = { ch, 0 };
                m_gfx->Text(FontSize::Large, kx + keyW / 2 - 8, y, sel ? t.accent : t.fg, s);
            }
        }

        // Special row.
        const int y = top + 4 * rowH;
        const int sw = 200, gap = 12, totW = 4 * sw + 3 * gap, x0 = cx - totW / 2;
        for (int c = 0; c < 4; c++) {
            const bool sel = (m_kb_row == kKbSpecialRow && m_kb_col == c);
            const int kx = x0 + c * (sw + gap);
            SDL_Color fill = (c == 0 && m_kb_upper) ? t.accent : t.fg;
            m_gfx->FillRect(kx, y - 4, sw, rowH - 12, WithAlpha(fill, sel ? 90 : 26));
            m_gfx->TextCentered(FontSize::Normal, kx + sw / 2, y + 4,
                                sel ? t.accent : t.fg, T(kKbSpecial[c]));
        }

        DrawHint("A: Type   X: Shift   Y: Backspace   +: Done   B: Cancel");
    }

    void Menu::DrawDialog() {
        const Theme &t = m_theme.Current();
        // Dim the whole screen, then draw a centered box.
        m_gfx->FillRect(0, 0, gfx::Gfx::Width, gfx::Gfx::Height, SDL_Color{0,0,0,150});
        int cx = gfx::Gfx::Width / 2;
        int bw = 560, bh = 260;
        int bx = cx - bw / 2, by = gfx::Gfx::Height / 2 - bh / 2;
        m_gfx->FillRect(bx, by, bw, bh, WithAlpha(t.bg_bottom, 245));
        m_gfx->FillRect(bx, by, bw, 4, t.accent);

        m_gfx->TextCentered(FontSize::Large, cx, by + 40, t.title, T("Close running application?"));
        const char *opts[2] = { T("Yes"), T("No") };
        for (int i = 0; i < 2; i++) {
            bool sel = (i == m_dialog_cursor);
            int y = by + 120 + i * 48;
            if (sel) m_gfx->FillRect(cx - 90, y - 4, 180, 42, WithAlpha(t.accent, 60));
            m_gfx->TextCentered(FontSize::Normal, cx, y, sel ? t.accent : t.fg, opts[i]);
        }
        DrawHint("Up/Down: Choose    A: Confirm    B: Cancel");
    }

} // namespace sl::menu::ui



