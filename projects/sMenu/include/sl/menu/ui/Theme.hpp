#pragma once
#include <SDL2/SDL.h>
#include <vector>

// sLaunch theming (SDL2)
// A theme is a background (vertical gradient top->bottom, optionally overlaid
// with a wallpaper image loaded from the SD card) plus a set of UI colors.
// Five themes ship built in; any number of user "Custom" themes can be created,
// edited and deleted, all persisted to sdmc:/slaunch/config/theme.cfg. Users
// can drop their own wallpaper under sdmc:/slaunch(/themes) and reference it.

namespace sl::menu::ui {

    // True only when the SD card is mounted. When false, every sdmc:/ path
    // operation (fopen/opendir/stat/mkdir) dereferences a null devoptab and
    // data-aborts, so all persistence/wallpaper/font-file I/O is skipped and
    // the menu runs on built-in defaults + the pl system font. Set by main.
    extern bool g_sd_ok;

    enum BackgroundStyle {
        BackgroundStyle_Gradient = 0,
        BackgroundStyle_Ribbon   = 1,
    };

    struct Theme {
        char      name[24];
        SDL_Color bg_top;      // gradient start
        SDL_Color bg_bottom;   // gradient end
        SDL_Color fg;          // normal text
        SDL_Color accent;      // selected / highlight
        SDL_Color dim;         // secondary text
        SDL_Color title;       // headings
        char      wallpaper[96]; // optional photo/video overlay; "" for none
        // Plate drawn behind the system-entry icons (Theming, Album, Power...).
        // Those icons are white artwork on a solid field, so the field is a
        // theme choice rather than something baked into the PNG. Black matches
        // how the shipped icons were originally drawn.
        SDL_Color icon_bg { 0, 0, 0, 255 };
        // Individual effect toggles -- any combination may be active.
        int       wallpaper_dim  = 1;         // dark scrim overlay (0/1)
        int       wallpaper_blur = 0;         // gaussian blur (0/1)
        int       wallpaper_blur_radius = 8;  // blur radius in px (2-32)
        int       wallpaper_snow = 0;         // snow overlay (0/1)
        int       wallpaper_fps    = 10;      // frames-per-second for video frame sequences (1-30)
        int       background_style = BackgroundStyle_Gradient;

        // Ribbon-wave parameters (only used when background_style == Ribbon)
        int       ribbon_line_count   = 18;   // number of wave lines (1-40)
        int       ribbon_thickness    = 3;    // pixel thickness per line (1-20)
        int       ribbon_amplitude    = 20;   // peak vertical deviation (5-60)
        int       ribbon_seed         = 0;    // color/phase seed (0-99) -- re-roll look
        int       ribbon_layers       = 1;    // overlapping ribbon passes (1-4)
        int       ribbon_y_center     = 360;  // preferred vertical center (40-680)
    };

    constexpr int BuiltinThemeCount = 5;

    class ThemeManager {
    public:
        void Load();          // read selection + custom themes from SD
        void Save() const;    // persist them

        int          Count() const { return BuiltinThemeCount + (int)m_custom.size(); }
        int          CustomCount() const { return (int)m_custom.size(); }
        int          CurrentIndex() const { return m_current; }
        const Theme &Current() const { return At(m_current); }
        const Theme &At(int i) const;
        bool         IsCustom(int i) const { return i >= BuiltinThemeCount && i < Count(); }

        // A custom theme by *global* index (must satisfy IsCustom).
        Theme &CustomAt(int i) { return m_custom[i - BuiltinThemeCount]; }

        // Append a new custom theme (a copy of the current one). Returns its
        // global index.
        int  AddCustom();
        // Delete the custom theme at global index i.
        void DeleteCustom(int i);

        void Select(int i);

    private:
        Theme m_builtin[BuiltinThemeCount];
        std::vector<Theme> m_custom;
        int   m_current = 2; // default AMOLED

        void InitBuiltins();
    };

} // namespace sl::menu::ui
