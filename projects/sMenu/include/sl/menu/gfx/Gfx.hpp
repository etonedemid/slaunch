#pragma once
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <string>
#include <unordered_map>

// Thin SDL2 rendering wrapper for sLaunch's menu.
// Renders at 1280x720. Text uses the console's shared system font (via the
// pl service) so no font file needs to be bundled. Wallpapers are loaded with
// SDL2_image from the SD card.

namespace sl::menu::gfx {

    enum class FontSize { Small = 0, Normal = 1, Large = 2, Title = 3, Count = 4 };

    class Gfx {
    public:
        static constexpr int Width  = 1280;
        static constexpr int Height = 720;

        bool Init();
        void Exit();

        SDL_Renderer *Renderer() { return m_renderer; }

        // Frame
        void Clear(SDL_Color c);
        void Present();

        // Primitives
        void FillRect(int x, int y, int w, int h, SDL_Color c);
        void GradientV(SDL_Color top, SDL_Color bottom);

        // Text
        int  TextWidth(FontSize s, const char *text);
        int  LineHeight(FontSize s);
        void Text(FontSize s, int x, int y, SDL_Color c, const char *text);
        void TextCentered(FontSize s, int cx, int y, SDL_Color c, const char *text);

        // Images
        SDL_Texture *LoadImage(const char *path);   // nullptr on failure
        void         FreeImage(SDL_Texture *tex);
        void         DrawCover(SDL_Texture *tex, Uint8 alpha = 255); // fullscreen cover-fit

        // Fonts
        // The system (pl) font is always loaded and used as the "default".
        // A content font can be loaded from a .ttf/.otf on the SD card; when
        // present it is used for drawing unless UseDefaultFont(true) forces the
        // default (so the Fonts screen stays readable regardless of choice).
        bool LoadContentFont(const char *path); // false on failure (keeps prev)
        void ClearContentFont();                // revert to system font
        bool HasContentFont() const { return m_altLoaded; }
        void UseDefaultFont(bool v) { m_useDefault = v; }

    private:
        SDL_Window   *m_window   = nullptr;
        SDL_Renderer *m_renderer = nullptr;
        TTF_Font     *m_sysFonts[(int)FontSize::Count] = {}; // system (pl) - default
        TTF_Font     *m_altFonts[(int)FontSize::Count] = {}; // selected content font
        bool          m_altLoaded  = false;
        bool          m_useDefault = false;

        TTF_Font *Font(FontSize s) {
            if (!m_useDefault && m_altLoaded && m_altFonts[(int)s]) return m_altFonts[(int)s];
            return m_sysFonts[(int)s];
        }
        void FreeAltFonts();

        // --- Text texture cache -------------------------------------------
        // Glyph rasterisation + GPU upload is by far the most expensive thing
        // per frame, so each unique (font,size,string) is rendered once (in
        // white) and reused; per-draw Color/alpha is applied with texture
        // Color/alpha modulation. Cleared when the active font changes.
        struct CachedText { SDL_Texture *tex; int w; int h; };
        std::unordered_map<std::string, CachedText> m_textCache;
        const CachedText &GetText(FontSize s, const char *text);
        void ClearTextCache();

        // --- Gradient background cache ------------------------------------
        // GradientV would otherwise issue 720 draw calls per frame; instead we
        // bake it into a 1xHeight texture and stretch-blit it, regenerating
        // only when the theme Colors change.
        SDL_Texture *m_gradTex = nullptr;
        SDL_Color    m_gradTop = {}, m_gradBottom = {};
        bool         m_gradValid = false;
    };

} // namespace sl::menu::gfx
