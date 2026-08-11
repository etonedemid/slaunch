#include <sl/menu/ui/Menu.hpp>
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

namespace sl::menu::ui {

    using gfx::FontSize;

    // ---- submenu row ids ---------------------------------------------------
    // Declared up here rather than beside each screen's handler because the
    // touch code near the top of the file has to know how many rows a screen
    // has; a stale hand-counted number there silently makes the last row
    // untappable.
    namespace {
        enum { TH_Themes = 0, TH_UiMode, TH_TextPos, TH_ListIcons,
               TH_IconPack, TH_Fonts, TH_Music, TH_Widgets, TH_Entries,
               TH_Welcome, TH_Updates,
               TH_About, TH_Back, TH_Count };

        enum { EF_Background = 0, EF_Wallpaper, EF_WallpaperDim, EF_WallpaperBlur,
               EF_WallpaperBlurRadius, EF_WallpaperSnow, EF_WallpaperFps,
               EF_Top, EF_Bottom, EF_Text,
               EF_Accent, EF_Secondary, EF_Title, EF_IconBg,
               EF_RibbonLines, EF_RibbonThickness, EF_RibbonAmplitude,
               EF_RibbonSeed, EF_RibbonLayers, EF_RibbonYCenter,
               EF_Rename, EF_Save, EF_Delete, EF_Count };

        enum { MU_Enabled = 0, MU_Track, MU_Volume, MU_Shuffle, MU_Back, MU_Count };

        // Return true when row *r* belongs to the wallpaper-effects block.
        inline bool IsEffectRow(int r) {
            return r >= EF_WallpaperDim && r <= EF_WallpaperSnow;
        }
        // Return true when row *r* is the blur-radius setting (hidden when blur off).
        inline bool IsBlurRadiusRow(int r) { return r == EF_WallpaperBlurRadius; }
        // Return true when row *r* belongs to the ribbon block.
        inline bool IsRibbonRow(int r) {
            return r >= EF_RibbonLines && r <= EF_RibbonYCenter;
        }
        // Return true when the wallpaper path is a directory (video frame sequence).
        inline bool IsVideoPath(const char *path) {
            if (!path || !path[0]) return false;
            struct stat st;
            return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
        }

        // The entries the user can hide from the main menu. Theming is
        // deliberately absent: it has to stay or these toggles become
        // unreachable.
        struct SysEntry { ItemKind kind; const char *name; };
        const SysEntry kSysEntries[] = {
            { ItemKind::RandomGame,   "Random game"   },
            { ItemKind::Controllers,  "Controllers"   },
            { ItemKind::Album,        "Album"         },
            { ItemKind::UserPage,     "User Page"     },
            { ItemKind::WebBrowser,   "Web Browser"   },
            { ItemKind::MiiEdit,      "Mii Edit"      },
            { ItemKind::Settings,     "Settings"      },
            { ItemKind::Power,        "Power"         },
            { ItemKind::HomebrewMenu, "Homebrew menu" },
        };
        constexpr int kSysEntryN = (int)(sizeof(kSysEntries) / sizeof(kSysEntries[0]));
    }

    // Layout (1280x720)
    static constexpr int kTopBarH   = 56;
    static constexpr int kListX      = 120;
    static constexpr int kListTop    = 150;
    static constexpr int kRowH       = 54;
    static constexpr int kListW       = 1040;
    static constexpr int kHintY       = 682;

    // On-screen distance between neighbouring entries, per layout. Touch drag
    // divides by these to move the content at the same rate as the finger, and
    // the hit-tests invert them, so they have to be the numbers the renderers
    // actually use.
    static constexpr int kListSpacing = 48;    // List/Cover: row to row
    static constexpr int kListCenterY = 360;   // vertical centre of the carousel
    static constexpr int kLinePitch   = 210;   // Line: cover centre to cover centre
    static constexpr int kGridTile    = 120;   // Grid: tile edge
    static constexpr int kGridGap     = 18;    // Grid: gap between tiles
    static constexpr int kGridPitch   = kGridTile + kGridGap;   // Grid: row to row
    static constexpr int kGridTopBase = 80;    // Grid: y of row 0 at scroll 0

    // Shelf mode geometry (Xbox-360 "My Games" style): uniform covers in a row,
    // the selected one anchored near the left. Shared by draw, hit-test and
    // touch scrolling (OnTouch).
    static constexpr int kShelfTile    = 208;   // cover edge
    static constexpr int kShelfGap     = 20;
    static constexpr int kShelfPitch   = kShelfTile + kShelfGap;
    static constexpr int kShelfAnchorX = 88;    // left edge of the selected cover
    static constexpr int kShelfTop     = 150;   // top edge of the cover row

    // XMB geometry (PSP cross-media bar, scaled to 1280x720). The bar row and
    // the entry column cross at (kXmbAnchorX, kXmbItemY); every other position
    // is derived from that. Shared by the renderer and the touch hit-tests.
    // The bar sits high so the column below it has room: five entries read at a
    // glance, which matters far more on a games console than the handheld's
    // symmetry did. Category icons span kXmbBarY..+kXmbCatSel with their label
    // just under, and the entry fade has to finish above that label.
    static constexpr int kXmbAnchorX   = 268;   // centre X of the selected column
    static constexpr int kXmbBarY      = 116;   // top of the selected category icon
    static constexpr int kXmbCatSel    = 88;    // selected category icon edge
    static constexpr int kXmbCatUnsel  = 56;    // unselected category icon edge
    static constexpr int kXmbCatPitch  = 152;   // spacing between category centres
    static constexpr int kXmbBarMidY   = kXmbBarY + kXmbCatSel / 2;  // the bar's glow line
    static constexpr int kXmbItemY     = 348;   // centre Y of the selected entry
    static constexpr int kXmbItemPitch = 60;    // spacing between entry centres
    static constexpr int kXmbItemSel   = 58;    // selected entry icon edge
    static constexpr int kXmbItemUnsel = 44;    // unselected entry icon edge
    static constexpr int kXmbFadeStart = 306;   // entries are fully lit at/below this
    static constexpr int kXmbFadeEnd   = 250;   // ...and fully gone at/above it
    static constexpr int kXmbItemBottom = 600;  // last row that clears the status line
    static constexpr int kXmbAbove     = 2;     // entries kept above the selection
    static constexpr int kXmbBelow     = 5;     // ...and below it

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
        m_hb_icons.Init(gfx, hb::IconDir);
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
        LoadOrder();
        LoadHbPins();
        LoadHbFavourites();
        LoadHbDonor();
        LoadSettings();
        LoadSysEntries();
        LoadNames();
        ScanIconPacks();
        LoadIconPackSetting();
        LoadPlayCache();    // last run's play times, refreshed from pdm after frame 1
        ShowPowerError();   // a chainload the daemon could not carry out
        // Widget loading (Lua parse + curl init) is deferred to InitDeferred so it
        // doesn't sit on the suspend->first-frame critical path.

