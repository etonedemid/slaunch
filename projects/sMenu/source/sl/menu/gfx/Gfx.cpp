#include <sl/menu/gfx/Gfx.hpp>
#ifdef SL_VULKAN
#include "fx_spirv.h"   // scripts/fx-spirv.py, from the effects' GLSL
#endif
#include <vector>
#include <algorithm>
#include <SDL3_image/SDL_image.h>
#include <switch.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <utility>

namespace sl::menu::gfx {

    static const int kPtSize[(int)FontSize::Count] = { 20, 26, 34, 46 };

    // The Vulkan build's window must be Vulkan from the start: an OpenGL one
    // brings up Mesa's GL beside NVK, and the process gets killed seconds in.
#ifdef SL_VULKAN
    static constexpr SDL_WindowFlags kWindowFlags = SDL_WINDOW_VULKAN;
#else
    static constexpr SDL_WindowFlags kWindowFlags = SDL_WINDOW_OPENGL;
#endif

    static SDL_FRect F(int x, int y, int w, int h) {
        return SDL_FRect{ (float)x, (float)y, (float)w, (float)h };
    }
    static SDL_FRect F(const SDL_Rect &r) { return F(r.x, r.y, r.w, r.h); }

    // Log the SDL error string so we see *why* a step fails, not just where.
#ifdef SL_VULKAN
    static void FxFrameDone();
#endif

    static void GfxLog(const char *step) {
        FILE *fp = fopen("sdmc:/slaunch/boot.log", "a");
        if (!fp) return;
        fprintf(fp, "gfx: %s FAILED: %s\n", step, SDL_GetError());
        fclose(fp);
    }

    bool Gfx::Init() {
        // Textures sample linearly: SDL3's default, where SDL2 needed a hint.
        // Nearest point-sampling came apart into crawling stair-steps on Flow's
        // rotated box faces, and blurred nothing it should have elsewhere.

        // Quick parameter checks only. The full ones look every renderer and
        // texture handle up in a table behind a read-write lock, and on
        // libnx releasing that lock is a kernel call - thousands of render
        // calls a frame made it 40% of the menu's time.
        SDL_SetHint(SDL_HINT_INVALID_PARAM_CHECKS, "1");

        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK)) {
            GfxLog("SDL_Init"); fatalThrow(MAKERESULT(360, 31));
        }

        // SDL_WINDOW_OPENGL makes SDL load the GLES/EGL library before creating
        // the window; the switch port's CreateWindow needs it for a GL window
        // (a Vulkan one brings its own swapchain).
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

        m_window = SDL_CreateWindow(m_title, win_w, win_h, kWindowFlags);
        if (!m_window && m_aa_on) {
            // The display would not take it: carry on at native size rather
            // than refusing to start.
            GfxLog("aa window");
            m_aa_on = false;
            m_ss    = 1;
            m_window = SDL_CreateWindow(m_title, Width, Height, kWindowFlags);
        }
        if (!m_window) { GfxLog("SDL_CreateWindow"); fatalThrow(MAKERESULT(360, 32)); }

        // GL, by name: the GPU effects below talk to its context directly.
        // GLES2 on the console (the path SDL2 always took there), desktop GL
        // first in the simulator. The Vulkan build draws through SDL_GPU,
        // and its effects take their SDL-drawn fallbacks.
#if defined(SL_VULKAN)
        m_renderer = SDL_CreateRenderer(m_window, "gpu");
#elif defined(__SWITCH__)
        m_renderer = SDL_CreateRenderer(m_window, "opengles2,opengl");
#else
        m_renderer = SDL_CreateRenderer(m_window, "opengl,opengles2");
