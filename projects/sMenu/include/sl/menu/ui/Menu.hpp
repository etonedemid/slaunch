#pragma once
#include <switch.h>
#include <sl/os/Applications.hpp>
#include <sl/menu/ui/Theme.hpp>
#include <sl/menu/gfx/Gfx.hpp>
#include <sl/menu/gfx/IconCache.hpp>
#include <sl/menu/gfx/Video.hpp>
#include <sl/menu/audio/Music.hpp>
#include <sl/menu/audio/Sound.hpp>
#include <sl/menu/hb/Homebrew.hpp>
#include <sl/menu/hb/Shortcuts.hpp>
#include <sl/menu/play/PlayStats.hpp>
#include <sl/menu/widgets/Widgets.hpp>
#include <sl/menu/news/News.hpp>
#include <sl/menu/dbg/Debug.hpp>
#include <sl/menu/cfg/UserCfg.hpp>
#include <sl/menu/usb/Mtp.hpp>
#include <vector>
#include <string>
#include <unordered_map>
#include <initializer_list>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <set>

// SDL2 menu for sLaunch.
// Screen state machine: Oobe -> Main / Themes / ThemeEditor, with an optional
// modal confirmation dialog. Input arrives as logical buttons (see Btn) that
// the host loop translates from SDL joystick events.

namespace sl::menu::ui {

    enum class Btn { None, Up, Down, Left, Right, A, B, X, Y, Plus, Minus, L, R };

    // New kinds are appended, never inserted: the numeric value is written into
    // the hidden-entries config as "s<n>", so renumbering would silently
    // re-point a user's existing hide list at the wrong entries.
    enum class ItemKind {
        Game, Theming, Themes, Fonts, Controllers, Album, UserPage,
        // Settings is retired - it opened the NetConnect applet, which is what
        // the Network entry does properly - but the value stays where it is:
        // the hide list is written by number, and two icon lookups still name it.
        WebBrowser, MiiEdit, HomebrewMenu, Homebrew, Settings, Power,
        RandomGame,
        MusicPlayer,   // opens the menu-music screen; its own XMB category
        // Icon-only kind: never added to m_items and never selectable, it exists
        // so the Media category header can have artwork of its own instead of
        // borrowing the Album entry's.
        MediaCat,
        // Network status / Wi-Fi. Appended, like every kind before it: these are
        // persisted by number in the hidden-entries setting as s<n>.
        Wifi,
        // A tile on the wall that hosts one of the Lua home widgets. Unlike every
        // other kind this one is not built in: the user adds it, and which ones
        // exist comes from the tile config. Its `name` is the widget's name,
        // which is also what ItemKey uses to tie the two together.
        WidgetTile,
        FileManager,   // browse and manage the SD card (Menu_Files.cpp)
    };

    // What the box-art worker is doing (see Menu::DrawFetchStatus). Stages run
    // top to bottom; the last three are outcomes.
    enum class FetchStage { Idle, Lookup, Index, Scan, Save, Sgdb, Shots,
                            Added, NotFound, Failed };

    // Horizontal alignment of the main list text.
    enum class TextAlign { Left, Center, Right };

    // One segment of a button-prompt hint bar: one to three button icons
    // (for combos like "L/R" or "A/Left/Right"), drawn side by side, then a
    // translated label. `icons` are stems under sdmc:/slaunch/icons/buttons/
    // ("a", "b", "l", "r", "up", "down", "left", "right", "plus", "minus",
    // "dpad", "rstick"); `label` is a T() key, same as any other UI string.
    struct HintSeg { std::initializer_list<const char*> icons; const char *label; };

    // Main-screen layout. List is the original text carousel; Line is a
    // horizontal cover carousel (EmulationStation style); Grid is a page of icon
    // tiles; Cover is a fullscreen single-cover pager; Shelf is an Xbox-360-style
    // row of uniform covers with a highlighted selection card; XMB mimics the
    // PSP/PS3 cross-media bar with category icons across the top and vertical
    // sub-items below. Line, Grid, Cover, and Shelf render the cached app icons.
    // Deck is the SteamOS gamepad layout: one big hero, a row of covers beside
    // it, and a row of news or widget cards under both, with the whole library
    // one button away. Appended like every mode before it - the active one is
    // persisted by number.
    enum class UiMode { List, Line, Grid, Cover, Shelf, XMB, Flow, Deck, Count };

    struct MenuItem {
        ItemKind    kind;
        u64         app_id = 0;
        std::string name;
        bool        is_gamecard  = false;
        bool        needs_update = false;
        bool        is_favourite = false;
        std::string hb_path;   // ItemKind::Homebrew: the .nro to launch
        // Argv to hand that .nro. Empty means "just its own path", which is the
        // hbmenu convention and what a plain homebrew launch has always sent;
        // a launcher shortcut puts the core and the ROM in here instead.
        std::string hb_argv;
        // XMB column this entry claims, overriding the one its kind would put it
        // in. Only launcher shortcuts set it - that is what makes a playlist a
        // column of its own rather than more entries under Homebrew.
        std::string category;
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
        // Argv for that launch; empty for a plain .nro, in which case the daemon
        // falls back to the quoted path the way hbmenu does.
        const std::string &HomebrewArgv() const { return m_hb_launch_argv; }
        u64 HomebrewDonor() const { return m_hb_donor; }

        // Payload to chainload, valid when OnButton returns PowerPayload.
        const std::string &PayloadPath() const { return m_payload_path; }

        ~Menu();

        void Init(gfx::Gfx *gfx, AccountUid user, u64 suspended_app_id, bool start_oobe);

        // Heavier init (widgets: Lua parse + network) run after the first frame is
        // on screen, so a HOME press shows the menu sooner. Call each frame; no-ops
        // after the first call.
        // Post-first-frame init. This used to run inline on the main thread and
        // was the several-second stall you saw after frame one: curl's global
        // init, a Lua parse per widget, opening the mixer, pulling in the MP3 /
        // OGG / FLAC decoders and reading ~1.4 MB of WAV off the SD - none of it
        // needed to show or drive the menu.
        //
        // It now starts a worker and returns immediately; PollDeferred folds the
        // result in when it lands. None of the work touches the renderer (Gfx is
        // only ever passed into Widgets::Render, never held), which is what makes
        // moving it off the main thread safe.
        void InitDeferred();       // starts the worker, returns at once
        void PollDeferred();       // main thread: join + finish up once it is done
        static void DeferredTrampoline(void *self);
        // True once the worker has been joined and its results are safe to read.
        // Everything the worker builds - widgets, mixer, sound chunks - is gated
        // on this, because the main thread must not read them mid-construction.
        bool IsDeferredReady() const { return m_deferred_joined; }

