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
        void FillTriangle(int x0, int y0, int x1, int y1, int x2, int y2, SDL_Color c);
        void GradientV(SDL_Color top, SDL_Color bottom);

        // Text
        int  TextWidth(FontSize s, const char *text);
        int  LineHeight(FontSize s);
        void Text(FontSize s, int x, int y, SDL_Color c, const char *text);
        void TextCentered(FontSize s, int cx, int y, SDL_Color c, const char *text);

        // Images
        SDL_Texture *LoadImage(const char *path);   // nullptr on failure
        // Load a system-entry icon (a white shape on a solid field). Icons that
        // carry no real alpha channel would blit their field as an opaque black
        // square, welding the background into the artwork; this turns their
        // brightness into the alpha channel so the field becomes a theme colour
        // (Theme::icon_bg) drawn behind them. Files that already have alpha are
        // loaded unchanged. Pass w/h = 0 to keep the file's own resolution.
        SDL_Texture *LoadGlyph(const char *path, int w, int h);
        // Load an image and downscale it once into a wxh static texture, so later
        // per-frame blits are cheap and it uses far less VRAM than the full-size
        // source (used for grid/line app icons). Falls back to the full image if
        // a render target can't be made.
        SDL_Texture *LoadImageScaled(const char *path, int w, int h);
        void         FreeImage(SDL_Texture *tex);
        void         DrawCover(SDL_Texture *tex, Uint8 alpha = 255); // fullscreen cover-fit
        // Blit a texture into the dst rect (scaled to fit exactly; app icons are
        // square so this preserves them). alpha modulates the whole image.
        void         DrawImage(SDL_Texture *tex, int x, int y, int w, int h, Uint8 alpha = 255);

        // ---- 3D quads (coverflow) -----------------------------------------
        // Camera sits at the origin looking down +z with y up; a quad is given
        // by its four corners in that space, in the order top-left, top-right,
        // bottom-right, bottom-left, and is perspective projected here.
        //
        // Each quad is cut into vertical strips before being handed to
        // SDL_RenderGeometry. That is not an optimisation, it is a correctness
        // fix: RenderGeometry interpolates texture coordinates linearly in
        // *screen* space, so a steeply rotated quad shears its texture the way
        // PS1 games do. Interpolating the corners in 3D and projecting each
        // strip separately keeps the error inside a strip small enough to
        // vanish. All strips share one texture, so the whole quad still costs a
        // single draw call.
        //
        // alpha_top/alpha_bottom fade down the quad via per-vertex colour,
        // which is what makes the mirrored reflection under a cover free.
        // flip_v mirrors the texture vertically, for that reflection.
        // uv is {u0, v0, u1, v1} in 0..1 and selects a sub-rectangle of the
        // texture. That is what lets one box wrap - back, spine, front in a
        // single image - texture three faces of a 3D box without slicing it
        // into separate textures or compositing a new one per title.
        void DrawQuad3D(SDL_Texture *tex, const float corners[4][3],
                        SDL_Color tint, Uint8 alpha_top, Uint8 alpha_bottom,
                        bool flip_v = false, int strips = 12,
                        const float uv[4] = nullptr);
        // Project a view-space point to screen coordinates. Exposed so layout
        // and touch hit-testing can agree with what was drawn.
        void Project3D(const float p[3], float &sx, float &sy) const;

        // Focal length in pixels. 900 over a 720-tall surface is about a 44
        // degree vertical field of view - wide enough for the row to splay out,
        // narrow enough that the centre cover is not distorted.
        static constexpr float Focal = 900.0f;

        // Fonts
        // The system (pl) font is always loaded and used as the "default".
        // A content font can be loaded from a .ttf/.otf on the SD card; when
        // present it is used for drawing unless UseDefaultFont(true) forces the
        // default (so the Fonts screen stays readable regardless of choice).
        bool LoadContentFont(const char *path); // false on failure (keeps prev)
        void ClearContentFont();                // revert to system font
        bool HasContentFont() const { return m_altLoaded; }
        void UseDefaultFont(bool v) { m_useDefault = v; }

        // ---- supersampling (desktop simulator only) -------------------------
        // Render the same 1280x720 layout into a surface N times larger, for
        // screenshots that are actually high resolution rather than an upscale.
        //
        // Everything the menu draws stays in 1280x720 coordinates: SDL's logical
        // size maps them onto the bigger target, and fonts are opened N times
        // larger so glyphs are rasterised at the output resolution instead of
        // being stretched. Text metrics are therefore divided back down by N,
        // which is exact at N=1 and can differ by a pixel above it - so trust
        // N=1 for pixel-exact layout checks and use N>1 for pictures.
        //
        // Must be called before Init(). The console always runs at 1.
        void SetSupersample(int n) { m_ss = (n < 1) ? 1 : (n > 4 ? 4 : n); }
        int  Supersample() const { return m_ss; }
        // Open a window (simulator) instead of the console's single fullscreen
        // surface. No effect on hardware, where SDL has one window anyway.
        void SetWindowTitle(const char *title) { m_title = title; }

    private:
        SDL_Window   *m_window   = nullptr;
        SDL_Renderer *m_renderer = nullptr;
        TTF_Font     *m_sysFonts[(int)FontSize::Count] = {}; // system (pl) - default
        TTF_Font     *m_altFonts[(int)FontSize::Count] = {}; // selected content font
        bool          m_altLoaded  = false;
        bool          m_useDefault = false;
        int           m_ss         = 1;        // supersample factor, 1 on console
        const char   *m_title      = "sLaunch";

        // Content-font sizes are opened on first use, not all at once.
        //
        // TTF_OpenFont re-reads and re-parses the file per call, and a CJK font
        // is tens of megabytes - opening all four sizes up front read 74 MB off
        // the SD card before the menu could draw anything, which measured as the
        // single largest cost in start-up. A layout typically draws two of the
        // four sizes, and the rest are opened only if something asks for them.
        std::string   m_altPath;
        TTF_Font     *Font(FontSize s);
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
        // 1x1 opaque white, created on demand. DrawQuad3D substitutes it for a
        // null texture: SDL_RenderGeometry is documented to accept NULL and draw
        // flat colour, but on this backend those triangles do not appear, which
        // silently loses every untextured face. Modulating a white pixel by the
        // vertex colour gives the same result and always draws.
        SDL_Texture *m_whiteTex = nullptr;
        SDL_Texture *WhiteTexture();
        SDL_Color    m_gradTop = {}, m_gradBottom = {};
        bool         m_gradValid = false;
    };

} // namespace sl::menu::gfx