#endif
        if (!m_renderer) { GfxLog("SDL_CreateRenderer"); fatalThrow(MAKERESULT(360, 33)); }
        if (FILE *fp = fopen("sdmc:/slaunch/boot.log", "a")) {
            fprintf(fp, "gfx: renderer %s\n", SDL_GetRendererName(m_renderer));
            fclose(fp);
        }
        SDL_SetRenderVSync(m_renderer, 1);
        SDL_SetRenderDrawBlendMode(m_renderer, SDL_BLENDMODE_BLEND);
        SetLogical();

        if (!TTF_Init()) fatalThrow(MAKERESULT(360, 34)); // TTF_Init
        m_textEngine = TTF_CreateRendererTextEngine(m_renderer);
        if (!m_textEngine) { GfxLog("TTF_CreateRendererTextEngine"); fatalThrow(MAKERESULT(360, 37)); }

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
            SDL_IOStream *rw = SDL_IOFromConstMem(font.address, font.size);
            m_sysFonts[i] = TTF_OpenFontIO(rw, true /*closeio*/, (float)(kPtSize[i] * m_ss));
            if (!m_sysFonts[i]) fatalThrow(MAKERESULT(360, 36)); // TTF_OpenFont
            // Light hinting + kerning: the shared font's default (normal)
            // hinting spaces glyphs out oddly at small UI sizes.
            TTF_SetFontHinting(m_sysFonts[i], TTF_HINTING_LIGHT);
            TTF_SetFontKerning(m_sysFonts[i], true);
        }
        return true;
    }

    void Gfx::ClearTextCache() {
        m_widthCache.clear();
        for (auto &kv : m_textCache) m_textRetired.push_back(kv.second.text);
        m_textCache.clear();
    }

    // Retire the `count` least recently drawn layouts.
    void Gfx::EvictText(size_t count) {
        std::vector<std::pair<uint64_t, const std::string *>> order;
        order.reserve(m_textCache.size());
        for (auto &kv : m_textCache) order.push_back({ kv.second.used, &kv.first });
        count = std::min(count, order.size());
        std::partial_sort(order.begin(), order.begin() + count, order.end(),
                          [](const auto &a, const auto &b) { return a.first < b.first; });
        for (size_t i = 0; i < count; i++) {
            auto it = m_textCache.find(*order[i].second);
            m_textRetired.push_back(it->second.text);
            m_textCache.erase(it);
        }
    }

    void Gfx::FreeAltFonts() {
        // Retired, like the layouts that use them, and closed after those in
        // Present: a TTF_Text must not outlive its font.
        ClearTextCache();
        for (auto &f : m_altFonts) { if (f) m_fontRetired.push_back(f); f = nullptr; }
        m_altPath.clear();
        m_altLoaded = false;
    }

    // Opened lazily per size; see the note on Font() in the header. Only one
    // size is opened here, both to validate the file and because the caller
    // needs a yes/no answer before committing to the font.
    bool Gfx::LoadContentFont(const char *path) {
        if (!path || !*path) return false;
        // Probed at the smallest size because that is the one every layout
        // draws (clock, battery, hints), so the validating open is not an extra
        // one - it is the first of the sizes that were going to be opened.
        TTF_Font *probe = TTF_OpenFont(path, (float)(kPtSize[(int)FontSize::Small] * m_ss));
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
            m_altFonts[i] = TTF_OpenFont(m_altPath.c_str(), (float)(kPtSize[i] * m_ss));
            // A size that will not open falls back to the system font for that
            // size only, rather than losing the chosen font everywhere.
            if (!m_altFonts[i]) return m_sysFonts[i];
        }
        return m_altFonts[i] ? m_altFonts[i] : m_sysFonts[i];
    }

    void Gfx::ClearContentFont() { FreeAltFonts(); }

    void Gfx::Exit() {
        ClearTextCache();
        ReapTextures();   // retired textures must not outlive the renderer
        if (m_textEngine) { TTF_DestroyRendererTextEngine(m_textEngine); m_textEngine = nullptr; }
        if (m_gradTex) SDL_DestroyTexture(m_gradTex);
        if (m_whiteTex) { SDL_DestroyTexture(m_whiteTex); m_whiteTex = nullptr; }
        if (m_scene) { SDL_DestroyTexture(m_scene); m_scene = nullptr; }
        if (m_small) { SDL_DestroyTexture(m_small); m_small = nullptr; }
        FreeAltFonts();
        ReapTextures();   // closes the fonts FreeAltFonts retired
        for (auto &f : m_sysFonts) { if (f) TTF_CloseFont(f); f = nullptr; }
        if (m_renderer) SDL_DestroyRenderer(m_renderer);
        if (m_window)   SDL_DestroyWindow(m_window);
        TTF_Quit();
        SDL_Quit();
    }

    void Gfx::Clear(SDL_Color c) {
        FxClose();   // an open GPU pass must end before SDL draws again
        BeginScene();
        SDL_SetRenderDrawColor(m_renderer, c.r, c.g, c.b, 255);
        SDL_RenderClear(m_renderer);
    }

    void Gfx::Present() {
        FxClose();   // an open GPU pass must end before SDL draws again
        EndScene();
        const u64 t_present = armGetSystemTick();
        SDL_RenderPresent(m_renderer);
#ifdef SL_VULKAN
        FxFrameDone();
#endif
        PerfSample(t_present);
        // The frame is submitted and nothing is bound from it any more, so this
        // is the one point where dropping a texture cannot pull it out from
        // under a draw that is still referencing it.
        ReapTextures();
    }

    // Where the frame goes, every two seconds, to sdmc:/slaunch/perf.log -
    // only while sdmc:/slaunch/config/profile exists (dbg::StartProfiler's
    // switch too), so ordinary runs never write to the card for it:
    // "draw" is everything between two presents (the menu's own work and its
    // SDL calls), "present" is
    // SDL_RenderPresent - on SDL_GPU that is where the frame's GPU commands
    // are actually recorded and submitted, plus waiting for a swapchain image.
    void Gfx::PerfSample(Uint64 t_present) {
        static const bool on = [] {
            FILE *f = fopen("sdmc:/slaunch/config/profile", "r");
            if (f) fclose(f);
            return f != nullptr;
        }();
        if (!on) return;
        const u64 now = armGetSystemTick();
        Perf &p = m_perf;
        const u64 draw = t_present - p.frame_start, present = now - t_present;
        p.draw += draw; p.present += present; p.frames++;
        if (draw + present > p.worst) p.worst = draw + present;
        p.frame_start = now;
        if (!p.window_start) { p = Perf{}; p.window_start = p.frame_start = now; return; }
        const double freq = (double)armGetSystemTickFreq();
        const double secs = (now - p.window_start) / freq;
        if (secs < 2.0) return;
        if (FILE *fp = fopen("sdmc:/slaunch/perf.log", p.opened ? "a" : "w")) {
            p.opened = true;
            fprintf(fp, "%s: %5.1f fps  draw %5.2f ms  present %5.2f ms  worst %5.1f ms\n",
                    SDL_GetRendererName(m_renderer), p.frames / secs,
                    p.draw * 1e3 / freq / p.frames, p.present * 1e3 / freq / p.frames,
                    p.worst * 1e3 / freq);
            fclose(fp);
        }
        const bool opened = p.opened;
        p = Perf{};
        p.opened = opened;
        p.window_start = p.frame_start = now;
    }

    void Gfx::ReapTextures() {
        for (TTF_Text *t : m_textRetired) TTF_DestroyText(t);
        m_textRetired.clear();
        for (TTF_Font *f : m_fontRetired) TTF_CloseFont(f);
        m_fontRetired.clear();
        for (SDL_Texture *t : m_textGraveyard)
            if (t) SDL_DestroyTexture(t);
        m_textGraveyard.clear();
    }

    // ---- live scene capture -------------------------------------------------
    void Gfx::BeginScene() {
        if (!m_scene) {
            int ow = 0, oh = 0;
            SDL_GetRenderOutputSize(m_renderer, &ow, &oh);
            if (ow <= 0 || oh <= 0) return;
            m_scene = SDL_CreateTexture(m_renderer, SDL_PIXELFORMAT_RGBA8888,
                                        SDL_TEXTUREACCESS_TARGET, ow, oh);
            if (!m_scene) return;             // no RT support: draw straight out
            SDL_SetTextureBlendMode(m_scene, SDL_BLENDMODE_NONE);
            // The frame is laid out in 1280x720 whatever the scene's size: the
            // mapping belongs to the scene target now, and the window gets
            // the finished scene 1:1.
            SDL_SetRenderTarget(m_renderer, m_scene);
            SetLogical();
            SDL_SetRenderTarget(m_renderer, nullptr);
            SDL_SetRenderLogicalPresentation(m_renderer, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED);
        }
        SDL_SetRenderTarget(m_renderer, m_scene);
    }

    // Everything the menu draws is in 1280x720 coordinates. At a supersample
    // factor above 1 the output is larger, and this keeps every coordinate
    // correct without touching a call site; the scale is an exact integer, so
    // it lands on whole pixels. SDL3 keeps this per render target: set on the
    // window, and on the scene texture once there is one.
    void Gfx::SetLogical() {
        if (m_aa_on || m_ss != 1)
            SDL_SetRenderLogicalPresentation(m_renderer, Width, Height,
                                             SDL_LOGICAL_PRESENTATION_STRETCH);
    }

    void Gfx::EndScene() {
        if (!m_scene) return;
        SDL_SetRenderTarget(m_renderer, nullptr);
        SDL_RenderTexture(m_renderer, m_scene, nullptr, nullptr);
    }

    void Gfx::DrawSceneBlurred(int x, int y, int w, int h, int downscale, Uint8 alpha) {
        FxClose();   // an open GPU pass must end before SDL draws again
        if (!m_scene || w <= 0 || h <= 0) return;
        if (downscale < 2) downscale = 2;

        int ow = 0, oh = 0;
        SDL_GetRenderOutputSize(m_renderer, &ow, &oh);
        const int sw = ow / downscale, sh = oh / downscale;
        if (sw <= 0 || sh <= 0) return;

        if (m_small && m_small_div != downscale) {
            SDL_DestroyTexture(m_small);
            m_small = nullptr;
        }
        if (!m_small) {
            m_small = SDL_CreateTexture(m_renderer, SDL_PIXELFORMAT_RGBA8888,
                                        SDL_TEXTUREACCESS_TARGET, sw, sh);
            if (!m_small) return;
            SDL_SetTextureBlendMode(m_small, SDL_BLENDMODE_BLEND);
            m_small_div = downscale;
        }

        // m_small has no logical size of its own (SDL3 keeps that per target),
        // so the whole scene lands across all of it.
        SDL_SetRenderTarget(m_renderer, m_small);
        SDL_RenderTexture(m_renderer, m_scene, nullptr, nullptr);   // bilinear downscale
        SDL_SetRenderTarget(m_renderer, m_scene);

        // Sample back only the part of the scene this rect covers, so it reads
        // as the panel frosting what is behind it rather than as a shrunken
        // copy of the whole screen.
        const float s = (float)ow / (float)Width / (float)downscale;
        const SDL_FRect src { x * s, y * s, w * s, h * s };
        const SDL_FRect dst = F(x, y, w, h);
        SDL_SetTextureAlphaMod(m_small, alpha);
        SDL_RenderTexture(m_renderer, m_small, &src, &dst);
        SDL_SetTextureAlphaMod(m_small, 255);
    }

    void Gfx::GlowRect(int x, int y, int w, int h, SDL_Color c, int spread) {
        FxClose();   // an open GPU pass must end before SDL draws again
        if (spread < 1) return;
        SDL_SetRenderDrawBlendMode(m_renderer, SDL_BLENDMODE_ADD);
        // Rings, not filled rects: a filled rect at every step would add light
        // across the whole interior as well, and whatever the halo surrounds
        // would come out washed flat. Only the band at each offset is drawn.
        for (int i = spread; i > 0; i--) {
            const float k = (float)i / (float)spread;      // 1 at the outside
            const float f = (1.0f - k) * (1.0f - k);       // falls off fast
            SDL_SetRenderDrawColor(m_renderer, c.r, c.g, c.b,
                                   (Uint8)((float)c.a * f * 0.45f));
            const SDL_FRect ring[4] = {
                F(x - i,     y - i,     w + 2 * i, 1        ),   // top
                F(x - i,     y + h + i, w + 2 * i, 1        ),   // bottom
                F(x - i,     y - i,     1,         h + 2 * i),   // left
                F(x + w + i, y - i,     1,         h + 2 * i),   // right
            };
            SDL_RenderFillRects(m_renderer, ring, 4);
        }
        SDL_SetRenderDrawBlendMode(m_renderer, SDL_BLENDMODE_BLEND);
    }

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
        FxClose();   // an open GPU pass must end before SDL draws again
        SDL_SetRenderDrawColor(m_renderer, c.r, c.g, c.b, c.a);
        const SDL_FRect r = F(x, y, w, h);
        SDL_RenderFillRect(m_renderer, &r);
    }

    void Gfx::GradientV(SDL_Color top, SDL_Color bottom) {
        FxClose();   // an open GPU pass must end before SDL draws again
        // Rebuild the baked gradient only when the Colors actually change.
        if (!m_gradValid || top.r != m_gradTop.r || top.g != m_gradTop.g ||
            top.b != m_gradTop.b || bottom.r != m_gradBottom.r ||
            bottom.g != m_gradBottom.g || bottom.b != m_gradBottom.b) {
            if (!m_gradTex)
                m_gradTex = SDL_CreateTexture(m_renderer, SDL_PIXELFORMAT_ARGB8888,
                                              SDL_TEXTUREACCESS_STREAMING, 1, Height);
            if (m_gradTex) {
                void *pixels; int pitch;
                if (SDL_LockTexture(m_gradTex, nullptr, &pixels, &pitch)) {
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
            const SDL_FRect dst = F(0, 0, Width, Height);
            SDL_RenderTexture(m_renderer, m_gradTex, nullptr, &dst);
        }
    }

    // Lay (font, size, string) out once and keep it; colour is set per draw.
    TTF_Text *Gfx::GetText(FontSize s, const char *text) {
        if (!text || !text[0]) return nullptr;
        TTF_Font *font = Font(s); // may be the system or the content font
        std::string key((const char *)&font, sizeof(font));
        key += text;

        auto it = m_textCache.find(key);
        if (it != m_textCache.end()) { it->second.used = ++m_textClock; return it->second.text; }

        TTF_Text *t = TTF_CreateText(m_textEngine, font, text, 0);
        m_texCreates++;
        if (!t) { m_texFailures++; return nullptr; }
        // A quarter at a time, so eviction is an occasional burst rather than
        // something every new label pays for once the cache is full.
        if (m_textCache.size() >= kTextCacheMax) EvictText(kTextCacheMax / 4);
        m_textCache.emplace(std::move(key), CachedText{ t, ++m_textClock });
        return t;
    }

    // Text is rasterised at m_ss times the layout size (see SetSupersample), so
    // every metric is divided back down: the menu lays out in 1280x720 whatever
    // the output resolution is.
    // Measured from glyph metrics, never by rasterising. Going through GetText
    // used to mean every measurement put a texture in the cache - and Ellipsize
    // measures ~7 throwaway prefixes per label it has to truncate, per row, per
    // frame. Short labels fit and never probe, so this stayed invisible until a
    // list of long names (a scanned ROM library) made every row take that path:
    // the cache hit its cap and flushed itself several times a second, churning
    // tens of MB of GPU textures until the driver fell over mid-frame.
    // TTF_GetStringSize returns exactly the width the drawn text has, so
    // nothing about layout changes.
    //
    // Memoised by font and string: every layout measures the same labels on
    // every frame (Ellipsize's binary search, word wrap, centred text, the
    // hint bar), and TTF_GetStringSize walks the glyphs each time - a real cost
    // on the console's CPU, not on a PC. Capped, and emptied whenever the
    // fonts change (ClearTextCache).
    int Gfx::TextWidth(FontSize s, const char *text) {
        if (!text || !text[0]) return 0;
        TTF_Font *font = Font(s);
        if (!font) return 0;
        std::string key((const char *)&font, sizeof(font));
        key += text;
        auto it = m_widthCache.find(key);
        if (it != m_widthCache.end()) return it->second;
        int w = 0, h = 0;
        if (!TTF_GetStringSize(font, text, 0, &w, &h)) return 0;
        if (m_widthCache.size() >= 4096) m_widthCache.clear();
        m_widthCache.emplace(std::move(key), w / m_ss);
        return w / m_ss;
    }

    int Gfx::LineHeight(FontSize s) { return TTF_GetFontHeight(Font(s)) / m_ss; }

    void Gfx::Text(FontSize s, int x, int y, SDL_Color c, const char *text) {
        FxClose();   // an open GPU pass must end before SDL draws again
        if (c.a == 0) return;   // see FillRect: 0 means invisible
        TTF_Text *t = GetText(s, text);
        if (!t) return;
        TTF_SetTextColor(t, c.r, c.g, c.b, c.a);
        // The glyphs are m_ss times the layout size. Drawing at 1/m_ss scale
        // from m_ss times the position puts them on the target's own pixels
        // one for one - the logical-size mapping scales straight back up.
        if (m_ss == 1) { TTF_DrawRendererText(t, (float)x, (float)y); return; }
        float sx = 1.0f, sy = 1.0f;
        SDL_GetRenderScale(m_renderer, &sx, &sy);
        SDL_SetRenderScale(m_renderer, sx / m_ss, sy / m_ss);
        TTF_DrawRendererText(t, (float)(x * m_ss), (float)(y * m_ss));
        SDL_SetRenderScale(m_renderer, sx, sy);
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
        if (Quad3DGpu(tex, c, tint, alpha_top, alpha_bottom, flip_v, uv)) return;
        FxClose();   // CPU path below goes through SDL
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
            out.color = SDL_FColor{ tint.r / 255.0f, tint.g / 255.0f, tint.b / 255.0f, a / 255.0f };
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

    // Decoded to RGBA8888 rather than left in the file's own format (RGB24
    // for a JPEG): SDL keeps a format the GPU lacks behind a hidden converted
    // copy, and the GPU card/3D paths sample the texture SDL reports - which
    // for those textures came out blank.
    SDL_Texture *Gfx::LoadImage(const char *path) {
        SDL_Surface *raw = IMG_Load(path);
        return raw ? ScaleToTexture(raw, raw->w, raw->h) : nullptr;
    }

    SDL_Texture *Gfx::LoadImageScaled(const char *path, int w, int h) {
        return ScaleToTexture(IMG_Load(path), w, h);
    }

    SDL_Texture *Gfx::LoadImageScaled(const void *data, size_t len, int w, int h) {
        if (!data || !len) return nullptr;
        return ScaleToTexture(IMG_Load_IO(SDL_IOFromConstMem(data, len), true), w, h);
    }

    SDL_Surface *Gfx::LoadSurfaceScaled(const char *path, int w, int h) {
        return ScaleSurface(IMG_Load(path), w, h);
    }

    // RGBA8888 at w x h. Falls back to the unscaled source when the target
    // surface cannot be allocated - the full-size image still beats nothing.
    SDL_Surface *Gfx::ScaleSurface(SDL_Surface *raw, int w, int h) {
        if (!raw) return nullptr;
        SDL_Surface *src = SDL_ConvertSurface(raw, SDL_PIXELFORMAT_RGBA8888);
        SDL_DestroySurface(raw);
        if (!src || (src->w == w && src->h == h)) return src;

        SDL_Surface *dst = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGBA8888);
        if (!dst) return src;
        // Linear, where SDL2's BlitScaled could only point-sample.
        SDL_BlitSurfaceScaled(src, nullptr, dst, nullptr, SDL_SCALEMODE_LINEAR);
        SDL_DestroySurface(src);
        return dst;
    }

    SDL_Texture *Gfx::ScaleToTexture(SDL_Surface *raw, int w, int h) {
        SDL_Surface *surf = ScaleSurface(raw, w, h);
        if (!surf) return nullptr;
        SDL_Texture *tex = SDL_CreateTextureFromSurface(m_renderer, surf);
        SDL_DestroySurface(surf);
        return tex;
    }

    SDL_Texture *Gfx::LoadImageCropped(const char *path, int w, int h, float biasY) {
        SDL_Surface *raw = IMG_Load(path);
        if (!raw) return nullptr;

        SDL_Surface *src = SDL_ConvertSurface(raw, SDL_PIXELFORMAT_RGBA8888);
        SDL_DestroySurface(raw);
        if (!src) return nullptr;

        // Scale-to-cover: the bigger of the two ratios is what makes both axes
        // reach at least w/h, so this - not the smaller one LoadImageScaled's
        // stretch effectively applies per axis - is the one that leaves no
        // border, only an overflow to crop.
        const float sx = (float)w / (float)src->w;
        const float sy = (float)h / (float)src->h;
        const float scale = sx > sy ? sx : sy;

        // The crop rect, in source pixels: exactly a w:h-shaped window, sized
        // down from src by that scale. Whichever axis the scale came from is
        // already full-size (cw == src->w or ch == src->h); the other is what
        // actually gets cropped.
        int cw = (int)(w / scale + 0.5f);
        int ch = (int)(h / scale + 0.5f);
        if (cw > src->w) cw = src->w;
        if (ch > src->h) ch = src->h;
        const int cx = (src->w - cw) / 2;               // horizontal crop stays centred
        const int cy = (int)((float)(src->h - ch) * biasY);
        SDL_Rect crop { cx, cy, cw, ch };

        SDL_Surface *dst = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGBA8888);
        if (!dst) {   // out of memory: the uncropped source still beats nothing
            SDL_Texture *tex = SDL_CreateTextureFromSurface(m_renderer, src);
            SDL_DestroySurface(src);
            return tex;
        }

        SDL_BlitSurfaceScaled(src, &crop, dst, nullptr, SDL_SCALEMODE_LINEAR);
        SDL_DestroySurface(src);

        SDL_Texture *tex = SDL_CreateTextureFromSurface(m_renderer, dst);
        SDL_DestroySurface(dst);
        return tex;
    }

    // Scanline-filled triangle. SDL2 has no filled-primitive call before
    // SDL_RenderGeometry, so the span between the two active edges is drawn as
    // a 1px rect per row. Only used for small shapes (the XMB selection wedge),
    // where a few dozen rows costs nothing.
#ifndef SL_VULKAN
    // =========================================================================
    // Raw GL, alongside SDL's renderer
    //
    // SDL owns the GL context; SDL_FlushRenderer is the documented way to hand it
    // over for a moment. Nothing here is linked against libGLESv2 - every entry
    // point comes from SDL_GL_GetProcAddress, so this builds and runs wherever
    // SDL itself does, and simply switches itself off where it cannot.
    //
    // Every piece of state SDL caches is read back in FxBegin and written back
    // in FxEnd: the program, the array buffer, the blend function, and whether
    // attribute 0 is enabled. That last one is what used to break the menu
    // after one Ocean frame: SDL's GLES2 renderer enables its position
    // attribute (location 0) once at startup and never again, so disabling it
    // on the way out left every later SDL draw rendering nothing.
    // =========================================================================
    namespace {

        typedef unsigned GLenum_;
        typedef unsigned GLuint_;
        typedef int      GLint_;
        typedef float    GLfloat_;

        constexpr GLenum_ GL_ARRAY_BUFFER_        = 0x8892;
        constexpr GLenum_ GL_STATIC_DRAW_         = 0x88E4;
        constexpr GLenum_ GL_FRAGMENT_SHADER_     = 0x8B30;
        constexpr GLenum_ GL_VERTEX_SHADER_       = 0x8B31;
        constexpr GLenum_ GL_COMPILE_STATUS_      = 0x8B81;
        constexpr GLenum_ GL_LINK_STATUS_         = 0x8B82;
        constexpr GLenum_ GL_FLOAT_               = 0x1406;
        constexpr GLenum_ GL_TRIANGLES_           = 0x0004;
        constexpr GLenum_ GL_TRIANGLE_STRIP_      = 0x0005;
        constexpr GLenum_ GL_BLEND_               = 0x0BE2;
        constexpr GLenum_ GL_ONE_                 = 1;
        constexpr GLenum_ GL_ONE_MINUS_SRC_ALPHA_ = 0x0303;
        constexpr GLenum_ GL_BLEND_DST_RGB_       = 0x80C8;
        constexpr GLenum_ GL_BLEND_SRC_RGB_       = 0x80C9;
        constexpr GLenum_ GL_BLEND_DST_ALPHA_     = 0x80CA;
        constexpr GLenum_ GL_BLEND_SRC_ALPHA_     = 0x80CB;
        constexpr GLenum_ GL_CURRENT_PROGRAM_     = 0x8B8D;
        constexpr GLenum_ GL_ARRAY_BUFFER_BINDING_= 0x8894;
        constexpr GLenum_ GL_VERTEX_ATTRIB_ARRAY_ENABLED_ = 0x8622;
        constexpr GLenum_ GL_ACTIVE_TEXTURE_      = 0x84E0;
        constexpr GLenum_ GL_TEXTURE0_            = 0x84C0;
        constexpr GLenum_ GL_TEXTURE_BINDING_2D_  = 0x8069;
        constexpr GLenum_ GL_TEXTURE_2D_          = 0x0DE1;

        struct GlFns {
            GLuint_ (*CreateShader)(GLenum_);
            void    (*ShaderSource)(GLuint_, GLint_, const char *const *, const GLint_ *);
            void    (*CompileShader)(GLuint_);
            void    (*GetShaderiv)(GLuint_, GLenum_, GLint_ *);
            void    (*GetShaderInfoLog)(GLuint_, GLint_, GLint_ *, char *);
            void    (*DeleteShader)(GLuint_);
            GLuint_ (*CreateProgram)(void);
            void    (*AttachShader)(GLuint_, GLuint_);
            void    (*BindAttribLocation)(GLuint_, GLuint_, const char *);
            void    (*LinkProgram)(GLuint_);
            void    (*GetProgramiv)(GLuint_, GLenum_, GLint_ *);
            void    (*UseProgram)(GLuint_);
            void    (*GenBuffers)(GLint_, GLuint_ *);
            void    (*BindBuffer)(GLenum_, GLuint_);
            void    (*BufferData)(GLenum_, long, const void *, GLenum_);
            GLint_  (*GetUniformLocation)(GLuint_, const char *);
            void    (*EnableVertexAttribArray)(GLuint_);
            void    (*DisableVertexAttribArray)(GLuint_);
            void    (*GetVertexAttribiv)(GLuint_, GLenum_, GLint_ *);
            void    (*VertexAttribPointer)(GLuint_, GLint_, GLenum_, unsigned char, GLint_, const void *);
            void    (*DrawArrays)(GLenum_, GLint_, GLint_);
            void    (*Uniform1f)(GLint_, GLfloat_);
            void    (*Uniform4f)(GLint_, GLfloat_, GLfloat_, GLfloat_, GLfloat_);
            void    (*GetIntegerv)(GLenum_, GLint_ *);
            void    (*Enable)(GLenum_);
            void    (*Disable)(GLenum_);
            void    (*BlendFuncSeparate)(GLenum_, GLenum_, GLenum_, GLenum_);
            unsigned char (*IsEnabled)(GLenum_);
            void    (*ActiveTexture)(GLenum_);
            void    (*BindTexture)(GLenum_, GLuint_);
        };

        GlFns g_gl{};
        bool  g_gl_loaded = false;

        template <typename T> bool Load(T &fn, const char *name) {
            fn = (T)SDL_GL_GetProcAddress(name);
            return fn != nullptr;
        }

        bool LoadGl() {
            if (g_gl_loaded) return g_gl.CreateShader != nullptr;
            g_gl_loaded = true;
            bool ok = true;
            ok &= Load(g_gl.CreateShader, "glCreateShader");
            ok &= Load(g_gl.ShaderSource, "glShaderSource");
            ok &= Load(g_gl.CompileShader, "glCompileShader");
            ok &= Load(g_gl.GetShaderiv, "glGetShaderiv");
            ok &= Load(g_gl.GetShaderInfoLog, "glGetShaderInfoLog");
            ok &= Load(g_gl.DeleteShader, "glDeleteShader");
            ok &= Load(g_gl.CreateProgram, "glCreateProgram");
            ok &= Load(g_gl.AttachShader, "glAttachShader");
            ok &= Load(g_gl.BindAttribLocation, "glBindAttribLocation");
            ok &= Load(g_gl.LinkProgram, "glLinkProgram");
            ok &= Load(g_gl.GetProgramiv, "glGetProgramiv");
            ok &= Load(g_gl.UseProgram, "glUseProgram");
            ok &= Load(g_gl.GenBuffers, "glGenBuffers");
            ok &= Load(g_gl.BindBuffer, "glBindBuffer");
            ok &= Load(g_gl.BufferData, "glBufferData");
            ok &= Load(g_gl.GetUniformLocation, "glGetUniformLocation");
            ok &= Load(g_gl.EnableVertexAttribArray, "glEnableVertexAttribArray");
            ok &= Load(g_gl.DisableVertexAttribArray, "glDisableVertexAttribArray");
            ok &= Load(g_gl.GetVertexAttribiv, "glGetVertexAttribiv");
            ok &= Load(g_gl.VertexAttribPointer, "glVertexAttribPointer");
            ok &= Load(g_gl.DrawArrays, "glDrawArrays");
            ok &= Load(g_gl.Uniform1f, "glUniform1f");
            ok &= Load(g_gl.Uniform4f, "glUniform4f");
            ok &= Load(g_gl.GetIntegerv, "glGetIntegerv");
            ok &= Load(g_gl.Enable, "glEnable");
            ok &= Load(g_gl.Disable, "glDisable");
            ok &= Load(g_gl.BlendFuncSeparate, "glBlendFuncSeparate");
            ok &= Load(g_gl.IsEnabled, "glIsEnabled");
            ok &= Load(g_gl.ActiveTexture, "glActiveTexture");
            ok &= Load(g_gl.BindTexture, "glBindTexture");
            if (!ok) g_gl.CreateShader = nullptr;
            return ok;
        }

        // Prepended to every effect. Positions are in the menu's own 1280x720
        // space; Clip maps them to the viewport SDL has already set up, and
        // uFlip turns y over when a render target (y-up storage) is bound.
        static_assert(Gfx::Width == 1280 && Gfx::Height == 720, "Clip() assumes 1280x720");
        const char *kVsPrelude =
            "#ifdef GL_ES\nprecision highp float;\n#endif\n"
            "attribute vec4 aV;\n"
            "uniform float uFlip;\n"
            "uniform float uTime;\n"
            "const float TAU = 6.2831853;\n"
            "vec4 Clip(vec2 p) { return vec4(p.x / 640.0 - 1.0, (1.0 - p.y / 360.0) * uFlip, 0.0, 1.0); }\n"
            "float Hash(float v) { return fract(sin(v) * 43758.5453); }\n"
            "vec2 Corner() { return vec2(mod(aV.w, 2.0), floor(aV.w / 2.0)); }\n";
        const char *kFsPrelude =
            "#ifdef GL_ES\n#ifdef GL_FRAGMENT_PRECISION_HIGH\nprecision highp float;\n"
            "#else\nprecision mediump float;\n#endif\n#endif\n"
            "uniform float uTime;\n";

        // One vertex buffer serves every effect: vertex k carries
        // (k/2, k%2) for triangle strips and (k/6, corner) for quads, and each
        // shader derives its own geometry from those numbers and its uniforms.
        constexpr int kFxVerts = 4096;

        struct FxProg {
            GLuint_ prog = 0;
            std::vector<std::pair<const char *, GLint_>> loc;   // by literal
        };
        std::vector<FxProg> g_progs;
        std::vector<std::pair<const char *, int>> g_prog_of;    // vs literal -> index
        int g_cur = -1;

        struct Saved { GLint_ prog, buf, attr0, bs, bd, bsa, bda, unit, tex; bool blend; } g_saved;

        // The GL name SDL gave a texture, from whichever GL renderer made it.
        GLuint_ GlName(SDL_Texture *tex) {
            const SDL_PropertiesID p = SDL_GetTextureProperties(tex);
            Sint64 n = SDL_GetNumberProperty(p, SDL_PROP_TEXTURE_OPENGLES2_TEXTURE_NUMBER, 0);
            if (!n) n = SDL_GetNumberProperty(p, SDL_PROP_TEXTURE_OPENGL_TEXTURE_NUMBER, 0);
            return (GLuint_)n;
        }

        GLuint_ Compile(GLenum_ type, const char *prelude, const char *src) {
            GLuint_ sh = g_gl.CreateShader(type);
            if (!sh) return 0;
            const char *parts[2] = { prelude, src };
            g_gl.ShaderSource(sh, 2, parts, nullptr);
            g_gl.CompileShader(sh);
            GLint_ ok = 0;
            g_gl.GetShaderiv(sh, GL_COMPILE_STATUS_, &ok);
            if (!ok) {
                char log[512] = {};
                g_gl.GetShaderInfoLog(sh, sizeof(log), nullptr, log);
                if (FILE *fp = fopen("sdmc:/slaunch/boot.log", "a")) {
                    fprintf(fp, "gfx: shader compile failed: %s\n", log);
                    fclose(fp);
                }
                g_gl.DeleteShader(sh);
                return 0;
            }
            return sh;
        }

        GLint_ Loc(const char *name) {
            FxProg &p = g_progs[g_cur];
            for (auto &kv : p.loc) if (kv.first == name) return kv.second;
            const GLint_ l = g_gl.GetUniformLocation(p.prog, name);
            p.loc.push_back({ name, l });
            return l;
        }

    } // namespace

    bool Gfx::ShaderFxInit() {
        if (m_fx_tried) return m_fx_tried > 0;
        m_fx_tried = -1;                       // pessimistic until it all works
        // Escape hatch: if the GPU path ever misbehaves on some firmware, this
        // file puts every effect back on the rect-drawn fallback.
        if (FILE *f = fopen("sdmc:/slaunch/config/no_gpu_fx", "r")) { fclose(f); return false; }
        // Only GL renderers have a context to share; anything else (SDL_GPU)
        // keeps every effect on its SDL-drawn fallback.
        const char *rname = SDL_GetRendererName(m_renderer);
        if (!rname || (strcmp(rname, "opengl") != 0 && strcmp(rname, "opengles2") != 0))
            return false;
        m_gles = strcmp(rname, "opengles2") == 0;
        if (!LoadGl()) return false;

        std::vector<float> v((size_t)kFxVerts * 4);
        static const int kCorner[6] = { 0, 1, 2, 2, 1, 3 };   // two triangles
        for (int k = 0; k < kFxVerts; k++) {
            v[k * 4 + 0] = (float)(k >> 1);
            v[k * 4 + 1] = (float)(k & 1);
            v[k * 4 + 2] = (float)(k / 6);
            v[k * 4 + 3] = (float)kCorner[k % 6];
        }
        GLuint_ vbo = 0;
        g_gl.GenBuffers(1, &vbo);
        if (!vbo) return false;
        SDL_FlushRenderer(m_renderer);
        GLint_ prev_buf = 0;
        g_gl.GetIntegerv(GL_ARRAY_BUFFER_BINDING_, &prev_buf);
        g_gl.BindBuffer(GL_ARRAY_BUFFER_, vbo);
        g_gl.BufferData(GL_ARRAY_BUFFER_, (long)(v.size() * sizeof(float)), v.data(), GL_STATIC_DRAW_);
        g_gl.BindBuffer(GL_ARRAY_BUFFER_, (GLuint_)prev_buf);

        m_fx_vbo    = vbo;
        m_fx_tex_ok = FxTextureSelfTest();
        m_fx_tried  = 1;
        return true;
    }

    int Gfx::FxProgram(const char *vs, const char *fs) {
        if (!ShaderFxInit()) return -1;
        for (auto &kv : g_prog_of) if (kv.first == vs) return kv.second;

        int id = -1;   // a failure is cached too, so it is not retried per frame
        GLuint_ v = Compile(GL_VERTEX_SHADER_, kVsPrelude, vs);
        GLuint_ f = v ? Compile(GL_FRAGMENT_SHADER_, kFsPrelude, fs) : 0;
        GLuint_ prog = (v && f) ? g_gl.CreateProgram() : 0;
        if (prog) {
            g_gl.AttachShader(prog, v);
            g_gl.AttachShader(prog, f);
            g_gl.BindAttribLocation(prog, 0, "aV");
            g_gl.LinkProgram(prog);
            GLint_ ok = 0;
            g_gl.GetProgramiv(prog, GL_LINK_STATUS_, &ok);
            if (ok) { g_progs.push_back({ prog, {} }); id = (int)g_progs.size() - 1; }
        }
        if (v) g_gl.DeleteShader(v);
        if (f) g_gl.DeleteShader(f);
        g_prog_of.push_back({ vs, id });
        return id;
    }

    // Passes are lazy: one stays open across consecutive GPU draws (switching
    // program is cheap, the state was saved when it opened) and is closed by
    // the next SDL draw through Gfx, or by FxClose. Each open costs an SDL
    // batch flush, so a row of cards drawn back to back pays for one.
    bool Gfx::FxBegin(int prog) {
        if (prog < 0 || prog >= (int)g_progs.size()) return false;
        if (g_cur == prog) return true;
        if (g_cur >= 0) {
            g_cur = prog;
            g_gl.UseProgram(g_progs[prog].prog);
            FxSet("uFlip", SDL_GetRenderTarget(m_renderer) ? -1.0f : 1.0f);
            FxSet("uTime", (float)armGetSystemTick() / (float)armGetSystemTickFreq());
            return true;
        }

        // Hand the context over: everything the renderer has queued must be on
        // the GPU before we touch GL state it is not expecting to change.
        SDL_FlushRenderer(m_renderer);

        Saved &s = g_saved;
        g_gl.GetIntegerv(GL_CURRENT_PROGRAM_, &s.prog);
        g_gl.GetIntegerv(GL_ARRAY_BUFFER_BINDING_, &s.buf);
        g_gl.GetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_ENABLED_, &s.attr0);
        g_gl.GetIntegerv(GL_BLEND_SRC_RGB_, &s.bs);
        g_gl.GetIntegerv(GL_BLEND_DST_RGB_, &s.bd);
        g_gl.GetIntegerv(GL_BLEND_SRC_ALPHA_, &s.bsa);
        g_gl.GetIntegerv(GL_BLEND_DST_ALPHA_, &s.bda);
        s.blend = g_gl.IsEnabled(GL_BLEND_) != 0;
        // Samplers read unit 0, which SDL does not promise is the active one.
        // What is bound there goes back on the way out: SDL skips rebinding a
        // texture it believes is still bound.
        g_gl.GetIntegerv(GL_ACTIVE_TEXTURE_, &s.unit);
        g_gl.ActiveTexture(GL_TEXTURE0_);
        g_gl.GetIntegerv(GL_TEXTURE_BINDING_2D_, &s.tex);

        g_cur = prog;
        g_gl.UseProgram(g_progs[prog].prog);
        g_gl.BindBuffer(GL_ARRAY_BUFFER_, m_fx_vbo);
        g_gl.EnableVertexAttribArray(0);
        g_gl.VertexAttribPointer(0, 4, GL_FLOAT_, 0, 0, nullptr);
        // Shaders write premultiplied colour, so one function covers both an
        // ordinary blend (rgb*a, a) and a purely additive glow (rgb, 0).
        g_gl.Enable(GL_BLEND_);
        g_gl.BlendFuncSeparate(GL_ONE_, GL_ONE_MINUS_SRC_ALPHA_, GL_ONE_, GL_ONE_MINUS_SRC_ALPHA_);

        FxSet("uFlip", SDL_GetRenderTarget(m_renderer) ? -1.0f : 1.0f);
        FxSet("uTime", (float)armGetSystemTick() / (float)armGetSystemTickFreq());
        return true;
    }

    void Gfx::FxSet(const char *name, float a) {
        if (g_cur < 0) return;
        const GLint_ l = Loc(name);
        if (l >= 0) g_gl.Uniform1f(l, a);
    }
    void Gfx::FxSet(const char *name, float a, float b, float c, float d) {
        if (g_cur < 0) return;
        const GLint_ l = Loc(name);
        if (l >= 0) g_gl.Uniform4f(l, a, b, c, d);
    }
    void Gfx::FxSet(const char *name, SDL_Color c) {
        FxSet(name, c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, c.a / 255.0f);
    }
    void Gfx::FxStrip(int columns) {
        if (g_cur < 0 || columns < 2) return;
        g_gl.DrawArrays(GL_TRIANGLE_STRIP_, 0, std::min(columns, kFxVerts / 2) * 2);
    }
    void Gfx::FxQuads(int count) {
        if (g_cur < 0 || count < 1) return;
        g_gl.DrawArrays(GL_TRIANGLES_, 0, std::min(count, kFxVerts / 6) * 6);
    }

    // Whether SDL hands out the GL names of its textures, checked once with
    // two textures; without them the textured GPU paths stand down.
    bool Gfx::FxTexturesWork() { return m_fx_tex_ok; }
    // Run once from ShaderFxInit, which always happens before any pass opens.
    bool Gfx::FxTextureSelfTest() {
        SDL_Texture *other = SDL_CreateTexture(m_renderer, SDL_PIXELFORMAT_ARGB8888,
                                               SDL_TEXTUREACCESS_STATIC, 1, 1);
        SDL_Texture *white = WhiteTexture();
        if (!other || !white) { if (other) SDL_DestroyTexture(other); return false; }
        const GLuint_ a = GlName(white), b = GlName(other);
        SDL_DestroyTexture(other);
        return a != 0 && b != 0 && a != b;
    }

    void Gfx::FxTexture(SDL_Texture *tex) {
        if (g_cur < 0) return;
        if (!tex) tex = WhiteTexture();
        if (tex != m_fx_tex) {
            const GLuint_ name = GlName(tex);
            if (!name) return;
            g_gl.BindTexture(GL_TEXTURE_2D_, name);
            m_fx_tex = tex;
        }
        // SDL3's GL renderers size textures exactly (no power-of-two padding)
        FxSet("uTexScale", 1.0f, 1.0f, 0.0f, 0.0f);  // per program, so always
        // How the texture's bytes sit in GL. SDL's GLES2 renderer stores every
        // format but ABGR8888/BGR888 with red and blue swapped (a format it
        // does not support becomes ARGB8888 or RGB888 behind the scenes) and
        // fixes that in its own shaders - which ours are not. Formats without
        // alpha leave junk in that channel on either renderer.
        const SDL_PixelFormat fmt = tex->format;
        const bool swap = m_gles && fmt != SDL_PIXELFORMAT_ABGR8888 &&
                          fmt != SDL_PIXELFORMAT_XBGR8888;
        FxSet("uSwz", swap ? 1.0f : 0.0f, SDL_ISPIXELFORMAT_ALPHA(fmt) ? 0.0f : 1.0f, 0.0f, 0.0f);
    }

    void Gfx::FxClose() { FxEnd(); }

    void Gfx::FxEnd() {
        if (g_cur < 0) return;
        const Saved &s = g_saved;
        if (m_fx_tex) { g_gl.BindTexture(GL_TEXTURE_2D_, (GLuint_)s.tex); m_fx_tex = nullptr; }
        g_gl.ActiveTexture((GLenum_)s.unit);
        if (!s.attr0) g_gl.DisableVertexAttribArray(0);
        g_gl.BindBuffer(GL_ARRAY_BUFFER_, (GLuint_)s.buf);
        g_gl.UseProgram((GLuint_)s.prog);
        g_gl.BlendFuncSeparate((GLenum_)s.bs, (GLenum_)s.bd, (GLenum_)s.bsa, (GLenum_)s.bda);
        if (!s.blend) g_gl.Disable(GL_BLEND_);
        g_cur = -1;
    }