        void SetApps(std::vector<AppEntry> apps);

        // Handle one logical button press. Returns an action (+ app id for launch).
        Action OnButton(Btn b, u64 &out_app_id);

        // Touch input from the host loop. phase: 0 = down, 1 = move, 2 = up.
        // Drives keyboard/color-picker taps, long-press drag for home widgets,
        // and double-tap-to-launch on the main screen. Returns an action (e.g.
        // LaunchApp) the same way OnButton does.
        Action OnTouch(int phase, int x, int y, u64 &out_app_id);

        // The physical HOME button (main.cpp polls hidsys for it and calls
        // this on a fresh press; see its own comment there for why this is
        // separate from OnButton rather than one more Btn value). Deck mode:
        // opens the side menu, same as Minus. Every other mode: resumes the
        // suspended game, if there is one - a quick way back into it that
        // doesn't care what screen is currently up. Does nothing otherwise.
        Action OnHomeButton(u64 &out_app_id);

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
        // Right stick, pushed in every frame by main(). Only Flow reads it.
        void SetRightStick(float x, float y) { m_rstick_x = x; m_rstick_y = y; }
        void SetUser(AccountUid uid, const char *nickname);
        void SetStatus(const char *msg);

        // The input loop marks whether a directional press is a fresh press or
        // an auto-repeat; the list wraps only on a fresh press at an end.
        void SetNavFresh(bool fresh) { m_nav_fresh = fresh; }

        // Developer overlay (L + R + Minus). Drawn over whatever screen is up,
        // and it never consumes input, so the menu behaves identically with it
        // on - what it measures is the menu as it normally runs.
        void ToggleDebugOverlay();

        // True once the menu has asked the daemon to show the keyboard; the host
        // loop should then let the applet exit so qlaunch can show swkbd.
        bool WantsExit() const { return m_want_exit; }

        // An action the menu deferred rather than returning from OnButton, and
        // which is now ready. The launch bounce needs this: dispatching a launch
        // exits the applet immediately, so there would be no frames left to
        // animate in. The host polls this after Render and dispatches it exactly
        // as it dispatches an OnButton result. Returns None when there is
        // nothing waiting, and clears itself when it returns anything else.
        Action TakePendingAction(u64 &out_app_id);

    private:
        enum class Screen { Oobe, Welcome, Main, Theming, Themes, ThemeEditor, ColorPicker,
                            Fonts, Widgets, WidgetOptions, Keyboard, Music, Homebrew, About,
                            SysEntries, Power, Payloads, Album, FlowMenu, FlowSettings, Files,
                            Network, CoverPicker,
                            // Deck: its side menu, the full-library grid, and the
                            // reader a news card opens into.
                            DeckMenu, DeckLibrary, DeckNews };
        enum class Dialog { None, ConfirmCloseForLaunch, ConfirmCloseGame, ConfirmPower,
                            ConfirmRetroArch, ConfirmDelete };

        void RebuildItems();

        // Chooses and plays the cue for a press, from what the handler did.
        void   PlayButtonSfx(Btn b, Action a);
        // Raised by a handler when its press deserves the confirmation cue
        // rather than the ordinary click; cleared before every dispatch.
        bool   m_sfx_confirm = false;

        // A file under this account's config folder,
        // "sdmc:/slaunch/config/users/<account id>/<filename>". See UserCfg.hpp
        // for which settings are per account and which belong to the console.
        std::string GetUserConfigPath(const char *filename) const;
        void        EnsureUserConfigDir() const;

        // The per-screen routing OnButton and OnTouch both funnel through, so
        // a tap gets the same PlayButtonSfx() cue a button press does. See the
        // definition for why this is split out of OnButton.
        Action DispatchButton(Btn b, u64 &out_app_id);

        Action OnButtonOobe(Btn b);
        Action OnButtonMain(Btn b, u64 &out_app_id);
        // What an A press on m_items[m_cursor] does; also called directly by
        // DeckMenu/FlowMenu for their own selection. See its own definition.
        Action ActivateSelected(u64 &out_app_id);
        Action OnButtonOptions(Btn b, u64 &out_app_id);
        Action OnButtonTheming(Btn b);
        Action OnButtonAbout(Btn b);
        void   DrawAbout();

        // ---- network screen -------------------------------------------------
        // nifm answers over IPC, so the values are polled on a timer and cached
        // rather than read per frame.
        Action OnButtonNetwork(Btn b);
        void   DrawNetwork();
        void   RefreshNetwork(bool force);
        struct NetStatus {
            bool        connected = false;
            bool        ethernet  = false;
            bool        wifi_on   = true;
            int         strength  = 0;      // 0-3
            std::string name;               // SSID / profile name
            std::string ip;
        };
        NetStatus m_net{};
        int       m_net_cursor = 0;
        u64       m_net_tick   = 0;

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

        // ---- Album (screenshot browser) -------------------------------------
        // Screenshots live under sdmc:/Nintendo/Album/<year>/<month>/<day>/.
        // Exactly one image is ever decoded at a time - the selected one - and
        // it is freed the moment the cursor moves off it. A console can hold
        // thousands of 1280x720 JPEGs, so a thumbnail cache here would be a
        // straightforward way to run the menu out of memory.
        Action OnButtonAlbum(Btn b);
        void   OpenAlbumViewer();         // scan (lazily) + show the browser
        // ---- tiles (UiMode::Grid) -------------------------------------------
        // A Windows 8 / Windows Phone start screen: flat tiles on a fixed unit
        // grid, some of them live. Games are one unit; the Album and Music tiles
        // are two wide and show content rather than an icon.
        struct TileRect { int item; int x, y, w, h; };
        void BuildTiles(std::vector<TileRect> &out) const;
        int  TileNeighbour(int dir) const;   // 0 left, 1 right, 2 up, 3 down
        int  TileRowOf(int item) const;      // packed row an entry landed on
        int  TileRowCount() const;
        int  TileFirstInRow(int row) const;  // leftmost entry on a row
        int  TileMaxScroll() const;          // rows the view can travel

