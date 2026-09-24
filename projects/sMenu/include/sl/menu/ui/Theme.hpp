#pragma once
#include <SDL2/SDL.h>
#include <vector>

// sLaunch theming (SDL2)
// A theme is a background (vertical gradient top->bottom, optionally overlaid
// with a wallpaper image loaded from the SD card) plus a set of UI colors.
// Five themes ship built in; any number of user "Custom" themes can be created,
// edited and deleted, all persisted to the account's own theme.cfg (see
// UserCfg.hpp - two people on one console keep two different looks). Users can
// drop their own wallpaper under sdmc:/slaunch(/themes) and reference it; the
// wallpapers themselves are shared, only the choice of one is per account.

namespace sl::menu::ui {

    // True only when the SD card is mounted. When false, every sdmc:/ path
    // operation (fopen/opendir/stat/mkdir) dereferences a null devoptab and
    // data-aborts, so all persistence/wallpaper/font-file I/O is skipped and
    // the menu runs on built-in defaults + the pl system font. Set by main.
    extern bool g_sd_ok;

    // Persisted by number in theme.cfg - append only.
    enum BackgroundStyle {
        BackgroundStyle_Gradient = 0,
        BackgroundStyle_Ribbon   = 1,
        BackgroundStyle_Stars    = 2,
        BackgroundStyle_Aurora   = 3,
        BackgroundStyle_Grid     = 4,
        BackgroundStyle_RibbonHD = 5,   // the ribbon, finer and lit
        BackgroundStyle_Ocean    = 6,   // GL fragment shader
        BackgroundStyle_Count
    };

    struct Theme {
        char      name[24];
        SDL_Color bg_top;      // gradient start
        SDL_Color bg_bottom;   // gradient end
        SDL_Color fg;          // normal text
        SDL_Color accent;      // selected / highlight
        SDL_Color dim;         // secondary text
        SDL_Color title;       // headings
        char      wallpaper[96]; // optional overlay: an image, or an .mp4 video; "" for none
        // Plate drawn behind the system-entry icons (Theming, Album, Power...).
        // Those icons are white artwork on a solid field, so the field is a
        // theme choice rather than something baked into the PNG. Black matches
        // how the shipped icons were originally drawn.
        SDL_Color icon_bg { 0, 0, 0, 255 };
        // The artwork on that plate. The shipped icons are a white shape, and
        // LoadGlyph turns their brightness into alpha, so what reaches the
        // screen is a silhouette - which takes a colour the same way text does.
        // White keeps every existing theme looking exactly as it did.
        SDL_Color icon_fg { 255, 255, 255, 255 };

        // What the animated backgrounds are drawn in. Kept separate from the UI
        // palette because the two jobs pull in opposite directions: accent has
        // to stay readable against the background, and a background is free to
        // be anything. Themes written before these existed get accent/title, so
        // nothing changes look until they are set.
        SDL_Color fx_a { 90, 170, 255, 255 };   // primary   (ribbon, grid lines, stars)
        SDL_Color fx_b { 150, 195, 255, 255 };  // secondary (gradients: sun, aurora, sheen)
        // Opacity of the plate behind system icons, 0-255. Kept separate from
        // icon_bg rather than living in its .a, because the shared colour
        // serializer writes only r,g,b and reads alpha back as 255 - putting it
        // in the colour would mean it silently reset on every theme load.
        int icon_bg_alpha = 0;     // no plate by default: the glyph on the background
        // Individual effect toggles -- any combination may be active.
        int       wallpaper_dim  = 1;         // dark scrim overlay (0/1)
        int       wallpaper_blur = 0;         // gaussian blur (0/1)
        int       wallpaper_blur_radius = 8;  // blur radius in px (2-32)
        int       wallpaper_snow = 0;         // snow overlay (0/1)
        int       background_style = BackgroundStyle_Gradient;

        // Per-background settings.
        //
        // Every animated background steers off the same eight numbers, but they
        // mean different things to each - "amplitude" is a wave height to the
        // ribbon and a scroll speed to the retrowave grid. One shared set meant
        // tuning one background quietly retuned the others, so each style keeps
        // its own, and switching between them restores what you last set.
        struct FxParams {
            int lines;        // count: wave lines / grid lines / stars / curtains
            int thickness;    // stroke or size
            int amplitude;    // travel: wave height, scroll or twinkle speed
            int seed;         // which arrangement
            int layers;       // depth, sun size, flare, softness
            int y;            // horizon / hem / row centre, in pixels
            int x;            // horizontal placement, percent of width
            int cam;          // eye height above the ground, percent
        };

        // Indexed by BackgroundStyle. Defaults are per style because each one
        // wants a different starting point - a horizon at 360 is the middle of
        // the screen for the grid and nonsense for the starfield.
        FxParams fx[BackgroundStyle_Count] = {
            /* Gradient  */ { 18, 3, 20, 0,  1, 360, 50, 50 },   // unused
            /* Ribbon    */ {  1, 3, 20, 0,  3, 360, 50, 50 },
            /* Stars     */ { 18, 3, 20, 7,  6, 720, 50, 50 },
            /* Aurora    */ {  3, 6, 30, 12, 8, 420, 50, 50 },
            /* Grid      */ { 24, 4, 30, 45, 7, 380, 50, 50 },
            /* RibbonHD  */ {  1, 4, 22, 3,  3, 360, 50, 50 },
            /* Ocean     */ { 18, 6, 20, 0,  5, 300, 50, 50 },
        };

        FxParams       &Fx()       { return fx[Clamp(background_style)]; }
        const FxParams &Fx() const { return fx[Clamp(background_style)]; }

        static int Clamp(int style) {
            return (style < 0 || style >= (int)BackgroundStyle_Count)
                 ? (int)BackgroundStyle_Gradient : style;
        }
    };

    constexpr int BuiltinThemeCount = 5;

    class ThemeManager {
    public:
        void Load();          // read selection + custom themes from SD
        void Save() const;    // persist them

        int          Count() const { return BuiltinThemeCount + (int)m_custom.size(); }
        int          CustomCount() const { return (int)m_custom.size(); }
        int          CurrentIndex() const { return m_current; }
        int          AppliedIndex() const { return m_applied; }   // saved, not just previewed
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
        int   m_current = 2; // default AMOLED - live preview cursor, moves as the Themes list is browsed
        // Save() is const (it only writes to disk) but still needs to record
        // that m_current is now the applied theme, hence mutable.
        mutable int m_applied = 2; // last theme actually confirmed (Save()'d) - what AddCustom() should copy

        void InitBuiltins();
    };

} // namespace sl::menu::ui
