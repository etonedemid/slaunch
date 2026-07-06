#include <sl/menu/ui/Menu.hpp>
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
        m_widgets.Init();   // loads widget config, starts the network thread

        m_screen = start_oobe ? Screen::Oobe : Screen::Main;
        RebuildItems();
    }

    Menu::~Menu() {
        // Stop the widget network thread before the process (and sockets) tear
        // down.
        m_widgets.Exit();
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
        if (m_sort == SortMode::Alpha) {
            auto by_alpha = [](const MenuItem &x, const MenuItem &y) {
                return strcasecmp(x.name.c_str(), y.name.c_str()) < 0;
            };
            std::sort(favs.begin(), favs.end(), by_alpha);
            std::sort(rest.begin(), rest.end(), by_alpha);
        }

        // Favourites pinned at the very top for quick access.
        for (auto &it : favs) m_items.push_back(std::move(it));

        // System shortcuts.
        auto add = [&](ItemKind k, const char *name) {
            MenuItem it; it.kind = k; it.name = name; m_items.push_back(std::move(it));
        };
        add(ItemKind::Theming,      "Theming");
        add(ItemKind::Controllers,  "Controllers");
        add(ItemKind::Album,        "Album");
        add(ItemKind::UserPage,     "User Page");
        add(ItemKind::WebBrowser,   "Web Browser");
        add(ItemKind::MiiEdit,      "Mii Edit");
        add(ItemKind::Settings,     "Settings");
        add(ItemKind::Power,        "Power");
        add(ItemKind::HomebrewMenu, "Homebrew menu");

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

    void Menu::SelectApp(u64 app_id) {
        for (int i = 0; i < (int)m_items.size(); i++) {
            if (m_items[i].kind == ItemKind::Game && m_items[i].app_id == app_id) {
                m_cursor = i;
                m_scroll_pos = (float)i;
                return;
            }
        }
    }

    void Menu::SetApps(std::vector<AppEntry> apps) {
        m_apps = std::move(apps);
        RebuildItems();
        // On first load, drop the cursor onto the suspended game so it's one
        // button away.
        if (m_suspended != 0 && !m_jumped_to_suspended) {
            SelectApp(m_suspended);
            m_jumped_to_suspended = true;
        }
    }
    void Menu::SetSuspendedApp(u64 app_id) { m_suspended = app_id; }
    void Menu::ClearSuspendedApp()          { m_suspended = 0; }

    // ---- Settings (text alignment) + custom names --------------------------
    void Menu::LoadSettings() {
        FILE *fp = fopen("sdmc:/slaunch/config/settings.txt", "r");
        if (!fp) return;
        int v = 0;
        if (fscanf(fp, "align=%d", &v) == 1 && v >= 0 && v <= 2)
            m_align = (TextAlign)v;
        fclose(fp);
    }

    void Menu::SaveSettings() {
        mkdir("sdmc:/slaunch", 0777);
        mkdir("sdmc:/slaunch/config", 0777);
        FILE *fp = fopen("sdmc:/slaunch/config/settings.txt", "w");
        if (!fp) return;
        fprintf(fp, "align=%d\n", (int)m_align);
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
        m_kb_purpose = KbPurpose::RenameGame;
        m_kb_app   = it.app_id;
        m_kb_text  = it.name;
        m_kb_row   = 0;
        m_kb_col   = 0;
        m_kb_upper = false;
        m_screen   = Screen::Keyboard;
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
            if (m_kb_purpose == KbPurpose::WeatherCity) {
                m_widgets.SetWeatherCity(m_kb_text.c_str());
                SetStatus("Location set");
                m_screen = Screen::Theming;
            } else {
                SetCustomName(m_kb_app, m_kb_text.c_str());
                RebuildItems();
                SelectApp(m_kb_app);
                SetStatus("Renamed");
                m_screen = Screen::Main;
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
        if (b == Btn::B)    m_screen = (m_kb_purpose == KbPurpose::WeatherCity)
                                        ? Screen::Theming : Screen::Main; // cancel
        return Action::None;
    }

    void Menu::SetUser(AccountUid uid, const char *nickname) {
        m_user = uid;
        strncpy(m_nickname, nickname, 32);
        m_nickname[32] = '\0';
    }

    void Menu::SetStatus(const char *msg) {
        strncpy(m_status, msg, 127);
        m_status[127] = '\0';
        m_status_tick = armGetSystemTick();
    }

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
            case Screen::Keyboard:    return OnButtonKeyboard(b);
        }
        return Action::None;
    }

    Menu::Action Menu::OnButtonOobe(Btn b) {
        constexpr int LastStep = 2;
        if (m_oobe_step == 1) {
            if (b == Btn::Down) { m_theme_cursor = (m_theme_cursor + 1) % TotalThemeCount; m_theme.Select(m_theme_cursor); }
            if (b == Btn::Up)   { m_theme_cursor = (m_theme_cursor + TotalThemeCount - 1) % TotalThemeCount; m_theme.Select(m_theme_cursor); }
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
        if (b == Btn::Down) {
            if (m_cursor < last)       m_cursor++;
            else if (m_nav_fresh)    { m_cursor = 0;    m_scroll_pos = 0.0f; }
            return Action::None;
        }
        if (b == Btn::Up) {
            if (m_cursor > 0)          m_cursor--;
            else if (m_nav_fresh)    { m_cursor = last; m_scroll_pos = (float)last; }
            return Action::None;
        }
        if (b == Btn::R) { m_cursor = std::min(m_cursor + 5, last); return Action::None; }
        if (b == Btn::L) { m_cursor = std::max(0, m_cursor - 5); return Action::None; }

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
            m_options.push_back({ IsFavourite(it.app_id) ? "Remove from Favourites"
                                                         : "Add to Favourites", OptFav });
            m_options.push_back({ "Rename", OptRename });
            if (m_suspended != 0 && it.app_id == m_suspended)
                m_options.push_back({ "Close game", OptCloseGame });
        }
        m_options.push_back({ m_sort == SortMode::Alpha ? "Sort: A - Z"
                                                        : "Sort: Default", OptSort });
        m_options.push_back({ "Cancel", OptDismiss });
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
    namespace { enum { TH_Themes = 0, TH_Fonts, TH_TextPos, TH_Weather, TH_WeatherCity, TH_Back, TH_Count }; }

    Menu::Action Menu::OnButtonTheming(Btn b) {
        auto cycleAlign = [&](int dir) {
            m_align = (TextAlign)(((int)m_align + dir + 3) % 3);
            SaveSettings();
        };
        if (b == Btn::Down) m_theming_cursor = (m_theming_cursor + 1) % TH_Count;
        if (b == Btn::Up)   m_theming_cursor = (m_theming_cursor + TH_Count - 1) % TH_Count;
        if (m_theming_cursor == TH_TextPos) {
            if (b == Btn::Right) cycleAlign(+1);
            if (b == Btn::Left)  cycleAlign(-1);
        }
        if (m_theming_cursor == TH_Weather && (b == Btn::Left || b == Btn::Right))
            m_widgets.SetWeatherEnabled(!m_widgets.WeatherEnabled());
        if (b == Btn::A) {
            switch (m_theming_cursor) {
                case TH_Themes:  m_screen = Screen::Themes; m_theme_cursor = m_theme.CurrentIndex(); break;
                case TH_Fonts:   m_screen = Screen::Fonts;  m_font_cursor = m_font_applied; break;
                case TH_TextPos: cycleAlign(+1); break;
                case TH_Weather: m_widgets.SetWeatherEnabled(!m_widgets.WeatherEnabled()); break;
                case TH_WeatherCity:
                    m_kb_purpose = KbPurpose::WeatherCity;
                    m_kb_text = m_widgets.WeatherCity();
                    m_kb_row = 0; m_kb_col = 0; m_kb_upper = false;
                    m_screen = Screen::Keyboard;
                    break;
                case TH_Back:    m_screen = Screen::Main; break;
            }
        }
        if (b == Btn::B) m_screen = Screen::Main;
        return Action::None;
    }

    Menu::Action Menu::OnButtonThemes(Btn b) {
        if (b == Btn::Down) { m_theme_cursor = (m_theme_cursor + 1) % TotalThemeCount; m_theme.Select(m_theme_cursor); }
        if (b == Btn::Up)   { m_theme_cursor = (m_theme_cursor + TotalThemeCount - 1) % TotalThemeCount; m_theme.Select(m_theme_cursor); }
        if (b == Btn::A) { m_theme.Select(m_theme_cursor); m_theme.Save(); SetStatus("Theme applied"); }
        if (b == Btn::Y && ThemeManager::IsCustom(m_theme_cursor)) {
            m_screen = Screen::ThemeEditor;
            m_edit_cursor = 0;
            ScanWallpapers();
            m_theme.Select(CustomThemeIndex);
            m_theme_cursor = CustomThemeIndex;
        }
        if (b == Btn::B) {
            m_theme.Load();
            m_theme_cursor = m_theme.CurrentIndex();
            m_screen = Screen::Theming;
        }
        return Action::None;
    }

    // Theme-editor rows: a background selector, six Color slots, and Save.
    namespace {
        enum { EF_Background = 0, EF_Top, EF_Bottom, EF_Text,
               EF_Accent, EF_Secondary, EF_Title, EF_Save, EF_Count };
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
        DIR *d = opendir("sdmc:/slaunch");
        if (!d) return;
        struct dirent *e;
        while ((e = readdir(d)) != nullptr) {
            const char *name = e->d_name;
            size_t len = strlen(name);
            if (len < 5) continue;
            const char *e4 = name + len - 4;
            const char *e5 = len >= 5 ? name + len - 5 : "";
            if (strcasecmp(e4, ".jpg") == 0 || strcasecmp(e4, ".png") == 0 ||
                strcasecmp(e4, ".bmp") == 0 || strcasecmp(e5, ".jpeg") == 0)
                m_wallpapers.push_back(std::string("sdmc:/slaunch/") + name);
        }
        closedir(d);
    }

    void Menu::CycleBackground(int dir) {
        Theme &c = m_theme.Custom();
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
        m_theme.Select(CustomThemeIndex);
        m_theme_cursor    = CustomThemeIndex;
        m_wallpaper_theme = -1; // force the wallpaper cache to reload
    }

    void Menu::OpenColorPicker(SDL_Color *target) {
        m_pick_target   = target;
        m_pick_original = *target;
        m_pick_channel  = 0;
        m_screen        = Screen::ColorPicker;
    }

    Menu::Action Menu::OnButtonEditor(Btn b) {
        Theme &c = m_theme.Custom();

        if (b == Btn::Down) m_edit_cursor = (m_edit_cursor + 1) % EF_Count;
        if (b == Btn::Up)   m_edit_cursor = (m_edit_cursor + EF_Count - 1) % EF_Count;

        if (m_edit_cursor == EF_Background) {
            if (b == Btn::Right) CycleBackground(+1);
            if (b == Btn::Left)  CycleBackground(-1);
        }

        if (b == Btn::A) {
            if (m_edit_cursor == EF_Save) {
                m_theme.Select(CustomThemeIndex);
                m_theme.Save();
                SetStatus("Custom theme saved");
                m_screen = Screen::Themes;
                m_theme_cursor = CustomThemeIndex;
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
            m_theme.Select(CustomThemeIndex); // live preview
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

        if (center_title && center_title[0])
            m_gfx->TextCentered(FontSize::Title, gfx::Gfx::Width / 2, 26, t.title, center_title);

        (void)kTopBarH;
    }

    void Menu::DrawHint(const char *hint) {
        const Theme &t = m_theme.Current();
        m_gfx->TextCentered(FontSize::Small, gfx::Gfx::Width / 2, kHintY, t.dim, hint);
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
            m_gfx->TextCentered(FontSize::Title, cx, 240, t.title, "Welcome to sLaunch");
            m_gfx->TextCentered(FontSize::Small, cx, 380, t.dim, "Let's get you set up.");
            DrawHint("A: Continue");
        } else if (m_oobe_step == 1) {
            m_gfx->TextCentered(FontSize::Large, cx, 120, t.title, "Choose a Theme");
            int top = 260;
            for (int i = 0; i < TotalThemeCount; i++) {
                bool sel = (i == m_theme_cursor);
                int y = top + i * 52;
                if (sel) m_gfx->FillRect(cx - 200, y - 4, 400, 46, WithAlpha(t.accent, 40));
                m_gfx->TextCentered(FontSize::Normal, cx, y, sel ? t.accent : t.fg, m_theme.At(i).name);
            }
            DrawHint("A: Continue    B: Back");
        } else {
            m_gfx->TextCentered(FontSize::Title, cx, 260, t.title, "All set!");
            m_gfx->TextCentered(FontSize::Normal, cx, 350, t.fg, "You're ready to go.");
            DrawHint("A: Finish");
        }
    }

    void Menu::DrawMain() {
        const Theme &t = m_theme.Current();
        DrawTopBar(nullptr);

        if (m_items.empty()) {
            m_gfx->TextCentered(FontSize::Normal, gfx::Gfx::Width / 2, 340, t.dim, "No apps found");
            DrawHint("+: Power");
            return;
        }

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

            // Favourites get a leading star (U+2605).
            std::string label = it.is_favourite ? (std::string("\xE2\x98\x85 ") + it.name)
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

        // Widget cards on the side opposite the list text (so they don't clash).
        if (m_widgets.AnyEnabled()) {
            const int cw = 300;
            const int wx = (m_align == TextAlign::Right) ? 40
                                                         : gfx::Gfx::Width - cw - 30;
            m_widgets.Render(m_gfx, t, wx, 150, cw);
        }

        // Fresh status (< 3s) else the control hint.
        if (m_status[0] != '\0') {
            u64 nowt = armGetSystemTick(), freq = armGetSystemTickFreq();
            if ((nowt - m_status_tick) < 3 * freq)
                m_gfx->TextCentered(FontSize::Normal, gfx::Gfx::Width / 2, kHintY - 44, t.accent, m_status);
            else
                m_status[0] = '\0';
        }
        DrawHint("A: Select    X: Options");
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
        const Theme &t = m_theme.Current();
        DrawTopBar("Theming");
        const char *aligns[3] = { "Left", "Center", "Right" };
        const char *labels[TH_Count] = {
            "Themes", "Fonts", "Text position", "Weather widget", "Weather location", "Back"
        };

        const int cx = gfx::Gfx::Width / 2, top = 176, rowH = 62;
        for (int i = 0; i < TH_Count; i++) {
            const bool sel = (i == m_theming_cursor);
            const int  y   = top + i * rowH;
            if (sel) m_gfx->FillRect(cx - 340, y - 6, 680, rowH - 12, WithAlpha(t.accent, 46));
            m_gfx->Text(FontSize::Large, cx - 300, y, sel ? t.accent : t.fg, labels[i]);
            const SDL_Color vc = sel ? t.accent : t.dim;
            if (i == TH_TextPos)
                m_gfx->Text(FontSize::Normal, cx + 120, y + 6, vc, aligns[(int)m_align]);
            else if (i == TH_Weather)
                m_gfx->Text(FontSize::Normal, cx + 120, y + 6, vc,
                            m_widgets.WeatherEnabled() ? "On" : "Off");
            else if (i == TH_WeatherCity) {
                const char *c = m_widgets.WeatherCity();
                m_gfx->Text(FontSize::Normal, cx + 120, y + 6, vc, c[0] ? c : "(not set)");
            }
        }
        DrawHint("B: Back");
    }

    void Menu::DrawThemes() {
        const Theme &t = m_theme.Current();
        DrawTopBar("Themes");
        int cx = gfx::Gfx::Width / 2;
        int top = 220;
        for (int i = 0; i < TotalThemeCount; i++) {
            bool sel = (i == m_theme_cursor);
            int y = top + i * 56;
            if (sel) m_gfx->FillRect(cx - 220, y - 6, 440, 50, WithAlpha(t.accent, 46));
            char label[48];
            snprintf(label, sizeof(label), "%s%s", m_theme.At(i).name,
                     (i == m_theme.CurrentIndex()) ? "  *" : "");
            m_gfx->TextCentered(FontSize::Large, cx, y, sel ? t.accent : t.fg, label);
        }

        if (m_status[0] != '\0') {
            u64 nowt = armGetSystemTick(), freq = armGetSystemTickFreq();
            if ((nowt - m_status_tick) < 3 * freq)
                m_gfx->TextCentered(FontSize::Normal, cx, kHintY - 44, t.accent, m_status);
        }

        if (ThemeManager::IsCustom(m_theme_cursor))
            DrawHint("A: Apply    Y: Customise    B: Back");
        else
            DrawHint("A: Apply    B: Back");
    }

    void Menu::DrawEditor() {
        const Theme &t = m_theme.Current(); // the custom theme, shown live
        DrawTopBar("Custom Theme");
        Theme &c = m_theme.Custom();

        const char *labels[EF_Count] = {
            "Background", "Gradient top", "Gradient bottom", "Text",
            "Accent", "Secondary", "Title", "Save & Apply"
        };

        const int lx = 200, vx = 720, rowH = 58, top = 150;
        for (int i = 0; i < EF_Count; i++) {
            const bool sel = (i == m_edit_cursor);
            const int  y   = top + i * rowH;
            if (sel) m_gfx->FillRect(lx - 30, y - 6, 900, rowH - 8, WithAlpha(t.accent, 46));
            m_gfx->Text(FontSize::Normal, lx, y, sel ? t.accent : t.fg, labels[i]);

            if (SDL_Color *col = EditorColor(c, i)) {
                m_gfx->FillRect(vx, y, 64, 36, *col);
                char hex[16];
                snprintf(hex, sizeof(hex), "#%02X%02X%02X", col->r, col->g, col->b);
                m_gfx->Text(FontSize::Small, vx + 84, y + 6, t.dim, hex);
            } else if (i == EF_Background) {
                const char *slash = c.wallpaper[0] ? strrchr(c.wallpaper, '/') : nullptr;
                const char *bg = (c.wallpaper[0] == '\0') ? "Gradient"
                                : (slash ? slash + 1 : c.wallpaper);
                m_gfx->Text(FontSize::Small, vx - 40, y + 6, t.dim, "<");
                m_gfx->Text(FontSize::Normal, vx, y, t.fg, bg);
            }
        }

        // Contextual hint for the current row.
        if (m_edit_cursor == EF_Background)
            DrawHint("Up/Down: Row    Left/Right: Change background    B: Back");
        else if (m_edit_cursor == EF_Save)
            DrawHint("Up/Down: Row    A: Save & apply    B: Back");
        else
            DrawHint("Up/Down: Row    A: Edit Color    B: Back");
    }

    void Menu::DrawColorPicker() {
        if (!m_pick_target) return;
        const int cx = gfx::Gfx::Width / 2;

        // Fixed, always-readable chrome (independent of the Color being edited).
        const SDL_Color white{240, 240, 240, 255}, dim{150, 150, 155, 255},
                        accent{90, 200, 255, 255}, track{50, 50, 58, 255};
        m_gfx->FillRect(0, 0, gfx::Gfx::Width, gfx::Gfx::Height, SDL_Color{12, 12, 16, 255});

        m_gfx->TextCentered(FontSize::Title, cx, 70, white, "Color");

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
            "Up/Down: Channel   Left/Right: Adjust   L/R: coarse   A: Done   B: Cancel");
    }

    void Menu::DrawFonts() {
        const Theme &t = m_theme.Current();
        DrawTopBar("Fonts");

        int cx = gfx::Gfx::Width / 2;
        int n = (int)m_font_names.size();
        int listTop = 170;
        int maxRows = 7;
        int start = 0;
        if (m_font_cursor >= maxRows) start = m_font_cursor - maxRows + 1;

        for (int i = 0; i < maxRows && (start + i) < n; i++) {
            int idx = start + i;
            bool sel = (idx == m_font_cursor);
            int y = listTop + i * 48;
            if (sel) m_gfx->FillRect(cx - 300, y - 4, 600, 44, WithAlpha(t.accent, 46));
            char label[80];
            snprintf(label, sizeof(label), "%s%s", m_font_names[idx].c_str(),
                     (idx == m_font_applied) ? "  *" : "");
            m_gfx->TextCentered(FontSize::Normal, cx, y, sel ? t.accent : t.fg, label);
        }

        // Live preview of the highlighted font (loaded into the alt slot).
        EnsurePreviewFont(m_font_cursor);
        int py = listTop + maxRows * 48 + 30;
        m_gfx->TextCentered(FontSize::Small, cx, py, t.dim, "Preview");
        m_gfx->UseDefaultFont(false); // render the sample IN the highlighted font
        m_gfx->TextCentered(FontSize::Large, cx, py + 34, t.fg, "The quick brown fox 0123");
        m_gfx->UseDefaultFont(true);

        if (m_status[0] != '\0') {
            u64 nowt = armGetSystemTick(), freq = armGetSystemTickFreq();
            if ((nowt - m_status_tick) < 3 * freq)
                m_gfx->TextCentered(FontSize::Normal, cx, kHintY - 44, t.accent, m_status);
        }
        DrawHint("Up/Down: Preview    A: Apply    B: Back");
    }

    void Menu::DrawKeyboard() {
        const Theme &t = m_theme.Current();
        const int cx = gfx::Gfx::Width / 2;
        DrawTopBar("Rename");

        // Current text in an input box.
        m_gfx->FillRect(cx - 400, 130, 800, 60, WithAlpha(t.fg, 24));
        m_gfx->FillRect(cx - 400, 186, 800, 3, t.accent);
        std::string shown = m_kb_text.empty() ? "(empty)" : m_kb_text;
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
                                sel ? t.accent : t.fg, kKbSpecial[c]);
        }

        DrawHint("D-Pad: Move   A: Type   X: Shift   Y: Backspace   +: Done   B: Cancel");
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

        m_gfx->TextCentered(FontSize::Large, cx, by + 40, t.title, "Close running application?");
        const char *opts[2] = { "Yes", "No" };
        for (int i = 0; i < 2; i++) {
            bool sel = (i == m_dialog_cursor);
            int y = by + 120 + i * 48;
            if (sel) m_gfx->FillRect(cx - 90, y - 4, 180, 42, WithAlpha(t.accent, 60));
            m_gfx->TextCentered(FontSize::Normal, cx, y, sel ? t.accent : t.fg, opts[i]);
        }
        DrawHint("Up/Down: Choose    A: Confirm    B: Cancel");
    }

} // namespace sl::menu::ui