#else   // SL_VULKAN
    // =========================================================================
    // The same effects on SDL_GPU
    //
    // SDL's GPU renderer takes a custom fragment shader but never a vertex
    // shader, and these effects build their geometry in the vertex shader. So
    // a pass draws with SDL_GPU directly - its own command buffer, its own
    // pipelines, the SPIR-V scripts/fx-spirv.py made from the GLSL above -
    // into a transparent layer the size of the output, which FxEnd hands to
    // SDL to lay over the frame. The shaders write premultiplied colour, so
    // compositing the layer premultiplied gives exactly what drawing them in
    // place would have.
    //
    // A pass is submitted when it ends, ahead of the frame SDL is still
    // building - the GPU runs it first, and SDL's draw of the layer after.
    // That is why every pass in a frame gets a layer of its own: a second
    // pass re-using the first's layer would overwrite it before SDL's frame
    // got to draw it.
    // =========================================================================
    namespace {
        constexpr int kFxVerts = 4096;   // as the GL path: vertex k = (k/2, k%2, k/6, corner)

        struct VkProg {
            const fxspv::Program *src = nullptr;
            SDL_GPUShader *vs = nullptr, *fs = nullptr;
            SDL_GPUGraphicsPipeline *pipe[2] = {};   // triangle list, strip
            std::vector<float> u;                    // n_uniforms vec4s
        };
        SDL_GPUDevice *g_dev = nullptr;
        SDL_GPUBuffer *g_vbuf = nullptr;
        SDL_GPUSampler *g_sampler = nullptr;
        std::vector<VkProg> g_progs;
        std::vector<std::pair<const char *, int>> g_prog_of;   // vs literal -> index
        int g_cur = -1;

        SDL_GPUCommandBuffer *g_cmd = nullptr;
        SDL_GPURenderPass *g_pass = nullptr;
        SDL_Texture *g_bound = nullptr;              // FxTexture's, for the next draw
        std::vector<SDL_Texture *> g_layers;
        size_t g_layer_next = 0;                     // this frame's next free layer

        const SDL_BlendMode kPremultiplied = SDL_ComposeCustomBlendMode(
            SDL_BLENDFACTOR_ONE, SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA, SDL_BLENDOPERATION_ADD,
            SDL_BLENDFACTOR_ONE, SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA, SDL_BLENDOPERATION_ADD);

        uint32_t Fnv(const char *s, uint32_t h = 0x811c9dc5u) {
            for (; *s; s++) h = (h ^ (unsigned char)*s) * 0x01000193u;
            return h;
        }
        const fxspv::Program *Find(const char *vs, const char *fs) {
            const uint32_t h = Fnv(fs, Fnv(vs ? vs : ""));
            for (const auto &p : fxspv::kPrograms) if (p.hash == h) return &p;
            return nullptr;
        }
        SDL_GPUTexture *GpuTexture(SDL_Texture *tex) {
            return (SDL_GPUTexture *)SDL_GetPointerProperty(
                SDL_GetTextureProperties(tex), SDL_PROP_TEXTURE_GPU_TEXTURE_POINTER, nullptr);
        }
        SDL_GPUShader *Shader(const uint32_t *code, size_t size, SDL_GPUShaderStage stage, int samplers) {
            SDL_GPUShaderCreateInfo ci{};
            ci.code_size = size;
            ci.code = (const Uint8 *)code;
            ci.entrypoint = "main";
            ci.format = SDL_GPU_SHADERFORMAT_SPIRV;
            ci.stage = stage;
            ci.num_samplers = (Uint32)samplers;
            ci.num_uniform_buffers = 1;
            return SDL_CreateGPUShader(g_dev, &ci);
        }
        SDL_GPUGraphicsPipeline *Pipeline(VkProg &p, bool strip) {
            SDL_GPUGraphicsPipeline *&pipe = p.pipe[strip];
            if (pipe) return pipe;
            SDL_GPUVertexBufferDescription vb{};
            vb.slot = 0;
            vb.pitch = 16;
            vb.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
            SDL_GPUVertexAttribute va{};
            va.location = 0;
            va.buffer_slot = 0;
            va.format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
            SDL_GPUColorTargetDescription ct{};
            ct.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;   // the layers (RGBA32)
            ct.blend_state.enable_blend = true;
            ct.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
            ct.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
            ct.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
            ct.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
            ct.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
            ct.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
            SDL_GPUGraphicsPipelineCreateInfo ci{};
            ci.vertex_shader = p.vs;
            ci.fragment_shader = p.fs;
            ci.vertex_input_state.vertex_buffer_descriptions = &vb;
            ci.vertex_input_state.num_vertex_buffers = 1;
            ci.vertex_input_state.vertex_attributes = &va;
            ci.vertex_input_state.num_vertex_attributes = 1;
            ci.primitive_type = strip ? SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP : SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
            ci.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
            ci.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
            ci.target_info.color_target_descriptions = &ct;
            ci.target_info.num_color_targets = 1;
            pipe = SDL_CreateGPUGraphicsPipeline(g_dev, &ci);
            if (!pipe) GfxLog("fx pipeline");
            return pipe;
        }
        void Draw(bool strip, Uint32 verts) {
            if (g_cur < 0 || !g_pass) return;
            VkProg &p = g_progs[g_cur];
            SDL_GPUGraphicsPipeline *pipe = Pipeline(p, strip);
            if (!pipe) return;
            SDL_BindGPUGraphicsPipeline(g_pass, pipe);
            const SDL_GPUBufferBinding vb{ g_vbuf, 0 };
            SDL_BindGPUVertexBuffers(g_pass, 0, &vb, 1);
            if (p.src->n_samplers) {
                SDL_GPUTextureSamplerBinding tb{};
                tb.texture = g_bound ? GpuTexture(g_bound) : nullptr;
                tb.sampler = g_sampler;
                if (!tb.texture) return;
                SDL_BindGPUFragmentSamplers(g_pass, 0, &tb, 1);
            }
            const Uint32 bytes = (Uint32)(p.u.size() * sizeof(float));
            SDL_PushGPUVertexUniformData(g_cmd, 0, p.u.data(), bytes);
            SDL_PushGPUFragmentUniformData(g_cmd, 0, p.u.data(), bytes);
            SDL_DrawGPUPrimitives(g_pass, verts, 1, 0, 0);
        }
    }

    bool Gfx::ShaderFxInit() {
        if (m_fx_tried) return m_fx_tried > 0;
        m_fx_tried = -1;
        if (FILE *f = fopen("sdmc:/slaunch/config/no_gpu_fx", "r")) { fclose(f); return false; }
        g_dev = SDL_GetGPURendererDevice(m_renderer);
        if (!g_dev) return false;

        std::vector<float> v((size_t)kFxVerts * 4);
        static const int kCorner[6] = { 0, 1, 2, 2, 1, 3 };   // two triangles
        for (int k = 0; k < kFxVerts; k++) {
            v[k * 4 + 0] = (float)(k >> 1);
            v[k * 4 + 1] = (float)(k & 1);
            v[k * 4 + 2] = (float)(k / 6);
            v[k * 4 + 3] = (float)kCorner[k % 6];
        }
        const Uint32 size = (Uint32)(v.size() * sizeof(float));
        SDL_GPUBufferCreateInfo bi{};
        bi.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
        bi.size = size;
        g_vbuf = SDL_CreateGPUBuffer(g_dev, &bi);
        SDL_GPUTransferBufferCreateInfo ti{};
        ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        ti.size = size;
        SDL_GPUTransferBuffer *tb = g_vbuf ? SDL_CreateGPUTransferBuffer(g_dev, &ti) : nullptr;
        if (!tb) { GfxLog("fx vertex buffer"); return false; }
        memcpy(SDL_MapGPUTransferBuffer(g_dev, tb, false), v.data(), size);
        SDL_UnmapGPUTransferBuffer(g_dev, tb);
        SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(g_dev);
        SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
        const SDL_GPUTransferBufferLocation from{ tb, 0 };
        const SDL_GPUBufferRegion to{ g_vbuf, 0, size };
        SDL_UploadToGPUBuffer(copy, &from, &to, false);
        SDL_EndGPUCopyPass(copy);
        SDL_SubmitGPUCommandBuffer(cmd);
        SDL_ReleaseGPUTransferBuffer(g_dev, tb);

        SDL_GPUSamplerCreateInfo si{};
        si.min_filter = si.mag_filter = SDL_GPU_FILTER_LINEAR;
        si.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
        si.address_mode_u = si.address_mode_v = si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        g_sampler = SDL_CreateGPUSampler(g_dev, &si);
        if (!g_sampler) return false;

        m_fx_tex_ok = true;
        m_fx_tried = 1;
        return true;
    }

    int Gfx::FxProgram(const char *vs, const char *fs) {
        if (!ShaderFxInit()) return -1;
        for (auto &kv : g_prog_of) if (kv.first == vs) return kv.second;
        int id = -1;   // a failure is cached too, so it is not retried per frame
        const fxspv::Program *src = Find(vs, fs);
        if (src && src->vs) {
            VkProg p;
            p.src = src;
            p.vs = Shader(src->vs, src->vs_size, SDL_GPU_SHADERSTAGE_VERTEX, 0);
            p.fs = Shader(src->fs, src->fs_size, SDL_GPU_SHADERSTAGE_FRAGMENT, src->n_samplers);
            p.u.assign((size_t)std::max(1, src->n_uniforms) * 4, 0.0f);
            if (p.vs && p.fs) { g_progs.push_back(std::move(p)); id = (int)g_progs.size() - 1; }
            else GfxLog("fx shader");
        }
        g_prog_of.push_back({ vs, id });
        return id;
    }

    bool Gfx::FxBegin(int prog) {
        if (prog < 0 || prog >= (int)g_progs.size()) return false;
        if (g_cur < 0) {
            int ow = 0, oh = 0;
            SDL_GetRenderOutputSize(m_renderer, &ow, &oh);
            if (g_layer_next == g_layers.size()) {
                SDL_Texture *l = SDL_CreateTexture(m_renderer, SDL_PIXELFORMAT_RGBA32,
                                                   SDL_TEXTUREACCESS_TARGET, ow, oh);
                if (!l) return false;
                SDL_SetTextureBlendMode(l, kPremultiplied);
                g_layers.push_back(l);
            }
            SDL_Texture *layer = g_layers[g_layer_next];
            g_cmd = SDL_AcquireGPUCommandBuffer(g_dev);
            if (!g_cmd) return false;
            SDL_GPUColorTargetInfo ct{};
            ct.texture = GpuTexture(layer);
            ct.clear_color = { 0.0f, 0.0f, 0.0f, 0.0f };
            ct.load_op = SDL_GPU_LOADOP_CLEAR;
            ct.store_op = SDL_GPU_STOREOP_STORE;
            g_pass = ct.texture ? SDL_BeginGPURenderPass(g_cmd, &ct, 1, nullptr) : nullptr;
            if (!g_pass) { SDL_CancelGPUCommandBuffer(g_cmd); g_cmd = nullptr; return false; }
            g_layer_next++;
        }
        g_cur = prog;
        g_bound = nullptr;
        // y up, as on GL's window: SDL_GPU flips Vulkan's viewport (a negative
        // height) so every backend's clip space points the same way.
        FxSet("uFlip", 1.0f);
        FxSet("uTime", (float)armGetSystemTick() / (float)armGetSystemTickFreq());
        return true;
    }

    static float *FxSlot(const char *name) {
        if (g_cur < 0) return nullptr;
        VkProg &p = g_progs[g_cur];
        for (int i = 0; i < p.src->n_uniforms; i++)
            if (!strcmp(p.src->uniforms[i], name)) return &p.u[(size_t)i * 4];
        return nullptr;
    }
    void Gfx::FxSet(const char *name, float a) {
        if (float *u = FxSlot(name)) u[0] = a;
    }
    void Gfx::FxSet(const char *name, float a, float b, float c, float d) {
        if (float *u = FxSlot(name)) { u[0] = a; u[1] = b; u[2] = c; u[3] = d; }
    }
    void Gfx::FxSet(const char *name, SDL_Color c) {
        FxSet(name, c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, c.a / 255.0f);
    }
    void Gfx::FxStrip(int columns) {
        if (columns >= 2) Draw(true, (Uint32)std::min(columns, kFxVerts / 2) * 2);
    }
    void Gfx::FxQuads(int count) {
        if (count >= 1) Draw(false, (Uint32)std::min(count, kFxVerts / 6) * 6);
    }

    bool Gfx::FxTexturesWork() { return m_fx_tex_ok; }
    bool Gfx::FxTextureSelfTest() { return true; }

    // SDL_GPU's formats carry their own channel order, so no swizzle; the
    // formats without alpha still leave junk there.
    void Gfx::FxTexture(SDL_Texture *tex) {
        if (g_cur < 0) return;
        g_bound = tex ? tex : WhiteTexture();
        m_fx_tex = g_bound;
        FxSet("uTexScale", 1.0f, 1.0f, 0.0f, 0.0f);
        FxSet("uSwz", 0.0f, SDL_ISPIXELFORMAT_ALPHA(g_bound->format) ? 0.0f : 1.0f, 0.0f, 0.0f);
    }

    void Gfx::FxClose() { FxEnd(); }

    void Gfx::FxEnd() {
        if (g_cur < 0) return;
        g_cur = -1;
        g_bound = nullptr;
        m_fx_tex = nullptr;
        SDL_EndGPURenderPass(g_pass);
        SDL_SubmitGPUCommandBuffer(g_cmd);
        g_pass = nullptr;
        g_cmd = nullptr;
        SDL_RenderTexture(m_renderer, g_layers[g_layer_next - 1], nullptr, nullptr);
    }

    // Called by Present: the frame that drew this frame's layers is submitted,
    // so the next one can have them back.
    static void FxFrameDone() { g_layer_next = 0; }
