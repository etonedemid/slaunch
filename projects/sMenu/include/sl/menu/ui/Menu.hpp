#pragma once
#include <switch.h>
#include <sl/os/Applications.hpp>
#include <sl/menu/ui/Theme.hpp>
#include <sl/menu/gfx/Gfx.hpp>
#include <sl/menu/gfx/IconCache.hpp>
#include <sl/menu/audio/Music.hpp>
#include <sl/menu/audio/Sound.hpp>
#include <sl/menu/hb/Homebrew.hpp>
#include <sl/menu/play/PlayStats.hpp>
#include <sl/menu/widgets/Widgets.hpp>
#include <vector>
#include <string>
#include <unordered_map>
#include <atomic>

// SDL2 menu for sLaunch.
// Screen state machine: Oobe -> Main / Themes / ThemeEditor, with an optional
// modal confirmation dialog. Input arrives as logical buttons (see Btn) that
// the host loop translates from SDL joystick events.

namespace sl::menu::ui {

    enum class Btn { None, Up, Down, Left, Right, A, B, X, Y, Plus, Minus, L, R };

    enum class ItemKind {
        Game, Theming, Themes, Fonts, Controllers, Album, UserPage,
        WebBrowser, MiiEdit, HomebrewMenu, Homebrew, Settings, Power,
        RandomGame,
    };

    // Horizontal alignment of the main list text.
    enum class TextAlign { Left, Center, Right };

    // Main-screen layout. List is the original text carousel; Line is a
    // horizontal cover carousel (EmulationStation style); Grid is a page of icon
    // tiles; Cover is a fullscreen single-cover pager; Shelf is an Xbox-360-style
    // row of uniform covers with a highlighted selection card; XMB mimics the
    // PSP/PS3 cross-media bar with category icons across the top and vertical
    // sub-items below. Line, Grid, Cover, and Shelf render the cached app icons.
    enum class UiMode { List, Line, Grid, Cover, Shelf, XMB, Count };

    struct MenuItem {
        ItemKind    kind;
        u64         app_id = 0;
        std::string name;
        bool        is_gamecard  = false;
        bool        needs_update = false;
        bool        is_favourite = false;
        std::string hb_path;   // ItemKind::Homebrew: the .nro to launch
        u64         hb_icon = 0;
    };

    // Order games are listed in (favourites are always pinned above the rest).
    // New modes go on the end: the active one is persisted by number.
    enum class SortMode { Default, TitleAsc, TitleDesc, GamecardFirst,
                          RecentlyPlayed, MostPlayed, Count };

    struct AppEntry {
        u64         app_id;
        std::string name;
        bool        is_gamecard;
        bool        needs_update;
    };

    class Menu {
    public:
        enum class Action {
            None, LaunchApp, ResumeApp, TerminateApp,
            OpenAlbum, OpenUserPage, OpenNetConnect, OpenMiiEdit,
            OpenWebBrowser, OpenControllers, OpenHomebrewMenu,
            PowerSleep, PowerReboot, PowerShutdown, PowerPayload,
            LaunchHomebrew, LaunchHomebrewApp, FinishSetup, Quit,
        };

        // Path of the .nro to launch, valid when OnButton returns LaunchHomebrew[App].
        const std::string &HomebrewPath() const { return m_hb_launch_path; }
        u64 HomebrewDonor() const { return m_hb_donor; }

        // Payload to chainload, valid when OnButton returns PowerPayload.
        const std::string &PayloadPath() const { return m_payload_path; }

        ~Menu();

        void Init(gfx::Gfx *gfx, AccountUid user, u64 suspended_app_id, bool start_oobe);

        // Heavier init (widgets: Lua parse + network) run after the first frame is
        // on screen, so a HOME press shows the menu sooner. Call each frame; no-ops
        // after the first call.
        void InitDeferred();

        void SetApps(std::vector<AppEntry> apps);

        // Handle one logical button press. Returns an action (+ app id for launch).
        Action OnButton(Btn b, u64 &out_app_id);

        // Touch input from the host loop. phase: 0 = down, 1 = move, 2 = up.
        // Drives keyboard/color-picker taps, long-press drag for home widgets,
        // and double-tap-to-launch on the main screen. Returns an action (e.g.
        // LaunchApp) the same way OnButton does.
        Action OnTouch(int phase, int x, int y, u64 &out_app_id);

        // Draw the current frame.
        void Render();