        // Wall shape. The counts are the user's; everything else is derived from
        // them so the tiles always square up inside the band.
        int  TileCols()    const { return m_tile_cols; }
        int  TileRowsVis() const { return m_tile_rows; }
        int  TileUnit()    const;    // one square unit, sized to fit
        int  TilePitch()   const;
        int  TileWideW()   const;    // two units and the gap between them
        int  TileLeft()    const;    // x of column 0
        int  TileTop()     const;    // y of row 0 at scroll 0
        int  TileRowsOf(int h) const;
        void SetTileCols(int n);
        void SetTileRows(int n);
        // Anti-aliasing. Read by main() straight from the config before Gfx is
        // created, because it decides how the renderer is built; the copy here
        // is only so Theming can show and change it.
        bool m_antialias = false;
        int  m_tile_cols = 9;
        int  m_tile_rows = 4;

        // ---- per-entry tile customisation -----------------------------------
        // Size and colour, keyed by ItemKey and persisted to config/tiles.txt.
        // A key with no matching entry anywhere else is also how a widget tile
        // is remembered - the config is the only record that it exists.
        struct TileCfg {
            int       w = 0, h = 0;      // units; 0 means "use the kind's default"
            bool      has_color = false;
            SDL_Color color {};
        };
        std::unordered_map<std::string, TileCfg> m_tilecfg;
        void      LoadTileCfg();
        void      SaveTileCfg();
        TileCfg  &TileCfgFor(const std::string &key);
        void      TileSpan(const MenuItem &it, int &w, int &h) const;
        void      CycleTileSize(const std::string &key);
        const char *TileSizeLabel(const std::string &key) const;

        // Displayed colours, eased toward the configured ones each frame so a
        // recolour arrives as a fade rather than a jump.
        std::unordered_map<std::string, SDL_Color> m_tile_shown;
        SDL_Color TileShownColor(const std::string &key, SDL_Color target);

        // Widget tiles render through a scratch target at the width widgets are
        // authored for, then scale into whatever box they were given. One
        // texture serves every widget tile, because each is drawn and blitted
        // before the next one starts.
        SDL_Texture *m_tile_wscratch = nullptr;
        void FreeWidgetTileTextures();
        int  WidgetIndexByName(const std::string &n);
        void DrawWidgetTile(const TileRect &r, const MenuItem &it, Uint8 a);
        void AddWidgetTile(int widget_index);
        // By value: it rebuilds the entry list, which is where the caller's
        // reference would have been living.
        void RemoveWidgetTile(std::string name);
        void DrawTileFace(const TileRect &r, const MenuItem &it, bool sel, Uint8 a, int pass = -1);
        SDL_Color TileColor(int idx) const;  // theme accent, hue-shifted per tile
        void UpdateLiveAlbum();              // advance the cycling picture tile

        // The picture tile holds one image and cross-fades to the next, so at
        // most two are ever decoded - a tile is 304x148, so this is cheap.
        SDL_Texture *m_tile_pic      = nullptr;
        SDL_Texture *m_tile_pic_next = nullptr;
        int          m_tile_pic_idx  = -1;
        u64          m_tile_pic_tick = 0;
        float        m_tile_pic_fade = 1.0f;

        void   ScanAlbum();               // fill m_album from the SD
        void   EnsureAlbumTexture();      // decode the selected shot, free the old
        void   FreeAlbumTexture();
        // Video clips (.mp4) in the album are played back in the menu on NVDEC
        // (gfx::VideoPlayer) rather than handed to the system Album applet.
        // PlayAlbumVideo mutes the background music for the clip's duration and
        // returns false if it cannot be opened; StopAlbumVideo restores the
        // music to whatever state it was in before the clip started.
        bool   PlayAlbumVideo(const std::string &path);
        void   PreviewAlbumVideo(const std::string &path);   // silent, in the list panel
        void   StopAlbumVideo();
        std::vector<std::string> m_album; // screenshot paths, newest last
        int          m_album_cursor  = 0;
        // Animated position of the list, chasing m_album_cursor. Wrapping from
        // the last shot back to the first is snapped rather than animated, so
        // the list does not fly the whole length of the library to get there.
        float        m_album_scroll  = 0.0f;
        bool         m_album_scanned = false;
        bool         m_album_full    = false;  // fullscreen view of the selection
        SDL_Texture *m_album_tex     = nullptr;
        int          m_album_tex_idx = -1;     // which m_album entry m_album_tex is
        // Album clip playback (see PlayAlbumVideo / StopAlbumVideo). Owns its own
        // decode thread + GPU texture, independent of m_video_player (the
        // wallpaper), so a clip can play on top of a video wallpaper.
        gfx::VideoPlayer m_album_video;
        bool         m_album_video_active = false;   // a clip is currently playing
        std::string  m_album_video_path;             // which m_album entry is open
        bool         m_album_video_ok     = false;   // ...and whether it opened
        bool         m_album_music_was_on = true;    // music state before the clip
        // Full screen viewer state (see AlbumShow / DrawAlbumViewerUi).
        bool         m_album_slideshow  = false;
        bool         m_album_ui_pinned  = false;     // photo info bar kept up
        u64          m_album_ui_tick    = 0;         // last input, for the fade
        u64          m_album_slide_tick = 0;         // when the current slide began
        void         AlbumShow(int i);
        void         AlbumPokeUi();
        void         DrawAlbumViewerUi();
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
        // back is where B/A return to, and preview drives the theme live-refresh
        // that only makes sense while editing a theme's own colours.
        void OpenColorPicker(SDL_Color *target, Screen back = Screen::ThemeEditor,
                             bool preview = true);
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
        void BuildOptions();
        int  UnplacedWidgets();   // widgets not already on the wall        // populate the X-menu for the current selection

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
        // XMB header: title hard left, clock and battery hard right. Takes an
        // already-localized title, since its two callers get theirs from
        // different places.
        void DrawXmbHeader(const char *title);
        // "Downloading..." only - the one hint that isn't a button prompt.
        // Everything else goes through the icon overload below.
        void DrawHint(const char *hint);
        void DrawHint(std::initializer_list<HintSeg> segs);
        // Tappable "go back" corner, drawn on every non-Main screen; see the
        // definition and the kBackTap* constants for why it exists.
        void DrawBackTap();
        void DrawStatusHint(std::initializer_list<HintSeg> segs); // fresh status line (if any) + hint
        // Shared main-menu-style carousel used by the sub-screens too. Each row
        // is a label plus an optional right-hand value string.
        void DrawCarousel(const std::vector<std::string> &labels,
                          const std::vector<std::string> &values,
                          int cursor, float &scroll_pos);
        // XMB-styled variant of the above, used for every sub-screen while the
        // main layout is XMB so the whole menu reads as one thing.
        // alpha scales the whole column, so a caller sliding a category row can
        // cross-fade the list under it the way the main screen does.
        void DrawCarouselXmb(const std::vector<std::string> &labels,
                             const std::vector<std::string> &values,
                             int cursor, float &scroll_pos, Uint8 alpha = 255);
        void DrawOobe();
        void DrawMain();
        void DrawMainList();        // original text carousel
        void DrawMainLine();        // horizontal cover carousel (EmulationStation)
        void DrawMainGrid();        // page of icon tiles
        void DrawMainCover();       // fullscreen single-cover pager
        void DrawMainShelf();       // Xbox-360-style uniform cover row
        void DrawMainXmb();          // PSP/PS3 cross-media bar
        void DrawMainFlow();         // WiiFlow-style 3D coverflow
        int  FlowItemAt(int px, int py) const;  // box under a touch, or -1
        float m_rstick_x = 0.0f, m_rstick_y = 0.0f;

