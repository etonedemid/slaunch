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
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK) != 0) {
            GfxLog("SDL_Init"); fatalThrow(MAKERESULT(360, 31));
        }

        // SDL_WINDOW_OPENGL makes SDL load the GLES/EGL library before creating
        // the window; the switch port's CreateWindow requires egl_data to exist
        // (otherwise "EGL not initialized" -> failure).
        m_window = SDL_CreateWindow("sLaunch", SDL_WINDOWPOS_CENTERED,
                                    SDL_WINDOWPOS_CENTERED, Width, Height,
                                    SDL_WINDOW_OPENGL);
        if (!m_window) { GfxLog("SDL_CreateWindow"); fatalThrow(MAKERESULT(360, 32)); }

        m_renderer = SDL_CreateRenderer(
            m_window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        if (!m_renderer) fatalThrow(MAKERESULT(360, 33)); // CreateRenderer (GPU)
        SDL_SetRenderDrawBlendMode(m_renderer, SDL_BLENDMODE_BLEND);

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
            m_sysFonts[i] = TTF_OpenFontRW(rw, 1 /*freesrc*/, kPtSize[i]);
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
        m_altLoaded = false;
        ClearTextCache(); // cached textures referenced the now-freed fonts
    }

    bool Gfx::LoadContentFont(const char *path) {
        TTF_Font *loaded[(int)FontSize::Count] = {};
        for (int i = 0; i < (int)FontSize::Count; i++) {
            loaded[i] = TTF_OpenFont(path, kPtSize[i]);
            if (!loaded[i]) {
                for (int j = 0; j < i; j++) TTF_CloseFont(loaded[j]);
                return false; // keep the previously active font
            }
        }
        FreeAltFonts();
        for (int i = 0; i < (int)FontSize::Count; i++) m_altFonts[i] = loaded[i];
        m_altLoaded = true;
        return true;
    }

    void Gfx::ClearContentFont() { FreeAltFonts(); }

    void Gfx::Exit() {
        ClearTextCache();
        if (m_gradTex) SDL_DestroyTexture(m_gradTex);
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

    void Gfx::FillRect(int x, int y, int w, int h, SDL_Color c) {
        SDL_SetRenderDrawColor(m_renderer, c.r, c.g, c.b, c.a ? c.a : 255);
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

    int Gfx::TextWidth(FontSize s, const char *text) { return GetText(s, text).w; }

    int Gfx::LineHeight(FontSize s) { return TTF_FontHeight(Font(s)); }

    void Gfx::Text(FontSize s, int x, int y, SDL_Color c, const char *text) {
        const CachedText &e = GetText(s, text);
        if (!e.tex) return;
        SDL_SetTextureColorMod(e.tex, c.r, c.g, c.b);
        SDL_SetTextureAlphaMod(e.tex, c.a ? c.a : 255);
        SDL_Rect dst { x, y, e.w, e.h };
        SDL_RenderCopy(m_renderer, e.tex, nullptr, &dst);
    }

    void Gfx::TextCentered(FontSize s, int cx, int y, SDL_Color c, const char *text) {
        int w = TextWidth(s, text);
        Text(s, cx - w / 2, y, c, text);
    }

    SDL_Texture *Gfx::LoadImage(const char *path) {
        return IMG_LoadTexture(m_renderer, path);
    }

    SDL_Texture *Gfx::LoadImageScaled(const char *path, int w, int h) {
        SDL_Texture *src = IMG_LoadTexture(m_renderer, path);
        if (!src) return nullptr;

        SDL_Texture *dst = SDL_CreateTexture(m_renderer, SDL_PIXELFORMAT_RGBA8888,
                                             SDL_TEXTUREACCESS_TARGET, w, h);
        if (!dst) return src;   // no render-target support: keep the full-size one

        SDL_SetTextureBlendMode(dst, SDL_BLENDMODE_BLEND);
        SDL_Texture *prev = SDL_GetRenderTarget(m_renderer);
        SDL_SetRenderTarget(m_renderer, dst);
        SDL_SetRenderDrawColor(m_renderer, 0, 0, 0, 0);
        SDL_RenderClear(m_renderer);
        SDL_RenderCopy(m_renderer, src, nullptr, nullptr); // downscale once
        SDL_SetRenderTarget(m_renderer, prev);

        SDL_DestroyTexture(src);
        return dst;
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

        SDL_SetRenderDrawColor(m_renderer, c.r, c.g, c.b, c.a ? c.a : 255);
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