        void SetSuspendedApp(u64 app_id);
        void ClearSuspendedApp();

        // The daemon signals this when the SD card is pulled out while powered on.
        // The menu then shows a full-screen warning until the daemon reboots (all
        // menu content is served from the SD card, so we must not keep running).
        void ShowSdRemoved() { m_sd_removed = true; }
        // The host loads the app list on a worker thread; this shows a "Loading"
        // message on the home screen until it's ready.
        void SetLoading(bool v) { m_loading = v; }
        void SetUser(AccountUid uid, const char *nickname);
        void SetStatus(const char *msg);

        // The input loop marks whether a directional press is a fresh press or
        // an auto-repeat; the list wraps only on a fresh press at an end.
        void SetNavFresh(bool fresh) { m_nav_fresh = fresh; }

        // True once the menu has asked the daemon to show the keyboard; the host
        // loop should then let the applet exit so qlaunch can show swkbd.
        bool WantsExit() const { return m_want_exit; }

    private:
        enum class Screen { Oobe, Welcome, Main, Theming, Themes, ThemeEditor, ColorPicker,
                            Fonts, Widgets, WidgetOptions, Keyboard, Music, Homebrew, About,
                            SysEntries, Power, Payloads };
        enum class Dialog { None, ConfirmCloseForLaunch, ConfirmCloseGame, ConfirmPower };

        void RebuildItems();

        Action OnButtonOobe(Btn b);
        Action OnButtonMain(Btn b, u64 &out_app_id);
        Action OnButtonOptions(Btn b, u64 &out_app_id);
        Action OnButtonTheming(Btn b);
        Action OnButtonAbout(Btn b);
        void   DrawAbout();

        // ---- power screen ---------------------------------------------------
        // Replaces the old "+ sleeps immediately": sleep, restart, power off, and
        // (when payloads are present on the SD) chainloading one of them.
        Action OnButtonPower(Btn b);
        void   DrawPower();
        void   EnterPower();          // scan payloads, build the rows, show it
        void   BuildPowerRows();      // which rows are offered right now
        Action OnButtonPayloads(Btn b);
        void   DrawPayloads();
        void   ScanPayloads();        // .bin files under the usual payload folders
        // Ask before anything that closes a running game / drops the console.
        void   AskPower(Action act, const char *prompt);
        void   ShowPowerError();      // one-shot reason left by a failed chainload
        std::vector<int>         m_power_rows;      // row ids currently shown
        int                      m_power_cursor  = 0;
        std::vector<std::string> m_payloads;        // full paths of the .bin files
        std::vector<std::string> m_payload_names;   // display names, same order
        int                      m_payload_cursor  = 0;
        bool                     m_payloads_scanned = false;
        std::string              m_payload_path;    // set on Action::PowerPayload
        Action      m_power_confirm = Action::None; // what ConfirmPower will run
        std::string m_dialog_title;                 // dialog heading
        std::string m_dialog_note;                  // optional line under it

        // ---- play statistics (pdm) -----------------------------------------
        // Total play time / last played per title, for the two play-based sorts
        // and the info line on the main screen. Queried on a worker so the pdm
        // round-trips never sit on a frame.
        static void PlayStatsTrampoline(void *self);
        void StartPlayStats();        // no-op while one is already running
        void PollPlayStats();         // main thread: swap results in when it ends
        void LoadPlayCache();         // last run's numbers, for the first frame
        void SavePlayCache() const;
        const play::PlayInfo *Play(u64 app_id) const;
        Thread m_play_thread{};
        std::vector<u64>            m_play_ids;      // worker input (ids snapshot)
        std::vector<play::PlayInfo> m_play_result;   // worker output
        std::unordered_map<u64, play::PlayInfo> m_play;  // live stats by title id
        bool m_play_running = false;
        bool m_play_dirty   = true;   // app list changed -> re-query
        std::atomic<bool> m_play_done{false};

        // Post-setup welcome screen: greets the user by name over the opening
        // jingle, then hands off to the main menu (A skips).
        Action OnButtonWelcome(Btn b);
        void   DrawWelcome();
        void   EnterWelcome();        // pick a greeting, start the timer
        u64    m_welcome_start = 0;   // tick the welcome screen was entered
        int    m_welcome_msg   = 0;   // index into the greeting pool (picked on entry)
        bool   m_welcome_enabled = true;   // setting (persisted)