        // Animated position of the flow row, in item units.
        float m_flow_scroll = 0.0f;
        // Momentum, shared by every layout.
        //
        // Each layout keeps its scroll position in a different member, so rather
        // than repeating the same fling five times, ActiveAxis() hands back
        // whichever one is live and the fling works through that.
        // wrap: this axis is endless, so a position past either end folds back
        // into the list instead of stopping there.
        struct ScrollAxis { float *pos = nullptr; int max = 0; bool wrap = false; };
        ScrollAxis ActiveAxis();
        void  SyncCursorFromScroll();   // scroll -> cursor, per layout
        void  StepFling();              // advance and decay, once a frame
        bool  ScrollBusy() const;       // finger down, or still coasting
        float m_fling_vel  = 0.0f;      // items per second
        u64   m_fling_tick = 0;         // last frame, for the fling step
        u64   m_drag_tick  = 0;         // last drag sample, for measuring speed
        // Camera yaw (look left/right) and dolly (stick Y pulls the row closer
        // or pushes it away, which reads as a field-of-view change).
        float m_flow_yaw    = 0.0f;
        float m_flow_dolly  = 0.0f;
        // Extra spin on the selected box, so you can turn it around and read the
        // back of the case. Driven by the right stick and springs back when let
        // go, so you never end up parked facing the wrong way.
        float m_flow_spin   = 0.0f;
        // Indices into m_items of just the launchable content. Flow is a shelf of
        // games, so the system entries live behind Minus instead of sitting in
        // the row pretending to be boxes.
        std::vector<int> m_flow_items;
        int  m_flow_menu_cursor = 0;
        void FlowRebuild();
        // Optional box wrap (back | spine | front) from
        // sdmc:/slaunch/covers/template.png. Never shipped with the menu: it is
        // user-supplied artwork, so nothing here depends on it existing. Without
        // it Flow draws flat covers; with it, boxes with a real spine.
        SDL_Texture *m_flow_wrap = nullptr;
        bool         m_flow_wrap_tried = false;
        void         EnsureFlowWrap();
        // Box front art, keyed by title id. A null value is cached too, so a
        // title with no cover costs one failed open rather than one per frame.
        std::unordered_map<u64, SDL_Texture *> m_covers;
        SDL_Texture *FlowCover(const MenuItem &it);
        // Deck's hero tile, keyed and cached the same way (a null entry for a
        // miss included) but a different file and a different crop - see
        // HeroArt's own definition, Menu_Deck.cpp.
        std::unordered_map<u64, SDL_Texture *> m_hero_art;
        SDL_Texture *HeroArt(const MenuItem &it);
        int m_cover_budget  = 6;  // finished art to upload this frame
        // Set every time a cover is decoded, so the notice shows only while the
        // cache is actually being built and disappears on its own after.
        u64 m_cache_msg_tick = 0;
        int m_cache_built    = 0;

        // ---- background hero decode -----------------------------------------
        // The two panels on the back of a case are heroes: 1920x620 at source,
        // two per title, and only ever seen if you turn a box round. That makes
        // them the least important thing the shelf draws and by some way the
        // dearest to decode, so they are decoded off the main thread and simply
        // appear when they are ready.
        //
        // Only the decode runs on the worker. Creating the texture needs the
        // renderer, so the surfaces come back and the main thread uploads them.
        Thread            m_shot_thread {};
        std::atomic<bool> m_shot_done { false };
        bool              m_shot_running = false;
        u64               m_shot_job_id  = 0;
        SDL_Surface      *m_shot_surf_a  = nullptr;
        SDL_Surface      *m_shot_surf_b  = nullptr;
        char              m_shot_path_a[96] = {};
        char              m_shot_path_b[96] = {};
        void   StartShotDecode(u64 app_id);
        void   PollShotDecode();
        static void ShotDecodeTrampoline(void *self);
        // The two panels printed across the top of a case's back. Cached the
        // same way as covers, with a null entry remembered so a title without
        // them costs one failed open rather than one per frame.
        struct FlowShots { SDL_Texture *a = nullptr, *b = nullptr; };
        std::unordered_map<u64, FlowShots> m_shots;
        const FlowShots &FlowBackShots(const MenuItem &it);

        // ---- background art loader ------------------------------------------
        // Covers, box scans and hero art are read from the texture cache (or
        // decoded and cached) on one worker; the main thread only uploads the
        // finished surfaces. Art that is not ready yet is simply not drawn
        // for a frame or two, instead of the whole menu stopping for it.
        enum ArtKind { Art_Cover, Art_Wrap, Art_Hero };
        struct ArtJob { u64 id; int kind; u32 epoch; SDL_Surface *surf; bool built; };
        std::mutex              m_art_mx;
        std::condition_variable m_art_cv;
        std::deque<ArtJob>      m_art_q;       // newest at the back, served first
        std::vector<ArtJob>     m_art_done;
        bool                    m_art_quit = false;          // under m_art_mx
        std::set<std::pair<int, u64>> m_art_pending;         // main thread only
        u32                     m_art_epoch = 0;   // bumped when art on disk changes
        bool                    m_art_started = false;
        Thread                  m_art_thread {};
        void   QueueArt(int kind, u64 id);
        void   PollArt();
        void   StopArt();
        static void ArtTrampoline(void *self);

