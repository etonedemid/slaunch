#pragma once
#include <SDL2/SDL.h>
#include <vector>
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

        // Many rects, one colour, one draw call. The backgrounds build a shape
        // out of hundreds of slivers; issued one at a time that is hundreds of
        // calls a frame for a single line.
        void FillRects(const SDL_Rect *r, int n, SDL_Color c);

        // Same as FillRect but added to what is already there instead of
        // blended over it, so overlapping draws build up light. This is what
        // makes a glow look like light rather than like paint.
        void FillRectAdd(int x, int y, int w, int h, SDL_Color c);

        // Antialiased line, vertical-major. Coverage is split between the two
        // pixels either side of the exact x for each row, which is what stops a
        // near-vertical line from climbing in visible stair steps. Batched, so
        // the whole line is one draw call.
        void LineAA(float x0, float y0, float x1, float y1, SDL_Color c, float width = 1.0f);
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
        // Same decode and rescale, left as an RGBA8888 surface for the caller
        // to upload into a texture it already owns (IconCache's pool).
        SDL_Surface *LoadSurfaceScaled(const char *path, int w, int h);
        // Same, from an encoded image already in memory (e.g. embedded cover art).
        SDL_Texture *LoadImageScaled(const void *data, size_t len, int w, int h);
        // Same job, scale-to-cover instead of LoadImageScaled's stretch-to-fit:
        // the larger of the two axis ratios is used, so the target is filled
        // with no border, and whichever axis overflows is centre-cropped - a
        // source that isn't already w:h comes out cropped, not squashed. biasY
        // (0=top, 0.5=centre, 1=bottom) shifts a vertical crop off centre; it
        // has no effect on an axis that isn't being cropped at all.
        SDL_Texture *LoadImageCropped(const char *path, int w, int h, float biasY = 0.5f);
        void         FreeImage(SDL_Texture *tex);
        void         DrawCover(SDL_Texture *tex, Uint8 alpha = 255); // fullscreen cover-fit
        // Blit a texture into the dst rect (scaled to fit exactly; app icons are
        // square so this preserves them). alpha modulates the whole image.
        void         DrawImage(SDL_Texture *tex, int x, int y, int w, int h, Uint8 alpha = 255);
        // Same, but recolours the texture first - for a white-glyph-on-alpha
        // icon (LoadGlyph's output) that has to take the theme's colour, the
        // same way Text() tints a cached glyph rather than shipping one PNG
        // per possible colour.
        void         DrawImageTinted(SDL_Texture *tex, int x, int y, int w, int h,
                                     SDL_Color c, Uint8 alpha = 255);

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
                        bool flip_v = false, int strips = 20,
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

        // Anti-aliasing, by drawing the whole frame into an offscreen surface
        // twice the size and letting the filtered copy-down average it. Must be
        // set BEFORE Init - it decides how text is rasterised and whether the
        // offscreen surface is made at all. Ignored when SetSupersample has
        // already asked for a larger window (the simulator), which is the same
        // thing done a different way.
        void SetAntialias(bool on) { m_aa = on; }
        bool Antialias() const { return m_aa_on; }
        // Open a window (simulator) instead of the console's single fullscreen
        // surface. No effect on hardware, where SDL has one window anyway.
        void SetWindowTitle(const char *title) { m_title = title; }

        // ---- live effects ---------------------------------------------------
        // The frame is drawn into a texture instead of straight to the screen,
        // which is what makes these possible: they sample the menu as it is
        // this frame, not a still prepared earlier. Costs one extra fullscreen
        // blit per frame. If the GPU will not give us a render target the
        // capture is skipped and everything below degrades to nothing drawn,
        // never to a broken frame.

        // Blur whatever has been drawn so far behind this rect, and draw it
        // there - frosted glass over the live menu. downscale is the blur
        // strength: the scene is sampled through a 1/n texture, so 8 is soft
        // and 2 is barely there.
        void DrawSceneBlurred(int x, int y, int w, int h, int downscale = 8,
                              Uint8 alpha = 255);

        // ---- GPU effects ------------------------------------------------------
        // Shader programs drawn by talking to GL directly, between SDL draws
        // (see the raw GL section of Gfx.cpp for how the two share a context).
        // A program is a vertex + fragment body, compiled once and cached by
        // the vs pointer - pass string literals. Positions are in 1280x720
        // menu space via Clip(); fragment output is premultiplied, so
        // vec4(rgb*a, a) blends and vec4(rgb, 0) adds.
        //
        // FxProgram returns -1 and FxBegin false whenever GL is unavailable,
        // a shader fails to build, or sdmc:/slaunch/config/no_gpu_fx exists -
        // callers keep their rect-drawn version for exactly that case.
        int  FxProgram(const char *vs, const char *fs);
        bool FxBegin(int prog);
        void FxSet(const char *name, float a);
        void FxSet(const char *name, float a, float b, float c, float d);
        void FxSet(const char *name, SDL_Color c);   // rgba as 0..1
        void FxStrip(int columns);   // aV.x = column 0..n-1, aV.y = 0 / 1 edge
        void FxQuads(int count);     // aV.z = quad index, Corner() = 0..1 corner
        void FxEnd();
        // Bind a texture for the open program's `uTex` sampler (null = white).
        // Its coordinates must be multiplied by `uTexScale.xy`.
        void FxTexture(SDL_Texture *tex);
        // End any open pass. Gfx's own SDL draws do this themselves; code that
        // calls SDL directly (render targets, clip rects) must do it first.
        void FxClose();

        // A rounded card, drawn on the GPU in the open pass: image (or fill),
        // anti-aliased corners, a soft drop shadow, a selection ring + glow
        // and optionally a fading floor reflection. Returns false when the
        // GPU path is unavailable, so the caller can draw its plain version.
        struct CardStyle {
            float     radius  = 14.0f;
            float     shadow  = 18.0f;        // 0 = none
            float     glow    = 0.0f;         // 0..1, selection
            SDL_Color glow_col{ 255, 255, 255, 255 };
            SDL_Color fill{ 0, 0, 0, 0 };     // under the image, or on its own
            bool      glyph   = false;        // tex is a mask: paint it in `tint`
            SDL_Color tint{ 255, 255, 255, 255 };
            float     band    = 0.0f;         // darkened strip at the bottom, px
            bool      reflect = false;
        };
        bool Card(SDL_Texture *tex, float x, float y, float w, float h,
                  const CardStyle &style, Uint8 alpha = 255);
        bool CardsOk();   // Card will draw (textured ones included)

        // A blurred copy of src, made on the GPU (see Gfx.cpp); radius is in
        // full-size pixels. nullptr if render targets are unavailable.
        SDL_Texture *Blurred(SDL_Texture *src, int radius);

        // Additive halo around a rect. No render target: concentric rects in
        // ADD blending, which at this size reads the same as a real bloom and
        // costs `spread` fills.
        void GlowRect(int x, int y, int w, int h, SDL_Color c, int spread = 12);

    private:
        SDL_Texture *ScaleToTexture(SDL_Surface *raw, int w, int h);   // frees raw
        bool FxTexturesWork();
        bool FxTextureSelfTest();
        bool Quad3DGpu(SDL_Texture *tex, const float c[4][3], SDL_Color tint,
                       Uint8 alpha_top, Uint8 alpha_bottom, bool flip_v, const float uv[4]);
        SDL_Surface *ScaleSurface(SDL_Surface *raw, int w, int h);     // frees raw
        void BeginScene();   // called by Clear
        void EndScene();     // called by Present
        bool ShaderFxInit();              // compile on first use; false = unavailable
        unsigned m_fx_vbo = 0;
        SDL_Texture *m_fx_tex = nullptr;   // bound by FxTexture in the open pass
        bool m_fx_tex_ok = false;          // SDL_GL_BindTexture really binds
        bool m_gles = false;               // SDL's GLES2 renderer (the console's)
        int      m_fx_tried = 0;          // 0 not yet, 1 ready, -1 gave up

        SDL_Texture *m_scene = nullptr;   // this frame, as a texture
        SDL_Texture *m_small = nullptr;   // downscale scratch for the blur
        int          m_small_div = 0;     // what m_small was built for

        SDL_Window   *m_window   = nullptr;
        SDL_Renderer *m_renderer = nullptr;
        TTF_Font     *m_sysFonts[(int)FontSize::Count] = {}; // system (pl) - default
        TTF_Font     *m_altFonts[(int)FontSize::Count] = {}; // selected content font
        bool          m_altLoaded  = false;
        bool          m_useDefault = false;
        int           m_ss         = 1;        // content scale (text raster, layout)
        bool          m_aa         = false;    // requested before Init
        bool          m_aa_on      = false;    // the larger window was created
        // Scratch for DrawQuad3D's subdivision. A member so a face split into
        // a few hundred cells does not allocate every frame.
        std::vector<SDL_Vertex> m_geom;
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
        // `used` is an LRU stamp. The cache is bounded by the GPU memory it
        // holds rather than by entry count: a list of long names (a scanned ROM
        // library) makes each texture several times the size of a short game
        // title, so a count that was safe for one is not for the other.
        // `w`/`h` are the text's own size. The texture behind it may be larger
        // (a pool slot), so drawing always goes through a source rect rather
        // than taking the whole texture.
        struct CachedText { SDL_Texture *tex; int w; int h; uint64_t used; };

        // ---- wide-label slot pool ------------------------------------------
        // List labels are the only text that is both large and constantly
        // changing: a scanned ROM library draws ~15 of them per frame, every
        // one a different string, each 220-530 KB and every one a different
        // width. Caching those as individual textures means a GPU allocation
        // and a free per label per scroll step, at hundreds of distinct sizes -
        // measured at 1207 allocations across 859 size classes over 600 frames
        // of scrolling (scripts: see the churn harness in the commit message).
        //
        // The pool replaces that with a fixed set of identically sized slots,
        // allocated once and then only ever re-uploaded: 24 allocations, one
        // size, and nothing freed while drawing. Fragmentation and
        // free-while-bound both stop being possible rather than becoming less
        // likely, which matters because neither is reproducible off-console.
        //
        // Narrow text (the clock, hints, menu rows) keeps the ordinary cache -
        // it is small, and there is not much of it.
        // Two width classes, so a short name does not sit in a slot sized for
        // the longest one. Anything narrower than the first class is left to
        // the ordinary cache: the clock, the battery, a placeholder initial -
        // small, few, and the same strings frame after frame.
        // Anything taller than a slot (FontSize::Large titles) falls back too.
        // Three width classes. Short strings are by far the most numerous (tile
        // labels, hints, the clock), so they get the most slots at the least
        // cost; only a full-width list label needs the big ones. Counts were
        // picked by measuring until texture creation went flat while scrolling
        // in every layout - see the sim's "[sim] textures:" line.
        static constexpr int    kSlotH       = 96;    // tallest list line at 2x
        static constexpr int    kClassCount  = 3;
        static constexpr int    kClassW[3]   = { 256, 640, 1456 };
        static constexpr int    kClassN[3]   = { 48, 24, 20 };   // ~21 MB total
        struct TextSlot {
            SDL_Texture *tex = nullptr;
            std::string  key;
            int          w = 0, h = 0;
            int          cls = 0;     // width class this slot was made for
            uint64_t     used  = 0;
            uint64_t     frame = 0;   // last frame drawn; never reused inside one
        };
        std::vector<TextSlot> m_slots;
        std::unordered_map<std::string, int> m_slotOf;
        uint64_t m_frame = 0;
        // Returns the slot index for `key`, uploading `surf` into it, or -1 when
        // every slot is already spoken for this frame.
        // Texture accounting, for the debug overlay and the sim. `creates` is
        // the number the menu has asked the driver for since start: it should
        // go flat once the pool is warm, whatever the list is doing.
        struct TexStats { long creates; long failures; long slots; long cached; size_t cached_bytes; };
    public:
        TexStats Textures() const {
            return { m_texCreates, m_texFailures, (long)m_slots.size(),
                     (long)m_textCache.size(), m_textBytes };
        }
    private:
        long m_texCreates = 0;
        long m_texFailures = 0;   // creations the driver refused
        int  AcquireSlot(const std::string &key, SDL_Surface *surf);
        void FreeSlots();
        CachedText m_pooledRet {};   // GetText returns a reference; pooled hits use this
        std::unordered_map<std::string, CachedText> m_textCache;
        std::unordered_map<std::string, int> m_widthCache;   // TextWidth memo
        uint64_t m_textClock = 0;
        size_t   m_textBytes = 0;
        // Only a screenful of labels is ever on screen; this is several times
        // that, and still a small fraction of what an applet slot has to spare.
        static constexpr size_t kTextCacheBudget = 6u * 1024 * 1024;
        // Evicted textures wait here and are destroyed in Present(), never
        // mid-frame. Freeing one while Mesa still has it bound leaves a buffer
        // object in its context pointing at released memory, and the next
        // draw's pushbuf_validate walks that list and dereferences the null.
        // That is the crash; SDL only flushes its own pending command queue on
        // destroy, which does not cover a binding Mesa is still holding.
        std::vector<SDL_Texture *> m_textGraveyard;
        const CachedText &GetText(FontSize s, const char *text);
        void EvictText(size_t want_free);   // retire least-recently-used entries
        void ReapTextures();                // destroy the retired ones (Present only)
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