        // Which system entries are hidden from the main menu (bit per ItemKind).
        // Theming is never hideable, so the toggles stay reachable.
        u32  m_sys_hidden = 0;
        bool IsSysHidden(ItemKind k) const;
        void ToggleSysHidden(ItemKind k);
        void LoadSysEntries();
        void SaveSysEntries();
        Action OnButtonSysEntries(Btn b);
        void   DrawSysEntries();
        int  m_sys_cursor = 0;

        // "Random game": rolls through the library, lands on one, launches it.
        // Blocking (a few seconds) so it can return the launch action directly.
        u64  RollRandomGame();
        void DrawRollFrame(const std::vector<const MenuItem *> &pool, int idx, bool settled);
        // Shown once per console boot: these tag which boot we already greeted,
        // so returning from a game doesn't replay it.
        bool   BootWelcomePending() const;
        void   MarkBootWelcomed() const;

        // Optional online update check (opt-out in OOBE + Theming > About). Runs a
        // worker on menu start when enabled; sets m_upd_available if GitHub's latest
        // release is newer than this build. Never blocks the UI.
        static void UpdateCheckTrampoline(void *self);
        void StartUpdateCheck();
        void PollUpdateCheck();
        Thread m_upd_thread{};
        bool   m_upd_running   = false;
        std::atomic<bool> m_upd_done{false};
        bool   m_check_updates = true;    // setting (persisted)
        bool   m_upd_available = false;
        std::string m_upd_latest;         // e.g. "v0.6.0" when an update is found
        int    m_about_scroll  = 0;       // first visible changelog line in About
        Action OnButtonThemes(Btn b);
        Action OnButtonEditor(Btn b);
        Action OnButtonColorPicker(Btn b);
        Action OnButtonFonts(Btn b);
        Action OnButtonWidgets(Btn b);
        Action OnButtonWidgetOptions(Btn b);
        Action OnButtonMusic(Btn b);
        Action OnButtonHomebrew(Btn b);
        void OpenHomebrewBrowser();       // scan (lazily) + show the .nro list
        void LoadHbPins();                // pinned homebrew paths (main-menu entries)
        void SaveHbPins();
        // Pinned .nro -> name + cached icon; resolved on a worker (StartResolvePins).
        bool IsHbPinned(const std::string &path) const;
        void ToggleHbPin(const std::string &path);
        // Homebrew favourites (by .nro path): a favourited pin joins the games'
        // favourites group at the top of the menu, with the same leading star.
        bool IsHbFavourite(const std::string &path) const;
        void ToggleHbFavourite(const std::string &path);
        void LoadHbFavourites();
        void SaveHbFavourites();
        void LoadHbDonor();               // the game slot used to run homebrew as an app
        void SaveHbDonor();
        Action OnButtonKeyboard(Btn b);
        void DrawKeyboard();
        // Which on-screen key is at (x,y)? Fills row/col to match DrawKeyboard's
        // layout (row kKbSpecialRow is the special row). Returns false on a miss.
        bool KbKeyAt(int x, int y, int &row, int &col) const;
        Action OnButtonDialog(Btn b, u64 &out_app_id);

        // Theme editor helpers.
        void ScanWallpapers();      // list background images under slaunch/themes

        // Icon packs
        void ScanIconPacks();       // discover packs under slaunch/icon_packs
        void LoadIconPackSetting(); // read saved icon pack selection
        void SaveIconPackSetting(); // persist icon pack selection
        void OpenColorPicker(SDL_Color *target);
        void CycleBackground(int dir);  // Gradient <-> Ribbon
        void CycleWallpaper(int dir);    // cycle photo overlay independently

        // Favourites + sorting.
        bool IsFavourite(u64 app_id) const;
        void ToggleFavourite(u64 app_id);
        void LoadFavourites();
        void SaveFavourites();
        void LoadSort();
        void SaveSort();
        void LoadOrder();           // custom entry order (applied in Default sort)
        void SaveOrder();
        void MoveSelected(int dir); // move mode: shift the held entry by dir (+1/-1)
        std::string ItemKey(const MenuItem &it) const; // stable per-entry order key
        void SelectByKey(const std::string &key);      // move cursor to that entry
        bool SelectApp(u64 app_id); // move cursor to the item for app_id; false if absent
        void BuildOptions();        // populate the X-menu for the current selection