        // ---- SteamGridDB cover fetch ---------------------------------------
        // One title at a time on a worker, newest request wins, results land in
        // covers/<titleid>.jpg and are picked up by FlowCover on the next frame.
        // The key is read from config/steamgriddb.txt and never ships with the
        // menu; with no key the whole feature stays dormant.
        void   StartCoverFetch(u64 app_id, const std::string &name);
        void   PollCoverFetch();
        static void CoverFetchTrampoline(void *self);
        // What the fetcher is doing, so it is visible rather than silent. The
        // worker only ever writes it and the main thread only ever reads it.
        enum class CoverState { Idle, NoKey, BadKey, Searching, NoMatch, NoArt, Failed, Filtered, Got };
        std::atomic<int> m_cover_state { (int)CoverState::Idle };
        // What the art worker is doing right now, for the status bar
        // (DrawFetchStatus). Stages run top to bottom; the last three are
        // outcomes, shown for a moment once the worker is done.
        std::atomic<int>      m_fetch_stage { (int)FetchStage::Idle };
        std::atomic<uint64_t> m_fetch_now { 0 }, m_fetch_total { 0 };
        u64                   m_fetch_end_tick = 0;
        std::string           m_fetch_title;       // main thread's copy of the name
        void DrawFetchStatus();
        // USB file transfer (usb::Mtp*): a tag in the top bar while a computer
        // is connected, and a card with the file and its progress while
        // something is moving. Menu_Files.cpp.
        usb::MtpStatus m_usb;
        u64    m_usb_active_tick = 0;          // last frame something was moving
        usb::MtpOp m_usb_last_op = usb::MtpOp::None;   // what it was
        u64    m_usb_rate_tick = 0, m_usb_rate_bytes = 0;
        double m_usb_rate = 0.0;               // bytes a second, smoothed
        u32    m_usb_changes = 0;              // received + deleted, to refresh Files
        void   PollUsb();
        void   DrawUsbStatus();
        void   DrawUsbTag(int right_x, int y);   // right-aligned to right_x
        void FetchArtFor(const MenuItem &it);
        // SteamGridDB is opt-in: GameTDB (no key) is the source of box art,
        // and SteamGridDB is only asked when this is on and a key is set.
        bool m_sgdb_enabled = false;
        u64              m_cover_ok_count = 0;
        std::string m_sgdb_key;         // empty = feature off
        bool        m_sgdb_key_loaded = false;
        Thread      m_cover_thread {};
        std::atomic<bool> m_cover_done { false };
        bool        m_cover_running = false;
        u64         m_cover_id   = 0;   // title being fetched
        std::string m_cover_name;       // its name, for the search
        bool        m_cover_ok   = false;
        bool        m_shots_ok   = false;   // screenshots landed this fetch
        bool        m_hero_ok    = false;   // hero art landed this fetch
        bool        m_wrap_ok    = false;   // a GameTDB box scan landed this fetch
        // GameTDB box scans: the whole printed insert (back | spine | front),
        // one per game, drawn on Flow's cases. Loaded for the few boxes near
        // the selection only - they are big - and dropped as it moves on.
        SDL_Texture *GameWrap(const MenuItem &it);
        bool         TdbFront(u64 app_id);   // cover came from the same scan
        std::unordered_map<u64, SDL_Texture *> m_game_wraps;
        std::unordered_map<u64, bool>          m_tdb_front;
        int         m_tdb_region = 0;       // index into kTdbRegions (Theming)
        // Titles already attempted this session, so a miss is not retried on
        // every cursor move.
        std::unordered_map<u64, bool> m_cover_tried;

        // Real screenshots come from Steam's store API, which needs no key at
        // all: search the name for an appid, then ask for that app's
        // screenshots. SteamGridDB has none of its own past grids, heroes,
        // logos and icons, so a screenshot is the only art that's actually
        // *from* the game - which is what the back-of-case panels want, and
        // why they take Steam's shots and nothing else; Nintendo's own titles,
        // which Steam does not carry, simply have no panels rather than a
        // misleading stand-in.
        //
        // Deck's hero tile wants the opposite trade-off - it is the single
        // biggest, most-looked-at thing on that screen, and SteamGridDB's
        // heroes (wide key art, 1920x620) read as a banner where a gameplay
        // screenshot reads as a random moment. Fetched and cached completely
        // separately from the panels above (m_hero_ok / HeroArt, Menu_Deck.cpp),
        // even though both ride the same cover-fetch worker.
        //
        // One failed request turns the source off for the rest of the session:
        // something unreachable rather than merely empty would otherwise cost
        // the full timeout on every title before falling back. RAWG was tried
        // first and had to be dropped for exactly that - it stopped answering
        // altogether.
        bool m_steam_dead = false;
        bool        RawgKeyPresent();

        // ---- launch animation -----------------------------------------------
        // A short bounce on the chosen case before Flow hands off, so launching
        // reads as a deliberate act rather than the picture vanishing.
        // Appear fade, the mirror of the launch fade. The menu comes up over
        // whatever was on screen - usually the game you just left - so starting
        // black and lifting reads as the menu arriving rather than as a cut. It
        // also covers the first few frames, which are exactly the ones still
        // filling in icons and box art.
        u64    m_appear_tick   = 0;             // set on the first frame drawn
        // Dissolving in from the game's own last frame was tried and does not
        // work: capsscCaptureRawImageWithTimeout on ViLayerStack_LastFrame -
        // the stack the system keeps for exactly this - is refused from a
        // library applet, consistently and in 2ms. It is a system-applet
        // privilege, which sSystem has and the menu does not. Routing it through
        // the daemon would mean moving 3.7MB across, and the only channel is a
        // file on the card, which costs more than the effect is worth. So the
        // fade comes up from black.
        // Progress 0..1 rather than a start time. The first frames after a HOME
        // press are the slowest the menu ever draws - cache building, icons -
        // and a purely time-based fade simply elapsed between frame one and
        // frame two, so the whole thing came out as a single black frame and a
        // cut. Advancing this per frame, capped, guarantees it is actually seen.
        float m_appear_p    = 0.0f;
        u64   m_appear_prev = 0;


        u64    m_launch_tick   = 0;             // start tick; 0 when idle
        // The action has been handed to the host, but the animation state is
        // deliberately kept so the screen stays black until the applet exits.
        bool   m_launch_fired  = false;
        u64    m_launch_id     = 0;
        Action m_launch_action = Action::None;
        Action m_pending_action = Action::None; // waiting for the host to take it
        u64    m_pending_id     = 0;
        void   StartLaunchAnim(Action a, u64 app_id);
        // Progress 0..1, or -1 when no animation is running.
        float  LaunchAnimT() const;