#endif  // SL_VULKAN

    // ---- GPU wallpaper blur ---------------------------------------------------
    // Halve the image `levels` times, then double it back up again to half
    // size, every step a bilinear RenderCopy. Each halving is an exact 2x2 box
    // and each doubling a tent, so the chain lands close to a Gaussian whose
    // width doubles per level - and all of it is texture sampling on the GPU.
    SDL_Texture *Gfx::Blurred(SDL_Texture *src, int radius) {
        FxClose();   // an open GPU pass must end before SDL draws again
        if (!src) return nullptr;
        const int w = src->w, h = src->h;
        int levels = 1;
        while ((2 << levels) <= radius && levels < 6) levels++;

        // (the chain's targets have no logical size of their own: SDL3 keeps
        // that per target, so nothing needs switching off around them)
        SDL_Texture *prev_target = SDL_GetRenderTarget(m_renderer);
        SDL_BlendMode src_mode = SDL_BLENDMODE_BLEND;
        SDL_GetTextureBlendMode(src, &src_mode);
        SDL_SetTextureBlendMode(src, SDL_BLENDMODE_NONE);

        std::vector<SDL_Texture *> chain{ src };
        for (int i = 1; i <= levels; i++) {
            SDL_Texture *t = SDL_CreateTexture(m_renderer, SDL_PIXELFORMAT_RGBA8888,
                                               SDL_TEXTUREACCESS_TARGET,
                                               std::max(1, w >> i), std::max(1, h >> i));
            if (!t) break;
            SDL_SetTextureBlendMode(t, SDL_BLENDMODE_NONE);
            SDL_SetRenderTarget(m_renderer, t);
            SDL_RenderTexture(m_renderer, chain.back(), nullptr, nullptr);
            chain.push_back(t);
        }
        for (int i = (int)chain.size() - 2; i >= 1; i--) {
            SDL_SetRenderTarget(m_renderer, chain[i]);
            SDL_RenderTexture(m_renderer, chain[i + 1], nullptr, nullptr);
        }

        SDL_SetRenderTarget(m_renderer, prev_target);
        SDL_SetTextureBlendMode(src, src_mode);

        for (size_t i = 2; i < chain.size(); i++) SDL_DestroyTexture(chain[i]);
        if (chain.size() < 2) return nullptr;
        SDL_SetTextureBlendMode(chain[1], SDL_BLENDMODE_BLEND);
        return chain[1];
    }

    // ---- GPU cards ---------------------------------------------------------------
    namespace {
        const char *kCardVs = R"(
uniform vec4 uQuad;   // where the quad goes: the card plus its shadow margin
varying vec2 vP;
void main() {
    vP = uQuad.xy + Corner() * uQuad.zw;
    gl_Position = Clip(vP);
}
)";
        // Everything is a signed distance to the rounded rectangle, so the
        // edge is anti-aliased for free and the shadow and glow are just
        // falloffs of the same number.
        const char *kCardFs = R"(