        // Welcome screen: after setup, and once per console boot. The menu applet
        // restarts every time you come back from a game, so we tag the boot we
        // greeted. armGetSystemTick() counts from power-on, so (wall clock -
        // uptime) is a stable id within a boot and differs across boots.
        if (start_oobe) {
            m_screen = Screen::Oobe;
        } else if (m_welcome_enabled && m_suspended == 0 && BootWelcomePending()) {
            EnterWelcome();
            MarkBootWelcomed();
        } else {
            m_screen = Screen::Main;
        }
        RebuildItems();
    }

    // ---- once-per-boot welcome bookkeeping ---------------------------------
    namespace {
        constexpr const char *kWelcomedPath = "sdmc:/slaunch/cache/welcomed.txt";
        long long BootId() {   // approximate console boot time, in epoch seconds
            const u64 up = armGetSystemTick() / armGetSystemTickFreq();
            return (long long)time(nullptr) - (long long)up;
        }
    }

    bool Menu::BootWelcomePending() const {
        FILE *fp = fopen(kWelcomedPath, "r");
        if (!fp) return true;                 // never greeted -> this boot counts
        long long stored = 0;
        const bool got = (fscanf(fp, "%lld", &stored) == 1);
        fclose(fp);
        if (!got) return true;
        // Same boot if the recorded boot id is within a few seconds of ours.
        const long long d = BootId() - stored;
        return (d > 5 || d < -5);
    }

    void Menu::MarkBootWelcomed() const {
        mkdir("sdmc:/slaunch", 0777);
        mkdir("sdmc:/slaunch/cache", 0777);
        FILE *fp = fopen(kWelcomedPath, "w");
        if (!fp) return;
        fprintf(fp, "%lld\n", BootId());
        fclose(fp);
    }

    void Menu::InitDeferred() {
        if (m_deferred_done) return;
        m_deferred_done = true;
        m_widgets.Init();   // loads widget config, starts the network thread
        const bool audio = m_music.Init();  // opens the mixer + resumes music (off-thread)
        m_sfx.Init(audio);                  // UI sounds share the mixer
        // Welcome chime only on a fresh open (no game suspended behind us), so it
        // isn't heard every single time you HOME out of a game. On the boot
        // welcome screen the opening jingle takes its place (audio only comes up
        // here, after the first frame, so it can't be played from Init).
        if (audio && m_suspended == 0) {
            if (m_screen == Screen::Welcome) {
                m_sfx.Play(audio::Sfx::Opening);
                m_welcome_start = armGetSystemTick();   // hold the screen for the jingle
            } else {
                m_sfx.Play(audio::Sfx::Welcome);
            }
        }
        // Resolve pinned homebrew names/icons on a worker thread (never blocks the
        // menu-start path); they show fallback names until it lands.
        StartResolvePins();
        // Optional: check GitHub for a newer release (off the main thread).
        StartUpdateCheck();
    }

    Menu::~Menu() {
        // Let the homebrew workers finish before we tear down (they write into
        // members and would outlive them otherwise).
        if (m_hb_scan_running) {
            threadWaitForExit(&m_hb_thread);
            threadClose(&m_hb_thread);
            m_hb_scan_running = false;
        }
        if (m_pin_running) {
            threadWaitForExit(&m_pin_thread);
            threadClose(&m_pin_thread);
            m_pin_running = false;
        }
        if (m_upd_running) {
            threadWaitForExit(&m_upd_thread);
            threadClose(&m_upd_thread);
            m_upd_running = false;
        }
        if (m_play_running) {
            threadWaitForExit(&m_play_thread);
            threadClose(&m_play_thread);
            m_play_running = false;
        }
        // Free SFX chunks before Music::Exit() closes the mixer, then stop the
        // network thread.
        m_sfx.Exit();
        m_music.Exit();
        m_widgets.Exit();
        // Free textures while the renderer is still alive (main() runs gfx.Exit()
        // only after the Menu is destroyed).
        m_icons.Exit();
        m_hb_icons.Exit();
        if (m_wallpaper)   { m_gfx->FreeImage(m_wallpaper);   m_wallpaper = nullptr; }
        if (m_wallpaper_blur) { m_gfx->FreeImage(m_wallpaper_blur); m_wallpaper_blur = nullptr; }
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
        auto sort_group = [&](std::vector<MenuItem> &v) {
            auto title = [](const MenuItem &x, const MenuItem &y) {
                return strcasecmp(x.name.c_str(), y.name.c_str());
            };
            // Play-stat sorts: entries with no record (homebrew, never-launched
            // games) keep their relative order at the bottom rather than mixing
            // into the middle as zeroes.
            auto by_stat = [&](const MenuItem &x, const MenuItem &y, bool recent) {
                const play::PlayInfo *px = Play(x.app_id), *py = Play(y.app_id);
                const u64 vx = !px ? 0 : (recent ? px->last_played : px->seconds);
                const u64 vy = !py ? 0 : (recent ? py->last_played : py->seconds);
                if (vx != vy) return vx > vy;
                return title(x, y) < 0;
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
                case SortMode::RecentlyPlayed:
                    std::stable_sort(v.begin(), v.end(),
                              [&](const MenuItem &x, const MenuItem &y){ return by_stat(x, y, true); });
                    break;
                case SortMode::MostPlayed:
                    std::stable_sort(v.begin(), v.end(),
                              [&](const MenuItem &x, const MenuItem &y){ return by_stat(x, y, false); });
                    break;
                default: break;   // Default: keep the built arrangement (custom
                                  // move-order is applied to the whole list below)
            }
        };
        // Pinned homebrew (name + cached icon resolved by ResolvePins; fallback
        // file-base name until then). Favourited pins join the favourites group so
        // they sort to the very top with the games; the rest sit just below.
        std::vector<MenuItem> hb_pinned;
        for (auto &p : m_hb_pins) {
            MenuItem it;
            it.kind        = ItemKind::Homebrew;
            it.hb_path     = p.path;
            it.name        = p.name;
            it.hb_icon     = p.icon_key;
            it.is_favourite = IsHbFavourite(p.path);
            (it.is_favourite ? favs : hb_pinned).push_back(std::move(it));
        }

        sort_group(favs);   // games + favourited homebrew, ordered by sort mode
        sort_group(rest);

        // Favourites pinned at the very top for quick access.
        for (auto &it : favs) m_items.push_back(std::move(it));

        // Remaining (non-favourite) pinned homebrew, just under the favourites.
        for (auto &it : hb_pinned) m_items.push_back(std::move(it));

        // System shortcuts (hidden ones are skipped; Theming is never hideable).
        auto add = [&](ItemKind k, const char *name) {
            if (IsSysHidden(k)) return;
            MenuItem it; it.kind = k; it.name = name; m_items.push_back(std::move(it));
        };
        add(ItemKind::Theming,      T("Theming"));          // always shown
        add(ItemKind::RandomGame,   T("Random game"));
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

        // Custom arrangement (from Move): reorder the whole list by the saved key
        // order. Seeded from the built arrangement, so it starts as a no-op and
        // only reflects entries the user has actually moved; new/unlisted entries
        // sort stably to the end in their default position. Default sort only.
        if (m_sort == SortMode::Default && !m_order.empty()) {
            // Rank each entry once up front. Looking the key up inside the
            // comparator instead (a scan of m_order per comparison, plus an
            // ItemKey string built per comparison) made this the slowest thing
            // in the menu on a large library.
            std::unordered_map<std::string, int> rank_of;
            rank_of.reserve(m_order.size());
            for (int i = 0; i < (int)m_order.size(); i++)
                rank_of.emplace(m_order[i], i);
            const int unranked = (int)m_order.size() + 1;

            std::vector<std::pair<int, const MenuItem *>> keyed;
            keyed.reserve(m_items.size());
            for (const auto &it : m_items) {
                auto f = rank_of.find(ItemKey(it));
                keyed.emplace_back(f == rank_of.end() ? unranked : f->second, &it);
            }
            std::stable_sort(keyed.begin(), keyed.end(),
                [](const auto &x, const auto &y){ return x.first < y.first; });

            std::vector<MenuItem> sorted;
            sorted.reserve(m_items.size());
            for (const auto &k : keyed) sorted.push_back(*k.second);
            m_items = std::move(sorted);
        }

        if (m_cursor >= (int)m_items.size())
            m_cursor = m_items.empty() ? 0 : (int)m_items.size() - 1;

        XmbRebuild();   // regroup the cross-media bar for the new list
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

    // Homebrew favourites are kept as .nro paths, one per line.
    bool Menu::IsHbFavourite(const std::string &path) const {
        return std::find(m_hb_favs.begin(), m_hb_favs.end(), path) != m_hb_favs.end();
    }

    void Menu::ToggleHbFavourite(const std::string &path) {
        auto it = std::find(m_hb_favs.begin(), m_hb_favs.end(), path);
        if (it != m_hb_favs.end()) m_hb_favs.erase(it);
        else                       m_hb_favs.push_back(path);
        SaveHbFavourites();
    }

    void Menu::LoadHbFavourites() {
        m_hb_favs.clear();
        FILE *fp = fopen("sdmc:/slaunch/config/hb_favourites.txt", "r");
        if (!fp) return;
        char line[FS_MAX_PATH + 2];
        while (fgets(line, sizeof(line), fp)) {
            line[strcspn(line, "\r\n")] = '\0';
            if (line[0]) m_hb_favs.push_back(line);
        }
        fclose(fp);
    }

    void Menu::SaveHbFavourites() {
        mkdir("sdmc:/slaunch", 0777);
        mkdir("sdmc:/slaunch/config", 0777);
        FILE *fp = fopen("sdmc:/slaunch/config/hb_favourites.txt", "w");
        if (!fp) return;
        for (auto &p : m_hb_favs) fprintf(fp, "%s\n", p.c_str());
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

    // Stable per-entry key for the custom order: games by title id, homebrew by
    // .nro path, system shortcuts by kind. Kept text so order.txt is one key/line.
    std::string Menu::ItemKey(const MenuItem &it) const {
        char b[40];
        switch (it.kind) {
            case ItemKind::Game:
                snprintf(b, sizeof(b), "g%016llX", (unsigned long long)it.app_id);
                return b;
            case ItemKind::Homebrew:
                return "h" + it.hb_path;
            default:
                snprintf(b, sizeof(b), "s%d", (int)it.kind);
                return b;
        }
    }

    void Menu::SelectByKey(const std::string &key) {
        for (int i = 0; i < (int)m_items.size(); i++)
            if (ItemKey(m_items[i]) == key) {
                m_cursor = i;
                m_scroll_pos = (float)i;
                m_grid_scroll = (float)(i / GridColumns());
                XmbSyncFromCursor();
                return;
            }
    }

    void Menu::LoadOrder() {
        m_order.clear();
        FILE *fp = fopen("sdmc:/slaunch/config/order.txt", "r");
        if (!fp) return;
        char line[FS_MAX_PATH + 4];
        while (fgets(line, sizeof(line), fp)) {
            line[strcspn(line, "\r\n")] = '\0';
            if (line[0]) m_order.push_back(line);
        }
        fclose(fp);
    }

    void Menu::SaveOrder() {
        mkdir("sdmc:/slaunch", 0777);
        mkdir("sdmc:/slaunch/config", 0777);
        FILE *fp = fopen("sdmc:/slaunch/config/order.txt", "w");
        if (!fp) return;
        for (auto &k : m_order) fprintf(fp, "%s\n", k.c_str());
        fclose(fp);
    }

    // Reorder the held entry one slot in the given direction, swapping with the
    // adjacent entry (any kind), then persist. Keeps the cursor on the moved item.
    void Menu::MoveSelected(int dir) {
        if (!m_move_mode || m_move_key.empty()) return;
        int cur = -1;
        for (int i = 0; i < (int)m_items.size(); i++)
            if (ItemKey(m_items[i]) == m_move_key) { cur = i; break; }
        const int nb = cur + dir;
        if (cur < 0 || nb < 0 || nb >= (int)m_items.size()) return;

        // Seed m_order from the full current arrangement the first time, so the
        // ranking is complete (a single move is otherwise ambiguous).
        if (m_order.empty())
            for (auto &it : m_items) m_order.push_back(ItemKey(it));

        auto pos = [&](const std::string &key) {
            for (int i = 0; i < (int)m_order.size(); i++) if (m_order[i] == key) return i;
            m_order.push_back(key); return (int)m_order.size() - 1;
        };
        std::swap(m_order[pos(m_move_key)], m_order[pos(ItemKey(m_items[nb]))]);
        RebuildItems();
        SelectByKey(m_move_key);   // keep the cursor on the moved entry
    }

    bool Menu::SelectApp(u64 app_id) {
        for (int i = 0; i < (int)m_items.size(); i++) {
            if (m_items[i].kind == ItemKind::Game && m_items[i].app_id == app_id) {
                m_cursor = i;
                m_scroll_pos = (float)i;
                m_grid_scroll = (float)(i / GridColumns());
                XmbSyncFromCursor();
                return true;
            }
        }
        return false;
    }

    void Menu::SetApps(std::vector<AppEntry> apps) {
        m_apps = std::move(apps);
        m_play_dirty = true;   // new/removed titles -> re-query their play stats
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
            else if (sscanf(line, "check_updates=%d", &v) == 1)
                m_check_updates = (v != 0);
            else if (sscanf(line, "welcome=%d", &v) == 1)
                m_welcome_enabled = (v != 0);
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
        fprintf(fp, "check_updates=%d\n", m_check_updates ? 1 : 0);
        fprintf(fp, "welcome=%d\n", m_welcome_enabled ? 1 : 0);
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
        // 4 character rows + a special bottom row (Shift/Space/Back/Clear/Done).
        const char *kKbRows[4] = {
            "1234567890",
            "qwertyuiop",
            "asdfghjkl",
            "zxcvbnm",
        };
        const char *kKbSpecial[5] = { "Shift", "Space", "Back", "Clear", "Done" };
        constexpr int kKbSpecialRow  = 4;
        constexpr int kKbSpecialCols = 5;

        int KbRowLen(int row) {
            if (row < 4) return (int)strlen(kKbRows[row]);
            return kKbSpecialCols; // special row
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
                    case 3: m_kb_text.clear(); break;               // Clear
                    case 4: commit(); break;                        // Done
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

        // ---- submenus: tap to select a row ----
        if (m_screen != Screen::Main && !m_options_open && m_dialog == Dialog::None) {
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
                    case Screen::Music:
                        cursor = &m_music_cursor;     rows = MU_Count; break;
                    case Screen::Homebrew:
                        cursor = &m_hb_cursor;        rows = (int)m_hb.size(); break;
                    case Screen::SysEntries:
                        cursor = &m_sys_cursor;       rows = kSysEntryN; break;
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
                        if (IsRibbonRow(i) && c->background_style != BackgroundStyle_Ribbon) continue;
                        if (i == EF_WallpaperFps && !IsVideoPath(c->wallpaper)) continue;
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

            if (!m_touch_scroll_active && move_dist_sq >= kScrollThresholdSq) {
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

                switch (m_ui_mode) {
                    case UiMode::Line:      // horizontal cover carousel
                        m_scroll_pos = clamped(dragged(dx, kLinePitch), last);
                        m_cursor = (int)lroundf(m_scroll_pos);
                        break;
                    case UiMode::Shelf:     // horizontal cover row
                        m_scroll_pos = clamped(dragged(dx, kShelfPitch), last);
                        m_cursor = (int)lroundf(m_scroll_pos);
                        break;
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
                        m_grid_scroll = clamped(dragged(dy, kGridPitch),
                                                (int)m_items.size() / GridColumns() - 1);
                        break;
                    default:                // List and Cover
                        m_scroll_pos = clamped(dragged(dy, kListSpacing), last);
                        m_cursor = (int)lroundf(m_scroll_pos);
                        break;
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
                // Short tap: select or launch item under finger.
                const int idx = MainItemAt(x, y);
                if (idx >= 0 && idx < (int)m_items.size()) {
                    if (idx == m_cursor) return OnButtonMain(Btn::A, out_app_id);
                    m_cursor = idx;
                    if (m_ui_mode == UiMode::XMB) XmbSyncFromCursor();
                }
            }

            m_touching = false; m_touch_widget = -1;
            return Action::None;
        }

        // down: start widget drag or prepare for touch scroll / tap
        m_touching = true;
        m_touch_lx = x; m_touch_ly = y;
        m_touch_start_x = x; m_touch_start_y = y;
        m_touch_scroll_start = (m_ui_mode == UiMode::Grid) ? m_grid_scroll
                             : (m_ui_mode == UiMode::XMB)  ? m_xmb_item_scroll
                                                           : m_scroll_pos;

        // Check for widget hit first.
        m_touch_widget = m_widgets.AnyEnabled() ? m_widgets.HitTest(x, y) : -1;
        if (m_touch_widget >= 0) {
            m_drag_active = true; m_drag_widget = m_touch_widget;
            return Action::None;
        }

        // Not on a widget: start in "tap" mode. If the finger moves beyond a
        // threshold before lift, we switch to scrolling.
        m_touch_scroll_active = false;
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
        if (m_sd_removed)            return Action::None;   // frozen: awaiting reboot
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
            case Screen::Music:         return OnButtonMusic(b);
            case Screen::Homebrew:      return OnButtonHomebrew(b);
            case Screen::About:         return OnButtonAbout(b);
            case Screen::Welcome:       return OnButtonWelcome(b);
            case Screen::SysEntries:    return OnButtonSysEntries(b);
            case Screen::Power:         return OnButtonPower(b);
            case Screen::Payloads:      return OnButtonPayloads(b);
        }
        return Action::None;
    }

    Menu::Action Menu::OnButtonOobe(Btn b) {
        constexpr int LastStep = 4;
        if (m_oobe_step == 1) {   // theme - applies live
            const int n = m_theme.Count();
            if (b == Btn::Down) { m_theme_cursor = (m_theme_cursor + 1) % n; m_theme.Select(m_theme_cursor); }
            if (b == Btn::Up)   { m_theme_cursor = (m_theme_cursor + n - 1) % n; m_theme.Select(m_theme_cursor); }
        }
        if (m_oobe_step == 2) {   // layout (vertical list, like the rest of the menu)
            const int n = (int)UiMode::Count;
            if (b == Btn::Right || b == Btn::Down) m_ui_mode = (UiMode)(((int)m_ui_mode + 1) % n);
            if (b == Btn::Left  || b == Btn::Up)   m_ui_mode = (UiMode)(((int)m_ui_mode + n - 1) % n);
        }
        if (m_oobe_step == LastStep) {   // done - update-check opt-out
            if (b == Btn::Left || b == Btn::Right) m_check_updates = !m_check_updates;
        }
        if (b == Btn::A) {
            // Re-seed the shared list scroll so the next step's carousel doesn't
            // slide in from the previous step's position.
            if (m_oobe_step == 0) m_sub_scroll = (float)m_theme_cursor;
            if (m_oobe_step == 1) m_sub_scroll = (float)(int)m_ui_mode;
            if (m_oobe_step < LastStep) { m_oobe_step++; }
            else {
                m_theme.Save();
                SaveSettings();   // persist the chosen layout (ui_mode)
                // Hand off to the welcome screen (opening jingle + the user's name)
                // instead of dropping straight into the menu.
                if (m_welcome_enabled) { EnterWelcome(); m_sfx.Play(audio::Sfx::Opening); }
                else                     m_screen = Screen::Main;
                return Action::FinishSetup;
            }
        }
        if (b == Btn::B && m_oobe_step > 0) {
            m_oobe_step--;
            if (m_oobe_step == 1) m_sub_scroll = (float)m_theme_cursor;
            if (m_oobe_step == 2) m_sub_scroll = (float)(int)m_ui_mode;
        }
        return Action::None;
    }

    Menu::Action Menu::OnButtonMain(Btn b, u64 &out_app_id) {
        if (m_items.empty()) {
            if (b == Btn::Plus) EnterPower();
            return Action::None;
        }
        // Move mode: the D-pad reorders the held entry instead of navigating.
        if (m_move_mode) {
            if (b == Btn::Left  || b == Btn::Up)   { MoveSelected(-1); return Action::None; }
            if (b == Btn::Right || b == Btn::Down) { MoveSelected(+1); return Action::None; }
            if (b == Btn::A) { m_move_mode = false; SaveOrder(); SetStatus("Order saved"); }
            if (b == Btn::B) { m_move_mode = false; LoadOrder(); RebuildItems(); SelectByKey(m_move_key); }
            return Action::None;
        }
        if (b == Btn::L) m_sfx.Play(audio::Sfx::PageLeft);
        if (b == Btn::R) m_sfx.Play(audio::Sfx::PageRight);
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
        } else if (m_ui_mode == UiMode::XMB) {
            // Cross-media bar: left/right rides the category bar, up/down walks
            // the selected category's column. Both keep m_cursor pointing at the
            // same entry, so A/X and the options overlay need no special case.
            if (m_xmb_cols.empty()) return Action::None;
            if (m_xmb_col < 0) XmbSyncFromCursor();
            if (m_xmb_col < 0) { m_xmb_col = 0; m_xmb_item = 0; }

            const int ncols = (int)m_xmb_cols.size();
            const int ncur  = (int)m_xmb_cols[m_xmb_col].items.size();

            // Columns clamp at the ends like the handheld's: there are only a
            // few and they are all on screen, so wrapping would just disorient.
            if (b == Btn::Right || b == Btn::Left) {
                const int dir = (b == Btn::Right) ? +1 : -1;
                const int next = m_xmb_col + dir;
                if (next >= 0 && next < ncols) {
                    m_xmb_col  = next;
                    // Land on the entry you were last on in that column would be
                    // nice, but the handheld always opens a column at the top.
                    m_xmb_item = 0;
                    m_xmb_item_scroll = 0.0f;
                    XmbApplyCursor();
                }
                return Action::None;
            }
            // The column does wrap on a fresh press: a library can be hundreds
            // of entries long and reaching the end from the top is otherwise a
            // very long hold. Auto-repeat still stops at the ends.
            if (b == Btn::Down) {
                if (m_xmb_item < ncur - 1)   m_xmb_item++;
                else if (m_nav_fresh)      { m_xmb_item = 0; m_xmb_item_scroll = 0.0f; }
                XmbApplyCursor();
                return Action::None;
            }
            if (b == Btn::Up) {
                if (m_xmb_item > 0)          m_xmb_item--;
                else if (m_nav_fresh)      { m_xmb_item = ncur - 1; m_xmb_item_scroll = (float)m_xmb_item; }
                XmbApplyCursor();
                return Action::None;
            }
            // Shoulders page through the column, matching the other layouts.
            if (b == Btn::R || b == Btn::L) {
                m_xmb_item = std::min(std::max(0, m_xmb_item + (b == Btn::R ? 5 : -5)), ncur - 1);
                XmbApplyCursor();
                return Action::None;
            }
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
                        m_dialog_title.clear();   // default "Close running application?"
                        m_dialog_note.clear();
                        m_dialog = Dialog::ConfirmCloseForLaunch;
                        return Action::None;
                    }
                    out_app_id = it.app_id;
                    return Action::LaunchApp;
                case ItemKind::RandomGame: {
                    const u64 pick = RollRandomGame();   // animates, then lands
                    if (pick == 0) { SetStatus("No games to pick from"); return Action::None; }
                    if (m_suspended != 0 && pick == m_suspended) return Action::ResumeApp;
                    if (m_suspended != 0) {
                        m_pending_launch = pick;
                        m_dialog_cursor  = 1;
                        m_dialog_title.clear();
                        m_dialog_note.clear();
                        m_dialog = Dialog::ConfirmCloseForLaunch;
                        return Action::None;
                    }
                    out_app_id = pick;
                    return Action::LaunchApp;
                }
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
                case ItemKind::HomebrewMenu: OpenHomebrewBrowser(); return Action::None;
                case ItemKind::Homebrew:
                    m_hb_launch_path = it.hb_path;
                    // Run as an application if a donor is set, else as an applet.
                    return m_hb_donor ? Action::LaunchHomebrewApp : Action::LaunchHomebrew;
                case ItemKind::Settings:     return Action::OpenNetConnect;
                case ItemKind::Power:        EnterPower(); return Action::None;
            }
        }
        if (b == Btn::X) {
            BuildOptions();
            m_options_cursor = 0;
            if (!m_options.empty()) m_options_open = true;
            return Action::None;
        }
        if (b == Btn::Plus) { EnterPower(); return Action::None; }
        return Action::None;
    }

    // ---- X "Options" overlay ------------------------------------------------
    namespace { enum { OptFav = 0, OptRename, OptMove, OptUnpinHb, OptSetDonor,
                       OptSort, OptCloseGame, OptDismiss }; }

    void Menu::BuildOptions() {
        m_options.clear();
        if (m_items.empty()) return;
        const MenuItem &it = m_items[m_cursor];
        if (it.kind == ItemKind::Game) {
            m_options.push_back({ IsFavourite(it.app_id) ? T("Remove from Favourites")
                                                         : T("Add to Favourites"), OptFav });
            m_options.push_back({ T("Rename"), OptRename });
            m_options.push_back({ m_hb_donor == it.app_id ? T("Homebrew donor (set)")
                                                          : T("Use as homebrew donor"), OptSetDonor });
        }
        if (it.kind == ItemKind::Homebrew) {
            m_options.push_back({ IsHbFavourite(it.hb_path) ? T("Remove from Favourites")
                                                            : T("Add to Favourites"), OptFav });
            m_options.push_back({ T("Remove from menu"), OptUnpinHb });
        }
        // Any entry can be reordered.
        m_options.push_back({ T("Move"), OptMove });
        if (it.kind == ItemKind::Game && m_suspended != 0 && it.app_id == m_suspended)
            m_options.push_back({ T("Close game"), OptCloseGame });
        m_options.push_back({ std::string(T("Sort: ")) + SortLabel(), OptSort });
        m_options.push_back({ T("Cancel"), OptDismiss });
    }

    const char *Menu::SortLabel() const {
        switch (m_sort) {
            case SortMode::TitleAsc:       return T("Title A - Z");
            case SortMode::TitleDesc:      return T("Title Z - A");
            case SortMode::GamecardFirst:  return T("Game card first");
            case SortMode::RecentlyPlayed: return T("Recently played");
            case SortMode::MostPlayed:     return T("Most played");
            default:                       return T("Default");
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
        const std::string sel_key  = ItemKey(sel);
        const std::string sel_hb   = sel.hb_path;

        switch (m_options[m_options_cursor].action) {
            case OptFav: {
                bool now_fav;
                if (sel_is_game) { now_fav = !IsFavourite(sel_id);   ToggleFavourite(sel_id); }
                else             { now_fav = !IsHbFavourite(sel_hb); ToggleHbFavourite(sel_hb); }
                RebuildItems();
                SelectByKey(sel_key);   // generic: works for games and homebrew
                SetStatus(now_fav ? "Added to Favourites" : "Removed from Favourites");
                m_options_open = false;
                return Action::None;
            }
            case OptRename:
                m_options_open = false;
                RenameSelected();   // shows the software keyboard, then rebuilds
                return Action::None;
            case OptMove:
                // Custom order only takes effect in Default sort.
                if (m_sort != SortMode::Default) { m_sort = SortMode::Default; SaveSort(); RebuildItems(); }
                m_move_mode = true;
                m_move_key  = sel_key;
                SelectByKey(sel_key);
                SetStatus("Move: D-pad to reorder, A to place");
                m_options_open = false;
                return Action::None;
            case OptUnpinHb:
                ToggleHbPin(sel_hb);   // remove from the main menu
                SetStatus("Removed from menu");
                m_options_open = false;
                return Action::None;
            case OptSetDonor:
                m_hb_donor = (m_hb_donor == sel_id) ? 0 : sel_id;   // toggle
                SaveHbDonor();
                SetStatus(m_hb_donor ? "Homebrew donor set (browser Y = run as app)"
                                     : "Homebrew donor cleared");
                m_options_open = false;
                return Action::None;
            case OptSort:
                m_sort = (SortMode)(((int)m_sort + 1) % (int)SortMode::Count);
                SaveSort();
                RebuildItems();
                SelectByKey(sel_key);
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

    // ---- Theming submenu ---------------------------------------------------

    // ---- online update check (opt-out) -------------------------------------
    namespace {
        // Pluck a "key":"value" string field out of the GitHub release JSON.
        std::string JsonStr(const std::string &j, const char *key) {
            std::string pat = std::string("\"") + key + "\"";
            size_t p = j.find(pat); if (p == std::string::npos) return "";
            p = j.find(':', p + pat.size()); if (p == std::string::npos) return "";
            size_t s = j.find('"', p); if (s == std::string::npos) return "";
            size_t e = j.find('"', s + 1); if (e == std::string::npos) return "";
            return j.substr(s + 1, e - s - 1);
        }
        int CmpVer(const char *a, const char *b) {   // >0 if a newer than b; skips a 'v'
            auto parse = [](const char *s, int v[3]) {
                v[0] = v[1] = v[2] = 0;
                if (s && (*s == 'v' || *s == 'V')) s++;
                if (s) sscanf(s, "%d.%d.%d", &v[0], &v[1], &v[2]);
            };
            int va[3], vb[3]; parse(a, va); parse(b, vb);
            for (int i = 0; i < 3; i++) if (va[i] != vb[i]) return va[i] - vb[i];
            return 0;
        }
    }

    void Menu::UpdateCheckTrampoline(void *self) {
        Menu *m = static_cast<Menu *>(self);
        std::string body;
        if (net::Get("https://api.github.com/repos/etonedemid/slaunch/releases/latest",
                     body, 15)) {
            std::string tag = JsonStr(body, "tag_name");
            if (!tag.empty() && CmpVer(tag.c_str(), SL_VERSION) > 0) {
                m->m_upd_latest    = tag;
                m->m_upd_available = true;
            }
        }
        m->m_upd_done.store(true, std::memory_order_release);
    }

    void Menu::StartUpdateCheck() {
        if (!m_check_updates || m_upd_running || m_upd_available) return;
        m_upd_done.store(false, std::memory_order_release);
        if (R_SUCCEEDED(threadCreate(&m_upd_thread, &Menu::UpdateCheckTrampoline, this,
                                     nullptr, 0x8000, 0x3B, -2))) {
            threadStart(&m_upd_thread);
            m_upd_running = true;
        }
    }

    void Menu::PollUpdateCheck() {
        if (!m_upd_running || !m_upd_done.load(std::memory_order_acquire)) return;
        threadWaitForExit(&m_upd_thread);
        threadClose(&m_upd_thread);
        m_upd_running = false;
    }

    // ---- Welcome (after setup / once per boot) -----------------------------
    namespace {
        constexpr u64 kWelcomeMs   = 4600;   // ~the opening jingle's length
        constexpr u64 kWelcomeFade = 520;    // in at the start, out at the end

        // The greeting itself is the only varying text, so the screen stays a
        // greeting and a name rather than a greeting, a name and a slogan.
        const char *kWelcomeMsgs[] = {
            "Welcome home",
            "Good to see you",
            "Ready when you are",
            "Let's play",
        };
        constexpr int kWelcomeMsgN = (int)(sizeof(kWelcomeMsgs) / sizeof(kWelcomeMsgs[0]));
    }

    void Menu::EnterWelcome() {
        m_screen        = Screen::Welcome;
        m_welcome_start = armGetSystemTick();
        m_welcome_msg   = (int)(randomGet64() % kWelcomeMsgN);
    }

    // ---- system entry visibility -------------------------------------------
    // kSysEntries / kSysEntryN live near the top of the file (the touch code
    // needs the row count).

    bool Menu::IsSysHidden(ItemKind k) const {
        if (k == ItemKind::Theming) return false;      // never hideable
        return (m_sys_hidden & (1u << (u32)k)) != 0;
    }

    void Menu::ToggleSysHidden(ItemKind k) {
        if (k == ItemKind::Theming) return;
        m_sys_hidden ^= (1u << (u32)k);
        SaveSysEntries();
        RebuildItems();
    }

    void Menu::LoadSysEntries() {
        m_sys_hidden = 0;
        FILE *fp = fopen("sdmc:/slaunch/config/sysentries.txt", "r");
        if (!fp) return;
        unsigned v = 0;
        if (fscanf(fp, "%u", &v) == 1) m_sys_hidden = (u32)v;
        fclose(fp);
    }

    void Menu::SaveSysEntries() {
        mkdir("sdmc:/slaunch", 0777);
        mkdir("sdmc:/slaunch/config", 0777);
        FILE *fp = fopen("sdmc:/slaunch/config/sysentries.txt", "w");
        if (!fp) return;
        fprintf(fp, "%u\n", (unsigned)m_sys_hidden);
        fclose(fp);
    }

    Menu::Action Menu::OnButtonSysEntries(Btn b) {
        if (b == Btn::B) { m_screen = Screen::Theming; m_sub_scroll = 0; return Action::None; }
        if (b == Btn::Down) m_sys_cursor = (m_sys_cursor + 1) % kSysEntryN;
        if (b == Btn::Up)   m_sys_cursor = (m_sys_cursor + kSysEntryN - 1) % kSysEntryN;
        if (b == Btn::A || b == Btn::Left || b == Btn::Right)
            ToggleSysHidden(kSysEntries[m_sys_cursor].kind);
        return Action::None;
    }

    void Menu::DrawSysEntries() {
        DrawTopBar(T("Menu entries"));
        std::vector<std::string> labels, values;
        for (int i = 0; i < kSysEntryN; i++) {
            labels.push_back(T(kSysEntries[i].name));
            values.push_back(IsSysHidden(kSysEntries[i].kind) ? T("Hidden") : T("Shown"));
        }
        DrawCarousel(labels, values, m_sys_cursor, m_sub_scroll);
        DrawHint("Up/Down: Select   B: Back");
    }

    // ---- Power screen -------------------------------------------------------
    // The daemon is the one that can actually sleep/reboot/shut the console down,
    // so every row here ends up as an SMI command; the menu only picks which one
    // and asks for confirmation first.
    namespace {
        enum { PW_Sleep = 0, PW_Restart, PW_Shutdown, PW_Payload, PW_Back };

        // Where payloads live on a normal Atmosphere/hekate card.
        const char *kPayloadDirs[] = { "sdmc:/payloads", "sdmc:/bootloader/payloads" };
        constexpr const char *kDefaultPayload = "sdmc:/atmosphere/reboot_payload.bin";
    }

    void Menu::ScanPayloads() {
        if (m_payloads_scanned) return;   // cheap, but the dirs cannot change under us
        m_payloads_scanned = true;
        m_payloads.clear();
        m_payload_names.clear();

        std::vector<std::pair<std::string, std::string>> found;   // name, path
        auto add = [&](std::string path, std::string name) {
            for (auto &f : found) if (f.second == path) return;   // listed twice
            found.emplace_back(std::move(name), std::move(path));
        };

        for (const char *dir : kPayloadDirs) {
            DIR *d = opendir(dir);
            if (!d) continue;
            while (dirent *e = readdir(d)) {
                const size_t len = strlen(e->d_name);
                if (len < 5 || strcasecmp(e->d_name + len - 4, ".bin") != 0) continue;
                add(std::string(dir) + "/" + e->d_name, std::string(e->d_name, len - 4));
            }
            closedir(d);
        }
        // The payload Atmosphere itself reboots into, if one is set up.
        struct stat st;
        if (stat(kDefaultPayload, &st) == 0 && st.st_size > 0)
            add(kDefaultPayload, T("Default payload"));

        std::sort(found.begin(), found.end(),
                  [](const auto &a, const auto &b) {
                      return strcasecmp(a.first.c_str(), b.first.c_str()) < 0;
                  });
        for (auto &f : found) {
            m_payload_names.push_back(f.first);
            m_payloads.push_back(f.second);
        }
        if (m_payload_cursor >= (int)m_payloads.size()) m_payload_cursor = 0;
    }

    void Menu::BuildPowerRows() {
        m_power_rows.clear();
        m_power_rows.push_back(PW_Sleep);
        m_power_rows.push_back(PW_Restart);
        m_power_rows.push_back(PW_Shutdown);
        if (!m_payloads.empty()) m_power_rows.push_back(PW_Payload);  // nothing to
        m_power_rows.push_back(PW_Back);                              // chainload -> hide
    }

    void Menu::EnterPower() {
        ScanPayloads();
        BuildPowerRows();
        m_power_cursor = 0;
        m_sub_scroll   = 0;
        m_screen       = Screen::Power;
    }

    void Menu::AskPower(Action act, const char *prompt) {
        m_power_confirm = act;
        m_dialog_title  = T(prompt);
        m_dialog_note   = (m_suspended != 0) ? T("The suspended game will be closed.") : "";
        m_dialog_cursor = 1;      // default to No: none of this is undoable
        m_dialog        = Dialog::ConfirmPower;
    }

    Menu::Action Menu::OnButtonPower(Btn b) {
        if (b == Btn::B) { m_screen = Screen::Main; return Action::None; }
        const int n = (int)m_power_rows.size();
        if (n == 0) return Action::None;
        if (b == Btn::Down) m_power_cursor = (m_power_cursor + 1) % n;
        if (b == Btn::Up)   m_power_cursor = (m_power_cursor + n - 1) % n;
        if (b != Btn::A) return Action::None;

        switch (m_power_rows[m_power_cursor]) {
            case PW_Sleep:
                // The console wakes back into the menu, so leave the main screen up.
                m_screen = Screen::Main;
                return Action::PowerSleep;
            case PW_Restart:  AskPower(Action::PowerReboot,   "Restart the console?");  break;
            case PW_Shutdown: AskPower(Action::PowerShutdown, "Turn the console off?"); break;
            case PW_Payload:
                m_screen         = Screen::Payloads;
                m_payload_cursor = 0;
                m_sub_scroll     = 0;
                break;
            default: m_screen = Screen::Main; break;   // PW_Back
        }
        return Action::None;
    }

    void Menu::DrawPower() {
        DrawTopBar("Power");
        std::vector<std::string> labels, values;
        for (int id : m_power_rows) {
            switch (id) {
                case PW_Sleep:    labels.push_back(T("Sleep"));     values.push_back(""); break;
                case PW_Restart:  labels.push_back(T("Restart"));   values.push_back(""); break;
                case PW_Shutdown: labels.push_back(T("Power off")); values.push_back(""); break;
                case PW_Payload: {
                    char c[32];
                    snprintf(c, sizeof(c), "%d %s", (int)m_payloads.size(), T("found"));
                    labels.push_back(T("Reboot to payload"));
                    values.push_back(c);
                    break;
                }
                default: labels.push_back(T("Back")); values.push_back(""); break;
            }
        }
        DrawCarousel(labels, values, m_power_cursor, m_sub_scroll);
        DrawHint("Up/Down: Select   A: Confirm   B: Back");
    }

    Menu::Action Menu::OnButtonPayloads(Btn b) {
        if (b == Btn::B) { m_screen = Screen::Power; m_sub_scroll = (float)m_power_cursor; return Action::None; }
        const int n = (int)m_payloads.size();
        if (n == 0) return Action::None;
        if (b == Btn::Down) m_payload_cursor = (m_payload_cursor + 1) % n;
        if (b == Btn::Up)   m_payload_cursor = (m_payload_cursor + n - 1) % n;
        if (b == Btn::A) {
            m_payload_path = m_payloads[m_payload_cursor];
            char prompt[128];
            snprintf(prompt, sizeof(prompt), "%s %s?", T("Reboot to"),
                     m_payload_names[m_payload_cursor].c_str());
            AskPower(Action::PowerPayload, prompt);
        }
        return Action::None;
    }

    void Menu::DrawPayloads() {
        DrawTopBar("Reboot to payload");
        if (m_payloads.empty()) {
            const Theme &t = m_theme.Current();
            m_gfx->TextCentered(FontSize::Normal, gfx::Gfx::Width / 2, 340, t.dim,
                                T("No payloads found in sdmc:/payloads"));
            DrawHint("B: Back");
            return;
        }
        std::vector<std::string> values(m_payload_names.size());
        DrawCarousel(m_payload_names, values, m_payload_cursor, m_sub_scroll);
        DrawHint("Up/Down: Select   A: Reboot into it   B: Back");
    }

    // A chainload the daemon could not carry out leaves its reason on the SD;
    // show it once on the next menu start, then drop it.
    void Menu::ShowPowerError() {
        FILE *fp = fopen(sl::smi::PowerErrorPath, "r");
        if (!fp) return;
        char line[160] = {};
        if (fgets(line, sizeof(line), fp)) {
            line[strcspn(line, "\r\n")] = '\0';
            if (line[0]) SetStatus(line);
        }
        fclose(fp);
        remove(sl::smi::PowerErrorPath);
    }

    // ---- Play statistics ----------------------------------------------------
    // Last run's numbers, kept next to the app-list cache. Without them a
    // play-ordered menu would come up title-ordered and visibly reshuffle a few
    // frames later, when the pdm worker lands.
    namespace { constexpr const char *kPlayCachePath = "sdmc:/slaunch/cache/playstats.txt"; }

    void Menu::LoadPlayCache() {
        FILE *fp = fopen(kPlayCachePath, "r");
        if (!fp) return;
        char line[96];
        while (fgets(line, sizeof(line), fp)) {
            unsigned long long id = 0, secs = 0, last = 0;
            unsigned launches = 0;
            if (sscanf(line, "%llX=%llu,%llu,%u", &id, &secs, &last, &launches) != 4) continue;
            if (id == 0) continue;
            play::PlayInfo pi;
            pi.seconds     = secs;
            pi.last_played = last;
            pi.launches    = launches;
            m_play[id] = pi;
        }
        fclose(fp);
    }

    void Menu::SavePlayCache() const {
        mkdir("sdmc:/slaunch", 0777);
        mkdir("sdmc:/slaunch/cache", 0777);
        FILE *fp = fopen(kPlayCachePath, "w");
        if (!fp) return;
        for (auto &kv : m_play)
            fprintf(fp, "%016llX=%llu,%llu,%u\n", (unsigned long long)kv.first,
                    (unsigned long long)kv.second.seconds,
                    (unsigned long long)kv.second.last_played, kv.second.launches);
        fclose(fp);
    }

    const play::PlayInfo *Menu::Play(u64 app_id) const {
        if (app_id == 0) return nullptr;          // homebrew / system entries
        auto it = m_play.find(app_id);
        return (it == m_play.end()) ? nullptr : &it->second;
    }

    void Menu::PlayStatsTrampoline(void *self) {
        Menu *m = static_cast<Menu *>(self);
        play::Query(m->m_play_ids, m->m_play_result);
        m->m_play_done.store(true, std::memory_order_release);
    }

    void Menu::StartPlayStats() {
        if (m_play_running || m_apps.empty()) return;
        m_play_ids.clear();
        m_play_ids.reserve(m_apps.size());
        for (auto &a : m_apps) m_play_ids.push_back(a.app_id);
        m_play_result.clear();
        m_play_dirty = false;
        m_play_done.store(false, std::memory_order_release);
        if (R_SUCCEEDED(threadCreate(&m_play_thread, &Menu::PlayStatsTrampoline, this,
                                     nullptr, 0x8000, 0x3B, -2))) {
            threadStart(&m_play_thread);
            m_play_running = true;
        }
    }

    void Menu::PollPlayStats() {
        if (!m_play_running) {
            // Only after the first frame: play stats are background information and
            // must never hold up the menu appearing.
            if (m_deferred_done && m_play_dirty) StartPlayStats();
            return;
        }
        if (!m_play_done.load(std::memory_order_acquire)) return;
        threadWaitForExit(&m_play_thread);
        threadClose(&m_play_thread);
        m_play_running = false;

        for (size_t i = 0; i < m_play_ids.size() && i < m_play_result.size(); i++)
            m_play[m_play_ids[i]] = m_play_result[i];
        m_play_result.clear();
        SavePlayCache();   // next menu start has these on its first frame

        // The numbers just changed (a game was played since we last looked), so
        // redo the play-based sorts, keeping the cursor on the same entry.
        if (m_sort == SortMode::RecentlyPlayed || m_sort == SortMode::MostPlayed) {
            const std::string key = m_items.empty() ? std::string()
                                                    : ItemKey(m_items[m_cursor]);
            RebuildItems();
            if (!key.empty()) SelectByKey(key);
        }
    }

    // ---- Random game roll ---------------------------------------------------
    void Menu::DrawRollFrame(const std::vector<const MenuItem *> &pool, int idx, bool settled) {
        const Theme &t = m_theme.Current();
        const int cx = gfx::Gfx::Width / 2;
        DrawBackground();

        m_gfx->TextCentered(FontSize::Small, cx, 120, t.dim,
                            settled ? T("Let's play") : T("Picking something..."));

        // Icon card.
        const int size = 220, ix = cx - size / 2, iy = 190;
        m_icons.SetScale(gfx::IconCache::GridScale);
        SDL_Texture *tex = pool[idx]->app_id ? m_icons.Get(pool[idx]->app_id) : nullptr;
        if (tex) m_gfx->DrawImage(tex, ix, iy, size, size, 255);
        else     m_gfx->FillRect(ix, iy, size, size, WithAlpha(t.bg_top, 220));

        const SDL_Color edge = settled ? t.accent : WithAlpha(t.accent, 130);
        m_gfx->FillRect(ix - 4, iy - 4, size + 8, 4, edge);
        m_gfx->FillRect(ix - 4, iy + size, size + 8, 4, edge);
        m_gfx->FillRect(ix - 4, iy, 4, size, edge);
        m_gfx->FillRect(ix + size, iy, 4, size, edge);

        m_gfx->TextCentered(FontSize::Normal, cx, iy + size + 34,
                            settled ? t.accent : t.fg, pool[idx]->name.c_str());
        m_gfx->Present();
    }

    // Rolls through the library with a decelerating tick and returns whatever it
    // lands on (the landing slot is what makes it random). 0 if there are no games.
    u64 Menu::RollRandomGame() {
        std::vector<const MenuItem *> pool;
        for (const auto &i : m_items)
            if (i.kind == ItemKind::Game) pool.push_back(&i);
        if (pool.empty()) return 0;
        const int n = (int)pool.size();

        const u64 freq  = armGetSystemTickFreq();
        const u64 t0    = armGetSystemTick();
        constexpr u64 kRollMs = 2000;
        int  idx      = (int)(randomGet64() % n);
        u64  nextStep = 0;

        for (;;) {
            const u64 ms = (armGetSystemTick() - t0) * 1000 / freq;
            if (ms >= kRollMs) break;
            if (ms >= nextStep) {
                idx = (idx + 1) % n;
                const float p = (float)ms / (float)kRollMs;      // 0..1
                nextStep = ms + (u64)(40.0f + p * p * 300.0f);   // ease out
            }
            DrawRollFrame(pool, idx, false);
        }

        // Hold on the result so it reads before the game launches.
        const u64 t1 = armGetSystemTick();
        while ((armGetSystemTick() - t1) * 1000 / freq < 800)
            DrawRollFrame(pool, idx, true);

        return pool[idx]->app_id;
    }

    Menu::Action Menu::OnButtonWelcome(Btn b) {
        if (b == Btn::A || b == Btn::B || b == Btn::Plus) m_screen = Screen::Main;   // skip
        return Action::None;
    }

    // Two lines and a rule, centred on the screen: a greeting, the user's name,
    // and a hairline that draws itself outward underneath. Everything shares one
    // fade so the screen arrives and leaves as a single object instead of a
    // stack of separately animated parts.
    void Menu::DrawWelcome() {
        const Theme &t = m_theme.Current();
        const int cx = gfx::Gfx::Width / 2;
        const int cy = gfx::Gfx::Height / 2;

        const u64 freq = armGetSystemTickFreq();
        const u64 ms   = (armGetSystemTick() - m_welcome_start) * 1000 / freq;

        // Fade in, hold, fade out. Without the tail the screen used to cut
        // straight to the menu the instant the timer expired.
        float k = 1.0f;
        if (ms < kWelcomeFade)                    k = (float)ms / (float)kWelcomeFade;
        else if (ms > kWelcomeMs - kWelcomeFade)  k = (float)(kWelcomeMs - ms) / (float)kWelcomeFade;
        if (k < 0.0f) k = 0.0f;
        if (k > 1.0f) k = 1.0f;
        const Uint8 a = (Uint8)(k * 255.0f);

        // Ease the rise so the text settles rather than sliding linearly.
        const float ease = 1.0f - (1.0f - k) * (1.0f - k);
        const int   rise = (int)((1.0f - ease) * 16.0f);

        const char *greet = T(kWelcomeMsgs[m_welcome_msg % kWelcomeMsgN]);
        const bool  named = (m_nickname[0] != '\0');

        if (named) {
            m_gfx->TextCentered(FontSize::Small, cx, cy - 74 + rise, WithAlpha(t.dim, a), greet);
            m_gfx->TextCentered(FontSize::Title, cx, cy - 38 + rise, WithAlpha(t.title, a), m_nickname);
        } else {
            // No account name to show, so the greeting carries the screen alone.
            m_gfx->TextCentered(FontSize::Title, cx, cy - 38 + rise, WithAlpha(t.title, a), greet);
        }

        const int lw = (int)(200.0f * ease);
        m_gfx->FillRect(cx - lw / 2, cy + 40, lw, 2, WithAlpha(t.accent, a));
    }

    // ---- About + changelog -------------------------------------------------
    namespace {
        struct LogLine { bool head; const char *text; };
        // Newest first. Headers are version tags; the rest are one-line summaries.
        const LogLine kChangelog[] = {
            { true,  "v0.8.0" },
            { false, "XMB mode - the PSP cross-media bar, with touch" },
            { false, "Closing a homebrew returns to the menu instead of relaunching it" },
            { false, "Reboot and shutdown fixed (no more half-asleep console)" },
            { false, "Icon packs (Theming > Icon pack) + bundled Minimal pack" },
            { false, "Translations: Russian, Japanese, German, Spanish, Chinese" },
            { false, "Korean/Chinese consoles now use their own system font" },
            { false, "Theme colour for the icon background; drag scrolls the content" },
            { true,  "v0.7.0" },
            { false, "Welcome screen that greets you by name on boot and after setup" },
            { false, "Show or hide any system entry (Theming > Menu entries)" },
            { false, "Random game entry - rolls through your library and picks one" },
            { false, "Setup restyled to match the rest of the menu" },
            { true,  "v0.6.0" },
            { false, "About screen with this changelog" },
            { false, "Optional update check on startup (opt-out in setup / Theming)" },
            { false, "Installer: check + install updates online, reboot button" },
            { false, "Installer: full uninstall, keeps your settings on reinstall" },
            { true,  "v0.5.0" },
            { false, "Homebrew runs as a full application via a donor game (crash fixed)" },
            { false, "Favourite homebrew - it sits up top with your favourite games" },
            { false, "Homebrew icons cached + loaded instantly, no more menu hang" },
            { false, "Fixed the ~2s freeze on every menu appearance" },
            { false, "Fixed widget flickering; Web Browser opens Google" },
            { true,  "v0.4.0" },
            { false, "Background music from the SD, resumes where it left off" },
            { false, "UI sounds; homebrew browser (pin, launch as applet or app)" },
            { false, "Reorder any entry, more sorting options, reworked setup" },
            { true,  "v0.3.1" },
            { false, "Localization, faster boot/suspend, Shelf UI mode" },
            { true,  "v0.2.0" },
            { false, "Lua widgets, app icons, extra UI modes" },
        };
        constexpr int kChangelogN = (int)(sizeof(kChangelog) / sizeof(kChangelog[0]));
        constexpr int kAboutTop = 300, kAboutRowH = 34, kAboutVisible = 9;
    }

    Menu::Action Menu::OnButtonAbout(Btn b) {
        const int maxScroll = std::max(0, kChangelogN - kAboutVisible);
        if (b == Btn::Down) m_about_scroll = std::min(maxScroll, m_about_scroll + 1);
        if (b == Btn::Up)   m_about_scroll = std::max(0, m_about_scroll - 1);
        if (b == Btn::B) { m_screen = Screen::Theming; m_sub_scroll = TH_About; }
        return Action::None;
    }

    void Menu::DrawAbout() {
        const Theme &t = m_theme.Current();
        DrawTopBar("About");
        const int cx = gfx::Gfx::Width / 2;

        m_gfx->TextCentered(FontSize::Large, cx, 92, t.title, "sLaunch");

        char ver[80];
        snprintf(ver, sizeof(ver), "%s v%s", T("Version"), SL_VERSION);
        if (m_upd_available) {
            char up[140];
            snprintf(up, sizeof(up), "%s  -  %s %s", ver, T("update available:"), m_upd_latest.c_str());
            m_gfx->TextCentered(FontSize::Small, cx, 158, t.accent, up);
        } else {
            char line[140];
            snprintf(line, sizeof(line), "%s  -  %s", ver,
                     m_check_updates ? T("up to date") : T("update check off"));
            m_gfx->TextCentered(FontSize::Small, cx, 158, t.dim, line);
        }
        m_gfx->TextCentered(FontSize::Small, cx, 190, t.dim, "github.com/etonedemid/slaunch");

        // "What's new" changelog (scrollable).
        const int lx = cx - 360;
        m_gfx->Text(FontSize::Normal, lx, 246, t.accent, T("What's new"));
        m_gfx->FillRect(lx, 284, 720, 2, WithAlpha(t.dim, 90));

        for (int i = 0; i < kAboutVisible; i++) {
            const int idx = m_about_scroll + i;
            if (idx >= kChangelogN) break;
            const LogLine &l = kChangelog[idx];
            const int y = kAboutTop + i * kAboutRowH;
            if (l.head) m_gfx->Text(FontSize::Normal, lx, y, t.fg, l.text);
            else        m_gfx->Text(FontSize::Small, lx + 24, y + 4, t.dim, l.text);
        }

        // Scroll affordances.
        if (m_about_scroll > 0)
            m_gfx->TextCentered(FontSize::Small, cx, kAboutTop - 26, t.dim, "^");
        if (m_about_scroll + kAboutVisible < kChangelogN)
            m_gfx->TextCentered(FontSize::Small, cx, kAboutTop + kAboutVisible * kAboutRowH, t.dim, "v");

        DrawHint("Up/Down: Scroll   B: Back");
    }

    Menu::Action Menu::OnButtonTheming(Btn b) {
        auto openWidgets = [&]() {
            m_screen = Screen::Widgets;
            m_widget_cursor = 0;
            m_sub_scroll = 0;
        };
        if (b == Btn::Down) m_theming_cursor = (m_theming_cursor + 1) % TH_Count;
        if (b == Btn::Up)   m_theming_cursor = (m_theming_cursor + TH_Count - 1) % TH_Count;
        auto cycleUiMode = [&]() {
            const int dir = (b == Btn::Right) ? +1 : -1;
            const int n = (int)UiMode::Count;
            m_ui_mode = (UiMode)(((int)m_ui_mode + dir + n) % n);
            // A deliberate switch into XMB keeps the entry you were on, rather
            // than resetting the bar to its Games default.
            XmbSyncFromCursor(); m_xmb_placed = true;
            SaveSettings();
        };
        auto cycleAlign = [&]() {
            const int dir = (b == Btn::Right) ? +1 : -1;
            m_align = (TextAlign)(((int)m_align + dir + 3) % 3);
            SaveSettings();
        };
        auto toggleListIcons = [&]() { m_list_icons = !m_list_icons; SaveSettings(); };
        // Index 0 is the built-in set; 1.. are the folders found under
        // sdmc:/slaunch/icon_packs. Switching drops the cached textures so the
        // new art shows up on the next frame.
        auto cycleIconPack = [&](int dir) {
            const int n = (int)m_icon_packs.size() + 1;
            m_icon_pack_idx = (m_icon_pack_idx + dir + n) % n;
            InvalidateSysIcons();
            SaveIconPackSetting();
        };
        auto toggleUpdates = [&]() { m_check_updates = !m_check_updates; SaveSettings(); };
        if (m_theming_cursor == TH_Updates && (b == Btn::Left || b == Btn::Right))
            toggleUpdates();
        auto toggleWelcome = [&]() { m_welcome_enabled = !m_welcome_enabled; SaveSettings(); };
        if (m_theming_cursor == TH_Welcome && (b == Btn::Left || b == Btn::Right))
            toggleWelcome();
        if (m_theming_cursor == TH_UiMode && (b == Btn::Left || b == Btn::Right))
            cycleUiMode();
        if (m_theming_cursor == TH_TextPos && (b == Btn::Left || b == Btn::Right))
            cycleAlign();
        if (m_theming_cursor == TH_ListIcons && (b == Btn::Left || b == Btn::Right))
            toggleListIcons();
        if (m_theming_cursor == TH_IconPack && (b == Btn::Left || b == Btn::Right))
            cycleIconPack(b == Btn::Right ? +1 : -1);
        if (b == Btn::A) {
            switch (m_theming_cursor) {
                case TH_Themes:      m_screen = Screen::Themes;       m_theme_cursor = m_theme.CurrentIndex(); m_sub_scroll = m_theme_cursor; break;
                case TH_Fonts:       m_screen = Screen::Fonts;        m_font_cursor = m_font_applied; m_sub_scroll = m_font_cursor; break;
                case TH_Music:       m_screen = Screen::Music;        m_music_cursor = 0; m_sub_scroll = 0; break;
                case TH_Widgets:     openWidgets(); break;
                case TH_Entries:     m_screen = Screen::SysEntries;   m_sys_cursor = 0; m_sub_scroll = 0; break;
                case TH_IconPack:    cycleIconPack(+1); break;
                case TH_Welcome:     toggleWelcome(); break;
                case TH_Updates:     toggleUpdates(); break;
                case TH_About:       m_screen = Screen::About;        m_about_scroll = 0; break;
                case TH_Back:        m_screen = Screen::Main; break;
                default: break; // UI mode / text pos / list icons: left/right already handles them
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

    // Theme-editor rows (EF_*) and their visibility predicates live near the top
    // of the file; the touch code needs them. This is just the colour lookup.
    namespace {
    static SDL_Color *EditorColor(Theme &c, int row) {
        switch (row) {
            case EF_Top:       return &c.bg_top;
            case EF_Bottom:    return &c.bg_bottom;
            case EF_Text:      return &c.fg;
            case EF_Accent:    return &c.accent;
            case EF_Secondary: return &c.dim;
            case EF_Title:     return &c.title;
            case EF_IconBg:    return &c.icon_bg;
            default:           return nullptr;
        }
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

    // ---- Icon packs --------------------------------------------------------
    // Discover user icon packs: each subdirectory of sdmc:/slaunch/icon_packs/
    // is treated as a pack. The pack name is the directory name.
    void Menu::ScanIconPacks() {
        m_icon_packs.clear();
        DIR *d = opendir("sdmc:/slaunch/icon_packs");
        if (!d) return;
        struct dirent *e;
        while ((e = readdir(d)) != nullptr) {
            if (e->d_type == DT_DIR && e->d_name[0] != '.') {
                m_icon_packs.push_back(e->d_name);
            }
        }
        closedir(d);
        std::sort(m_icon_packs.begin(), m_icon_packs.end());
    }

    void Menu::LoadIconPackSetting() {
        FILE *fp = fopen("sdmc:/slaunch/config/icon_pack.txt", "r");
        if (!fp) return;
        char line[64];
        while (fgets(line, sizeof(line), fp)) {
            int v = 0;
            if (sscanf(line, "icon_pack=%d", &v) == 1)
                m_icon_pack_idx = v;
        }
        fclose(fp);
        // Clamp to valid range (0 = built-in, 1..N = packs)
        const int max_idx = (int)m_icon_packs.size();
        if (m_icon_pack_idx < 0) m_icon_pack_idx = 0;
        if (m_icon_pack_idx > max_idx) m_icon_pack_idx = 0;
    }

    void Menu::SaveIconPackSetting() {
        mkdir("sdmc:/slaunch", 0777);
        mkdir("sdmc:/slaunch/config", 0777);
        FILE *fp = fopen("sdmc:/slaunch/config/icon_pack.txt", "w");
        if (!fp) return;
        fprintf(fp, "icon_pack=%d\n", m_icon_pack_idx);
        fclose(fp);
    }

    void Menu::CycleBackground(int dir) {
        if (!m_theme.IsCustom(m_editing_theme)) return;
        Theme &c = m_theme.CustomAt(m_editing_theme);
        const int maxStyle = 2; // Gradient <-> Ribbon
        c.background_style = (c.background_style + dir + maxStyle) % maxStyle;
        m_theme.Select(m_editing_theme);
        m_theme_cursor    = m_editing_theme;
        m_wallpaper_theme = -1;
    }

    // Cycle the optional photo overlay (independent of background style).
    // Rotates: empty -> first wallpaper -> next wallpaper -> ... -> empty.
    void Menu::CycleWallpaper(int dir) {
        if (!m_theme.IsCustom(m_editing_theme)) return;
        Theme &c = m_theme.CustomAt(m_editing_theme);
        if (m_wallpapers.empty()) {
            c.wallpaper[0] = '\0';
            return;
        }
        // Find current index (or -1 if empty).
        int idx = -1;
        for (int i = 0; i < (int)m_wallpapers.size(); i++) {
            if (m_wallpapers[i] == c.wallpaper) { idx = i; break; }
        }
        int n = (int)m_wallpapers.size() + 1; // +1 for the empty slot
        idx = (idx + 1 + dir + n) % n;        // +1 because idx=-1 maps to slot 0
        if (idx == 0) {
            c.wallpaper[0] = '\0';
        } else {
            strncpy(c.wallpaper, m_wallpapers[idx - 1].c_str(), sizeof(c.wallpaper) - 1);
            c.wallpaper[sizeof(c.wallpaper) - 1] = '\0';
        }
        m_theme.Select(m_editing_theme);
        m_theme_cursor    = m_editing_theme;
        m_wallpaper_theme = -1;
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

        if (b == Btn::Down) {
            m_edit_cursor = (m_edit_cursor + 1) % EF_Count;
            auto skipHidden = [&](int &cursor, int dir) {
                while (IsRibbonRow(cursor) && c.background_style != BackgroundStyle_Ribbon) {
                    cursor = (cursor + dir + EF_Count) % EF_Count;
                }
                while (cursor == EF_WallpaperFps && !IsVideoPath(c.wallpaper)) {
                    cursor = (cursor + dir + EF_Count) % EF_Count;
                }
                while (IsBlurRadiusRow(cursor) && !c.wallpaper_blur) {
                    cursor = (cursor + dir + EF_Count) % EF_Count;
                }
            };
            skipHidden(m_edit_cursor, +1);
        }
        if (b == Btn::Up) {
            m_edit_cursor = (m_edit_cursor + EF_Count - 1) % EF_Count;
            auto skipHidden = [&](int &cursor, int dir) {
                while (IsRibbonRow(cursor) && c.background_style != BackgroundStyle_Ribbon) {
                    cursor = (cursor + dir + EF_Count) % EF_Count;
                }
                while (cursor == EF_WallpaperFps && !IsVideoPath(c.wallpaper)) {
                    cursor = (cursor + dir + EF_Count) % EF_Count;
                }
                while (IsBlurRadiusRow(cursor) && !c.wallpaper_blur) {
                    cursor = (cursor + dir + EF_Count) % EF_Count;
                }
            };
            skipHidden(m_edit_cursor, -1);
        }

        if (m_edit_cursor == EF_Background) {
            if (b == Btn::Right) CycleBackground(+1);
            if (b == Btn::Left)  CycleBackground(-1);
        }
        if (m_edit_cursor == EF_Wallpaper) {
            if (b == Btn::Right) CycleWallpaper(+1);
            if (b == Btn::Left)  CycleWallpaper(-1);
        }
        // Toggle individual effects with left/right/A.
        if (m_edit_cursor == EF_WallpaperDim) {
            if (b == Btn::Right || b == Btn::Left || b == Btn::A) c.wallpaper_dim = !c.wallpaper_dim;
        }
        if (m_edit_cursor == EF_WallpaperBlur) {
            if (b == Btn::Right || b == Btn::Left || b == Btn::A) c.wallpaper_blur = !c.wallpaper_blur;
        }
        if (m_edit_cursor == EF_WallpaperBlurRadius) {
            if (b == Btn::Right) c.wallpaper_blur_radius = (c.wallpaper_blur_radius < 32) ? c.wallpaper_blur_radius + 2 : 2;
            if (b == Btn::Left)  c.wallpaper_blur_radius = (c.wallpaper_blur_radius >  2) ? c.wallpaper_blur_radius - 2 : 32;
        }
        if (m_edit_cursor == EF_WallpaperSnow) {
            if (b == Btn::Right || b == Btn::Left || b == Btn::A) c.wallpaper_snow = !c.wallpaper_snow;
        }
        if (m_edit_cursor == EF_WallpaperFps) {
            if (b == Btn::Right) c.wallpaper_fps = (c.wallpaper_fps < 30) ? c.wallpaper_fps + 1 : 1;
            if (b == Btn::Left)  c.wallpaper_fps = (c.wallpaper_fps >  1) ? c.wallpaper_fps - 1 : 30;
        }

        // Adjust ribbon parameters with left/right.
        if (c.background_style == BackgroundStyle_Ribbon) {
            if (m_edit_cursor == EF_RibbonLines) {
                if (b == Btn::Right) c.ribbon_line_count  = (c.ribbon_line_count  < 40) ? c.ribbon_line_count  + 1 : 1;
                if (b == Btn::Left)  c.ribbon_line_count  = (c.ribbon_line_count  >  1) ? c.ribbon_line_count  - 1 : 40;
            }
            if (m_edit_cursor == EF_RibbonThickness) {
                if (b == Btn::Right) c.ribbon_thickness   = (c.ribbon_thickness   < 20) ? c.ribbon_thickness   + 1 : 1;
                if (b == Btn::Left)  c.ribbon_thickness   = (c.ribbon_thickness   >  1) ? c.ribbon_thickness   - 1 : 20;
            }
            if (m_edit_cursor == EF_RibbonAmplitude) {
                if (b == Btn::Right) c.ribbon_amplitude   = (c.ribbon_amplitude   < 60) ? c.ribbon_amplitude   + 1 : 5;
                if (b == Btn::Left)  c.ribbon_amplitude   = (c.ribbon_amplitude   >  5) ? c.ribbon_amplitude   - 1 : 60;
            }
            if (m_edit_cursor == EF_RibbonSeed) {
                if (b == Btn::Right) c.ribbon_seed = (c.ribbon_seed < 99) ? c.ribbon_seed + 1 : 0;
                if (b == Btn::Left)  c.ribbon_seed = (c.ribbon_seed >  0) ? c.ribbon_seed - 1 : 99;
            }
            if (m_edit_cursor == EF_RibbonLayers) {
                if (b == Btn::Right) c.ribbon_layers = (c.ribbon_layers < 4) ? c.ribbon_layers + 1 : 1;
                if (b == Btn::Left)  c.ribbon_layers = (c.ribbon_layers > 1) ? c.ribbon_layers - 1 : 4;
            }
            if (m_edit_cursor == EF_RibbonYCenter) {
                if (b == Btn::Right) c.ribbon_y_center = (c.ribbon_y_center < 680) ? c.ribbon_y_center + 10 : 40;
                if (b == Btn::Left)  c.ribbon_y_center = (c.ribbon_y_center >  40) ? c.ribbon_y_center - 10 : 680;
            }
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
                // .ttc is a TrueType collection; FreeType opens face 0, which is
                // what the bundled CJK font wants.
                const char *ext = name + len - 4;
                if (strcasecmp(ext, ".ttf") != 0 && strcasecmp(ext, ".otf") != 0 &&
                    strcasecmp(ext, ".ttc") != 0) continue;

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
            if (!yes) { m_pending_launch = 0; m_power_confirm = Action::None; return Action::None; }
            if (which == Dialog::ConfirmCloseForLaunch) {
                out_app_id = m_pending_launch;
                m_pending_launch = 0;
                return Action::LaunchApp;
            }
            if (which == Dialog::ConfirmCloseGame) return Action::TerminateApp;
            if (which == Dialog::ConfirmPower) {
                const Action act = m_power_confirm;
                m_power_confirm  = Action::None;
                return act;
            }
        }
        return Action::None;
    }

    // =========================================================================
    // CPU Gaussian blur -- reads an image file as SDL_Surface (CPU memory),
    // applies a separable Gaussian convolution, uploads the result as a
    // new texture.
    //
    // sigma is derived from wallpaper_blur_radius (2-32) as sigma = radius/2
    // so sigma in [1 .. 16].  Kernel half-width = ceil(3*sigma) which covers
    // +/-3sigma of the distribution.  The kernel is normalised to 2^16 so we use
    // fixed-point arithmetic and shift right by 16 at the end -- no float
    // math needed in the inner loop.
    SDL_Texture *Menu::BlurImage(const char *path) {
        if (!path) return nullptr;

        // Load as SDL_Surface -- always CPU-accessible, unlike SDL_Texture.
        SDL_Surface *surface = IMG_Load(path);
        if (!surface) return nullptr;

        int sw = surface->w;
        int sh = surface->h;

        const Theme &t = m_theme.Current();
        int radius = t.wallpaper_blur_radius;
        if (radius < 2)  radius = 2;
        if (radius > 32) radius = 32;
        float sigma = radius / 2.0f;  // 1 .. 16

        // ---- Build Gaussian kernel (fixed-point, Q16) ----
        int half = (int)lroundf(3.0f * sigma) + 1;  // +/-3sigma coverage
        if (half < 1) half = 1;
        const int kernel_size = 2 * half + 1;
        std::vector<uint16_t> kernel(kernel_size);
        double sum = 0;
        for (int i = -half; i <= half; i++) {
            double w = exp(-0.5 * i * i / (sigma * sigma));
            kernel[i + half] = (uint16_t)(w * 65536.0 + 0.5);
            sum += w;
        }
        // Normalise so weights sum to 65536 (= 1.0 in Q16)
        for (int i = 0; i < kernel_size; i++)
            kernel[i] = (uint16_t)(kernel[i] * 65536.0 / sum + 0.5);

        // ---- Convert surface to RGBA8888 buffer ----
        const int np = sw * sh;
        std::vector<uint8_t> rgba(np * 4);
        {
            SDL_Surface *conv = SDL_ConvertSurfaceFormat(surface, SDL_PIXELFORMAT_RGBA8888, 0);
            if (conv) {
                memcpy(rgba.data(), conv->pixels, (size_t)np * 4);
                SDL_FreeSurface(conv);
            } else {
                // Fallback: manual pixel copy (may be wrong channel order)
                memcpy(rgba.data(), surface->pixels, (size_t)sh * surface->pitch);
            }
        }
        SDL_FreeSurface(surface);

        // ---- Separable Gaussian convolution ----
        std::vector<uint8_t> tmp(np * 4);

        // Horizontal pass: rgba -> tmp
        for (int y = 0; y < sh; y++) {
            for (int x = 0; x < sw; x++) {
                uint32_t acc[4] = {0};
                for (int k = -half; k <= half; k++) {
                    int sx = x + k;
                    if (sx < 0) sx = 0;
                    if (sx >= sw) sx = sw - 1;
                    uint16_t w = kernel[k + half];
                    const uint8_t *px = &rgba[(y * sw + sx) * 4];
                    acc[0] += px[0] * w;
                    acc[1] += px[1] * w;
                    acc[2] += px[2] * w;
                    acc[3] += px[3] * w;
                }
                uint8_t *dst = &tmp[(y * sw + x) * 4];
                dst[0] = (uint8_t)(acc[0] >> 16);
                dst[1] = (uint8_t)(acc[1] >> 16);
                dst[2] = (uint8_t)(acc[2] >> 16);
                dst[3] = (uint8_t)(acc[3] >> 16);
            }
        }

        // Vertical pass: tmp -> rgba
        for (int y = 0; y < sh; y++) {
            for (int x = 0; x < sw; x++) {
                uint32_t acc[4] = {0};
                for (int k = -half; k <= half; k++) {
                    int sy = y + k;
                    if (sy < 0) sy = 0;
                    if (sy >= sh) sy = sh - 1;
                    uint16_t w = kernel[k + half];
                    const uint8_t *px = &tmp[(sy * sw + x) * 4];
                    acc[0] += px[0] * w;
                    acc[1] += px[1] * w;
                    acc[2] += px[2] * w;
                    acc[3] += px[3] * w;
                }
                uint8_t *dst = &rgba[(y * sw + x) * 4];
                dst[0] = (uint8_t)(acc[0] >> 16);
                dst[1] = (uint8_t)(acc[1] >> 16);
                dst[2] = (uint8_t)(acc[2] >> 16);
                dst[3] = (uint8_t)(acc[3] >> 16);
            }
        }

        // ---- Upload result ----
        SDL_Renderer *rend = m_gfx->Renderer();
        SDL_Texture *dst = SDL_CreateTexture(rend, SDL_PIXELFORMAT_RGBA8888,
                                             SDL_TEXTUREACCESS_STATIC, sw, sh);
        if (!dst) return nullptr;
        SDL_UpdateTexture(dst, nullptr, rgba.data(), sw * 4);
        SDL_SetTextureBlendMode(dst, SDL_BLENDMODE_BLEND);
        return dst;
    }

    // =========================================================================
    // Rendering
    void Menu::EnsureWallpaper() {
        int idx = m_theme.CurrentIndex();
        const Theme &t = m_theme.Current();

        auto reloadBlur = [&]() {
            if (m_wallpaper_blur) { m_gfx->FreeImage(m_wallpaper_blur); m_wallpaper_blur = nullptr; }
            if (t.wallpaper_blur && !m_wallpaper_path.empty())
                m_wallpaper_blur = BlurImage(m_wallpaper_path.c_str());
        };

        // Check if wallpaper path changed -> reload
        if (idx != m_wallpaper_theme) {
            if (m_wallpaper) { m_gfx->FreeImage(m_wallpaper); m_wallpaper = nullptr; }
            if (m_wallpaper_blur) { m_gfx->FreeImage(m_wallpaper_blur); m_wallpaper_blur = nullptr; }
            m_wallpaper_path.clear();
            m_wallpaper_theme = idx;
            m_video_frames.clear();
            m_video_frame_idx = 0;
            m_video_frame_tick = 0;

            if (g_sd_ok && t.wallpaper[0]) {
                // Check if it's a directory (video frame sequence)
                struct stat st;
                if (stat(t.wallpaper, &st) == 0 && S_ISDIR(st.st_mode)) {
                    DIR *d = opendir(t.wallpaper);
                    if (d) {
                        struct dirent *e;
                        while ((e = readdir(d)) != nullptr) {
                            const char *name = e->d_name;
                            size_t len = strlen(name);
                            if (len < 5) continue;
                            const char *e4 = name + len - 4;
                            const char *e5 = len >= 5 ? name + len - 5 : "";
                            if (strcasecmp(e4, ".jpg") == 0 || strcasecmp(e4, ".png") == 0 ||
                                strcasecmp(e4, ".bmp") == 0 || strcasecmp(e5, ".jpeg") == 0)
                                m_video_frames.push_back(std::string(t.wallpaper) + "/" + name);
                        }
                        closedir(d);
                        std::sort(m_video_frames.begin(), m_video_frames.end());
                    }
                }
                // If not a directory or directory was empty, load as single image
                if (m_video_frames.empty()) {
                    m_wallpaper_path = t.wallpaper;
                    m_wallpaper = m_gfx->LoadImage(t.wallpaper);
                } else {
                    m_wallpaper_path = m_video_frames[0];
                    m_wallpaper = m_gfx->LoadImage(m_video_frames[0].c_str());
                }
                reloadBlur();
            }
        } else {
            // Same theme - advance video frame if configured
            if (!m_video_frames.empty()) {
                const u64 now = armGetSystemTick();
                const int fps = t.wallpaper_fps;
                const u64 interval = armGetSystemTickFreq() / (fps > 0 ? fps : 10);
                if (now - m_video_frame_tick >= interval) {
                    m_video_frame_tick = now;
                    m_video_frame_idx = (m_video_frame_idx + 1) % (int)m_video_frames.size();
                    m_wallpaper_path = m_video_frames[m_video_frame_idx];
                    if (m_wallpaper) { m_gfx->FreeImage(m_wallpaper); m_wallpaper = nullptr; }
                    m_wallpaper = m_gfx->LoadImage(m_video_frames[m_video_frame_idx].c_str());
                    reloadBlur();
                }
            }
        }
    }

    namespace {
        // =========================================================================
        // PS3 XMB-style ribbon background: several translucent horizontal lines
        // that undulate slowly in a sinusoidal fashion, flowing across the screen.
        //
        // Each "line" is a thin filled band whose Y position follows a sine wave
        // over both X (spatial) and time (temporal).  The band is drawn as a series
        // of small horizontal segments so the wave looks smooth.
        // =========================================================================

        void DrawRibbonBackground(gfx::Gfx *gfx, const Theme &t) {
            const int W = gfx::Gfx::Width;
            const int H = gfx::Gfx::Height;
            const float elapsed = (float)armGetSystemTick() / (float)armGetSystemTickFreq();

            // Pre-compute the three palette colors.
            SDL_Color colors[3];
            colors[0] = t.accent; // accent
            colors[1] = SDL_Color{255, 255, 255, 0}; // white wash
            colors[2] = t.dim;    // dim tint

            // Clamp theme parameters to safe ranges.
            int numLines   = t.ribbon_line_count;
            int thickness  = t.ribbon_thickness;
            int amplitude  = t.ribbon_amplitude;
            int seed       = t.ribbon_seed;
            int layers     = t.ribbon_layers;
            int y_center    = t.ribbon_y_center;
            if (numLines   <  1) numLines   =  1;
            if (numLines   > 40) numLines   = 40;
            if (thickness  <  1) thickness  =  1;
            if (thickness  >  20) thickness  =  20;
            if (amplitude  <  1) amplitude  =  1;
            if (amplitude  > 60) amplitude  = 60;
            if (seed       <  0) seed       =  0;
            if (seed      > 99) seed       = 99;
            if (layers     <  1) layers     =  1;
            if (layers     >  4) layers     =  4;
            if (y_center   <  40) y_center   =  40;
            if (y_center  > 680) y_center   = 680;

            const int step = 4;
            const float margin = 40.0f;
            const float usableH = (float)(H - 2 * margin);

            // Multi-layer rendering: each layer adds a full pass of ribbon lines
            // with a phase offset derived from the seed, creating XMB-style depth.
            const float layerAlphaDiv = 1.0f / (float)layers;

            for (int L = 0; L < layers; L++) {
                float layerPhase = (float)L * 1.618f + (float)seed * 0.37f;

                for (int li = 0; li < numLines; li++) {
                // Which color layer: 0=accent, 1=white-wash, 2=dim
                // Distribute: ~55% accent, ~30% white-wash, ~15% dim
                int colorIdx;
                if (li < (numLines * 55 + 49) / 100)       colorIdx = 0;
                else if (li < (numLines * 85 + 49) / 100)  colorIdx = 1;
                else                                       colorIdx = 2;

                SDL_Color c = colors[colorIdx];

                // Base alpha divided by layer count to avoid washout when stacking.
                float baseAlpha;
                switch (colorIdx) {
                    case 0: baseAlpha = 45.0f; break;
                    case 1: baseAlpha = 18.0f; break;
                    default: baseAlpha = 12.0f; break;
                }
                baseAlpha *= layerAlphaDiv;

                // Slow alpha breathing per line.
                float pulseFreq = 0.04f + (li % 5) * 0.015f;
                baseAlpha *= (0.7f + 0.3f * sinf((elapsed * pulseFreq + li * 0.7f) * 6.2831853f));
                c.a = (Uint8)baseAlpha;

                // Vertical position: pseudo-random spread around y_center.
                // Uses a deterministic hash so the layout is stable per frame
                // but lines don't cluster evenly by index.
                float hash = sinf(li * 127.1f + seed * 317.0f) * 43758.5453f;
                hash = hash - (int)hash; // fraction 0..1
                float spread = usableH * 0.45f; // half-spread around center
                float baseY = (float)y_center + (hash - 0.5f) * 2.0f * spread;

                // Seed perturbs spatial frequency so different seeds look distinct.
                float seedSpatial = (float)seed * 0.05f;
                float spatialFreq = 0.8f + 0.6f * sinf(li * 1.3f + 0.5f + seedSpatial);

                // Seed perturbs temporal frequency too.
                float seedTemporal = (float)seed * 0.002f;
                float temporalFreq;
                switch (colorIdx) {
                    case 0: temporalFreq = 0.06f + 0.03f * sinf(li * 0.9f) + seedTemporal; break;
                    case 1: temporalFreq = 0.08f + 0.04f * sinf(li * 1.1f) + seedTemporal; break;
                    default: temporalFreq = 0.04f + 0.02f * sinf(li * 0.7f) + seedTemporal; break;
                }
                if (temporalFreq < 0.03f) temporalFreq = 0.03f;

                // Phase offset includes layer offset for depth.
                float phase = (float)li / (float)numLines + 0.1f + layerPhase;

                // Line-specific amplitude variation.
                float lineAmp = (float)amplitude * (0.7f + 0.3f * sinf(li * 1.7f + 2.0f));

                // Draw the wave as a chain of thin horizontal segments.
                for (int x = 0; x < W; x += step) {
                    float nx = (float)x / (float)W; // normalized 0..1
                    float wave = sinf((nx * spatialFreq + elapsed * temporalFreq + phase) * 6.2831853f);
                    int y = (int)(baseY + wave * lineAmp);
                    gfx->FillRect(x, y, step, thickness, c);
                }
            }
        }
    }
    }

    void Menu::DrawBackground() {
        const Theme &t = m_theme.Current();
        m_gfx->GradientV(t.bg_top, t.bg_bottom);

        // Wallpaper first (when set), so ribbons draw on top.
        EnsureWallpaper();
        if (m_wallpaper) {
            const int W = gfx::Gfx::Width;
            const int H = gfx::Gfx::Height;

            // Draw the wallpaper (blurred if that toggle is on).
            if (t.wallpaper_blur && m_wallpaper_blur) {
                m_gfx->DrawCover(m_wallpaper_blur, 255);
            } else {
                m_gfx->DrawCover(m_wallpaper, 255);
            }

            // Dim overlay (independent toggle).
            if (t.wallpaper_dim) {
                m_gfx->FillRect(0, 0, W, H, SDL_Color{0,0,0,90});
            }

            // Snow overlay (independent toggle).
            if (t.wallpaper_snow) {
                const float elapsed = (float)armGetSystemTick() / (float)armGetSystemTickFreq();
                const int count = 120;
                for (int i = 0; i < count; i++) {
                    const float seed = (float)i * 7.77f;
                    const int baseX = (int)((sinf(seed * 3.1f) * 0.5f + 0.5f) * (float)W);
                    const float speed = 15.0f + sinf(seed * 2.3f) * 10.0f;
                    const float drift = sinf(seed * 5.1f) * 30.0f;
                    const int sz = 1 + (int)((sinf(seed * 1.7f) * 0.5f + 0.5f) * 3.0f);
                    const Uint8 baseA = (Uint8)(60 + (int)((sinf(seed * 4.3f) * 0.5f + 0.5f) * 140));
                    const float yf = fmodf(elapsed * speed + seed * 100.0f, (float)(H + 40)) - 20.0f;
                    const int y = (int)yf;
                    const int x = baseX + (int)(sinf(elapsed * 0.5f + seed) * drift);
                    if (y < 0 || y >= H) continue;
                    m_gfx->FillRect(x, y, sz, sz, SDL_Color{255, 255, 255, baseA});
                }
            }
        }

        if (t.background_style == BackgroundStyle_Ribbon) {
            DrawRibbonBackground(m_gfx, t);
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

        // Right side: nickname then battery (with charging indicator), laid out
        // by measured width so the gap is even and nothing crowds the screen edge.
        u32 charge = 0;
        psmGetBatteryChargePercentage(&charge);
        PsmChargerType charger = PsmChargerType_Unconnected;
        bool is_charging = false;
        if (R_SUCCEEDED(psmGetChargerType(&charger)))
            is_charging = charger != PsmChargerType_Unconnected;
        // Charging is marked with a leading '+' rather than a bolt glyph: the
        // menu can be rendered in any user-supplied font, and a font without
        // that glyph draws tofu here.
        char batt[24];
        snprintf(batt, sizeof(batt), is_charging ? "+%lu%%" : "%lu%%",
                 (unsigned long)charge);
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
        PollHbScan();       // swap in the homebrew browser list when its worker finishes
        PollResolvePins();  // fold in pinned-homebrew names/icons when its worker finishes
        PollUpdateCheck();  // join the update-check worker when it finishes
        PollPlayStats();    // start/collect the pdm play-time query

        // SD card pulled while powered on: nothing else matters, show the warning
        // (in the always-loaded system font) until the daemon reboots the console.
        if (m_sd_removed) {
            m_gfx->UseDefaultFont(true);
            DrawSdRemoved();
            m_gfx->Present();
            return;
        }

        m_music.Update();   // advance playback position / roll to the next track

        // Welcome screen bows out on its own once the jingle has played.
        if (m_screen == Screen::Welcome) {
            const u64 ms = (armGetSystemTick() - m_welcome_start) * 1000 / armGetSystemTickFreq();
            if (ms >= kWelcomeMs) m_screen = Screen::Main;
        }

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
            case Screen::Music:       DrawMusic(); break;
            case Screen::Homebrew:    DrawHomebrew(); break;
            case Screen::About:       DrawAbout(); break;
            case Screen::Welcome:     DrawWelcome(); break;
            case Screen::SysEntries:  DrawSysEntries(); break;
            case Screen::Power:       DrawPower(); break;
            case Screen::Payloads:    DrawPayloads(); break;
        }
        if (m_options_open) DrawOptions();
        if (m_dialog != Dialog::None) DrawDialog();
        m_gfx->Present();
    }

    void Menu::DrawSdRemoved() {
        const int W = gfx::Gfx::Width, H = gfx::Gfx::Height;
        const int cx = W / 2;

        // Classic full-screen blue warning.
        const SDL_Color blue  = {  15,  70, 180, 255 };
        const SDL_Color white = { 255, 255, 255, 255 };
        const SDL_Color soft  = { 205, 222, 255, 255 };
        m_gfx->FillRect(0, 0, W, H, blue);

        // Localised like everything else: the strings are already in memory, so
        // losing the card does not cost us the translation.
        m_gfx->TextCentered(FontSize::Title,  cx, 210, white, T("SD card removed"));
        m_gfx->FillRect(cx - 150, 292, 300, 3, white);
        m_gfx->TextCentered(FontSize::Normal, cx, 336, white,
                            T("Please only remove the SD card while the console is off."));
        m_gfx->TextCentered(FontSize::Small,  cx, 388, soft,
                            T("Taking it out while powered on can corrupt your data."));
        m_gfx->TextCentered(FontSize::Normal, cx, 470, soft, T("Restarting..."));
    }

    void Menu::DrawOobe() {
        const Theme &t = m_theme.Current();
        const int cx = gfx::Gfx::Width / 2;

        switch (m_oobe_step) {
            case 0: // Welcome
                m_gfx->TextCentered(FontSize::Title, cx, 210, t.title, "sLaunch");
                m_gfx->FillRect(cx - 130, 288, 260, 3, t.accent);
                m_gfx->TextCentered(FontSize::Normal, cx, 336, t.fg, T("A clean HOME Menu replacement"));
                m_gfx->TextCentered(FontSize::Small, cx, 392, t.dim,
                                    T("Let's set it up - just a few seconds."));
                DrawHint("A: Get started");
                break;

            case 1: { // Theme - the menu's own list, applied live
                DrawTopBar(T("Setup"));
                m_gfx->TextCentered(FontSize::Small, cx, 96, t.dim, T("Pick a theme"));
                std::vector<std::string> labels, values;
                for (int i = 0; i < m_theme.Count(); i++) {
                    labels.push_back(m_theme.At(i).name);
                    values.push_back(i == m_theme_cursor ? T("Applied") : std::string());
                }
                DrawCarousel(labels, values, m_theme_cursor, m_sub_scroll);
                DrawHint("Up/Down: Choose    A: Next    B: Back");
                break;
            }

            case 2: { // Layout - same list, description under it
                DrawTopBar(T("Setup"));
                m_gfx->TextCentered(FontSize::Small, cx, 96, t.dim, T("Choose a layout"));
                const char *names[6] = { T("List"), T("Line"), T("Grid"), T("Cover"), T("Shelf"), T("XMB") };
                const char *desc[6]  = { T("A simple scrolling text list"),
                                         T("A cover carousel (EmulationStation)"),
                                         T("A grid of app icons"),
                                         T("One fullscreen cover at a time"),
                                         T("An Xbox-360-style cover shelf"),
                                         T("PSP/PS3 cross-media bar") };
                std::vector<std::string> labels, values;
                for (int i = 0; i < 6; i++) { labels.push_back(names[i]); values.emplace_back(); }
                int cur = (int)m_ui_mode;
                DrawCarousel(labels, values, cur, m_sub_scroll);
                m_gfx->TextCentered(FontSize::Small, cx, 556, t.dim, desc[cur]);
                DrawHint("Up/Down: Choose    A: Next    B: Back");
                break;
            }

            case 3: { // Good to know
                DrawTopBar(T("Setup"));
                m_gfx->TextCentered(FontSize::Small, cx, 96, t.dim, T("Good to know"));
                struct Tip { const char *key; const char *val; };
                const Tip tips[] = {
                    { "X",        T("Options on any entry: favourite, rename, move") },
                    { "Theming",  T("Fonts, colours, background music, widgets") },
                    { "Homebrew", T("Browse .nro files and pin them to this menu") },
                    { "HOME",     T("Suspends your game and brings this back") },
                };
                const int lx = cx - 330, top = 200;
                for (int i = 0; i < 4; i++) {
                    const int y = top + i * 74;
                    const int kw = m_gfx->TextWidth(FontSize::Small, tips[i].key) + 26;
                    m_gfx->FillRect(lx, y - 4, 4, 40, t.accent);            // accent rule
                    m_gfx->FillRect(lx + 12, y - 4, kw, 40, WithAlpha(t.accent, 38));
                    m_gfx->Text(FontSize::Small, lx + 25, y + 4, t.accent, tips[i].key);
                    m_gfx->Text(FontSize::Normal, lx + 150, y, t.fg, tips[i].val);
                }
                DrawHint("A: Next    B: Back");
                break;
            }

            default: { // Done
                DrawTopBar(T("Setup"));
                m_gfx->TextCentered(FontSize::Large, cx, 190, t.title, T("You're all set"));
                m_gfx->FillRect(cx - 100, 250, 200, 3, t.accent);
                m_gfx->TextCentered(FontSize::Normal, cx, 292, t.fg, T("Enjoy sLaunch."));
                // Update-check opt-out, styled like a menu row.
                const int rx = cx - 300, ry = 400;
                m_gfx->FillRect(rx, ry - 10, 600, 56, WithAlpha(t.accent, 34));
                m_gfx->Text(FontSize::Normal, rx + 24, ry, t.fg,
                            T("Check for updates on startup"));
                const char *val = m_check_updates ? T("On") : T("Off");
                const int vw = m_gfx->TextWidth(FontSize::Normal, val);
                m_gfx->Text(FontSize::Normal, rx + 600 - 24 - vw, ry, t.accent, val);
                DrawHint("Left/Right: Change    A: Finish    B: Back");
                break;
            }
        }

        // Progress dots.
        constexpr int kSteps = 5;
        const int gap = 28;
        const int x0 = cx - (kSteps - 1) * gap / 2;
        for (int i = 0; i < kSteps; i++) {
            const bool cur = (i == m_oobe_step);
            const int sz = cur ? 12 : 8;
            m_gfx->FillRect(x0 + i * gap - sz / 2, 596 - sz / 2, sz, sz,
                            cur ? t.accent : WithAlpha(t.dim, 110));
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
            case UiMode::XMB:     DrawMainXmb();     break;
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
                SDL_Texture *ic = game ? m_icons.Get(it.app_id)
                                 : it.kind == ItemKind::Homebrew ? m_hb_icons.Get(it.hb_icon)
                                 : SystemIcon(it.kind);
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

        DrawStatusHint("A: Select    X: Options");
    }

    std::string Menu::Ellipsize(const std::string &s, int maxw, gfx::FontSize fs) const {
        if (maxw <= 0) return std::string();
        if (m_gfx->TextWidth(fs, s.c_str()) <= maxw) return s;

        int lo = 0, hi = (int)s.size();
        while (lo < hi) {
            int mid = (lo + hi + 1) / 2;
            while (mid > lo && ((unsigned char)s[mid] & 0xC0) == 0x80) mid--;  // UTF-8 boundary
            if (mid == lo) break;
            if (m_gfx->TextWidth(fs, (s.substr(0, mid) + "...").c_str()) <= maxw) lo = mid;
            else                                                                  hi = mid - 1;
        }
        while (lo > 0 && ((unsigned char)s[lo] & 0xC0) == 0x80) lo--;
        return s.substr(0, lo) + "...";
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
    // Clear all cached system icon textures so a pack switch takes effect.
    void Menu::InvalidateSysIcons() {
        for (auto &kv : m_sys_icons) { if (kv.second) m_gfx->FreeImage(kv.second); }
        m_sys_icons.clear();
    }

    // Load (and cache) the black/white icon drawn for a non-game menu entry.
    // When a custom icon pack is active (m_icon_pack_idx > 0), the icon is loaded
    // from sdmc:/slaunch/icon_packs/<pack_name>/<name>.png and rescaled to 64x64
    // so it matches the built-in size. If the custom icon is missing the lookup
    // falls back to the built-in at sdmc:/slaunch/icons/<name>.png.
    // Missing files -> nullptr and the tile falls back to its lettered placeholder.
    SDL_Texture *Menu::SystemIcon(ItemKind kind) {
        auto it = m_sys_icons.find((int)kind);
        if (it != m_sys_icons.end()) return it->second;

        const char *file = nullptr;
        switch (kind) {
            case ItemKind::Theming:      file = "theming";      break;
            case ItemKind::RandomGame:   file = "random";       break;
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
            // Try custom icon pack first (if selected). LoadImageScaled handles
            // both existence check and rescale in one pass, returning nullptr on miss.
            // LoadGlyph separates the artwork from its background field so the
            // theme can colour the field (see Theme::icon_bg); the shipped PNGs
            // are white-on-black with no alpha channel.
            //
            // Loaded at native resolution. Forcing 64x64 here threw away most of
            // a 256x256 pack and left the XMB bar upscaling a 64px texture to
            // 88px, which is exactly as soft as it sounds. There are only ten of
            // these and they are cached, so full size costs little.
            if (m_icon_pack_idx > 0 && m_icon_pack_idx <= (int)m_icon_packs.size()) {
                const std::string &pack = m_icon_packs[m_icon_pack_idx - 1];
                char path[256];
                snprintf(path, sizeof(path), "sdmc:/slaunch/icon_packs/%s/%s.png", pack.c_str(), file);
                tex = m_gfx->LoadGlyph(path, 0, 0);
            }
            // Fall back to built-in icons if custom pack missing or icon not found
            if (!tex) {
                char path[64];
                snprintf(path, sizeof(path), "sdmc:/slaunch/icons/%s.png", file);
                tex = m_gfx->LoadGlyph(path, 0, 0);
            }
        }
        m_sys_icons[(int)kind] = tex; // cache even nullptr so we don't re-stat
        return tex;
    }

    // Item index under a touch point in Grid mode (mirrors DrawMainGrid), or -1.
    int Menu::GridItemAt(int px, int py) const {
        const int cols = GridColumns();
        const int tile = kGridTile, gapX = kGridGap, pitch = kGridPitch, topBase = kGridTopBase;
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
        const int center_y = kListCenterY, spacing = kListSpacing;
        const int idx = (int)lroundf(m_scroll_pos + (float)(py - center_y) / spacing);
        return (idx >= 0 && idx < (int)m_items.size()) ? idx : -1;
    }

    // XMB: which category icon is under a touch point, or -1. Only the bar row
    // answers, so a tap on the column below never jumps categories.
    int Menu::XmbColAt(int x, int y) const {
        if (y < kXmbBarY - 20 || y > kXmbBarY + kXmbCatSel + 30) return -1;
        for (int c = 0; c < (int)m_xmb_cols.size(); c++) {
            const int cx = kXmbAnchorX + (int)((float)(c - m_xmb_col_scroll) * kXmbCatPitch);
            if (std::abs(x - cx) <= kXmbCatPitch / 2) return c;
        }
        return -1;
    }

    // XMB: which entry of the open column is under a touch point, or -1. Mirrors
    // the row placement in DrawMainXmb, and the whole row width is tappable.
    int Menu::XmbItemAt(int x, int y) const {
        (void)x;
        if (m_xmb_col < 0 || m_xmb_col >= (int)m_xmb_cols.size()) return -1;
        if (y < kXmbFadeStart - kXmbItemPitch / 2 || y > kXmbItemBottom) return -1;
        const auto &items = m_xmb_cols[m_xmb_col].items;
        const int i = (int)lroundf(m_xmb_item_scroll +
                                   (float)(y - kXmbItemY) / (float)kXmbItemPitch);
        if (i < 0 || i >= (int)items.size()) return -1;
        return items[i];
    }

    // Item index under a touch point, dispatched by the active layout.
    int Menu::MainItemAt(int x, int y) const {
        const int last = (int)m_items.size() - 1;
        if (m_ui_mode == UiMode::Grid) return GridItemAt(x, y);
        if (m_ui_mode == UiMode::XMB)  return XmbItemAt(x, y);
        if (m_ui_mode == UiMode::Line) {
            const int idx = (int)lroundf(m_scroll_pos +
                                         (float)(x - gfx::Gfx::Width / 2) / (float)kLinePitch);
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
        const bool isHb   = (it.kind == ItemKind::Homebrew);
        SDL_Texture *icon = isGame ? m_icons.Get(it.app_id)
                          : isHb   ? m_hb_icons.Get(it.hb_icon)
                          : SystemIcon(it.kind);

        if (icon && (isGame || isHb)) {
            m_gfx->DrawImage(icon, x, y, size, size, alpha);   // real artwork
        } else if (icon) {
            // System entries: the themed plate, then the artwork exactly as the
            // icon pack drew it. Only the plate is a theme colour.
            m_gfx->FillRect(x, y, size, size, WithAlpha(t.icon_bg, alpha));
            m_gfx->DrawImage(icon, x, y, size, size, alpha);
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
        const int tile     = kGridTile;
        const int gapX     = kGridGap;
        const int gapY     = kGridGap;
        const int pitch    = tile + gapY;                 // vertical distance between rows
        const int rowsVis  = 4;                           // rows fully on screen
        const int gridW    = cols * tile + (cols - 1) * gapX;
        const int startX   = (gfx::Gfx::Width - gridW) / 2;
        const int topBase  = kGridTopBase;                // y of row 0 at scroll 0
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

        auto ellipsize = [&](const std::string &s, int maxw, gfx::FontSize fs) {
            return Ellipsize(s, maxw, fs);
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
        // Last line of the card: the running badge, or how much this game has been
        // played (blank until the pdm worker lands, and for never-played titles).
        if (sel.app_id == m_suspended && m_suspended != 0) {
            m_gfx->Text(FontSize::Small, kShelfAnchorX, top + tile + 76, t.accent, T("Running"));
        } else if (const play::PlayInfo *pi = Play(sel.app_id)) {
            if (pi->seconds > 0) {
                const std::string line = play::FormatPlaytime(pi->seconds) + "   " +
                                         play::FormatLastPlayed(pi->last_played);
                m_gfx->Text(FontSize::Small, kShelfAnchorX, top + tile + 76, t.dim, line.c_str());
            }
        }

        DrawStatusHint("A: Launch    X: Options");
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


    // ---------------------------------------------------------------------------
    // XMB (PSP cross-media bar)
    // ---------------------------------------------------------------------------
    // Which column an entry belongs to. The six columns mirror the PSP's own
    // (Settings, Photo, Music, Video, Game, Network) mapped onto what a Switch
    // actually has, keeping Game in the same place along the bar.
    Menu::XmbCat Menu::XmbCatOf(const MenuItem &it) {
        switch (it.kind) {
            case ItemKind::Game:
            case ItemKind::RandomGame:   return XmbCat::Game;
            case ItemKind::Homebrew:
            case ItemKind::HomebrewMenu: return XmbCat::Homebrew;
            case ItemKind::Album:        return XmbCat::Photo;
            case ItemKind::UserPage:
            case ItemKind::MiiEdit:      return XmbCat::User;
            case ItemKind::WebBrowser:   return XmbCat::Network;
            default:                     return XmbCat::Settings;   // Theming, Controllers, Settings, Power...
        }
    }

    const char *Menu::XmbCatName(XmbCat c) {
        switch (c) {
            case XmbCat::Settings: return T("Settings");
            case XmbCat::Photo:    return T("Album");
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
            case XmbCat::Photo:    return ItemKind::Album;
            case XmbCat::User:     return ItemKind::UserPage;
            case XmbCat::Network:  return ItemKind::WebBrowser;
            case XmbCat::Game:     return ItemKind::RandomGame;
            case XmbCat::Homebrew: return ItemKind::HomebrewMenu;
            default:               return ItemKind::Settings;
        }
    }

    // Regroup m_items into the bar. Called from RebuildItems, so the per-frame
    // work is only ever "walk the visible slice of one column".
    void Menu::XmbRebuild() {
        m_xmb_cols.clear();
        std::vector<int> bucket[(int)XmbCat::Count];
        for (int i = 0; i < (int)m_items.size(); i++)
            bucket[(int)XmbCatOf(m_items[i])].push_back(i);

        for (int c = 0; c < (int)XmbCat::Count; c++) {
            if (bucket[c].empty()) continue;          // empty columns are not shown
            m_xmb_cols.push_back({ (XmbCat)c, std::move(bucket[c]) });
        }
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
            m_gfx->FillRect(x, y, size, size, WithAlpha(t.icon_bg, alpha));
            m_gfx->DrawImage(icon, x, y, size, size, alpha);
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

    // XMB mode: the PSP's cross-media bar. Categories ride a horizontal bar; the
    // selected one is anchored at a fixed point and its entries hang below it in
    // a column, the selected entry anchored in turn at the crossing point.
    //
    // The look leans on scale, brightness and space rather than panels and
    // outlines: no boxes, no frames, nothing enclosed. Entries drifting up past
    // the bar fade out behind it, and the whole column dips while the bar slides
    // sideways, so the two axes never look like two separate lists.
    void Menu::DrawMainXmb() {
        const Theme &t = m_theme.Current();
        m_icons.SetScale(gfx::IconCache::GridScale);
        DrawTopBar(nullptr);

        if (m_items.empty() || m_xmb_cols.empty()) { DrawMainEmpty(); return; }
        if (!m_xmb_placed) XmbOpenDefaultColumn();
        if (m_xmb_col < 0) { m_xmb_col = 0; m_xmb_item = 0; }

        const int W = gfx::Gfx::Width;

        m_xmb_col_scroll  += ((float)m_xmb_col  - m_xmb_col_scroll)  * 0.20f;
        m_xmb_item_scroll += ((float)m_xmb_item - m_xmb_item_scroll) * 0.26f;
        if (std::abs((float)m_xmb_col  - m_xmb_col_scroll)  < 0.004f) m_xmb_col_scroll  = (float)m_xmb_col;
        if (std::abs((float)m_xmb_item - m_xmb_item_scroll) < 0.004f) m_xmb_item_scroll = (float)m_xmb_item;

        const XmbColumn &col   = m_xmb_cols[m_xmb_col];
        const int        count = (int)col.items.size();

        // --- readability scrim ----------------------------------------------
        // Themes allow a photo wallpaper, and white text on an arbitrary photo
        // is a coin toss. A soft top-down wash darkens the band the bar and the
        // column live in, fading out before the bottom of the screen. Six rects
        // rather than a real gradient: invisible banding at these alphas.
        for (int i = 0; i < 6; i++) {
            const int y0 = kXmbBarY - 60 + i * 90;
            m_gfx->FillRect(0, y0, W, 90, SDL_Color{ 0, 0, 0, (Uint8)(52 - i * 8) });
        }

        // --- the bar --------------------------------------------------------
        // A single hairline through the icon centres, brightest at the crossing
        // point and falling away to each side, so the eye is pulled to the
        // selection instead of to a uniform stripe across the screen.
        for (int i = 0; i < 10; i++) {
            const int seg = W / 10;
            const int sx  = i * seg;
            const float d = std::abs((float)(sx + seg / 2 - kXmbAnchorX)) / (float)W;
            const Uint8 a = (Uint8)std::max(0.0f, 70.0f - d * 150.0f);
            if (a) m_gfx->FillRect(sx, kXmbBarMidY, seg, 1, WithAlpha(t.accent, a));
        }

        // --- category row ---------------------------------------------------
        for (int c = 0; c < (int)m_xmb_cols.size(); c++) {
            const float dist = std::abs((float)c - m_xmb_col_scroll);
            if (dist > 4.5f) continue;

            const float f  = std::max(0.0f, 1.0f - dist);
            const int   sz = kXmbCatUnsel + (int)((kXmbCatSel - kXmbCatUnsel) * f);
            const int   cx = kXmbAnchorX + (int)((float)(c - m_xmb_col_scroll) * kXmbCatPitch);
            if (cx < -kXmbCatSel || cx > W + kXmbCatSel) continue;

            // Unselected categories sit well back; the selected one is the only
            // fully lit thing on the bar. Distance drives brightness only - the
            // artwork itself is never recoloured.
            const Uint8 a = (Uint8)std::max(60.0f, 255.0f - dist * 130.0f);

            if (SDL_Texture *icon = SystemIcon(XmbCatIconKind(m_xmb_cols[c].cat))) {
                const int ix = cx - sz / 2, iy = kXmbBarMidY - sz / 2;
                m_gfx->FillRect(ix, iy, sz, sz, WithAlpha(t.icon_bg, a));
                m_gfx->DrawImage(icon, ix, iy, sz, sz, a);
            }

            if (c == m_xmb_col) {
                const char *nm = XmbCatName(m_xmb_cols[c].cat);
                m_gfx->TextCentered(FontSize::Small, cx, kXmbBarMidY + kXmbCatSel / 2 + 10,
                                    WithAlpha(t.accent, (Uint8)(a * 0.92f)), nm);
            }
        }

        // --- entry column ---------------------------------------------------
        // The column dips and fades while the bar slides, which is what sells
        // the two axes as one control rather than two stacked lists.
        const float slide    = std::min(1.0f, std::abs((float)m_xmb_col - m_xmb_col_scroll));
        const Uint8 colAlpha = (Uint8)(255.0f * (1.0f - slide));
        const int   colDip   = (int)(slide * 26.0f);
        if (colAlpha <= 8 || count == 0) {
            DrawStatusHint("A: Select    X: Options    L/R: Jump    +: Power");
            return;
        }

        const int firstv = std::max(0, (int)m_xmb_item_scroll - kXmbAbove - 1);
        const int lastv  = std::min(count - 1, (int)m_xmb_item_scroll + kXmbBelow + 1);

        for (int i = firstv; i <= lastv; i++) {
            const float dy = (float)i - m_xmb_item_scroll;
            const int   cy = kXmbItemY + (int)(dy * kXmbItemPitch) + colDip;
            if (cy > kXmbItemBottom) break;

            float fade = 1.0f;
            if (cy < kXmbFadeEnd) continue;
            if (cy < kXmbFadeStart)
                fade = (float)(cy - kXmbFadeEnd) / (float)(kXmbFadeStart - kXmbFadeEnd);

            const bool sel = (i == m_xmb_item);

            // Size eases with distance instead of stepping, so a scroll reads as
            // one continuous movement rather than icons popping between two sizes.
            const float prox = std::max(0.0f, 1.0f - std::abs(dy));
            const int   sz   = kXmbItemUnsel + (int)((kXmbItemSel - kXmbItemUnsel) * prox);
            const Uint8 a    = (Uint8)(colAlpha * fade * (0.55f + 0.45f * prox));

            DrawXmbIcon(m_items[col.items[i]], kXmbAnchorX, cy, sz, a);

            // A small accent wedge marks the selection: XMB never boxes the
            // current row, it just points at it.
            if (sel) {
                const int wx = kXmbAnchorX - kXmbItemSel / 2 - 20;
                const SDL_Color ac = WithAlpha(t.accent, (Uint8)(colAlpha * fade));
                m_gfx->FillTriangle(wx, cy - 8, wx + 9, cy, wx, cy + 8, ac);
            }

            const int      tx = kXmbAnchorX + kXmbItemSel / 2 + 26;
            const FontSize fs = sel ? FontSize::Large : FontSize::Normal;
            const int      th = m_gfx->LineHeight(fs);
            const std::string label = Ellipsize(m_items[col.items[i]].name, W - 200 - tx, fs);
            m_gfx->Text(fs, tx, cy - th / 2,
                        WithAlpha(sel ? t.title : t.fg, a), label.c_str());
        }

        // --- selected entry, described under its own name --------------------
        const MenuItem &sel = m_items[col.items[m_xmb_item]];
        const int selY = kXmbItemY + colDip;

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
        if (!info.empty()) {
            const int tx = kXmbAnchorX + kXmbItemSel / 2 + 26;
            m_gfx->Text(FontSize::Small, tx,
                        selY + m_gfx->LineHeight(FontSize::Large) / 2 + 2,
                        WithAlpha(t.dim, colAlpha),
                        Ellipsize(info, W - 200 - tx, FontSize::Small).c_str());
        }

        // Position within the column, far right, aligned with the selection.
        char pos[32];
        snprintf(pos, sizeof(pos), "%d / %d", m_xmb_item + 1, count);
        const int pw = m_gfx->TextWidth(FontSize::Small, pos);
        const int ph = m_gfx->LineHeight(FontSize::Small);
        m_gfx->Text(FontSize::Small, W - 48 - pw, selY - ph / 2,
                    WithAlpha(t.dim, colAlpha), pos);

        DrawStatusHint("A: Select    X: Options    L/R: Jump    +: Power");
    }

    void Menu::DrawTheming() {
        DrawTopBar("Theming");
        const char *modes[6]  = { T("List"), T("Line"), T("Grid"), T("Cover"), T("Shelf"), T("XMB") };
        const char *aligns[3] = { T("Left"), T("Center"), T("Right") };
        std::vector<std::string> labels = {
            T("Themes"), T("UI mode"), T("Text position"), T("List icons"),
            T("Icon pack"), T("Fonts"), T("Music"), T("Widgets"), T("Menu entries"),
            T("Welcome screen"), T("Check for updates"),
            T("About"), T("Back")
        };
        std::vector<std::string> values(labels.size());
        values[TH_UiMode]      = modes[(int)m_ui_mode];
        values[TH_TextPos]     = aligns[(int)m_align];
        values[TH_ListIcons]   = m_list_icons ? T("On") : T("Off");
        values[TH_IconPack]    = (m_icon_pack_idx > 0 &&
                                  m_icon_pack_idx <= (int)m_icon_packs.size())
                               ? m_icon_packs[m_icon_pack_idx - 1] : T("Built-in");
        values[TH_Music]       = m_music.Enabled() ? T("On") : T("Off");
        values[TH_Welcome]     = m_welcome_enabled ? T("On") : T("Off");
        values[TH_Updates]     = m_check_updates ? T("On") : T("Off");
        values[TH_About]       = m_upd_available ? T("Update available") : std::string("v") + SL_VERSION;
        {
            char c[32];
            snprintf(c, sizeof(c), "%d %s", m_widgets.Count(), T("found"));
            values[TH_Widgets] = c;
        }

        DrawCarousel(labels, values, m_theming_cursor, m_sub_scroll);
        DrawHint("Up/Down: Select   A: Open   Left/Right: Change   B: Back");
    }

    // ---- Music submenu -----------------------------------------------------

    Menu::Action Menu::OnButtonMusic(Btn b) {
        if (b == Btn::B) { m_screen = Screen::Theming; m_sub_scroll = TH_Music; return Action::None; }
        if (b == Btn::Down) m_music_cursor = (m_music_cursor + 1) % MU_Count;
        if (b == Btn::Up)   m_music_cursor = (m_music_cursor + MU_Count - 1) % MU_Count;

        const bool left = (b == Btn::Left), right = (b == Btn::Right), a = (b == Btn::A);
        switch (m_music_cursor) {
            case MU_Enabled:
                if (left || right || a) m_music.SetEnabled(!m_music.Enabled());
                break;
            case MU_Track:
                if (right || a) m_music.Next();
                else if (left)  m_music.Prev();
                break;
            case MU_Volume:
                if (right || a) m_music.SetVolume(m_music.Volume() + 5);
                else if (left)  m_music.SetVolume(m_music.Volume() - 5);
                break;
            case MU_Shuffle:
                if (left || right || a) m_music.ToggleShuffle();
                break;
            case MU_Back:
                if (a) { m_screen = Screen::Theming; m_sub_scroll = TH_Music; }
                break;
        }
        return Action::None;
    }

    void Menu::DrawMusic() {
        DrawTopBar("Music");
        const Theme &t = m_theme.Current();

        std::vector<std::string> labels = {
            T("Enabled"), T("Track"), T("Volume"), T("Shuffle"), T("Back")
        };
        std::vector<std::string> values(labels.size());
        values[MU_Enabled] = m_music.Enabled() ? T("On") : T("Off");
        if (m_music.TrackCount() == 0) {
            values[MU_Track] = T("No music found");
        } else {
            std::string nm = m_music.CurrentName();
            if (nm.size() > 30) nm = nm.substr(0, 29) + "...";
            char c[64];
            snprintf(c, sizeof(c), "%s  (%d/%d)", nm.c_str(),
                     m_music.TrackIndex() + 1, m_music.TrackCount());
            values[MU_Track] = c;
        }
        char vol[16];
        snprintf(vol, sizeof(vol), "%d%%", m_music.Volume());
        values[MU_Volume]  = vol;
        values[MU_Shuffle] = m_music.Shuffle() ? T("On") : T("Off");

        DrawCarousel(labels, values, m_music_cursor, m_sub_scroll);
        if (m_music.TrackCount() == 0)
            m_gfx->TextCentered(FontSize::Small, gfx::Gfx::Width / 2, 612, t.dim,
                                T("Put mp3/ogg files in sdmc:/slaunch/music"));
        DrawHint("Up/Down: Select   A/Left/Right: Change   B: Back");
    }

    // ---- Homebrew (.nro) browser -------------------------------------------
    void Menu::LoadHbPins() {
        m_hb_pins.clear();
        FILE *fp = fopen("sdmc:/slaunch/config/homebrew.txt", "r");
        if (!fp) return;
        char line[FS_MAX_PATH + 2];
        while (fgets(line, sizeof(line), fp)) {
            line[strcspn(line, "\r\n")] = '\0';
            if (!line[0]) continue;
            hb::HbEntry e;                 // fallback name until ResolvePins runs
            e.path = line;
            size_t slash = e.path.find_last_of('/');
            std::string base = (slash == std::string::npos) ? e.path : e.path.substr(slash + 1);
            size_t dot = base.find_last_of('.');
            e.name = (dot == std::string::npos) ? base : base.substr(0, dot);
            m_hb_pins.push_back(std::move(e));
        }
        fclose(fp);
    }

    // Give pinned .nro their real name + (cached) icon on the main menu without
    // opening the browser. Runs on a worker thread so it never sits on the menu
    // -start path: pins show their fallback (file-base) name for a moment, then
    // PollResolvePins folds the resolved names/icons in. Manifest-backed, so in
    // steady state the worker only stat()s each pin.
    void Menu::ResolvePinsTrampoline(void *self) {
        Menu *m = static_cast<Menu *>(self);
        hb::Resolve(m->m_pin_result);
        m->m_pin_done.store(true, std::memory_order_release);
    }

    void Menu::StartResolvePins() {
        if (m_hb_pins.empty() || m_pin_running) return;
        m_pin_result = m_hb_pins;   // copy paths (+ fallback names) for the worker
        m_pin_done.store(false, std::memory_order_release);
        if (R_SUCCEEDED(threadCreate(&m_pin_thread, &Menu::ResolvePinsTrampoline, this,
                                     nullptr, 0x20000, 0x3B, -2))) {
            threadStart(&m_pin_thread);
            m_pin_running = true;
        } else {
            hb::Resolve(m_hb_pins);   // fallback: synchronous
            RebuildItems();
        }
    }

    void Menu::PollResolvePins() {
        if (!m_pin_running || !m_pin_done.load(std::memory_order_acquire)) return;
        threadWaitForExit(&m_pin_thread);
        threadClose(&m_pin_thread);
        m_pin_running = false;
        // Fold resolved name/icon into the live pins by path (a merge, so a pin the
        // user toggled meanwhile isn't clobbered).
        for (auto &r : m_pin_result)
            for (auto &p : m_hb_pins)
                if (p.path == r.path) { p.name = r.name; p.icon_key = r.icon_key; break; }
        RebuildItems();
    }

    void Menu::SaveHbPins() {
        mkdir("sdmc:/slaunch", 0777);
        mkdir("sdmc:/slaunch/config", 0777);
        FILE *fp = fopen("sdmc:/slaunch/config/homebrew.txt", "w");
        if (!fp) return;
        for (auto &p : m_hb_pins) fprintf(fp, "%s\n", p.path.c_str());
        fclose(fp);
    }

    bool Menu::IsHbPinned(const std::string &path) const {
        for (auto &p : m_hb_pins) if (p.path == path) return true;
        return false;
    }

    void Menu::LoadHbDonor() {
        m_hb_donor = 0;
        FILE *fp = fopen("sdmc:/slaunch/config/hb_donor.txt", "r");
        if (!fp) return;
        char line[32];
        if (fgets(line, sizeof(line), fp)) m_hb_donor = strtoull(line, nullptr, 16);
        fclose(fp);
    }

    void Menu::SaveHbDonor() {
        mkdir("sdmc:/slaunch", 0777);
        mkdir("sdmc:/slaunch/config", 0777);
        FILE *fp = fopen("sdmc:/slaunch/config/hb_donor.txt", "w");
        if (!fp) return;
        fprintf(fp, "%016llX\n", (unsigned long long)m_hb_donor);
        fclose(fp);
    }

    void Menu::ToggleHbPin(const std::string &path) {
        auto it = std::find_if(m_hb_pins.begin(), m_hb_pins.end(),
                               [&](const hb::HbEntry &e){ return e.path == path; });
        if (it != m_hb_pins.end()) {
            m_hb_pins.erase(it);
        } else {
            // Resolve now so the pinned entry gets its name + icon immediately.
            hb::HbEntry e;
            for (auto &h : m_hb) if (h.path == path) { e = h; break; }
            if (e.path.empty()) e = hb::ReadOne(path);
            m_hb_pins.push_back(std::move(e));
        }
        SaveHbPins();
        RebuildItems();
    }

    // Scan homebrew on a worker thread: parsing NROs + extracting icons off the SD
    // can take a second or two, and doing it inline froze the menu on open. The
    // browser shows "Scanning..." until PollHbScan swaps the finished list in.
    void Menu::HbScanTrampoline(void *self) {
        Menu *m = static_cast<Menu *>(self);
        m->m_hb_scan_result = hb::Scan();
        m->m_hb_scan_done.store(true, std::memory_order_release);
    }

    void Menu::StartHbScan() {
        if (m_hb_scanned || m_hb_scan_running) return;
        m_hb_scan_done.store(false, std::memory_order_release);
        m_hb_scan_result.clear();
        if (R_SUCCEEDED(threadCreate(&m_hb_thread, &Menu::HbScanTrampoline, this,
                                     nullptr, 0x20000, 0x3B, -2))) {
            threadStart(&m_hb_thread);
            m_hb_scan_running = true;
        } else {
            m_hb = hb::Scan();   // fallback: synchronous (may briefly stall)
            m_hb_scanned = true;
            RebuildItems();
        }
    }

    void Menu::PollHbScan() {
        if (!m_hb_scan_running || !m_hb_scan_done.load(std::memory_order_acquire)) return;
        threadWaitForExit(&m_hb_thread);
        threadClose(&m_hb_thread);
        m_hb_scan_running = false;
        m_hb = std::move(m_hb_scan_result);
        m_hb_scanned = true;
        if (m_hb_cursor >= (int)m_hb.size()) m_hb_cursor = m_hb.empty() ? 0 : (int)m_hb.size() - 1;
        RebuildItems();
    }

    void Menu::OpenHomebrewBrowser() {
        StartHbScan();   // background; browser shows "Scanning..." until it lands
        m_screen = Screen::Homebrew;
        m_hb_cursor = 0;
    }

    Menu::Action Menu::OnButtonHomebrew(Btn b) {
        const int n = (int)m_hb.size();
        if (b == Btn::B) { m_screen = Screen::Main; return Action::None; }
        if (n == 0) return Action::None;
        if (b == Btn::Down) m_hb_cursor = (m_hb_cursor + 1) % n;
        if (b == Btn::Up)   m_hb_cursor = (m_hb_cursor + n - 1) % n;
        if (b == Btn::A) {   // launch: as an application if a donor is set, else applet
            m_hb_launch_path = m_hb[m_hb_cursor].path;
            return m_hb_donor ? Action::LaunchHomebrewApp : Action::LaunchHomebrew;
        }
        if (b == Btn::Y) {   // force applet mode (fallback when app mode misbehaves)
            m_hb_launch_path = m_hb[m_hb_cursor].path;
            return Action::LaunchHomebrew;
        }
        if (b == Btn::X) {   // pin / unpin from the main menu
            ToggleHbPin(m_hb[m_hb_cursor].path);
            SetStatus(IsHbPinned(m_hb[m_hb_cursor].path) ? "Pinned to menu" : "Unpinned");
        }
        return Action::None;
    }

    void Menu::DrawHomebrew() {
        const Theme &t = m_theme.Current();
        DrawTopBar("Homebrew");
        if (m_hb_scan_running) {
            m_gfx->TextCentered(FontSize::Normal, gfx::Gfx::Width / 2, 320, t.dim,
                                T("Scanning homebrew..."));
            DrawHint("B: Back");
            return;
        }
        if (m_hb.empty()) {
            m_gfx->TextCentered(FontSize::Normal, gfx::Gfx::Width / 2, 320, t.dim,
                                T("No .nro found in sdmc:/switch"));
            DrawHint("B: Back");
            return;
        }
        m_hb_icons.SetScale(gfx::IconCache::GridScale);

        const int rowH = 84, top = 118, visible = 6;
        int start = std::max(0, m_hb_cursor - visible / 2);
        if (start + visible > (int)m_hb.size()) start = std::max(0, (int)m_hb.size() - visible);
        for (int i = 0; i < visible && start + i < (int)m_hb.size(); i++) {
            const int idx = start + i;
            const hb::HbEntry &h = m_hb[idx];
            const int y = top + i * rowH;
            const bool sel = (idx == m_hb_cursor);
            if (sel) m_gfx->FillRect(60, y - 6, gfx::Gfx::Width - 120, rowH - 12, WithAlpha(t.accent, 40));
            SDL_Texture *ic = h.icon_key ? m_hb_icons.Get(h.icon_key) : nullptr;
            if (ic) m_gfx->DrawImage(ic, 80, y, 64, 64, 255);
            else    m_gfx->FillRect(80, y, 64, 64, WithAlpha(t.bg_bottom, 180));
            m_gfx->Text(FontSize::Normal, 164, y + 16, sel ? t.accent : t.fg, h.name.c_str());
            if (IsHbPinned(h.path))
                m_gfx->Text(FontSize::Small, gfx::Gfx::Width - 210, y + 22, t.accent, T("pinned"));
        }
        char pos[28];
        snprintf(pos, sizeof(pos), "%d / %d", m_hb_cursor + 1, (int)m_hb.size());
        m_gfx->Text(FontSize::Small, gfx::Gfx::Width - 150, 74, t.dim, pos);
        DrawHint(m_hb_donor ? "A: Run as app   Y: Applet mode   X: Pin   B: Back"
                            : "A: Launch   X: Pin to menu   B: Back");
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
            T("Background"), T("Photo"), T("Dim"), T("Blur"),
            T("Blur radius"), T("Snow"), T("Video fps"),
            T("Gradient top"), T("Gradient bottom"), T("Text"),
            T("Accent"), T("Secondary"), T("Title"), T("Icon background"),
            T("Wave lines"), T("Wave thickness"), T("Wave amplitude"),
            T("Ribbon seed"), T("Ribbon layers"), T("Ribbon Y"),
            T("Rename theme"), T("Save & Apply"), T("Delete theme")
        };

        // Build the visible row list (ribbon rows hidden when bg != Ribbon).
        int vis_ids[EF_Count];
        int vis_n = 0;
        int cursor_vis = 0;
        for (int i = 0; i < EF_Count; i++) {
            if (IsRibbonRow(i) && c.background_style != BackgroundStyle_Ribbon)
                continue;
            if (i == EF_WallpaperFps && !IsVideoPath(c.wallpaper))
                continue;
            if (IsBlurRadiusRow(i) && !c.wallpaper_blur)
                continue;
            if (i == m_edit_cursor) cursor_vis = vis_n;
            vis_ids[vis_n++] = i;
        }

        // Smooth scroll
        m_edit_scroll += (cursor_vis - m_edit_scroll) * 0.30f;
        if (std::abs(cursor_vis - m_edit_scroll) < 0.01f) m_edit_scroll = (float)cursor_vis;

        const int center_y = 360, spacing = 48, span = 7;
        for (int off = -span; off <= span; off++) {
            const int vi = (int)lroundf(m_edit_scroll) + off;
            if (vi < 0 || vi >= vis_n) continue;

            const int i    = vis_ids[vi];
            const float vdist = std::abs((float)vi - m_edit_scroll);
            const bool big   = vdist < 0.5f;
            const FontSize fs = big ? FontSize::Large : FontSize::Normal;
            const Uint8 alpha = (Uint8)std::max(24.0f, 255.0f - vdist * 52.0f);
            const int lh = m_gfx->LineHeight(fs);
            const int y  = center_y + (int)((vi - m_edit_scroll) * spacing) - lh / 2;
            if (y < 90 || y > kHintY - 30) continue;

            const bool sel = (i == m_edit_cursor);
            const SDL_Color rc = (i == EF_Delete) ? WithAlpha(SDL_Color{235, 90, 90, 255}, alpha)
                                                  : WithAlpha(sel ? t.accent : t.fg, alpha);
            m_gfx->Text(fs, kListX, y, rc, labels[i]);

            // Value column
            const int vx = 720;
            if (SDL_Color *col = EditorColor(c, i)) {
                m_gfx->FillRect(vx, y + (lh - 32) / 2, 60, 32, *col);
                char hex[16];
                snprintf(hex, sizeof(hex), "#%02X%02X%02X", col->r, col->g, col->b);
                m_gfx->Text(FontSize::Small, vx + 78, y + 4, WithAlpha(t.dim, alpha), hex);
            } else if (i == EF_Background) {
                const char *bg = (c.background_style == BackgroundStyle_Ribbon) ? T("Ribbon") : T("Gradient");
                m_gfx->Text(FontSize::Small, vx - 40, y + 4, WithAlpha(t.dim, alpha), "<");
                m_gfx->Text(FontSize::Normal, vx, y, WithAlpha(t.fg, alpha), bg);
            } else if (i == EF_Wallpaper) {
                const char *wp_label;
                if (c.wallpaper[0]) {
                    const char *slash = strrchr(c.wallpaper, '/');
                    wp_label = slash ? slash + 1 : c.wallpaper;
                } else {
                    wp_label = T("(none)");
                }
                m_gfx->Text(FontSize::Normal, vx, y, WithAlpha(t.fg, alpha), wp_label);
            } else if (i == EF_WallpaperDim || i == EF_WallpaperBlur || i == EF_WallpaperSnow) {
                // Show On/Off toggle.
                int val = (i == EF_WallpaperDim) ? c.wallpaper_dim
                       : (i == EF_WallpaperBlur) ? c.wallpaper_blur
                       : c.wallpaper_snow;
                m_gfx->Text(FontSize::Normal, vx, y, WithAlpha(t.fg, alpha),
                            val ? T("On") : T("Off"));
            } else if (i == EF_WallpaperBlurRadius) {
                char val[16];
                snprintf(val, sizeof(val), "%d", c.wallpaper_blur_radius);
                m_gfx->Text(FontSize::Small, vx - 40, y + 4, WithAlpha(t.dim, alpha), "<");
                m_gfx->Text(FontSize::Normal, vx, y, WithAlpha(t.fg, alpha), val);
                m_gfx->Text(FontSize::Small, vx + 40, y + 4, WithAlpha(t.dim, alpha), ">");
            } else if (i == EF_WallpaperFps) {
                char val[16];
                snprintf(val, sizeof(val), "%d", c.wallpaper_fps);
                m_gfx->Text(FontSize::Small, vx - 40, y + 4, WithAlpha(t.dim, alpha), "<");
                m_gfx->Text(FontSize::Normal, vx, y, WithAlpha(t.fg, alpha), val);
                m_gfx->Text(FontSize::Small, vx + 40, y + 4, WithAlpha(t.dim, alpha), ">");
            } else if (i == EF_RibbonLines) {
                char val[16];
                snprintf(val, sizeof(val), "%d", c.ribbon_line_count);
                m_gfx->Text(FontSize::Small, vx - 40, y + 4, WithAlpha(t.dim, alpha), "<");
                m_gfx->Text(FontSize::Normal, vx, y, WithAlpha(t.fg, alpha), val);
                m_gfx->Text(FontSize::Small, vx + 40, y + 4, WithAlpha(t.dim, alpha), ">");
            } else if (i == EF_RibbonThickness) {
                char val[16];
                snprintf(val, sizeof(val), "%d", c.ribbon_thickness);
                m_gfx->Text(FontSize::Small, vx - 40, y + 4, WithAlpha(t.dim, alpha), "<");
                m_gfx->Text(FontSize::Normal, vx, y, WithAlpha(t.fg, alpha), val);
                m_gfx->Text(FontSize::Small, vx + 40, y + 4, WithAlpha(t.dim, alpha), ">");
            } else if (i == EF_RibbonAmplitude) {
                char val[16];
                snprintf(val, sizeof(val), "%d", c.ribbon_amplitude);
                m_gfx->Text(FontSize::Small, vx - 40, y + 4, WithAlpha(t.dim, alpha), "<");
                m_gfx->Text(FontSize::Normal, vx, y, WithAlpha(t.fg, alpha), val);
                m_gfx->Text(FontSize::Small, vx + 40, y + 4, WithAlpha(t.dim, alpha), ">");
            } else if (i == EF_RibbonSeed) {
                char val[16];
                snprintf(val, sizeof(val), "%d", c.ribbon_seed);
                m_gfx->Text(FontSize::Small, vx - 40, y + 4, WithAlpha(t.dim, alpha), "<");
                m_gfx->Text(FontSize::Normal, vx, y, WithAlpha(t.fg, alpha), val);
                m_gfx->Text(FontSize::Small, vx + 40, y + 4, WithAlpha(t.dim, alpha), ">");
            } else if (i == EF_RibbonLayers) {
                char val[16];
                snprintf(val, sizeof(val), "%d", c.ribbon_layers);
                m_gfx->Text(FontSize::Small, vx - 40, y + 4, WithAlpha(t.dim, alpha), "<");
                m_gfx->Text(FontSize::Normal, vx, y, WithAlpha(t.fg, alpha), val);
                m_gfx->Text(FontSize::Small, vx + 40, y + 4, WithAlpha(t.dim, alpha), ">");
            } else if (i == EF_RibbonYCenter) {
                char val[16];
                snprintf(val, sizeof(val), "%d", c.ribbon_y_center);
                m_gfx->Text(FontSize::Small, vx - 40, y + 4, WithAlpha(t.dim, alpha), "<");
                m_gfx->Text(FontSize::Normal, vx, y, WithAlpha(t.fg, alpha), val);
                m_gfx->Text(FontSize::Small, vx + 40, y + 4, WithAlpha(t.dim, alpha), ">");
            }
        }

        // Contextual hint for the current row.
        if (m_edit_cursor == EF_Background)
            DrawHint("Up/Down: Row    Left/Right: Change background type    B: Back");
        else if (m_edit_cursor == EF_Wallpaper)
            DrawHint("Up/Down: Row    Left/Right: Choose photo overlay    B: Back");
        else if (IsEffectRow(m_edit_cursor))
            DrawHint("Up/Down: Row    Left/Right/A: Toggle effect    Left/Right: Adjust value    B: Back");
        else if (m_edit_cursor == EF_WallpaperFps)
            DrawHint("Up/Down: Row    Left/Right: Adjust video fps    B: Back");
        else if (IsRibbonRow(m_edit_cursor))
            DrawHint("Up/Down: Row    Left/Right: Adjust value    B: Back");
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
        const int sw = 176, gap = 12;
        const int totW = kKbSpecialCols * sw + (kKbSpecialCols - 1) * gap, x0 = cx - totW / 2;
        for (int c = 0; c < kKbSpecialCols; c++) {
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

        // Power prompts set their own heading (and sometimes a warning line); the
        // launch/close dialogs are always about the running game.
        m_gfx->TextCentered(FontSize::Large, cx, by + 40, t.title,
                            m_dialog_title.empty() ? T("Close running application?")
                                                   : m_dialog_title.c_str());
        if (!m_dialog_note.empty())
            m_gfx->TextCentered(FontSize::Small, cx, by + 88, t.dim, m_dialog_note.c_str());

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