        // ---- cover picker ---------------------------------------------------
        // Name matching picks one grid out of however many SteamGridDB holds,
        // and it is often not the one you would have chosen. This lists them all
        // and lets you take the one you want.
        //
        // Only the atomics cross the thread boundary. The url vectors are built
        // in full before m_pick_have is first released, and are not touched
        // afterwards, so reading them behind an acquire load of that counter is
        // safe without a lock.
        enum class PickState { Idle, Searching, Listing, Ready,
                               NoMatch, NoArt, Failed, NoKey, BadKey,
                               Applying, Applied };
        std::atomic<int>  m_pick_state { (int)PickState::Idle };
        std::atomic<int>  m_pick_have  { 0 };   // thumbnails on disk so far
        std::atomic<int>  m_pick_total { 0 };   // grids the search turned up
        std::atomic<bool> m_pick_done  { false };
        Thread            m_pick_thread {};
        bool              m_pick_running = false;
        bool              m_pick_apply   = false;  // worker is fetching the choice
        // Choosing must not wait for the listing to finish. The thumbnails are
        // downloaded one at a time, so insisting the worker be idle before A did
        // anything meant sitting through every remaining download - about ten
        // seconds - to take a cover already visible on screen. Pressing A now
        // asks the download loop to stop where it is and remembers the choice
        // until the worker has been joined.
        std::atomic<bool> m_pick_abort { false };
        bool              m_pick_want_apply = false;
        int               m_pick_choice  = 0;
        u64               m_pick_id      = 0;
        std::string       m_pick_name;
        std::vector<std::string>   m_pick_urls;    // full size
        std::vector<std::string>   m_pick_thumbs;  // preview
        std::vector<SDL_Texture *> m_pick_tex;
        int   m_pick_cursor = 0;
        float m_pick_scroll = 0.0f;               // animated row offset

        void   EnterCoverPicker();
        void   LeaveCoverPicker();
        void   PollCoverPicker();
        void   StartPickApply();   // spawn the worker that downloads the choice
        static void CoverPickTrampoline(void *self);
        Action OnButtonCoverPicker(Btn b);
        void   DrawCoverPicker();
        int    CoverPickAt(int x, int y) const;   // touch: cell under a point, or -1
        // Sort mode -> short label for the shelf header / options menu.
        const char *SortLabel() const;
        void DrawMainEmpty();       // "Loading..." / "No apps found" placeholder
        // Draw one app/entry as a square tile: cached icon if present, else a
        // themed placeholder with the name. Used by the Line and Grid modes.
        void DrawAppTile(const MenuItem &it, int x, int y, int size,
                         bool selected, Uint8 alpha, bool reflect = false);
        // Selection glow strength, gently pulsing, shared by every layout.
        static float SelectionGlow() {
            const float s = (float)armGetSystemTick() / (float)armGetSystemTickFreq();
            return 0.80f + 0.20f * sinf(s * 3.0f);
        }
        // The selected item's art, blurred, filling the screen behind a
        // layout; cross-fades when the selection settles somewhere new.
        void DrawSelectionBackdrop();
        std::string  m_bd_key;
        SDL_Texture *m_bd_cur = nullptr, *m_bd_old = nullptr;
        u64          m_bd_tick = 0, m_bd_moved = 0;
        bool         m_bd_pending = false;
        // Packed-layout queries, all answered from BuildTiles so scrolling,
        // touch and navigation can never disagree with what was drawn.
        // Item index under a touch point (or -1), for touch-to-select/launch.
        int  MainItemAt(int x, int y) const;   // dispatches by UI mode
        int  GridItemAt(int x, int y) const;
        int  ListItemAt(int x, int y) const;
        // Load (cached) the black/white icon for a non-game menu entry, or null.
        SDL_Texture *SystemIcon(ItemKind kind);
        void InvalidateSysIcons(); // clear the cached system icon textures
        // Load (cached) one button-prompt glyph from sdmc:/slaunch/icons/buttons/,
        // white-on-alpha so DrawHint can tint it to the theme like any other
        // hint text. Null (cached) on a missing file - a hint bar with a
        // dropped icon still fits its space, just without the glyph.
        SDL_Texture *HintIcon(const char *name);
        void DrawOptions();
        void DrawTheming();
        std::vector<int> ThemingRows() const;   // visible Theming rows
        bool SgdbKeyPresent();                  // loads the key on first ask
        void DrawThemes();
        void DrawEditor();
        void DrawColorPicker();
        void DrawFonts();
        void DrawSdRemoved();       // full-screen "SD card removed" warning
        void DrawWidgets();         // list detected Lua widgets
        void DrawWidgetOptions();   // exposed options of the selected widget
        void DrawMusic();           // menu-music controls
        void DrawHomebrew();        // .nro browser
        void DrawAlbum();           // screenshot browser + fullscreen viewer
        // ---- Deck layout ----------------------------------------------------
        void DrawMainDeck();        // hero + cover row + card row
        void DrawDeckMenu();        // side menu: everything that isn't a game
        void DrawDeckMenuClosing(float u);       // tail of closing it; see definition
        void DrawDeckMenuPanel(int xoff, Uint8 dimAlpha); // shared by both
        void DrawDeckLibrary();     // the whole library, as a grid of covers
        void DrawDeckNews();        // one story, read in place
        // Navigation for the Deck home screen. True when the press was dealt
        // with here; false lets OnButtonMain run its shared A/X/Plus handling,
        // which is what launches the selected game.
        bool DeckNav(Btn b, Action &out, u64 &out_app_id);
        Action OnButtonDeckMenu(Btn b, u64 &out_app_id);
        Action OnButtonDeckLibrary(Btn b, u64 &out_app_id);
        Action OnButtonDeckNews(Btn b);
        // Ask the feed for whatever the current tab needs. Cheap to call every
        // frame: the feed drops requests it has already served.
        void   DeckPollNews();
        const std::vector<news::Item> &DeckCards() const;
        // Card art, decoded once and cached by path.
        SDL_Texture *DeckArt(const std::string &path);
        void   DeckFreeArt();
        // Word-wrapped text into a fixed width, at most max_lines lines with the
        // last ellipsised. Returns the lines drawn, so a caller can advance.
        int    DeckWrapText(const std::string &s, int x, int y, int w,
                            gfx::FontSize fs, SDL_Color c, int max_lines);
        int    EnabledWidgetCount();
        // The home row: indices into m_items, most recently played first (that
        // is the entry the wide tile is for).
        void   DeckRow(std::vector<int> &out) const;
        int    DeckSlotX(int slot) const;    // left edge of a slot, unscrolled
        void   DeckSyncCursor();             // keep the selection on a real tile
        // The library grid, filtered by the selected tab: indices into m_items.
        void   DeckLibraryList(std::vector<int> &out) const;
        int    DeckLibraryAt(int px, int py) const;   // library cell, or -1
        int    DeckItemAt(int px, int py) const;      // hero/cover item, or -1
        // Taps the home screen handles itself: the tab row and the cards. True
        // when the tap was one of those, so the shared hit-test is not also run
        // on it. Touch for the Deck screens proper is in OnTouch.
        bool   DeckTap(int px, int py, u64 &out_app_id, Action &out);