uniform vec4 uRect;      // card x, y, w, h
uniform vec4 uStyle;     // radius, shadow, glow, reflection pass
uniform vec4 uFill, uGlowCol, uTint;
uniform vec4 uOpt;       // has texture, glyph, alpha, bottom band px
uniform vec4 uTexScale;
uniform vec4 uSwz;
uniform sampler2D uTex;
varying vec2 vP;
vec4 Sample(vec2 uv) {
    vec4 c = texture2D(uTex, uv);
    if (uSwz.x > 0.5) c = c.bgra;
    if (uSwz.y > 0.5) c.a = 1.0;
    return c;
}
float SdRound(vec2 p, vec2 c, vec2 hs, float r) {
    vec2 q = abs(p - c) - hs + r;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}
void main() {
    vec2  hs = uRect.zw * 0.5, c = uRect.xy + hs;
    float r  = min(uStyle.x, min(hs.x, hs.y));
    vec2  p  = vP;
    float fade = 1.0;
    if (uStyle.w > 0.5) {                       // reflection: mirror at the base
        float line = uRect.y + uRect.w + 4.0;
        fade = 0.30 * (1.0 - clamp((p.y - line) / (uRect.w * 0.45), 0.0, 1.0));
        p.y = 2.0 * line - p.y;
    }
    float d = SdRound(p, c, hs, r);
    float inside = clamp(0.5 - d, 0.0, 1.0);

    vec4 body = vec4(uFill.rgb * uFill.a, uFill.a);
    if (uOpt.x > 0.5) {
        vec4 tx = Sample((p - uRect.xy) / uRect.zw * uTexScale.xy);
        if (uOpt.y > 0.5) tx = vec4(uTint.rgb, tx.a * uTint.a);
        body = vec4(tx.rgb * tx.a, tx.a) + body * (1.0 - tx.a);
    }
    if (uOpt.w > 0.0 && p.y > uRect.y + uRect.w - uOpt.w)   // label band
        body = vec4(body.rgb * 0.45, max(body.a, 0.55));
    vec4 o = body * inside;

    if (uStyle.w > 0.5) {
        o *= fade;
    } else {
        if (uStyle.y > 0.0) {                    // soft shadow, dropped a little
            float ds = SdRound(vP - vec2(0.0, uStyle.y * 0.35), c, hs, r);
            float sh = (1.0 - smoothstep(-uStyle.y * 0.4, uStyle.y, ds)) * 0.45;
            o += vec4(0.0, 0.0, 0.0, sh) * (1.0 - o.a);
        }
        if (uStyle.z > 0.0) {                    // selection: ring and glow
            float ring = 1.0 - smoothstep(0.8, 2.2, abs(d - 2.0));
            float g = d > 0.0 ? exp(-d / 10.0) : 0.0;
            o.rgb += uGlowCol.rgb * (ring + g * 0.55) * uStyle.z;
            o.a = max(o.a, ring * uStyle.z);
        }
    }
    gl_FragColor = o * uOpt.z;
}
)";
    }