        // Appearance settings (text alignment) + custom game names.
        void LoadSettings();
        void SaveSettings();
        void LoadNames();
        void SaveNames();
        const std::string *CustomName(u64 app_id) const;
        void SetCustomName(u64 app_id, const char *name);
        void RenameSelected();      // software-keyboard rename of the selected game

        // Keyboard bridge (swkbd runs in the daemon; see Protocol.hpp).

        // Fit text into a pixel width, appending "..." when it has to be cut.
        // Cuts on a UTF-8 boundary (so a multi-byte glyph is never split in
        // half) and finds the cut by bisection, which keeps a long name to a
        // handful of width probes instead of one per dropped byte - each probe
        // otherwise rasterises and caches a throwaway string.
        std::string Ellipsize(const std::string &s, int maxw, gfx::FontSize fs) const;

        void DrawBackground();
        void DrawTopBar(const char *center_title);
        void DrawHint(const char *hint);
        void DrawStatusHint(const char *hint); // fresh status line (if any) + hint
        // Shared main-menu-style carousel used by the sub-screens too. Each row
        // is a label plus an optional right-hand value string.
        void DrawCarousel(const std::vector<std::string> &labels,
                          const std::vector<std::string> &values,
                          int cursor, float &scroll_pos);
        void DrawOobe();
        void DrawMain();
        void DrawMainList();        // original text carousel
        void DrawMainLine();        // horizontal cover carousel (EmulationStation)
        void DrawMainGrid();        // page of icon tiles
        void DrawMainCover();       // fullscreen single-cover pager
        void DrawMainShelf();       // Xbox-360-style uniform cover row
        void DrawMainXmb();          // PSP/PS3 cross-media bar
        // Sort mode -> short label for the shelf header / options menu.
        const char *SortLabel() const;
        void DrawMainEmpty();       // "Loading..." / "No apps found" placeholder
        // Draw one app/entry as a square tile: cached icon if present, else a
        // themed placeholder with the name. Used by the Line and Grid modes.
        void DrawAppTile(const MenuItem &it, int x, int y, int size,
                         bool selected, Uint8 alpha);
        int  GridColumns() const { return 8; }  // tiles per row in Grid mode
        // Item index under a touch point (or -1), for touch-to-select/launch.
        int  MainItemAt(int x, int y) const;   // dispatches by UI mode
        int  GridItemAt(int x, int y) const;
        int  ListItemAt(int x, int y) const;
        // Load (cached) the black/white icon for a non-game menu entry, or null.
        SDL_Texture *SystemIcon(ItemKind kind);
        void InvalidateSysIcons(); // clear the cached system icon textures
        void DrawOptions();
        void DrawTheming();
        void DrawThemes();
        void DrawEditor();
        void DrawColorPicker();
        void DrawFonts();
        void DrawSdRemoved();       // full-screen "SD card removed" warning
        void DrawWidgets();         // list detected Lua widgets
        void DrawWidgetOptions();   // exposed options of the selected widget
        void DrawMusic();           // menu-music controls
        void DrawHomebrew();        // .nro browser
        void DrawDialog();

        // Font selection
        void ScanFonts();          // discover installed .ttf/.otf under sdmc:/slaunch/fonts
        void LoadFontConfig();     // read + apply the saved selection
        void SaveFontConfig();
        void ApplyFont(int index); // 0 = default/system; else load the file
        void EnsurePreviewFont(int index); // load a font into the alt slot for preview

        // Wallpaper cache for the active theme.
        void EnsureWallpaper();

        gfx::Gfx    *m_gfx = nullptr;
        ThemeManager m_theme;

        SDL_Texture *m_wallpaper       = nullptr;
        SDL_Texture *m_wallpaper_blur  = nullptr;  // pre-baked low-res (blurred) copy
        std::string  m_wallpaper_path;              // current wallpaper file path
        int          m_wallpaper_theme = -1;
        // Video frame sequence: when wallpaper path is a directory, cycle through frames
        std::vector<std::string> m_video_frames;   // sorted frame paths
        int          m_video_frame_idx = 0;        // current frame index
        u64          m_video_frame_tick = 0;       // tick when last frame was swapped

        // Gaussian-blur an image file, baking the result into a new texture.
        // Reads the file as an SDL_Surface (CPU memory), blurs on CPU, uploads.
        // Caller owns the returned texture (free with m_gfx->FreeImage).
        SDL_Texture *BlurImage(const char *path);

