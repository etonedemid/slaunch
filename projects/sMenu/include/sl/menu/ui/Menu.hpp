#pragma once
#include <switch.h>
#include <sl/os/Applications.hpp>
#include <sl/menu/ui/Theme.hpp>
#include <sl/menu/gfx/Gfx.hpp>
#include <sl/menu/widgets/Widgets.hpp>
#include <vector>
#include <string>

// SDL2 menu for sLaunch.
// Screen state machine: Oobe -> Main / Themes / ThemeEditor, with an optional
// modal confirmation dialog. Input arrives as logical buttons (see Btn) that
// the host loop translates from SDL joystick events.

namespace sl::menu::ui {

    enum class Btn { None, Up, Down, Left, Right, A, B, X, Y, Plus, Minus, L, R };

    enum class ItemKind {
        Game, Theming, Themes, Fonts, Controllers, Album, UserPage,
        WebBrowser, MiiEdit, HomebrewMenu, Settings, Power,
    };

    // Horizontal alignment of the main list text.
    enum class TextAlign { Left, Center, Right };

    struct MenuItem {
        ItemKind    kind;
        u64         app_id = 0;
        std::string name;
        bool        is_gamecard  = false;
        bool        needs_update = false;
        bool        is_favourite = false;
    };

    // Order games are listed in (favourites are always pinned above the rest).
    enum class SortMode { Default, Alpha, Count };

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
            OpenWebBrowser, OpenControllers, OpenHomebrewMenu, OpenPower,
            FinishSetup, Quit,
        };

        ~Menu();

        void Init(gfx::Gfx *gfx, AccountUid user, u64 suspended_app_id, bool start_oobe);

        void SetApps(std::vector<AppEntry> apps);

        // Handle one logical button press. Returns an action (+ app id for launch).
        Action OnButton(Btn b, u64 &out_app_id);

        // Draw the current frame.
        void Render();

        void SetSuspendedApp(u64 app_id);
        void ClearSuspendedApp();
        void SetUser(AccountUid uid, const char *nickname);
        void SetStatus(const char *msg);

        // The input loop marks whether a directional press is a fresh press or
        // an auto-repeat; the list wraps only on a fresh press at an end.
        void SetNavFresh(bool fresh) { m_nav_fresh = fresh; }

        // True once the menu has asked the daemon to show the keyboard; the host
        // loop should then let the applet exit so qlaunch can show swkbd.
        bool WantsExit() const { return m_want_exit; }

    private:
        enum class Screen { Oobe, Main, Theming, Themes, ThemeEditor, ColorPicker, Fonts, Keyboard };
        enum class Dialog { None, ConfirmCloseForLaunch, ConfirmCloseGame };

        void RebuildItems();

        Action OnButtonOobe(Btn b);
        Action OnButtonMain(Btn b, u64 &out_app_id);
        Action OnButtonOptions(Btn b, u64 &out_app_id);
        Action OnButtonTheming(Btn b);
        Action OnButtonThemes(Btn b);
        Action OnButtonEditor(Btn b);
        Action OnButtonColorPicker(Btn b);
        Action OnButtonFonts(Btn b);
        Action OnButtonKeyboard(Btn b);
        void DrawKeyboard();
        Action OnButtonDialog(Btn b, u64 &out_app_id);

        // Theme editor helpers.
        void ScanWallpapers();      // list background images under slaunch/themes
        void OpenColorPicker(SDL_Color *target);
        void CycleBackground(int dir);  // Gradient <-> available images

        // Favourites + sorting.
        bool IsFavourite(u64 app_id) const;
        void ToggleFavourite(u64 app_id);
        void LoadFavourites();
        void SaveFavourites();
        void LoadSort();
        void SaveSort();
        void SelectApp(u64 app_id); // move cursor to the item for app_id
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

        void DrawBackground();
        void DrawTopBar(const char *center_title);
        void DrawHint(const char *hint);
        // Shared main-menu-style carousel used by the sub-screens too. Each row
        // is a label plus an optional right-hand value string.
        void DrawCarousel(const std::vector<std::string> &labels,
                          const std::vector<std::string> &values,
                          int cursor, float &scroll_pos);
        void DrawOobe();
        void DrawMain();
        void DrawOptions();
        void DrawTheming();
        void DrawThemes();
        void DrawEditor();
        void DrawColorPicker();
        void DrawFonts();
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
        int          m_wallpaper_theme = -1;

        Screen m_screen = Screen::Main;
        Dialog m_dialog = Dialog::None;

        std::vector<AppEntry> m_apps;
        std::vector<MenuItem> m_items;

        // Favourites (app ids, pinned to the top) + current sort order.
        std::vector<u64> m_favourites;
        SortMode         m_sort = SortMode::Default;

        // Appearance: main-list text alignment + user-renamed games.
        TextAlign m_align = TextAlign::Left;
        std::vector<std::pair<u64, std::string>> m_names; // app_id -> custom name
        int  m_theming_cursor = 0;
        bool m_jumped_to_suspended = false;
        bool m_want_exit = false;   // asked the daemon to show the keyboard

        // Home-screen network widgets (weather, chat).
        widgets::Widgets m_widgets;

        // X "Options" overlay for the selected item.
        struct OptionEntry { std::string label; int action; };
        std::vector<OptionEntry> m_options;
        bool m_options_open   = false;
        int  m_options_cursor = 0;

        int   m_cursor = 0;
        int   m_scroll = 0;
        float m_scroll_pos = 0.0f; // animated carousel position, eases to m_cursor
        float m_sub_scroll = 0.0f; // animated position for the sub-screen carousels
        bool  m_nav_fresh  = true; // is the current directional press fresh?
        int m_theme_cursor   = 0;
        int m_edit_cursor    = 0;
        int m_editing_theme  = -1; // global index of the custom theme being edited
        int m_oobe_step      = 0;

        // Soft-keyboard state.
        int  m_kb_purpose = 0;   // sl::smi::Kb_* constants
        u64  m_kb_app     = 0;   // context (app_id or theme index)
        std::string m_kb_text; // current input buffer
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