#ifndef SL_VULKAN
    static int g_card_prog = -2;
    bool Gfx::CardsOk() {
        if (g_card_prog == -2) g_card_prog = FxProgram(kCardVs, kCardFs);
        return g_card_prog >= 0 && m_fx_tex_ok;
    }
    bool Gfx::Card(SDL_Texture *tex, float x, float y, float w, float h,
                   const CardStyle &st, Uint8 alpha) {
        if (g_card_prog == -2) g_card_prog = FxProgram(kCardVs, kCardFs);
        const int prog = g_card_prog;
        if (w <= 0.0f || h <= 0.0f || prog < 0) return false;
        if (tex && !m_fx_tex_ok) return false;
        if (!FxBegin(prog)) return false;
        FxTexture(tex);
        FxSet("uRect", x, y, w, h);
        FxSet("uFill", st.fill);
        FxSet("uGlowCol", st.glow_col);
        FxSet("uTint", st.tint);
        FxSet("uOpt", tex ? 1.0f : 0.0f, st.glyph ? 1.0f : 0.0f, alpha / 255.0f, st.band);
        FxSet("uStyle", st.radius, st.shadow, st.glow, 0.0f);
        const float m = st.shadow + (st.glow > 0.0f ? 30.0f : 2.0f);
        FxSet("uQuad", x - m, y - m, w + 2 * m, h + 2 * m);
        FxQuads(1);
        if (st.reflect) {
            FxSet("uStyle", st.radius, 0.0f, 0.0f, 1.0f);
            FxSet("uQuad", x, y + h + 4.0f, w, h * 0.45f);
            FxQuads(1);
        }
        return true;
    }

