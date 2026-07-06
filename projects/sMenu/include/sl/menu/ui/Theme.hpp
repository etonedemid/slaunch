#pragma once
#include <SDL2/SDL.h>

// sLaunch theming (SDL2)
// A theme is a background (vertical gradient top->bottom, optionally overlaid
// with a wallpaper image loaded from the SD card) plus a set of UI colors.
// Five themes ship built in; a sixth "Custom" slot is user-editable and
// persisted to sdmc:/slaunch/config/theme.cfg. Users can drop their own
// wallpaper at sdmc:/slaunch/<file> and reference it from a theme.

namespace sl::menu::ui {

    // True only when the SD card is mounted. When false, every sdmc:/ path
    // operation (fopen/opendir/stat/mkdir) dereferences a null devoptab and
    // data-aborts, so all persistence/wallpaper/font-file I/O is skipped and
    // the menu runs on built-in defaults + the pl system font. Set by main.
    extern bool g_sd_ok;

    struct Theme {
        char      name[24];
        SDL_Color bg_top;      // gradient start
        SDL_Color bg_bottom;   // gradient end
        SDL_Color fg;          // normal text
        SDL_Color accent;      // selected / highlight
        SDL_Color dim;         // secondary text
        SDL_Color title;       // headings
        char      wallpaper[96]; // optional SD path; "" for none
    };

    constexpr int BuiltinThemeCount = 5;
    constexpr int CustomThemeIndex  = BuiltinThemeCount; // 5
    constexpr int TotalThemeCount   = BuiltinThemeCount + 1;

    class ThemeManager {
    public:
        void Load();          // read selection + custom palette from SD
        void Save() const;    // persist them

        int          Count() const { return TotalThemeCount; }
        int          CurrentIndex() const { return m_current; }
        const Theme &Current() const { return At(m_current); }
        const Theme &At(int i) const;
        Theme       &Custom() { return m_custom; }
        static bool  IsCustom(int i) { return i == CustomThemeIndex; }

        void Select(int i);

    private:
        Theme m_builtin[BuiltinThemeCount];
        Theme m_custom;
        int   m_current = 2; // default AMOLED

        void InitBuiltins();
    };

} // namespace sl::menu::ui