        news::Feed m_news;
        int   m_deck_row  = 0;      // 0 covers, 1 tabs, 2 cards
        int   m_deck_tab  = 0;      // 0 this game, 1 Nintendo, 2 widgets
        int   m_deck_card = 0;
        int   m_deck_menu_cursor = 0;
        int   m_deck_lib_tab     = 0;
        int   m_deck_lib_cursor  = 0;
        float m_deck_lib_scroll  = 0.0f;
        float m_deck_scroll      = 0.0f;   // eased cover-row position
        int   m_deck_reading     = 0;      // card open in the reader
        // Set while a screen was opened from the Deck side menu, so B comes back
        // to it rather than dropping onto the home screen.
        bool  m_from_deck_menu = false;
        // Set while the home screen is being painted as the side menu's
        // backdrop, so it leaves the hint line to the menu on top of it.
        bool  m_deck_backdrop  = false;
        std::unordered_map<std::string, SDL_Texture *> m_news_art;

        void DrawFlowMenu();        // Flow's Minus menu: everything that isn't a game
        void DrawFlowSettings();    // live tuning of the shelf layout
        Action OnButtonFlowSettings(Btn b);
        int  m_flowset_cursor = 0;
        void LoadFlowConfig();
        void SaveFlowConfig();
        // Set while a screen was opened from the Flow menu, so B goes back
        // there instead of dropping you onto the shelf.
        bool m_from_flow_menu = false;
        Screen BackTarget();
        Action OnButtonFlowMenu(Btn b, u64 &out_app_id);
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
        // Active when the theme's wallpaper is a video file (.mp4) instead of
        // a still image - see IsVideoPath. Owns its own decode thread and GPU
        // texture; DrawBackground reads GetTexture() the same way it reads
        // m_wallpaper for a static image.
        gfx::VideoPlayer m_video_player;

        // Gaussian-blur an image file, baking the result into a new texture.
        // Reads the file as an SDL_Surface (CPU memory), blurs on CPU, uploads.
        // Caller owns the returned texture (free with m_gfx->FreeImage).
        SDL_Texture *BlurImage(const char *path);

        Screen m_screen = Screen::Main;
        Dialog m_dialog = Dialog::None;

        // Screen-transition fade: Render() compares m_screen against
        // m_screen_seen every frame, and a mismatch (however m_screen got
        // changed - there are ~70 call sites across every mode and submenu)
        // stamps m_screen_trans_tick and starts the fade. One mechanism
        // catches every screen change without any of those call sites having
        // to know about it. 0 means no fade is running.
        Screen m_screen_seen      = Screen::Main;
        u64    m_screen_trans_tick = 0;
        // DeckMenu has its own slide instead (DrawDeckMenu, reading
        // m_screen_trans_tick itself) - set whenever it is either end of the
        // transition, so the generic full-screen darken doesn't also play
        // underneath the slide.
        bool   m_suppress_screen_fade = false;
        // Set when DeckMenu closes back to the Deck home screen (not when it
        // opens another screen instead), so Render() can keep drawing the
        // panel sliding back out over the now-current Main screen for one
        // more short window, instead of it just vanishing.
        u64    m_deck_menu_close_tick = 0;

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
        // Shelf draws portrait 2:3 tiles instead of square ones, using the
        // fetched box art when a game has it. Shares the cover cache with Flow.
        bool      m_shelf_vertical = false;
        // Whether moving past either end of the list loops round to the other.
        // On by default, which is how every layout has always behaved.
        bool      m_wrap_nav = true;
        // Screen furniture, both on by default. Hints are the control legend
        // along the bottom; the counter is the "3 / 47" position readout.
        // Status messages ("Saved", "Pinned") are not hints and always show.
        bool      m_show_hints   = true;
        bool      m_show_counter = true;
        // Whether launcher shortcuts join the other layouts too. XMB always
        // shows them - it gives them a column each, which is the whole point -
        // but a few thousand ROMs poured into the Grid or the Deck buries the
        // games they sit next to, so everywhere else is opt-in.
        bool      m_shortcuts_everywhere = false;
        // RetroArch playlists at all (beta, opt-in behind a warning).
        bool      m_retroarch = false;
        // Active name filter, empty when not searching. Deliberately not
        // persisted: a menu that opened still filtered, with no obvious way
        // back, would read as a menu that had lost your games.
        std::string m_search;
        // Periodic memory snapshot; see the trace block in Render().
        bool m_memtrace_on   = false;
        u64  m_memtrace_tick = 0;
        void OpenSearch();   // built-in keyboard, seeded with the current filter
        static bool ContainsFold(const std::string &hay, const std::string &needle);
        int  ShelfTileW() const { return m_shelf_vertical ? 152 : 208; }
        int  ShelfTileH() const { return m_shelf_vertical ? 228 : 208; }
        int  ShelfPitch() const { return ShelfTileW() + kShelfGapPx; }
        static constexpr int kShelfGapPx = 20;
        UiMode    m_ui_mode = UiMode::XMB;    // main-screen layout (default)
        // Language override; "auto" follows the console. Applied through
        // LocaleInit, which rebuilds the string table in place.
        char      m_lang[8] = "auto";
        int       m_lang_idx = 0;             // index into kLangs
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
        std::string    m_hb_launch_argv;      // ditto; empty = just the path
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