#else   // SL_VULKAN
    // The card is a custom fragment shader in SDL's own draw stream: SDL's
    // vertex shader passes the texture coordinate through untouched, so the
    // quad carries its pixel position there and the shader (kCardFs, made
    // SPIR-V by scripts/fx-spirv.py) works from that exactly as the GL one
    // works from its own. Changing the uniforms between cards makes SDL
    // flush the cards before, so each keeps its own.
    static SDL_GPURenderState *g_card_state = nullptr;
    static const fxspv::Program *g_card_src = nullptr;
    static int g_card_tried = 0;
    bool Gfx::CardsOk() {
        if (g_card_tried) return g_card_tried > 0;
        g_card_tried = -1;
        if (!ShaderFxInit()) return false;
        g_card_src = Find(kCardVs, kCardFs);
        if (!g_card_src) return false;
        SDL_GPUShader *fs = Shader(g_card_src->fs, g_card_src->fs_size,
                                   SDL_GPU_SHADERSTAGE_FRAGMENT, g_card_src->n_samplers);
        if (!fs) return false;
        SDL_GPURenderStateCreateInfo ci{};
        ci.fragment_shader = fs;
        g_card_state = SDL_CreateGPURenderState(m_renderer, &ci);
        if (!g_card_state) { GfxLog("card render state"); return false; }
        g_card_tried = 1;
        return true;
    }
    bool Gfx::Card(SDL_Texture *tex, float x, float y, float w, float h,
                   const CardStyle &st, Uint8 alpha) {
        if (w <= 0.0f || h <= 0.0f || !CardsOk()) return false;
        FxClose();
        std::vector<float> u((size_t)g_card_src->n_uniforms * 4, 0.0f);
        auto set = [&](const char *name, float a, float b, float c, float d) {
            for (int i = 0; i < g_card_src->n_uniforms; i++)
                if (!strcmp(g_card_src->uniforms[i], name)) {
                    float *v = &u[(size_t)i * 4];
                    v[0] = a; v[1] = b; v[2] = c; v[3] = d;
                }
        };
        auto col = [&](const char *name, SDL_Color c) {
            set(name, c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, c.a / 255.0f);
        };
        SDL_Texture *t = tex ? tex : WhiteTexture();
        set("uRect", x, y, w, h);
        col("uFill", st.fill);
        col("uGlowCol", st.glow_col);
        col("uTint", st.tint);
        set("uOpt", tex ? 1.0f : 0.0f, st.glyph ? 1.0f : 0.0f, alpha / 255.0f, st.band);
        set("uTexScale", 1.0f, 1.0f, 0.0f, 0.0f);
        set("uSwz", 0.0f, SDL_ISPIXELFORMAT_ALPHA(t->format) ? 0.0f : 1.0f, 0.0f, 0.0f);

        SDL_BlendMode prev = SDL_BLENDMODE_BLEND;
        SDL_GetTextureBlendMode(t, &prev);
        SDL_SetTextureBlendMode(t, kPremultiplied);
        SDL_SetGPURenderState(m_renderer, g_card_state);
        auto quad = [&](float qx, float qy, float qw, float qh) {
            SDL_Vertex v[4];
            const float xs[4] = { qx, qx + qw, qx + qw, qx }, ys[4] = { qy, qy, qy + qh, qy + qh };
            for (int i = 0; i < 4; i++) {
                v[i].position = { xs[i], ys[i] };
                v[i].tex_coord = { xs[i], ys[i] };   // the pixel position, for vP
                v[i].color = { 1.0f, 1.0f, 1.0f, 1.0f };
            }
            static const int idx[6] = { 0, 1, 2, 0, 2, 3 };
            SDL_SetGPURenderStateFragmentUniforms(g_card_state, 0, u.data(), (Uint32)(u.size() * sizeof(float)));
            SDL_RenderGeometry(m_renderer, t, v, 4, idx, 6);
        };
        set("uStyle", st.radius, st.shadow, st.glow, 0.0f);
        const float m = st.shadow + (st.glow > 0.0f ? 30.0f : 2.0f);
        quad(x - m, y - m, w + 2 * m, h + 2 * m);
        if (st.reflect) {
            set("uStyle", st.radius, 0.0f, 0.0f, 1.0f);
            quad(x, y + h + 4.0f, w, h * 0.45f);
        }
        SDL_SetGPURenderState(m_renderer, nullptr);
        SDL_SetTextureBlendMode(t, prev);
        return true;
    }
#endif  // SL_VULKAN

    // ---- GPU 3D quads -------------------------------------------------------------
    // The same quad DrawQuad3D describes, handed to the GPU as geometry with a
    // real w, so texturing is perspective-correct without any subdivision,
    // and lit: a key light from the upper left front gives each face its own
    // shade as it turns, and a specular highlight slides across it.
    namespace {
        const char *kQuadVs = R"(
uniform vec4 uC0, uC1, uC2, uC3;   // view-space corners: TL, TR, BR, BL
uniform vec4 uUV;                   // u0, v0, u1, v1
uniform vec4 uA;                    // alpha top, alpha bottom, flip v
uniform vec4 uTexScale;
varying vec2 vUV;
varying float vA;
varying vec3 vPos;
void main() {
    vec2 k = Corner();
    vec3 p = mix(mix(uC0.xyz, uC1.xyz, k.x), mix(uC3.xyz, uC2.xyz, k.x), k.y);
    float v = uA.z > 0.5 ? 1.0 - k.y : k.y;
    vUV  = vec2(mix(uUV.x, uUV.z, k.x), mix(uUV.y, uUV.w, v)) * uTexScale.xy;
    vA   = mix(uA.x, uA.y, k.y);
    vPos = p;
    // Gfx::Focal is 900; the divide by w (= depth) is the projection.
    gl_Position = vec4(p.x * 900.0 / 640.0, p.y * 900.0 / 360.0 * uFlip, 0.0, p.z);
}
)";
        const char *kQuadFs = R"(