        Screen m_screen = Screen::Main;
        Dialog m_dialog = Dialog::None;

        std::vector<AppEntry> m_apps;
        std::vector<MenuItem> m_items;

        // Favourites (app ids, pinned to the top) + current sort order.
        std::vector<u64> m_favourites;
        SortMode         m_sort = SortMode::Default;
        std::vector<std::string> m_order;     // custom entry order (Default sort), by key
        bool             m_move_mode = false; // reordering the held entry
        std::string      m_move_key;          // ItemKey of the entry being moved

        // Appearance: main-list text alignment + user-renamed games.
        TextAlign m_align = TextAlign::Left;
        bool      m_list_icons = true;         // show the icon column in List mode
        UiMode    m_ui_mode = UiMode::List;   // main-screen layout
        bool      m_loading = false;          // app list still loading in the background
        gfx::IconCache m_icons;               // app icon texture cache (Line/Grid)
        audio::Music   m_music;               // background menu music
        audio::Sound   m_sfx;                 // UI sound effects (nav, welcome)
        gfx::IconCache m_hb_icons;            // homebrew .nro icon cache

        // Custom icon packs: users drop PNGs (same names as built-in) into
        // sdmc:/slaunch/icon_packs/<pack_name>/.  Index 0 = "Built-in" (default).
        std::vector<std::string> m_icon_packs;   // discovered pack names
        int  m_icon_pack_idx = 0;                 // 0 = built-in
        std::vector<hb::HbEntry> m_hb;        // scanned homebrew (browser)
        std::vector<hb::HbEntry> m_hb_pins;   // homebrew pinned to the main menu (resolved)
        std::vector<std::string> m_hb_favs;   // pinned homebrew marked as favourites (paths)
        std::string    m_hb_launch_path;      // set on LaunchHomebrew[App]
        u64  m_hb_donor = 0;                  // donor game id for "run as app"
        int  m_hb_cursor = 0;
        bool m_hb_scanned = false;
        // The homebrew scan (parse NROs / extract icons) runs on a worker thread so
        // opening the browser never hangs the UI. The thread fills m_hb_scan_result;
        // the main thread swaps it into m_hb once m_hb_scan_done flips.
        Thread m_hb_thread{};
        std::vector<hb::HbEntry> m_hb_scan_result;
        bool m_hb_scan_running = false;
        std::atomic<bool> m_hb_scan_done{false};
        static void HbScanTrampoline(void *self);
        void StartHbScan();   // kick off the worker (no-op if already running/done)
        void PollHbScan();    // main thread: swap results in when the worker finishes

        // Pinned-homebrew resolve (name + icon) also runs on a worker so it never
        // sits on the menu-start path; the main thread folds names/icons back in.
        Thread m_pin_thread{};
        std::vector<hb::HbEntry> m_pin_result;
        bool m_pin_running = false;
        std::atomic<bool> m_pin_done{false};
        static void ResolvePinsTrampoline(void *self);
        void StartResolvePins();   // background resolve of m_hb_pins (no-op if empty/running)
        void PollResolvePins();    // main thread: fold resolved names/icons into m_hb_pins
        std::unordered_map<int, SDL_Texture*> m_sys_icons; // Icons
        std::vector<std::pair<u64, std::string>> m_names; // app_id -> custom name
        int  m_theming_cursor = 0;
        bool m_jumped_to_suspended = false;
        bool m_deferred_done = false; // InitDeferred (widgets) has run
        bool m_want_exit = false;   // asked the daemon to show the keyboard
        bool m_sd_removed = false;  // SD pulled while on -> show warning, await reboot

        // Home screen widgets
        widgets::Widgets m_widgets;

        // Options overlay for the selected item.
        struct OptionEntry { std::string label; int action; };
        std::vector<OptionEntry> m_options;
        bool m_options_open   = false;
        int  m_options_cursor = 0;

        int   m_cursor = 0;
        int   m_scroll = 0;
        float m_scroll_pos = 0.0f; // animated carousel position, eases to m_cursor
        float m_sub_scroll = 0.0f; // animated position for the sub-screen carousels
        bool  m_nav_fresh  = true; // is the current directional press fresh?

        // Grid anim
        float m_grid_scroll = 0.0f;
        float m_grid_hl_x   = -1.0f;
        float m_grid_hl_y   = 0.0f;