        // Launcher shortcuts (RetroArch playlists + shortcuts.txt). Read on the
        // same worker as the NRO scan rather than one of their own: both are SD
        // reads wanted at the same moment, and one thread doing them in turn
        // beats two contending for the card.
        std::vector<hb::Shortcut> m_shortcuts;
        std::vector<hb::Shortcut> m_shortcut_scan_result;
        // Art for a shortcut lives outside the icon cache (RetroArch's own
        // thumbnails), so the cache is handed this to resolve those keys.
        std::unordered_map<u64, std::string> m_shortcut_art;
        void AdoptShortcuts(std::vector<hb::Shortcut> &&found);

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
        std::unordered_map<std::string, SDL_Texture*> m_hint_icons; // button-prompt glyphs
        std::vector<std::pair<u64, std::string>> m_names; // app_id -> custom name
        int  m_theming_cursor = 0;
        bool m_jumped_to_suspended = false;
        // Set when the deferred worker is *started*, not when it finishes - it
        // gates the play-stats query, which is its own worker and is happy to
        // run alongside. Use m_deferred_joined for anything that needs the
        // worker's results to exist.
        bool m_deferred_done = false;
        Thread m_deferred_thread {};
        std::atomic<bool> m_deferred_flag { false };  // worker -> main: finished
        bool m_deferred_started = false;
        bool m_deferred_joined  = false;
        bool m_deferred_audio   = false;  // worker's Music::Init result
        bool m_want_exit = false;   // asked the daemon to show the keyboard
        bool m_sd_removed = false;  // SD pulled while on -> show warning, await reboot

        // Home screen widgets
        widgets::Widgets m_widgets;

        dbg::Overlay m_debug;
        dbg::Counters DebugCounters() const;   // cache/entry counts for the overlay

        // Options overlay for the selected item.
        // arg carries which thing the action applies to when the label alone is
        // not enough - the "Add widget" entries, one per unplaced widget.
        struct OptionEntry { std::string label; int action; int arg = 0; };
        std::vector<OptionEntry> m_options;
        // Which submenu the overlay is showing instead of the entry's own
        // options. B steps back out of one instead of closing the overlay.
        enum OptSub { Sub_None = 0, Sub_Widgets };
        int m_options_sub = Sub_None;
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
        // PSP order, near enough: Settings, then Media, then the things you
        // actually run. Media holds both screenshots and music rather than
        // splitting them into a column each - with one entry apiece that was two
        // near-empty columns where one reads better. Empty categories are
        // dropped by XmbRebuild.
        enum class XmbCat { Settings, Media, User, Network, Game, Homebrew, Count };
        struct XmbColumn {
            XmbCat           cat;
            // Set only for a shortcut category, which has no XmbCat of its own:
            // when non-empty it is the column's name, and the column borrows the
            // Game headline icon. Everything else leaves it empty and is named
            // by XmbCatName(cat) exactly as before.
            std::string      label;
            std::vector<int> items;   // indices into m_items, in list order
        };
        std::vector<XmbColumn> m_xmb_cols;      // non-empty columns only, PSP order
        int   m_xmb_col         = -1;   // index into m_xmb_cols (-1 = not placed yet)
        int   m_xmb_item        = 0;    // index into m_xmb_cols[m_xmb_col].items
        float m_xmb_col_scroll  = 0.0f; // animated bar position
        float m_xmb_item_scroll = 0.0f; // animated column position

        static char XmbInitial(const std::string &name);
        const char *XmbShoulderHint() const;   // "Letter" or "Jump"
        int  XmbLetterJump(int dir) const;   // next/prev initial in the open column
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
        static constexpr int kOobeSteps = 5;
        float        m_oobe_gallery = 0.0f;    // layout gallery position
        float        m_oobe_list    = 0.0f;    // theme list position
        SDL_Texture *m_oobe_prev[8] = {};      // layout screenshots, loaded lazily
        bool         m_oobe_prev_tried[8] = {};
        SDL_Texture *OobePreview(int mode);
        void         FreeOobePreviews();
        std::vector<std::string> WrapText(gfx::FontSize fs, const std::string &text, int width);
        // Animated position of the setup step row, which is drawn as an XMB
        // category row; it lags m_oobe_step so advancing a step slides.
        float m_oobe_scroll  = 0.0f;

        // Music + Widgets submenus
        // ---- File manager (Menu_Files.cpp) -----------------------------------
        struct FmEntry { std::string name; bool dir = false; long long size = -1; };
        enum class FmOp { None, Copy, Move, Delete };
        enum class FmAction { Copy, Cut, Paste, Rename, Delete, NewFolder };
        std::string            m_fm_path;
        std::vector<FmEntry>   m_fm_list;
        int                    m_fm_cursor = 0;
        float                  m_fm_scroll = 0.0f;
        bool                   m_fm_menu_open = false;
        int                    m_fm_menu_cursor = 0;
        std::string            m_fm_clip;             // copied / cut path, or empty
        bool                   m_fm_clip_cut = false;
        SDL_Texture           *m_fm_view = nullptr;   // a picture being shown
        // Background copy / move / delete.
        Thread                 m_fm_thread{};
        bool                   m_fm_running = false, m_fm_ok = false;
        FmOp                   m_fm_op = FmOp::None;
        std::string            m_fm_src, m_fm_dst;
        std::atomic<bool>      m_fm_done{false}, m_fm_cancel{false};
        std::atomic<u64>       m_fm_done_bytes{0}, m_fm_total_bytes{0};
        void   OpenFileManager();
        void   FmScan();
        void   FmGo(const std::string &path, const std::string &select);
        void   FmStart(FmOp op, const std::string &src, const std::string &dst);
        void   FmPoll();
        bool   FmCopyTree(const std::string &src, const std::string &dst);
        bool   FmDeleteTree(const std::string &p);
        static void FmWorkerTrampoline(void *self);
        std::vector<FmAction> FmActions() const;
        static const char *FmActionName(FmAction a);
        void   FmDo(FmAction a);
        void   FmKeyboardDone(bool rename, const std::string &text);
        void   FmConfirmDelete();
        Action OnButtonFiles(Btn b);
        void   OnTouchFiles(int x, int y);
        void   DrawFiles();

        int   m_music_cursor = 0;       // selected row of the music library
        float m_music_scroll = 0.0f;    // list position, chasing the cursor
        bool  m_from_theming_music = false;   // B goes back to Theming, not home
        void  OpenMusicPlayer(bool from_theming);
        void  OnTouchMusic(int x, int y);
        void  OnTouchAlbum(int x, int y);
        static std::string FormatTime(double seconds);   // m:ss / h:mm:ss
        // Cover of the current track (audio::Music::CoverArt), decoded once per
        // track change. The index is cached even when there is no art, so a
        // track without any is not re-read every frame.
        SDL_Texture *MusicArt();
        SDL_Texture *m_music_art     = nullptr;
        int          m_music_art_idx = -1;
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
        Screen     m_pick_return   = Screen::ThemeEditor;
        bool       m_pick_preview  = true;       // re-select the theme on each nudge
        bool       m_pick_tile     = false;      // editing a tile colour, so persist it

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