uniform sampler2D uTex;
uniform vec4 uTint;
uniform vec4 uSwz;
uniform vec4 uN;        // face normal (towards the camera), lighting amount
varying vec2 vUV;
varying float vA;
varying vec3 vPos;
void main() {
    vec4 tx  = texture2D(uTex, vUV);
    if (uSwz.x > 0.5) tx = tx.bgra;
    if (uSwz.y > 0.5) tx.a = 1.0;
    tx *= uTint;
    vec3 col = tx.rgb;
    if (uN.w > 0.0) {
        vec3 n = normalize(uN.xyz);
        vec3 L = normalize(vec3(-0.45, 0.55, -1.0));
        float diff = max(dot(n, L), 0.0);
        vec3 R = reflect(-L, n);
        float spec = pow(max(dot(R, normalize(-vPos)), 0.0), 28.0);
        col = col * mix(1.0, 0.55 + 0.55 * diff, uN.w) + vec3(spec * 0.35 * uN.w);
    }
    float a = tx.a * vA;
    gl_FragColor = vec4(col * a, a);
}
)";
    }

    bool Gfx::Quad3DGpu(SDL_Texture *tex, const float c[4][3], SDL_Color tint,
                        Uint8 alpha_top, Uint8 alpha_bottom, bool flip_v, const float uv[4]) {
        static int prog = -2;
        if (prog == -2) prog = FxProgram(kQuadVs, kQuadFs);
        if (prog < 0) return false;
        if (!m_fx_tex_ok) return false;
        if (!FxBegin(prog)) return false;
        FxTexture(tex);
        FxSet("uC0", c[0][0], c[0][1], c[0][2], 0.0f);
        FxSet("uC1", c[1][0], c[1][1], c[1][2], 0.0f);
        FxSet("uC2", c[2][0], c[2][1], c[2][2], 0.0f);
        FxSet("uC3", c[3][0], c[3][1], c[3][2], 0.0f);
        FxSet("uUV", uv ? uv[0] : 0.0f, uv ? uv[1] : 0.0f, uv ? uv[2] : 1.0f, uv ? uv[3] : 1.0f);
        FxSet("uA", alpha_top / 255.0f, alpha_bottom / 255.0f, flip_v ? 1.0f : 0.0f, 0.0f);
        FxSet("uTint", tint.r / 255.0f, tint.g / 255.0f, tint.b / 255.0f, 1.0f);
        // Face normal, turned to face the camera whichever way the corners run.
        const float ax = c[1][0] - c[0][0], ay = c[1][1] - c[0][1], az = c[1][2] - c[0][2];
        const float bx = c[3][0] - c[0][0], by = c[3][1] - c[0][1], bz = c[3][2] - c[0][2];
        float nx = ay * bz - az * by, ny = az * bx - ax * bz, nz = ax * by - ay * bx;
        const float mx = c[0][0] + c[2][0], my = c[0][1] + c[2][1], mz = c[0][2] + c[2][2];
        if (nx * mx + ny * my + nz * mz > 0.0f) { nx = -nx; ny = -ny; nz = -nz; }
        FxSet("uN", nx, ny, nz, flip_v ? 0.5f : 1.0f);   // reflections: softer
        FxQuads(1);
        return true;
    }

    void Gfx::FillRects(const SDL_Rect *r, int n, SDL_Color c) {
        FxClose();   // an open GPU pass must end before SDL draws again
        if (!r || n <= 0) return;
        SDL_SetRenderDrawColor(m_renderer, c.r, c.g, c.b, c.a);
        m_frects.resize((size_t)n);
        for (int i = 0; i < n; i++) m_frects[i] = F(r[i]);
        SDL_RenderFillRects(m_renderer, m_frects.data(), n);
    }

    void Gfx::FillRectAdd(int x, int y, int w, int h, SDL_Color c) {
        FxClose();   // an open GPU pass must end before SDL draws again
        if (w <= 0 || h <= 0) return;
        SDL_SetRenderDrawBlendMode(m_renderer, SDL_BLENDMODE_ADD);
        SDL_SetRenderDrawColor(m_renderer, c.r, c.g, c.b, c.a);
        const SDL_FRect r = F(x, y, w, h);
        SDL_RenderFillRect(m_renderer, &r);
        SDL_SetRenderDrawBlendMode(m_renderer, SDL_BLENDMODE_BLEND);
    }

    void Gfx::LineAA(float x0, float y0, float x1, float y1, SDL_Color c, float width) {
        if (width < 1.0f) width = 1.0f;
        if (y1 < y0) { std::swap(x0, x1); std::swap(y0, y1); }
        const int iy0 = (int)floorf(y0), iy1 = (int)ceilf(y1);
        const int rows = iy1 - iy0;
        if (rows <= 0) return;

        // Two slivers per row: the exact x lands between pixels, so the coverage
        // is handed out between the pixel on each side in proportion to how far
        // in it sits. That is the whole of the antialiasing.
        std::vector<SDL_Rect> a, b;
        a.reserve(rows); b.reserve(rows);
        const float dx = (x1 - x0) / (float)rows;
        float x = x0;
        for (int i = 0; i < rows; i++, x += dx) {
            const float fx = floorf(x);
            const float frac = x - fx;                 // 0 = dead on, ->1 = next pixel
            const int   w    = (int)width;
            a.push_back(SDL_Rect{ (int)fx,     iy0 + i, w, 1 });
            b.push_back(SDL_Rect{ (int)fx + w, iy0 + i, 1, 1 });
            // Coverage is per row, but a single colour per batch is what keeps
            // this to two draw calls; the split is fixed at the line's average
            // fractional position, which for a straight line is exact enough
            // that the steps stop reading as steps.
            (void)frac;
        }
        // Average coverage across the line - a straight line has a constant
        // sub-pixel slope, so one split serves every row.
        const float frac = (x0 - floorf(x0) + x1 - floorf(x1)) * 0.5f;
        SDL_Color ca = c, cb = c;
        ca.a = (Uint8)((float)c.a * (1.0f - frac));
        cb.a = (Uint8)((float)c.a * frac);
        FillRects(a.data(), (int)a.size(), ca);
        FillRects(b.data(), (int)b.size(), cb);
    }

    void Gfx::FillTriangle(int x0, int y0, int x1, int y1, int x2, int y2, SDL_Color c) {
        FxClose();   // an open GPU pass must end before SDL draws again
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
            const SDL_FRect r = F(left, y, right - left + 1, 1);
            SDL_RenderFillRect(m_renderer, &r);
        }
    }

    SDL_Texture *Gfx::LoadGlyph(const char *path, int w, int h) {
        SDL_Surface *raw = IMG_Load(path);
        if (!raw) return nullptr;

        SDL_Surface *s = SDL_ConvertSurface(raw, SDL_PIXELFORMAT_RGBA32);
        SDL_DestroySurface(raw);
        if (!s) return nullptr;

        // A file that already varies its alpha is a real cut-out; leave it be.
        // Otherwise the shape is encoded as brightness on a flat (black)
        // background, so brightness becomes the alpha and the colour goes white.
        bool has_alpha = false;
        const int n = s->w * s->h;
        Uint32 *px = (Uint32 *)s->pixels;
        if (SDL_MUSTLOCK(s)) SDL_LockSurface(s);
        const SDL_PixelFormatDetails *fd = SDL_GetPixelFormatDetails(s->format);
        for (int i = 0; i < n; i++) {
            Uint8 r, g, b, a;
            SDL_GetRGBA(px[i], fd, nullptr, &r, &g, &b, &a);
            if (a != 255) { has_alpha = true; break; }
        }
        if (!has_alpha) {
            for (int i = 0; i < n; i++) {
                Uint8 r, g, b, a;
                SDL_GetRGBA(px[i], fd, nullptr, &r, &g, &b, &a);
                const Uint8 lum = (Uint8)((r * 77 + g * 151 + b * 28) >> 8);
                px[i] = SDL_MapRGBA(fd, nullptr, 255, 255, 255, lum);
            }
        }
        if (SDL_MUSTLOCK(s)) SDL_UnlockSurface(s);

        SDL_Texture *full = SDL_CreateTextureFromSurface(m_renderer, s);
        SDL_DestroySurface(s);
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
        SDL_RenderTexture(m_renderer, full, nullptr, nullptr);
        SDL_SetRenderTarget(m_renderer, prev);
        SDL_DestroyTexture(full);
        return dst;
    }

    // Retired, not destroyed - see m_textGraveyard. The icon caches call this
    // from inside the draw loop (evicting to make room for the icon they are
    // about to load), which is exactly the moment Mesa is most likely to still
    // have the outgoing texture bound.
    void Gfx::FreeImage(SDL_Texture *tex) {
        if (tex && tex == m_fx_tex) FxClose();   // do not free what a pass has bound
        if (tex) m_textGraveyard.push_back(tex);
    }

    void Gfx::DrawImage(SDL_Texture *tex, int x, int y, int w, int h, Uint8 alpha) {
        FxClose();   // an open GPU pass must end before SDL draws again
        if (!tex || w <= 0 || h <= 0) return;
        const SDL_FRect dst = F(x, y, w, h);
        SDL_SetTextureAlphaMod(tex, alpha);
        SDL_RenderTexture(m_renderer, tex, nullptr, &dst);
        SDL_SetTextureAlphaMod(tex, 255); // don't leak the mod to other blits
    }

    void Gfx::DrawImageTinted(SDL_Texture *tex, int x, int y, int w, int h,
                              SDL_Color c, Uint8 alpha) {
        FxClose();
        if (!tex || w <= 0 || h <= 0) return;
        const SDL_FRect dst = F(x, y, w, h);
        SDL_SetTextureColorMod(tex, c.r, c.g, c.b);
        // c's own alpha (as Text() uses it) times the caller's fade multiplier,
        // the same combination IconPlate already does for a theme colour and a
        // row's fade alpha - so DrawImageTinted(tex, ..., t.dim) alone behaves
        // exactly like Text(..., t.dim, ...), and a caller fading a whole panel
        // down can still pass its own alpha on top without fighting t.dim's.
        SDL_SetTextureAlphaMod(tex, (Uint8)((int)c.a * alpha / 255));
        SDL_RenderTexture(m_renderer, tex, nullptr, &dst);
        SDL_SetTextureColorMod(tex, 255, 255, 255);
        SDL_SetTextureAlphaMod(tex, 255); // don't leak either mod to other blits
    }

    void Gfx::DrawCover(SDL_Texture *tex, Uint8 alpha) {
        FxClose();   // an open GPU pass must end before SDL draws again
        if (!tex) return;
        const int tw = tex->w, th = tex->h;
        if (tw <= 0 || th <= 0) return;

        // Cover-fit: scale so the image fills the screen, cropping the overflow.
        float scale = (float)Width / tw;
        if ((float)th * scale < Height)
            scale = (float)Height / th;
        const float dw = tw * scale, dh = th * scale;
        const SDL_FRect dst { (Width - dw) / 2, (Height - dh) / 2, dw, dh };

        SDL_SetTextureAlphaMod(tex, alpha);
        SDL_RenderTexture(m_renderer, tex, nullptr, &dst);
    }

} // namespace sl::menu::gfx