        // ---- XMB (PSP cross-media bar) --------------------------------------
        // The bar is a fixed set of columns in the PSP's own order; each holds
        // the indices of the m_items that belong to it. Built once whenever the
        // item list changes, so navigating and drawing never rescan m_items
        // (they used to, per item per frame).
        enum class XmbCat { Settings, Photo, User, Network, Game, Homebrew, Count };
        struct XmbColumn {
            XmbCat           cat;
            std::vector<int> items;   // indices into m_items, in list order
        };
        std::vector<XmbColumn> m_xmb_cols;      // non-empty columns only, PSP order
        int   m_xmb_col         = -1;   // index into m_xmb_cols (-1 = not placed yet)
        int   m_xmb_item        = 0;    // index into m_xmb_cols[m_xmb_col].items
        float m_xmb_col_scroll  = 0.0f; // animated bar position
        float m_xmb_item_scroll = 0.0f; // animated column position

        static XmbCat XmbCatOf(const MenuItem &it);
        static const char *XmbCatName(XmbCat c);
        static ItemKind    XmbCatIconKind(XmbCat c);
        bool  m_xmb_placed = false; // the bar has been positioned by the user
        void XmbRebuild();          // regroup m_items into m_xmb_cols
        void XmbOpenDefaultColumn();// first show: start on Games
        void XmbSyncFromCursor();   // point the bar at whatever m_cursor selects
        void XmbApplyCursor();      // m_cursor = the item the bar has selected
        int  XmbItemAt(int x, int y) const;   // item index under a touch, or -1
        int  XmbColAt(int x, int y) const;    // column under a touch, or -1
        // One bare icon on the bar: artwork for games, a tinted glyph otherwise.
        void DrawXmbIcon(const MenuItem &it, int cx, int cy, int size, Uint8 alpha);
        int m_theme_cursor   = 0;
        int m_edit_cursor    = 0;
        float m_edit_scroll  = 0.0f; // animated scroll position for the editor
        int m_editing_theme  = -1; // global index of the custom theme being edited
        int m_oobe_step      = 0;

        // Music + Widgets submenus
        int m_music_cursor  = 0;   // cursor in the music submenu
        int m_widget_cursor  = 0;  // cursor in the widget list
        int m_widget_sel     = 0;  // widget whose options are being edited
        int m_widgetopt_cursor = 0;

        // Soft-keyboard state.
        int  m_kb_purpose = 0;   // sl::smi::Kb_* constants
        u64  m_kb_app     = 0;   // context (app_id, theme index, or widget index)
        int  m_kb_opt     = 0;   // widget option index (Kb_WidgetOption)
        std::string m_kb_text; // current input buffer

        // touch: a touch on a widget grabs it for dragging
        bool m_drag_active   = false;
        int  m_drag_widget   = -1;
        bool m_touching      = false;
        int  m_touch_lx = 0, m_touch_ly = 0;   // last touch position
        int  m_touch_widget  = -1;             // widget under the touch, or -1
        // touch scrolling on main menu and submenus
        bool m_touch_scroll_active = false;
        int  m_touch_start_x = 0, m_touch_start_y = 0;
        float m_touch_scroll_start = 0.0f;      // scroll_pos at touch start
        int  m_kb_row    = 0;  // 0-3 char rows, 4 special
        int  m_kb_col    = 0;
        bool m_kb_upper  = false;

        // Theme editor: available background images + the live Color picker.
        std::vector<std::string> m_wallpapers;   // image paths under slaunch/themes
        SDL_Color *m_pick_target   = nullptr;    // Color being edited
        SDL_Color  m_pick_original = {};         // for cancel/revert
        int        m_pick_channel  = 0;          // 0=R 1=G 2=B

        // Fonts: names[0]/paths[0] = "Default (System)".
        std::vector<std::string> m_font_names;
        std::vector<std::string> m_font_paths;
        int m_font_cursor  = 0;
        int m_font_applied = 0;
        int m_font_preview = -1; // index currently loaded into the gfx alt slot

        u64 m_pending_launch = 0;
        int m_dialog_cursor  = 0; // 0 = Yes, 1 = No

        u64        m_suspended = 0;
        AccountUid m_user      = {};
        char       m_nickname[33] = "Player";
        char       m_status[128]  = "";
        u64        m_status_tick  = 0;

        static constexpr int VisibleRows = 9;
    };

} // namespace sl::menu::ui