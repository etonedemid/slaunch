#include <sl/menu/gfx/Gfx.hpp>
#include <SDL2/SDL_image.h>
#include <switch.h>
#include <cstdio>
#include <cstring>
#include <utility>

namespace sl::menu::gfx {

    static const int kPtSize[(int)FontSize::Count] = { 20, 26, 34, 46 };

    // Log the SDL error string so we see *why* a step fails, not just where.
    static void GfxLog(const char *step) {
        FILE *fp = fopen("sdmc:/slaunch/boot.log", "a");
        if (!fp) return;
        fprintf(fp, "gfx: %s FAILED: %s\n", step, SDL_GetError());
        fclose(fp);
    }

    bool Gfx::Init() {
        // Linear sampling, set before anything is created because SDL2 captures
        // the scale mode into each texture AT CREATION - a hint set later leaves
        // every existing texture on the old mode.
        //
        // The default is nearest, which point-samples: fine for an axis-aligned
        // blit landing on whole pixels, but Flow's box faces go through
        // SDL_RenderGeometry with the quad rotated, so every screen pixel snapped
        // to the nearest texel and the art came apart into stair-steps that
        // crawled as the box turned. It cleans up the scaled draws everywhere
        // else too - tile icons come out of a 192px cache into a ~126px box.
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");

        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK) != 0) {
            GfxLog("SDL_Init"); fatalThrow(MAKERESULT(360, 31));
        }

        // SDL_WINDOW_OPENGL makes SDL load the GLES/EGL library before creating
        // the window; the switch port's CreateWindow requires egl_data to exist
        // (otherwise "EGL not initialized" -> failure).
        // Anti-aliasing: render into a larger surface and let the display
        // filter it down. 1920x1080 rather than twice 720p, because that is the
        // most the console's display accepts.
        //
        // This used to draw into an offscreen texture and blit it down instead,
        // to avoid asking the display for anything. On hardware that produced a
        // black screen - the menu ran, drew and presented every frame quite
        // happily, but the copy back to the window never appeared - and it
        // could not be caught in the simulator, which has a PC GPU behind it.
        // Doing it through the window and the renderer's logical size uses only
        // the path the simulator has always run on, and nothing is drawn
        // anywhere the display cannot see.
        int win_w = Width * m_ss, win_h = Height * m_ss;
        if (m_aa && m_ss == 1) {
            win_w = 1920; win_h = 1080;
            m_ss  = 2;              // glyphs rasterised finer to suit
            m_aa_on = true;
        }

        m_window = SDL_CreateWindow(m_title, SDL_WINDOWPOS_CENTERED,
                                    SDL_WINDOWPOS_CENTERED,
                                    win_w, win_h, SDL_WINDOW_OPENGL);
        if (!m_window && m_aa_on) {
            // The display would not take it: carry on at native size rather
            // than refusing to start.
            GfxLog("aa window");
            m_aa_on = false;
            m_ss    = 1;
            m_window = SDL_CreateWindow(m_title, SDL_WINDOWPOS_CENTERED,
                                        SDL_WINDOWPOS_CENTERED,
                                        Width, Height, SDL_WINDOW_OPENGL);
        }
        if (!m_window) { GfxLog("SDL_CreateWindow"); fatalThrow(MAKERESULT(360, 32)); }

        m_renderer = SDL_CreateRenderer(
            m_window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        if (!m_renderer) fatalThrow(MAKERESULT(360, 33)); // CreateRenderer (GPU)
        SDL_SetRenderDrawBlendMode(m_renderer, SDL_BLENDMODE_BLEND);

        // Everything the menu draws is in 1280x720 coordinates. At a
        // supersample factor above 1 the output surface is larger, and this is
        // what keeps every existing coordinate correct without touching a
        // single call site. The scale is an exact integer, so the mapping lands
        // on whole pixels rather than blurring across them.
        if (m_aa_on || m_ss != 1)
            SDL_RenderSetLogicalSize(m_renderer, Width, Height);

        if (TTF_Init() != 0) fatalThrow(MAKERESULT(360, 34)); // TTF_Init
        IMG_Init(IMG_INIT_PNG | IMG_INIT_JPG);

        // Load the system shared font via the pl service, picking the one that
        // matches the console's language. Nintendo Standard covers Latin and
        // Japanese but has no Hangul and no Simplified/Traditional-specific
        // hanzi, so a Korean or Chinese console would otherwise draw the whole
        // menu (and every game name) as tofu boxes. These fonts ship with the
        // firmware, so this costs nothing and always matches the system look.
        PlSharedFontType want = PlSharedFontType_Standard;
        u64 lc = 0;
        if (R_SUCCEEDED(setGetSystemLanguage(&lc))) {
            char code[9] = {};
            memcpy(code, &lc, 8);
            if      (!strncmp(code, "ko",      2)) want = PlSharedFontType_KO;
            else if (!strncmp(code, "zh-Hant", 7)) want = PlSharedFontType_ChineseTraditional;
            else if (!strncmp(code, "zh-TW",   5)) want = PlSharedFontType_ChineseTraditional;
            else if (!strncmp(code, "zh",      2)) want = PlSharedFontType_ChineseSimplified;
        }

        PlFontData font = {};
        if (R_FAILED(plGetSharedFontByType(&font, want)) &&
            R_FAILED(plGetSharedFontByType(&font, PlSharedFontType_Standard)))
            fatalThrow(MAKERESULT(360, 35)); // pl shared font

        for (int i = 0; i < (int)FontSize::Count; i++) {
            // Fresh RWops per open; the font memory is owned by pl and stays valid.
            SDL_RWops *rw = SDL_RWFromConstMem(font.address, font.size);
            m_sysFonts[i] = TTF_OpenFontRW(rw, 1 /*freesrc*/, kPtSize[i] * m_ss);
            if (!m_sysFonts[i]) fatalThrow(MAKERESULT(360, 36)); // TTF_OpenFont
            // Light hinting + kerning: the shared font's default (normal)
            // hinting spaces glyphs out oddly at small UI sizes.
            TTF_SetFontHinting(m_sysFonts[i], TTF_HINTING_LIGHT);
            TTF_SetFontKerning(m_sysFonts[i], 1);
        }
        return true;
    }

    void Gfx::ClearTextCache() {
        for (auto &kv : m_textCache)
            if (kv.second.tex) SDL_DestroyTexture(kv.second.tex);
        m_textCache.clear();
    }

    void Gfx::FreeAltFonts() {
        for (auto &f : m_altFonts) { if (f) TTF_CloseFont(f); f = nullptr; }
        m_altPath.clear();
        m_altLoaded = false;
        ClearTextCache(); // cached textures referenced the now-freed fonts
    }

    // Opened lazily per size; see the note on Font() in the header. Only one
    // size is opened here, both to validate the file and because the caller
    // needs a yes/no answer before committing to the font.
    bool Gfx::LoadContentFont(const char *path) {
        if (!path || !*path) return false;
        // Probed at the smallest size because that is the one every layout
        // draws (clock, battery, hints), so the validating open is not an extra
        // one - it is the first of the sizes that were going to be opened.
        TTF_Font *probe = TTF_OpenFont(path, kPtSize[(int)FontSize::Small] * m_ss);
        if (!probe) return false;      // keep the previously active font

        FreeAltFonts();
        m_altPath = path;
        m_altFonts[(int)FontSize::Small] = probe;
        m_altLoaded = true;
        return true;
    }

    TTF_Font *Gfx::Font(FontSize s) {
        if (m_useDefault || !m_altLoaded) return m_sysFonts[(int)s];
        const int i = (int)s;
        if (!m_altFonts[i] && !m_altPath.empty()) {
            m_altFonts[i] = TTF_OpenFont(m_altPath.c_str(), kPtSize[i] * m_ss);
            // A size that will not open falls back to the system font for that
            // size only, rather than losing the chosen font everywhere.
            if (!m_altFonts[i]) return m_sysFonts[i];
        }
        return m_altFonts[i] ? m_altFonts[i] : m_sysFonts[i];
    }

    void Gfx::ClearContentFont() { FreeAltFonts(); }

    void Gfx::Exit() {
        ClearTextCache();
        if (m_gradTex) SDL_DestroyTexture(m_gradTex);
        if (m_whiteTex) { SDL_DestroyTexture(m_whiteTex); m_whiteTex = nullptr; }
        FreeAltFonts();
        for (auto &f : m_sysFonts) { if (f) TTF_CloseFont(f); f = nullptr; }
        if (m_renderer) SDL_DestroyRenderer(m_renderer);
        if (m_window)   SDL_DestroyWindow(m_window);
        IMG_Quit();
        TTF_Quit();
        SDL_Quit();
    }

    void Gfx::Clear(SDL_Color c) {
        SDL_SetRenderDrawColor(m_renderer, c.r, c.g, c.b, 255);
        SDL_RenderClear(m_renderer);
    }

    void Gfx::Present() { SDL_RenderPresent(m_renderer); }

    // Alpha 0 means invisible, not "unset".
    //
    // These three entry points used to read alpha as `c.a ? c.a : 255`, so a
    // fully transparent colour came out fully opaque - the exact inverse of what
    // was asked for. It was there to let a caller write an SDL_Color without an
    // alpha field and still get something visible, but no caller does that (the
    // struct is always built with all four components, or through WithAlpha),
    // and it quietly broke every fade that reaches zero. The renderer is in
    // SDL_BLENDMODE_BLEND, so 0 blends to nothing exactly as it should.
    void Gfx::FillRect(int x, int y, int w, int h, SDL_Color c) {
        SDL_SetRenderDrawColor(m_renderer, c.r, c.g, c.b, c.a);
        SDL_Rect r { x, y, w, h };
        SDL_RenderFillRect(m_renderer, &r);
    }

    void Gfx::GradientV(SDL_Color top, SDL_Color bottom) {
        // Rebuild the baked gradient only when the Colors actually change.
        if (!m_gradValid || top.r != m_gradTop.r || top.g != m_gradTop.g ||
            top.b != m_gradTop.b || bottom.r != m_gradBottom.r ||
            bottom.g != m_gradBottom.g || bottom.b != m_gradBottom.b) {
            if (!m_gradTex)
                m_gradTex = SDL_CreateTexture(m_renderer, SDL_PIXELFORMAT_ARGB8888,
                                              SDL_TEXTUREACCESS_STREAMING, 1, Height);
            if (m_gradTex) {
                void *pixels; int pitch;
                if (SDL_LockTexture(m_gradTex, nullptr, &pixels, &pitch) == 0) {
                    for (int y = 0; y < Height; y++) {
                        float t = (float)y / (float)(Height - 1);
                        Uint8 r = (Uint8)(top.r + (bottom.r - top.r) * t);
                        Uint8 g = (Uint8)(top.g + (bottom.g - top.g) * t);
                        Uint8 b = (Uint8)(top.b + (bottom.b - top.b) * t);
                        *(Uint32*)((Uint8*)pixels + y * pitch) =
                            (0xFFu << 24) | (r << 16) | (g << 8) | b;
                    }
                    SDL_UnlockTexture(m_gradTex);
                }
            }
            m_gradTop = top; m_gradBottom = bottom; m_gradValid = true;
        }
        if (m_gradTex) {
            SDL_Rect dst { 0, 0, Width, Height };
            SDL_RenderCopy(m_renderer, m_gradTex, nullptr, &dst);
        }
    }

    // Rasterise (font,size,string) once, in white, and cache the GPU texture;
    // Color/alpha are applied per draw via modulation.
    const Gfx::CachedText &Gfx::GetText(FontSize s, const char *text) {
        static const CachedText empty = { nullptr, 0, 0 };
        if (!text || !text[0]) return empty;

        TTF_Font *font = Font(s); // may be the system or the content font
        char keybuf[24];
        snprintf(keybuf, sizeof(keybuf), "%p", (void*)font);
        std::string key(keybuf); key += '\x1f'; key += text;

        auto it = m_textCache.find(key);
        if (it != m_textCache.end()) return it->second;

        SDL_Surface *surf = TTF_RenderUTF8_Blended(font, text, SDL_Color{255,255,255,255});
        if (!surf) return empty;
        CachedText ct;
        ct.w = surf->w; ct.h = surf->h;
        ct.tex = SDL_CreateTextureFromSurface(m_renderer, surf);
        SDL_FreeSurface(surf);
        if (ct.tex) SDL_SetTextureBlendMode(ct.tex, SDL_BLENDMODE_BLEND);

        // Bound the cache so transient strings (clock, counters) can't grow it
        // without limit.
        if (m_textCache.size() > 400) ClearTextCache();
        return m_textCache.emplace(std::move(key), ct).first->second;
    }

    // Text is rasterised at m_ss times the layout size (see SetSupersample), so
    // every metric is divided back down: the menu lays out in 1280x720 whatever
    // the output resolution is.
    int Gfx::TextWidth(FontSize s, const char *text) { return GetText(s, text).w / m_ss; }

    int Gfx::LineHeight(FontSize s) { return TTF_FontHeight(Font(s)) / m_ss; }

    void Gfx::Text(FontSize s, int x, int y, SDL_Color c, const char *text) {
        const CachedText &e = GetText(s, text);
        if (!e.tex) return;
        SDL_SetTextureColorMod(e.tex, c.r, c.g, c.b);
        SDL_SetTextureAlphaMod(e.tex, c.a);   // see FillRect: 0 means invisible
        // The glyph texture is m_ss times the layout size; the destination is in
        // layout space, and the logical-size mapping scales it back up to land
        // on the texture's own pixels one for one.
        SDL_Rect dst { x, y, e.w / m_ss, e.h / m_ss };
        SDL_RenderCopy(m_renderer, e.tex, nullptr, &dst);
    }

    void Gfx::TextCentered(FontSize s, int cx, int y, SDL_Color c, const char *text) {
        int w = TextWidth(s, text);
        Text(s, cx - w / 2, y, c, text);
    }

    // ---- 3D quads ---------------------------------------------------------

    SDL_Texture *Gfx::WhiteTexture() {
        if (m_whiteTex) return m_whiteTex;
        m_whiteTex = SDL_CreateTexture(m_renderer, SDL_PIXELFORMAT_RGBA32,
                                       SDL_TEXTUREACCESS_STATIC, 1, 1);
        if (!m_whiteTex) return nullptr;
        const Uint32 px = 0xFFFFFFFFu;
        SDL_UpdateTexture(m_whiteTex, nullptr, &px, 4);
        SDL_SetTextureBlendMode(m_whiteTex, SDL_BLENDMODE_BLEND);
        return m_whiteTex;
    }

    void Gfx::Project3D(const float p[3], float &sx, float &sy) const {
        // Guard the near plane: a corner swinging behind the camera would
        // otherwise divide through zero and fling the quad across the screen.
        const float z = (p[2] < 0.05f) ? 0.05f : p[2];
        sx = Width  * 0.5f + p[0] * Focal / z;
        sy = Height * 0.5f - p[1] * Focal / z;   // y is up in view space
    }

    void Gfx::DrawQuad3D(SDL_Texture *tex, const float c[4][3],
                         SDL_Color tint, Uint8 alpha_top, Uint8 alpha_bottom,
                         bool flip_v, int strips, const float uv[4]) {
        // A null texture means "flat colour". SDL_RenderGeometry is documented
        // to accept NULL for that, but those triangles do not actually appear on
        // this backend - which silently dropped every untextured face, so a box
        // with no cover art rendered as a hole rather than as a blank case.
        // A 1x1 white pixel modulated by the vertex colour is the same thing and
        // always draws.
        if (!tex) tex = WhiteTexture();
        if (strips < 1)  strips = 1;
        if (strips > 32) strips = 32;

        const float u0 = uv ? uv[0] : 0.0f, v0 = uv ? uv[1] : 0.0f;
        const float u1 = uv ? uv[2] : 1.0f, v1 = uv ? uv[3] : 1.0f;

        if (tex) {
            SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
            SDL_SetTextureColorMod(tex, 255, 255, 255);
            SDL_SetTextureAlphaMod(tex, 255);   // per-vertex colour carries alpha
        }

        // corners: 0 = TL, 1 = TR, 2 = BR, 3 = BL
        //
        // Subdivided in BOTH directions, not just across.
        //
        // Strips alone fix the horizontal squeeze and leave the vertical error
        // untouched, and the vertical error is the one you actually see: inside
        // a strip the two triangles interpolate v affinely across a trapezoid
        // whose near edge is taller than its far edge, so a horizontal line in
        // the texture is pulled off true by an amount that grows to the middle
        // of the strip and returns to zero at each seam. Repeated across the
        // strips that is a sawtooth - every straight line on the printed wrap
        // came out as a regular wave, most obvious there because the wrap is
        // flat artwork full of straight edges.
        //
        // Splitting each strip into rows as well makes every cell close to a
        // parallelogram, where affine and projective agree, and the error falls
        // away with the square of the cell size.
        // Seam placement across the quad, by perspective rather than by even
        // steps along the edge in 3D. Stepping the 3D parameter evenly spends
        // the subdivision in the wrong place: the near half of a turned face
        // covers far more of the screen than the far half, so its cells were
        // several times wider and carried several times the error. For a
        // fraction u across the projected face the object-space parameter is
        //     t = (u/zR) / ((1-u)/zL + u/zR)
        // which is the standard perspective-correct inverse - interpolate 1/z
        // linearly in screen space, then divide it back out.
        const float zL = 0.5f * (c[0][2] + c[3][2]);
        const float zR = 0.5f * (c[1][2] + c[2][2]);
        auto param = [zL, zR](float u) {
            const float l = (zL > 0.001f) ? zL : 0.001f;
            const float r = (zR > 0.001f) ? zR : 0.001f;
            const float iz = (1.0f - u) / l + u / r;
            if (iz <= 0.000001f) return u;
            return (u / r) / iz;
        };

        const float zT = 0.5f * (c[0][2] + c[1][2]);
        const float zB = 0.5f * (c[3][2] + c[2][2]);
        const float zmin = std::min(std::min(zL, zR), std::min(zT, zB));
        const float zmax = std::max(std::max(zL, zR), std::max(zT, zB));

        // A quad square-on to the camera has no projective error to correct, so
        // it stays a single row and costs nothing; the more its depth varies,
        // the finer it is cut.
        int rows = 1;
        if (zmin > 0.001f) {
            const float bend = (zmax / zmin) - 1.0f;
            rows = (int)ceilf(bend * 40.0f);
            if (rows < 1)  rows = 1;
            if (rows > 16) rows = 16;
        }

        m_geom.clear();
        m_geom.reserve((size_t)strips * rows * 6);

        // u/vtex arrive as 0..1 across the quad and are mapped into the sub-rect.
        auto vert = [&](const float p[3], float u, float vtex, Uint8 a) {
            SDL_Vertex out;
            Project3D(p, out.position.x, out.position.y);
            out.color = SDL_Color{ tint.r, tint.g, tint.b, a };
            const float vv = flip_v ? 1.0f - vtex : vtex;
            out.tex_coord = SDL_FPoint{ u0 + (u1 - u0) * u, v0 + (v1 - v0) * vv };
            m_geom.push_back(out);
        };

        auto fade = [&](float b) {
            return (Uint8)(alpha_top + (int)((float)alpha_bottom - (float)alpha_top) * b);
        };

        for (int ry = 0; ry < rows; ry++) {
            const float b0 = (float)ry / (float)rows;
            const float b1 = (float)(ry + 1) / (float)rows;
            const Uint8 a0 = fade(b0), a1 = fade(b1);

            for (int s = 0; s < strips; s++) {
                const float t0 = param((float)s / (float)strips);
                const float t1 = param((float)(s + 1) / (float)strips);

                // The cell's four corners, bilinear in 3D. Four coplanar points
                // interpolate to a coplanar point, so every cell stays on the
                // face - this is the step that keeps the mapping honest.
                float p00[3], p10[3], p11[3], p01[3];
                for (int k = 0; k < 3; k++) {
                    const float topA = c[0][k] + (c[1][k] - c[0][k]) * t0;
                    const float topB = c[0][k] + (c[1][k] - c[0][k]) * t1;
                    const float botA = c[3][k] + (c[2][k] - c[3][k]) * t0;
                    const float botB = c[3][k] + (c[2][k] - c[3][k]) * t1;
                    p00[k] = topA + (botA - topA) * b0;
                    p01[k] = topA + (botA - topA) * b1;
                    p10[k] = topB + (botB - topB) * b0;
                    p11[k] = topB + (botB - topB) * b1;
                }

                vert(p00, t0, b0, a0);
                vert(p10, t1, b0, a0);
                vert(p11, t1, b1, a1);

                vert(p00, t0, b0, a0);
                vert(p11, t1, b1, a1);
                vert(p01, t0, b1, a1);
            }
        }

        SDL_RenderGeometry(m_renderer, tex, m_geom.data(), (int)m_geom.size(),
                           nullptr, 0);
    }

    SDL_Texture *Gfx::LoadImage(const char *path) {
        return IMG_LoadTexture(m_renderer, path);
    }

    SDL_Texture *Gfx::LoadImageScaled(const char *path, int w, int h) {
        SDL_Surface *raw = IMG_Load(path);
        if (!raw) return nullptr;

        SDL_Surface *src = SDL_ConvertSurfaceFormat(raw, SDL_PIXELFORMAT_RGBA8888, 0);
        SDL_FreeSurface(raw);
        if (!src) return nullptr;

        // Already the requested size: upload as-is.
        if (src->w == w && src->h == h) {
            SDL_Texture *tex = SDL_CreateTextureFromSurface(m_renderer, src);
            SDL_FreeSurface(src);
            return tex;
        }

        SDL_Surface *dst = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32,
                                                          SDL_PIXELFORMAT_RGBA8888);
        if (!dst) {   // out of memory: the full-size image still beats nothing
            SDL_Texture *tex = SDL_CreateTextureFromSurface(m_renderer, src);
            SDL_FreeSurface(src);
            return tex;
        }

        SDL_BlitScaled(src, nullptr, dst, nullptr);
        SDL_FreeSurface(src);

        SDL_Texture *tex = SDL_CreateTextureFromSurface(m_renderer, dst);
        SDL_FreeSurface(dst);
        return tex;
    }

    // Scanline-filled triangle. SDL2 has no filled-primitive call before
    // SDL_RenderGeometry, so the span between the two active edges is drawn as
    // a 1px rect per row. Only used for small shapes (the XMB selection wedge),
    // where a few dozen rows costs nothing.
    void Gfx::FillTriangle(int x0, int y0, int x1, int y1, int x2, int y2, SDL_Color c) {
        // Sort vertices top to bottom; the triangle then splits at y1 into a
        // flat-bottom half and a flat-top half sharing the long y0..y2 edge.
        if (y0 > y1) { std::swap(x0, x1); std::swap(y0, y1); }
        if (y0 > y2) { std::swap(x0, x2); std::swap(y0, y2); }
        if (y1 > y2) { std::swap(x1, x2); std::swap(y1, y2); }
        if (y2 == y0) return;                       // zero height, nothing to fill

        SDL_SetRenderDrawColor(m_renderer, c.r, c.g, c.b, c.a);   // see FillRect
        for (int y = y0; y <= y2; y++) {
            // Long edge, spanning the whole triangle.
            const int lx = x0 + (int)((long long)(x2 - x0) * (y - y0) / (y2 - y0));
            // Short edge: the upper one until y1, the lower one after it.
            const bool lower = (y > y1);
            const int  ya = lower ? y1 : y0, yb = lower ? y2 : y1;
            const int  xa = lower ? x1 : x0, xb = lower ? x2 : x1;
            const int  sx = (yb == ya) ? xb
                          : xa + (int)((long long)(xb - xa) * (y - ya) / (yb - ya));

            const int left  = lx < sx ? lx : sx;
            const int right = lx < sx ? sx : lx;
            SDL_Rect r { left, y, right - left + 1, 1 };
            SDL_RenderFillRect(m_renderer, &r);
        }
    }

    SDL_Texture *Gfx::LoadGlyph(const char *path, int w, int h) {
        SDL_Surface *raw = IMG_Load(path);
        if (!raw) return nullptr;

        SDL_Surface *s = SDL_ConvertSurfaceFormat(raw, SDL_PIXELFORMAT_RGBA32, 0);
        SDL_FreeSurface(raw);
        if (!s) return nullptr;

        // A file that already varies its alpha is a real cut-out; leave it be.
        // Otherwise the shape is encoded as brightness on a flat (black)
        // background, so brightness becomes the alpha and the colour goes white.
        bool has_alpha = false;
        const int n = s->w * s->h;
        Uint32 *px = (Uint32 *)s->pixels;
        if (SDL_MUSTLOCK(s)) SDL_LockSurface(s);
        for (int i = 0; i < n; i++) {
            Uint8 r, g, b, a;
            SDL_GetRGBA(px[i], s->format, &r, &g, &b, &a);
            if (a != 255) { has_alpha = true; break; }
        }
        if (!has_alpha) {
            for (int i = 0; i < n; i++) {
                Uint8 r, g, b, a;
                SDL_GetRGBA(px[i], s->format, &r, &g, &b, &a);
                const Uint8 lum = (Uint8)((r * 77 + g * 151 + b * 28) >> 8);
                px[i] = SDL_MapRGBA(s->format, 255, 255, 255, lum);
            }
        }
        if (SDL_MUSTLOCK(s)) SDL_UnlockSurface(s);

        SDL_Texture *full = SDL_CreateTextureFromSurface(m_renderer, s);
        SDL_FreeSurface(s);
        if (!full) return nullptr;
        SDL_SetTextureBlendMode(full, SDL_BLENDMODE_BLEND);
        if (w <= 0 || h <= 0) return full;

        // Bake the downscale once so per-frame blits stay cheap.
        SDL_Texture *dst = SDL_CreateTexture(m_renderer, SDL_PIXELFORMAT_RGBA8888,
                                             SDL_TEXTUREACCESS_TARGET, w, h);
        if (!dst) return full;
        SDL_SetTextureBlendMode(dst, SDL_BLENDMODE_BLEND);
        SDL_Texture *prev = SDL_GetRenderTarget(m_renderer);
        SDL_SetRenderTarget(m_renderer, dst);
        SDL_SetRenderDrawColor(m_renderer, 0, 0, 0, 0);
        SDL_RenderClear(m_renderer);
        SDL_RenderCopy(m_renderer, full, nullptr, nullptr);
        SDL_SetRenderTarget(m_renderer, prev);
        SDL_DestroyTexture(full);
        return dst;
    }

    void Gfx::FreeImage(SDL_Texture *tex) {
        if (tex) SDL_DestroyTexture(tex);
    }

    void Gfx::DrawImage(SDL_Texture *tex, int x, int y, int w, int h, Uint8 alpha) {
        if (!tex || w <= 0 || h <= 0) return;
        SDL_Rect dst { x, y, w, h };
        SDL_SetTextureAlphaMod(tex, alpha);
        SDL_RenderCopy(m_renderer, tex, nullptr, &dst);
        SDL_SetTextureAlphaMod(tex, 255); // don't leak the mod to other blits
    }

    void Gfx::DrawCover(SDL_Texture *tex, Uint8 alpha) {
        if (!tex) return;
        int tw = 0, th = 0;
        SDL_QueryTexture(tex, nullptr, nullptr, &tw, &th);
        if (tw <= 0 || th <= 0) return;

        // Cover-fit: scale so the image fills the screen, cropping the overflow.
        float scale = (float)Width / tw;
        if ((float)th * scale < Height)
            scale = (float)Height / th;
        int dw = (int)(tw * scale), dh = (int)(th * scale);
        SDL_Rect dst { (Width - dw) / 2, (Height - dh) / 2, dw, dh };

        SDL_SetTextureAlphaMod(tex, alpha);
        SDL_RenderCopy(m_renderer, tex, nullptr, &dst);
    }

} // namespace sl::menu::gfx
