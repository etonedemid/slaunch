#include <sl/menu/gfx/Gfx.hpp>
#include <vector>
#include <algorithm>
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
        m_widthCache.clear();
        for (auto &kv : m_textCache)
            if (kv.second.tex) m_textGraveyard.push_back(kv.second.tex);
        m_textCache.clear();
        m_textBytes = 0;
        // Slot textures are kept - only their contents are stale. Dropping the
        // keys makes every label re-rasterise into a slot on next use, which is
        // what a font or supersample change needs, with no allocation at all.
        m_slotOf.clear();
        for (auto &sl : m_slots) { sl.key.clear(); sl.used = 0; sl.frame = 0; }
    }

    // Upload `surf` into a reusable slot and return its index, or -1 if every
    // slot has already been drawn this frame.
    //
    // A slot drawn earlier in this same frame must never be handed out again:
    // SDL batches draws and resolves textures at flush time, so overwriting one
    // now would retroactively change what the earlier row shows. With 24 slots
    // and ~15 rows on screen that headroom is never actually reached, but the
    // caller falls back to an ordinary texture if it ever is.
    int Gfx::AcquireSlot(const std::string &key, SDL_Surface *surf) {
        auto hit = m_slotOf.find(key);
        if (hit != m_slotOf.end()) {
            TextSlot &sl = m_slots[hit->second];
            sl.used  = ++m_textClock;
            sl.frame = m_frame;     // claimed for this frame; not recyclable
            return hit->second;
        }
        if (surf->h > kSlotH) return -1;                    // too tall to pool
        int cls = -1;
        for (int c = 0; c < kClassCount; c++) if (surf->w <= kClassW[c]) { cls = c; break; }
        if (cls < 0) return -1;                             // wider than any slot

        int live = 0;
        for (const auto &sl : m_slots) if (sl.cls == cls) live++;

        int idx = -1;
        if (live < kClassN[cls]) {                  // grow on demand, never shrink
            TextSlot sl;
            sl.tex = SDL_CreateTexture(m_renderer, SDL_PIXELFORMAT_ARGB8888,
                                       SDL_TEXTUREACCESS_STATIC, kClassW[cls], kSlotH);
            m_texCreates++;
            if (!sl.tex) { m_texFailures++; return -1; }
            SDL_SetTextureBlendMode(sl.tex, SDL_BLENDMODE_BLEND);
            sl.cls = cls;
            m_slots.push_back(sl);
            idx = (int)m_slots.size() - 1;
        } else {
            for (int i = 0; i < (int)m_slots.size(); i++) {
                if (m_slots[i].cls != cls) continue;
                if (m_slots[i].frame == m_frame) continue;     // in use this frame
                if (idx < 0 || m_slots[i].used < m_slots[idx].used) idx = i;
            }
            if (idx < 0) return -1;                            // all spoken for
            m_slotOf.erase(m_slots[idx].key);
        }

        // Padded by a pixel on the right and bottom so the linear filter cannot
        // pick up whatever the previous occupant left just outside the text.
        SDL_Surface *pad = SDL_CreateRGBSurfaceWithFormat(
            0, std::min(surf->w + 1, kClassW[cls]), std::min(surf->h + 1, kSlotH),
            32, SDL_PIXELFORMAT_ARGB8888);
        if (!pad) return -1;
        SDL_SetSurfaceBlendMode(surf, SDL_BLENDMODE_NONE);   // copy alpha, don't blend
        SDL_BlitSurface(surf, nullptr, pad, nullptr);
        SDL_Rect dst{ 0, 0, pad->w, pad->h };
        SDL_UpdateTexture(m_slots[idx].tex, &dst, pad->pixels, pad->pitch);
        SDL_FreeSurface(pad);

        TextSlot &sl = m_slots[idx];
        sl.key = key; sl.w = surf->w; sl.h = surf->h;
        sl.used = ++m_textClock; sl.frame = m_frame;
        m_slotOf[key] = idx;
        return idx;
    }

    void Gfx::FreeSlots() {
        for (auto &sl : m_slots) if (sl.tex) SDL_DestroyTexture(sl.tex);
        m_slots.clear();
        m_slotOf.clear();
    }

    // Free at least `want_free` bytes, oldest first.
    //
    // This used to be "past 400 entries, throw the whole cache away". With a
    // few dozen short titles that never fired. With thousands of long ROM names
    // it fired constantly, and each time it destroyed every label on screen and
    // rebuilt them the next frame - tens of MB of GPU textures cycling several
    // times a second, until the allocator handed back nothing and the driver
    // dereferenced it mid-frame. Evicting only what is needed, least-recently-
    // used first, keeps the labels that are actually visible resident and makes
    // the steady state flat however long the list is.
    void Gfx::EvictText(size_t want_free) {
        if (m_textCache.empty()) return;
        // Small scan: the cache holds a few hundred entries at most, and this
        // runs only when a new label pushes it over budget.
        std::vector<std::pair<uint64_t, const std::string *>> order;
        order.reserve(m_textCache.size());
        for (auto &kv : m_textCache) order.push_back({ kv.second.used, &kv.first });
        std::sort(order.begin(), order.end(),
                  [](const auto &a, const auto &b) { return a.first < b.first; });

        size_t freed = 0;
        for (auto &e : order) {
            if (freed >= want_free) break;
            auto it = m_textCache.find(*e.second);
            if (it == m_textCache.end()) continue;
            const size_t bytes = (size_t)it->second.w * it->second.h * 4;
            freed += bytes;
            if (it->second.tex) m_textGraveyard.push_back(it->second.tex);
            m_textBytes -= std::min(m_textBytes, bytes);
            m_textCache.erase(it);
        }
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
        FreeSlots();
        ReapTextures();   // retired textures must not outlive the renderer
        if (m_gradTex) SDL_DestroyTexture(m_gradTex);
        if (m_whiteTex) { SDL_DestroyTexture(m_whiteTex); m_whiteTex = nullptr; }
        if (m_scene) { SDL_DestroyTexture(m_scene); m_scene = nullptr; }
        if (m_small) { SDL_DestroyTexture(m_small); m_small = nullptr; }
        FreeAltFonts();
        for (auto &f : m_sysFonts) { if (f) TTF_CloseFont(f); f = nullptr; }
        if (m_renderer) SDL_DestroyRenderer(m_renderer);
        if (m_window)   SDL_DestroyWindow(m_window);
        IMG_Quit();
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
        SDL_RenderPresent(m_renderer);
        m_frame++;          // slots claimed for the finished frame are free again
        // The frame is submitted and nothing is bound from it any more, so this
        // is the one point where dropping a texture cannot pull it out from
        // under a draw that is still referencing it.
        ReapTextures();
    }

    void Gfx::ReapTextures() {
        for (SDL_Texture *t : m_textGraveyard)
            if (t) SDL_DestroyTexture(t);
        m_textGraveyard.clear();
    }

    // ---- live scene capture -------------------------------------------------
    void Gfx::BeginScene() {
        if (!m_scene) {
            int ow = 0, oh = 0;
            SDL_GetRendererOutputSize(m_renderer, &ow, &oh);
            if (ow <= 0 || oh <= 0) return;
            m_scene = SDL_CreateTexture(m_renderer, SDL_PIXELFORMAT_RGBA8888,
                                        SDL_TEXTUREACCESS_TARGET, ow, oh);
            if (!m_scene) return;             // no RT support: draw straight out
            SDL_SetTextureBlendMode(m_scene, SDL_BLENDMODE_NONE);
        }
        SDL_SetRenderTarget(m_renderer, m_scene);
    }

    void Gfx::EndScene() {
        if (!m_scene) return;
        SDL_SetRenderTarget(m_renderer, nullptr);
        SDL_RenderCopy(m_renderer, m_scene, nullptr, nullptr);
    }

    void Gfx::DrawSceneBlurred(int x, int y, int w, int h, int downscale, Uint8 alpha) {
        FxClose();   // an open GPU pass must end before SDL draws again
        if (!m_scene || w <= 0 || h <= 0) return;
        if (downscale < 2) downscale = 2;

        int ow = 0, oh = 0;
        SDL_GetRendererOutputSize(m_renderer, &ow, &oh);
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

        // Logical size maps every draw from 1280x720 onto the window. It has to
        // come off while we are drawing into a texture of a different size, or
        // the scene lands in the top-left corner of it at 1:1.
        int lw = 0, lh = 0;
        SDL_RenderGetLogicalSize(m_renderer, &lw, &lh);
        if (lw || lh) SDL_RenderSetLogicalSize(m_renderer, 0, 0);

        SDL_SetRenderTarget(m_renderer, m_small);
        SDL_RenderCopy(m_renderer, m_scene, nullptr, nullptr);   // bilinear downscale
        SDL_SetRenderTarget(m_renderer, m_scene);

        if (lw || lh) SDL_RenderSetLogicalSize(m_renderer, lw, lh);

        // Sample back only the part of the scene this rect covers, so it reads
        // as the panel frosting what is behind it rather than as a shrunken
        // copy of the whole screen.
        const float s = (float)ow / (float)Width / (float)downscale;
        SDL_Rect src { (int)(x * s), (int)(y * s), (int)(w * s), (int)(h * s) };
        SDL_Rect dst { x, y, w, h };
        SDL_SetTextureAlphaMod(m_small, alpha);
        SDL_RenderCopy(m_renderer, m_small, &src, &dst);
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
            const SDL_Rect ring[4] = {
                { x - i,     y - i,     w + 2 * i, 1         },   // top
                { x - i,     y + h + i, w + 2 * i, 1         },   // bottom
                { x - i,     y - i,     1,         h + 2 * i },   // left
                { x + w + i, y - i,     1,         h + 2 * i },   // right
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
        SDL_Rect r { x, y, w, h };
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
        if (it != m_textCache.end()) { it->second.used = ++m_textClock; return it->second; }

        SDL_Surface *surf = TTF_RenderUTF8_Blended(font, text, SDL_Color{255,255,255,255});
        if (!surf) return empty;
        CachedText ct;
        ct.w = surf->w; ct.h = surf->h;

        // Pool slot first: no allocation, nothing freed. This is the path every
        // list label takes, and the one that used to churn.
        {
            const int idx = AcquireSlot(key, surf);
            if (idx >= 0) {
                ct.tex  = m_slots[idx].tex;
                ct.used = ++m_textClock;
                SDL_FreeSurface(surf);
                // Deliberately not entered in m_textCache: the slot owns the
                // texture, and a cache entry would free it out from under the
                // pool on eviction. m_slotOf is the lookup for these.
                m_pooledRet = ct;
                return m_pooledRet;
            }
            // Pool full for this frame, or the text is outsized: fall through
            // and make it an ordinary texture, exactly as before.
        }

        const size_t bytes = (size_t)surf->w * surf->h * 4;
        ct.tex = SDL_CreateTextureFromSurface(m_renderer, surf);
        m_texCreates++;
        if (!ct.tex) m_texFailures++;
        SDL_FreeSurface(surf);
        if (ct.tex) SDL_SetTextureBlendMode(ct.tex, SDL_BLENDMODE_BLEND);

        // Make room before adding, so the budget is a ceiling rather than
        // something we notice having already passed.
        // Free down to half the budget rather than to exactly the budget, so
        // eviction happens in an occasional burst instead of on every single
        // new label once the cache is full - which is what scrolling a long
        // list does, and what turned a rare event into a per-frame one.
        if (m_textBytes + bytes > kTextCacheBudget)
            EvictText(m_textBytes + bytes - kTextCacheBudget / 2);

        ct.used = ++m_textClock;
        m_textBytes += bytes;
        return m_textCache.emplace(std::move(key), ct).first->second;
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
    // TTF_SizeUTF8 returns exactly the width TTF_RenderUTF8_Blended's surface
    // would have, so nothing about layout changes.
    //
    // Memoised by font and string: every layout measures the same labels on
    // every frame (Ellipsize's binary search, word wrap, centred text, the
    // hint bar), and TTF_SizeUTF8 walks the glyphs each time - a real cost
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
        if (TTF_SizeUTF8(font, text, &w, &h) != 0) return 0;
        if (m_widthCache.size() >= 4096) m_widthCache.clear();
        m_widthCache.emplace(std::move(key), w / m_ss);
        return w / m_ss;
    }

    int Gfx::LineHeight(FontSize s) { return TTF_FontHeight(Font(s)) / m_ss; }

    void Gfx::Text(FontSize s, int x, int y, SDL_Color c, const char *text) {
        FxClose();   // an open GPU pass must end before SDL draws again
        const CachedText &e = GetText(s, text);
        if (!e.tex) return;
        SDL_SetTextureColorMod(e.tex, c.r, c.g, c.b);
        SDL_SetTextureAlphaMod(e.tex, c.a);   // see FillRect: 0 means invisible
        // The glyph texture is m_ss times the layout size; the destination is in
        // layout space, and the logical-size mapping scales it back up to land
        // on the texture's own pixels one for one.
        //
        // Source rect rather than the whole texture: a pooled slot is bigger
        // than the text in it. For an ordinary cached texture the rect is the
        // whole thing, so this costs nothing there.
        SDL_Rect src { 0, 0, e.w, e.h };
        SDL_Rect dst { x, y, e.w / m_ss, e.h / m_ss };
        SDL_RenderCopy(m_renderer, e.tex, &src, &dst);
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

    // Decoded to RGBA8888 rather than left in the file's own format (RGB24
    // for a JPEG): SDL keeps a format the GPU lacks behind a hidden converted
    // copy, and the GPU card/3D paths sample what SDL_GL_BindTexture hands
    // them - which for those textures came out blank.
    SDL_Texture *Gfx::LoadImage(const char *path) {
        SDL_Surface *raw = IMG_Load(path);
        return raw ? ScaleToTexture(raw, raw->w, raw->h) : nullptr;
    }

    SDL_Texture *Gfx::LoadImageScaled(const char *path, int w, int h) {
        return ScaleToTexture(IMG_Load(path), w, h);
    }

    SDL_Texture *Gfx::LoadImageScaled(const void *data, size_t len, int w, int h) {
        if (!data || !len) return nullptr;
        return ScaleToTexture(IMG_Load_RW(SDL_RWFromConstMem(data, (int)len), 1), w, h);
    }

    SDL_Surface *Gfx::LoadSurfaceScaled(const char *path, int w, int h) {
        return ScaleSurface(IMG_Load(path), w, h);
    }

    // RGBA8888 at w x h. Falls back to the unscaled source when the target
    // surface cannot be allocated - the full-size image still beats nothing.
    SDL_Surface *Gfx::ScaleSurface(SDL_Surface *raw, int w, int h) {
        if (!raw) return nullptr;
        SDL_Surface *src = SDL_ConvertSurfaceFormat(raw, SDL_PIXELFORMAT_RGBA8888, 0);
        SDL_FreeSurface(raw);
        if (!src || (src->w == w && src->h == h)) return src;

        SDL_Surface *dst = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32,
                                                          SDL_PIXELFORMAT_RGBA8888);
        if (!dst) return src;
        SDL_BlitScaled(src, nullptr, dst, nullptr);
        SDL_FreeSurface(src);
        return dst;
    }

    SDL_Texture *Gfx::ScaleToTexture(SDL_Surface *raw, int w, int h) {
        SDL_Surface *surf = ScaleSurface(raw, w, h);
        if (!surf) return nullptr;
        SDL_Texture *tex = SDL_CreateTextureFromSurface(m_renderer, surf);
        SDL_FreeSurface(surf);
        return tex;
    }

    SDL_Texture *Gfx::LoadImageCropped(const char *path, int w, int h, float biasY) {
        SDL_Surface *raw = IMG_Load(path);
        if (!raw) return nullptr;

        SDL_Surface *src = SDL_ConvertSurfaceFormat(raw, SDL_PIXELFORMAT_RGBA8888, 0);
        SDL_FreeSurface(raw);
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

        SDL_Surface *dst = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32,
                                                          SDL_PIXELFORMAT_RGBA8888);
        if (!dst) {   // out of memory: the uncropped source still beats nothing
            SDL_Texture *tex = SDL_CreateTextureFromSurface(m_renderer, src);
            SDL_FreeSurface(src);
            return tex;
        }

        SDL_BlitScaled(src, &crop, dst, nullptr);
        SDL_FreeSurface(src);

        SDL_Texture *tex = SDL_CreateTextureFromSurface(m_renderer, dst);
        SDL_FreeSurface(dst);
        return tex;
    }

    // Scanline-filled triangle. SDL2 has no filled-primitive call before
    // SDL_RenderGeometry, so the span between the two active edges is drawn as
    // a 1px rect per row. Only used for small shapes (the XMB selection wedge),
    // where a few dozen rows costs nothing.
    // =========================================================================
    // Raw GL, alongside SDL's renderer
    //
    // SDL owns the GL context; SDL_RenderFlush is the documented way to hand it
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

        struct Saved { GLint_ prog, buf, attr0, bs, bd, bsa, bda, unit; bool blend; } g_saved;

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
        SDL_RenderFlush(m_renderer);
        GLint_ prev_buf = 0;
        g_gl.GetIntegerv(GL_ARRAY_BUFFER_BINDING_, &prev_buf);
        g_gl.BindBuffer(GL_ARRAY_BUFFER_, vbo);
        g_gl.BufferData(GL_ARRAY_BUFFER_, (long)(v.size() * sizeof(float)), v.data(), GL_STATIC_DRAW_);
        g_gl.BindBuffer(GL_ARRAY_BUFFER_, (GLuint_)prev_buf);

        m_fx_vbo    = vbo;
        m_fx_tex_ok = FxTextureSelfTest();
        {
            SDL_RendererInfo info{};
            m_gles = SDL_GetRendererInfo(m_renderer, &info) == 0 && info.name &&
                     strcmp(info.name, "opengles2") == 0;
        }
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
        SDL_RenderFlush(m_renderer);

        Saved &s = g_saved;
        g_gl.GetIntegerv(GL_CURRENT_PROGRAM_, &s.prog);
        g_gl.GetIntegerv(GL_ARRAY_BUFFER_BINDING_, &s.buf);
        g_gl.GetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_ENABLED_, &s.attr0);
        g_gl.GetIntegerv(GL_BLEND_SRC_RGB_, &s.bs);
        g_gl.GetIntegerv(GL_BLEND_DST_RGB_, &s.bd);
        g_gl.GetIntegerv(GL_BLEND_SRC_ALPHA_, &s.bsa);
        g_gl.GetIntegerv(GL_BLEND_DST_ALPHA_, &s.bda);
        s.blend = g_gl.IsEnabled(GL_BLEND_) != 0;
        // Samplers read unit 0, and SDL_GL_BindTexture binds to whichever unit
        // is active - which SDL does not promise is 0.
        g_gl.GetIntegerv(GL_ACTIVE_TEXTURE_, &s.unit);
        g_gl.ActiveTexture(GL_TEXTURE0_);

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

    // Bound through SDL, which keeps its own "what is bound" cache right; the
    // scale is non-1 only where SDL padded the texture to a power of two.
    // Whether SDL_GL_BindTexture really binds. sdl2-compat (SDL2 on SDL3, what
    // desktop Linux now ships as "SDL2") returns success and binds nothing,
    // which would draw every textured GPU quad in one flat colour - so it is
    // checked once, with two textures, and the textured paths stand down.
    bool Gfx::FxTexturesWork() { return m_fx_tex_ok; }
    // Run once from ShaderFxInit, which always happens before any pass opens.
    bool Gfx::FxTextureSelfTest() {
        SDL_Texture *other = SDL_CreateTexture(m_renderer, SDL_PIXELFORMAT_ARGB8888,
                                               SDL_TEXTUREACCESS_STATIC, 1, 1);
        SDL_Texture *white = WhiteTexture();
        if (!other || !white) { if (other) SDL_DestroyTexture(other); return false; }
        GLint_ a = 0, b = 0;
        SDL_GL_BindTexture(white, nullptr, nullptr);
        g_gl.GetIntegerv(GL_TEXTURE_BINDING_2D_, &a);
        SDL_GL_UnbindTexture(white);
        SDL_GL_BindTexture(other, nullptr, nullptr);
        g_gl.GetIntegerv(GL_TEXTURE_BINDING_2D_, &b);
        SDL_GL_UnbindTexture(other);
        SDL_DestroyTexture(other);
        return a != 0 && b != 0 && a != b;
    }

    void Gfx::FxTexture(SDL_Texture *tex) {
        if (g_cur < 0) return;
        if (!tex) tex = WhiteTexture();
        static float sw = 1.0f, sh = 1.0f;       // of whatever is bound now
        if (tex != m_fx_tex) {
            if (SDL_GL_BindTexture(tex, &sw, &sh) != 0) return;
            m_fx_tex = tex;
        }
        FxSet("uTexScale", sw, sh, 0.0f, 0.0f);  // per program, so always
        // How the texture's bytes sit in GL. SDL's GLES2 renderer stores every
        // format but ABGR8888/BGR888 with red and blue swapped (a format it
        // does not support becomes ARGB8888 or RGB888 behind the scenes) and
        // fixes that in its own shaders - which ours are not. Formats without
        // alpha leave junk in that channel on either renderer.
        Uint32 fmt = 0;
        SDL_QueryTexture(tex, &fmt, nullptr, nullptr, nullptr);
        const bool swap = m_gles && fmt != SDL_PIXELFORMAT_ABGR8888 &&
                          fmt != SDL_PIXELFORMAT_BGR888;
        FxSet("uSwz", swap ? 1.0f : 0.0f, SDL_ISPIXELFORMAT_ALPHA(fmt) ? 0.0f : 1.0f, 0.0f, 0.0f);
    }

    void Gfx::FxClose() { FxEnd(); }

    void Gfx::FxEnd() {
        if (g_cur < 0) return;
        if (m_fx_tex) { SDL_GL_UnbindTexture(m_fx_tex); m_fx_tex = nullptr; }
        const Saved &s = g_saved;
        g_gl.ActiveTexture((GLenum_)s.unit);
        if (!s.attr0) g_gl.DisableVertexAttribArray(0);
        g_gl.BindBuffer(GL_ARRAY_BUFFER_, (GLuint_)s.buf);
        g_gl.UseProgram((GLuint_)s.prog);
        g_gl.BlendFuncSeparate((GLenum_)s.bs, (GLenum_)s.bd, (GLenum_)s.bsa, (GLenum_)s.bda);
        if (!s.blend) g_gl.Disable(GL_BLEND_);
        g_cur = -1;
    }

    // ---- GPU wallpaper blur ---------------------------------------------------
    // Halve the image `levels` times, then double it back up again to half
    // size, every step a bilinear RenderCopy. Each halving is an exact 2x2 box
    // and each doubling a tent, so the chain lands close to a Gaussian whose
    // width doubles per level - and all of it is texture sampling on the GPU.
    SDL_Texture *Gfx::Blurred(SDL_Texture *src, int radius) {
        FxClose();   // an open GPU pass must end before SDL draws again
        int w = 0, h = 0;
        if (!src || SDL_QueryTexture(src, nullptr, nullptr, &w, &h) != 0) return nullptr;
        int levels = 1;
        while ((2 << levels) <= radius && levels < 6) levels++;

        SDL_Texture *prev_target = SDL_GetRenderTarget(m_renderer);
        int lw = 0, lh = 0;
        SDL_RenderGetLogicalSize(m_renderer, &lw, &lh);
        if (lw || lh) SDL_RenderSetLogicalSize(m_renderer, 0, 0);
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
            SDL_RenderCopy(m_renderer, chain.back(), nullptr, nullptr);
            chain.push_back(t);
        }
        for (int i = (int)chain.size() - 2; i >= 1; i--) {
            SDL_SetRenderTarget(m_renderer, chain[i]);
            SDL_RenderCopy(m_renderer, chain[i + 1], nullptr, nullptr);
        }

        SDL_SetRenderTarget(m_renderer, prev_target);
        if (lw || lh) SDL_RenderSetLogicalSize(m_renderer, lw, lh);
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
        // The Switch portlib's SDL_RenderFillRects uses a NEON fast path for
        // n >= 8 that requires 16-byte aligned rects and a count multiple of 4.
        // Ensure the call is safe by copying to an aligned buffer when needed.
        const uintptr_t addr = reinterpret_cast<uintptr_t>(r);
        const bool aligned = (addr % 16 == 0);
        const bool mult4   = (n % 4 == 0);
        if (n >= 8 && (!aligned || !mult4)) {
            // Small static buffer for the common case. Max expected batch is
            // <1024 rects (LineAA ~720, grid ~322), so stack is sufficient.
            constexpr int kStackMax = 1024;
            if (n <= kStackMax) {
                alignas(16) SDL_Rect stackBuf[kStackMax];
                SDL_Rect *tmp = stackBuf;
                memcpy(tmp, r, n * sizeof(SDL_Rect));
                if (!mult4) {
                    const int pad = 4 - (n % 4);
                    for (int i = 0; i < pad; ++i) tmp[n + i] = tmp[n - 1];
                    n += pad;
                }
                SDL_RenderFillRects(m_renderer, tmp, n);
                return;
            }
            // Fallback for very large batches: just call original.
        }
        SDL_RenderFillRects(m_renderer, r, n);
    }

    void Gfx::FillRectAdd(int x, int y, int w, int h, SDL_Color c) {
        FxClose();   // an open GPU pass must end before SDL draws again
        if (w <= 0 || h <= 0) return;
        SDL_SetRenderDrawBlendMode(m_renderer, SDL_BLENDMODE_ADD);
        SDL_SetRenderDrawColor(m_renderer, c.r, c.g, c.b, c.a);
        SDL_Rect r { x, y, w, h };
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
        SDL_Rect dst { x, y, w, h };
        SDL_SetTextureAlphaMod(tex, alpha);
        SDL_RenderCopy(m_renderer, tex, nullptr, &dst);
        SDL_SetTextureAlphaMod(tex, 255); // don't leak the mod to other blits
    }

    void Gfx::DrawImageTinted(SDL_Texture *tex, int x, int y, int w, int h,
                              SDL_Color c, Uint8 alpha) {
        FxClose();
        if (!tex || w <= 0 || h <= 0) return;
        SDL_Rect dst { x, y, w, h };
        SDL_SetTextureColorMod(tex, c.r, c.g, c.b);
        // c's own alpha (as Text() uses it) times the caller's fade multiplier,
        // the same combination IconPlate already does for a theme colour and a
        // row's fade alpha - so DrawImageTinted(tex, ..., t.dim) alone behaves
        // exactly like Text(..., t.dim, ...), and a caller fading a whole panel
        // down can still pass its own alpha on top without fighting t.dim's.
        SDL_SetTextureAlphaMod(tex, (Uint8)((int)c.a * alpha / 255));
        SDL_RenderCopy(m_renderer, tex, nullptr, &dst);
        SDL_SetTextureColorMod(tex, 255, 255, 255);
        SDL_SetTextureAlphaMod(tex, 255); // don't leak either mod to other blits
    }

    void Gfx::DrawCover(SDL_Texture *tex, Uint8 alpha) {
        FxClose();   // an open GPU pass must end before SDL draws again
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
