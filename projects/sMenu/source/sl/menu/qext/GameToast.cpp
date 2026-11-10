#include <sl/menu/qext/GameToast.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>

// The drawing half is plain SDL (surfaces, SDL_ttf) so the host test can run it;
// the vi layer and the thread are below, Switch only.
namespace sl::menu::qext::toast {

    namespace {
        constexpr float W = 1280.0f, H = 720.0f;

        float Clamp01(float v) { return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; }
        float Ease(float v) { v = Clamp01(v); return 1.0f - (1.0f - v) * (1.0f - v) * (1.0f - v); }

        // `a` (0..1) of r,g,b over the pixel, straight alpha both sides.
        inline void Over(Uint8 *p, float r, float g, float b, float a) {
            if (a <= 0.0f) return;
            if (a > 1.0f) a = 1.0f;
            const float keep = (p[3] / 255.0f) * (1.0f - a), oa = a + keep;
            p[0] = (Uint8)((r * a + p[0] * keep) / oa + 0.5f);
            p[1] = (Uint8)((g * a + p[1] * keep) / oa + 0.5f);
            p[2] = (Uint8)((b * a + p[2] * keep) / oa + 0.5f);
            p[3] = (Uint8)(oa * 255.0f + 0.5f);
        }

        float BX(const Canvas &cv, float x) { return (x - cv.region.x) * cv.scale; }
        float BY(const Canvas &cv, float y) { return (y - cv.region.y) * cv.scale; }

        // Signed distance from a point to a rounded box (centre, half sizes, radius).
        float SdRound(float px, float py, float cx, float cy, float hw, float hh, float r) {
            const float qx = std::fabs(px - cx) - (hw - r), qy = std::fabs(py - cy) - (hh - r);
            const float ox = std::max(qx, 0.0f), oy = std::max(qy, 0.0f);
            return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f) - r;
        }

        // A rounded box in design units, anti-aliased. `spread` > 0 makes it a
        // soft shadow of that many units instead (black, `alpha` at the edge).
        void Box(Canvas &cv, float x, float y, float w, float h, float r, SDL_Color c, float alpha,
                 float spread = 0.0f) {
            const float s = cv.scale, bw = w * s, bh = h * s;
            const float br = std::min(r * s, std::min(bw, bh) / 2.0f), sp = spread * s;
            const float bx = BX(cv, x), by = BY(cv, y) + sp * 0.25f * (spread > 0.0f);
            const float cx = bx + bw / 2.0f, cy = by + bh / 2.0f;
            const int x0 = std::max(0, (int)std::floor(bx - sp) - 1), x1 = std::min(cv.w, (int)std::ceil(bx + bw + sp) + 1);
            const int y0 = std::max(0, (int)std::floor(by - sp) - 1), y1 = std::min(cv.h, (int)std::ceil(by + bh + sp) + 1);
            const float a0 = c.a / 255.0f * alpha;
            for (int yy = y0; yy < y1; yy++) {
                Uint8 *row = cv.px + yy * cv.pitch;
                for (int xx = x0; xx < x1; xx++) {
                    const float d = SdRound(xx + 0.5f, yy + 0.5f, cx, cy, bw / 2.0f, bh / 2.0f, br);
                    float cov;
                    if (sp > 0.0f) { cov = Clamp01(1.0f - d / sp); cov *= cov; }
                    else cov = Clamp01(0.5f - d);
                    if (cov > 0.0f) Over(row + xx * 4, c.r, c.g, c.b, a0 * cov);
                }
            }
        }

        // The characters some face can draw; the rest (most emoji) would be boxes.
        std::string Drawable(const Fonts &f, const std::string &s) {
            std::string out;
            for (size_t i = 0; i < s.size();) {
                const unsigned char c = (unsigned char)s[i];
                int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
                if (!n || i + n > s.size()) { i++; continue; }
                Uint32 cp = n == 1 ? c : c & (0x7F >> n);
                for (int k = 1; k < n; k++) cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
                const bool space = cp == ' ' || cp == '\n' || cp == '\r' || cp == '\t';
                if (space) { if (!out.empty() && out.back() != ' ') out += ' '; }   // one, where emoji went too
                else if (cp >= 0x20 && cp != 0x7F) {
                    bool ok = cp < 0x80;
                    for (int k = 0; !ok && k < f.count; k++) ok = TTF_FontHasGlyph(f.face[k], cp);
                    if (ok) out.append(s, i, n);
                }
                i += n;
            }
            while (!out.empty() && out.back() == ' ') out.pop_back();
            return out;
        }

        // Cut to fit `maxw` pixels, with "..." when anything went.
        std::string Fit(TTF_Font *f, const std::string &s, int maxw) {
            int mw = 0;
            size_t len = 0;
            if (maxw <= 0 || !TTF_MeasureString(f, s.c_str(), 0, maxw, &mw, &len) || len >= s.size()) return s;
            int dw = 0;
            size_t dl = 0;
            TTF_MeasureString(f, "...", 0, 0, &dw, &dl);
            TTF_MeasureString(f, s.c_str(), 0, std::max(0, maxw - dw), &mw, &len);
            while (len > 0 && ((unsigned char)s[len] & 0xC0) == 0x80) len--;   // whole characters
            while (len > 0 && s[len - 1] == ' ') len--;
            return s.substr(0, len) + "...";
        }

        // One line at (x, y) top left, design units; `cx` >= 0 centres it on (cx, y) instead.
        void Text(Canvas &cv, const Fonts &f, float x, float y, float maxw, const std::string &raw,
                  SDL_Color c, float alpha, float cx = -1.0f) {
            if (!f.count || alpha <= 0.0f) return;
            const std::string s = Fit(f.face[0], Drawable(f, raw), (int)(maxw * cv.scale));
            if (s.empty()) return;
            SDL_Surface *ts = TTF_RenderText_Blended(f.face[0], s.c_str(), 0, SDL_Color{ 255, 255, 255, 255 });
            if (!ts) return;
            SDL_Surface *g = SDL_ConvertSurface(ts, SDL_PIXELFORMAT_RGBA32);
            SDL_DestroySurface(ts);
            if (!g) return;
            const int ox = cx >= 0.0f ? (int)std::lround(BX(cv, cx) - g->w / 2.0f) : (int)std::lround(BX(cv, x));
            const int oy = cx >= 0.0f ? (int)std::lround(BY(cv, y) - g->h / 2.0f) : (int)std::lround(BY(cv, y));
            const float a0 = c.a / 255.0f * alpha;
            for (int j = 0; j < g->h; j++) {
                const int yy = oy + j;
                if (yy < 0 || yy >= cv.h) continue;
                const Uint8 *src = (const Uint8 *)g->pixels + j * g->pitch;
                Uint8 *row = cv.px + yy * cv.pitch;
                for (int i = 0; i < g->w; i++) {
                    const int xx = ox + i;
                    if (xx < 0 || xx >= cv.w || !src[i * 4 + 3]) continue;
                    Over(row + xx * 4, c.r, c.g, c.b, a0 * src[i * 4 + 3] / 255.0f);
                }
            }
            SDL_DestroySurface(g);
        }

        // The sender's picture in a circle.
        void Avatar(Canvas &cv, SDL_Surface *av, float x, float y, float size, float alpha) {
            const int n = std::max(1, (int)std::lround(size * cv.scale));
            SDL_Surface *sc = SDL_ScaleSurface(av, n, n, SDL_SCALEMODE_LINEAR);
            if (!sc) return;
            SDL_Surface *p = sc->format == SDL_PIXELFORMAT_RGBA32 ? sc : SDL_ConvertSurface(sc, SDL_PIXELFORMAT_RGBA32);
            if (p != sc) SDL_DestroySurface(sc);
            if (!p) return;
            const int ox = (int)std::lround(BX(cv, x)), oy = (int)std::lround(BY(cv, y));
            const float r = n / 2.0f;
            for (int j = 0; j < n; j++) {
                const int yy = oy + j;
                if (yy < 0 || yy >= cv.h) continue;
                const Uint8 *src = (const Uint8 *)p->pixels + j * p->pitch;
                Uint8 *row = cv.px + yy * cv.pitch;
                for (int i = 0; i < n; i++) {
                    const int xx = ox + i;
                    if (xx < 0 || xx >= cv.w) continue;
                    const float d = std::hypot(i + 0.5f - r, j + 0.5f - r) - r;
                    const float cov = Clamp01(0.5f - d);
                    if (cov <= 0.0f) continue;
                    const Uint8 *q = src + i * 4;
                    Over(row + xx * 4, q[0], q[1], q[2], q[3] / 255.0f * cov * alpha);
                }
            }
            SDL_DestroySurface(p);
        }

        // No picture: the first letter of the name on the disc.
        void Initial(Canvas &cv, const Fonts &f, const std::string &title, float cx, float cy, float alpha) {
            std::string first = Drawable(f, title);
            if (first.empty()) return;
            const unsigned char c = (unsigned char)first[0];
            const size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
            first = first.substr(0, n);
            if (n == 1 && first[0] >= 'a' && first[0] <= 'z') first[0] = (char)(first[0] - 32);
            Text(cv, f, 0.0f, cy, 200.0f, first, SDL_Color{ 255, 255, 255, 255 }, alpha, cx);
        }
    }

    SDL_Rect Region(Look::Style style) {
        switch (style) {
            case Look::Achievement: return SDL_Rect{ 324, 546, 632, 140 };   // the pill, centred low
            case Look::Xmb:         return SDL_Rect{ 800, 20, 480, 120 };    // top right, in from the edge
            default:                return SDL_Rect{ 740, 0, 540, 136 };     // top right, down from the top
        }
    }

    // The same three as Menu::DrawDiscordBanner, except that the box and card
    // slide back out rather than fade: whether vi blends this layer as straight
    // or premultiplied alpha is not known, and only a fade would show the difference.
    bool Draw(Canvas &cv, const Look &look, const Fonts &f, const std::string &title,
              const std::string &text, SDL_Surface *av, float e) {
        const float left = kSecs - e;
        const SDL_Color white{ 255, 255, 255, 255 };
        switch (look.style) {
            case Look::Achievement: {
                // A disc pops up, the dark pill unrolls from it, then the words.
                const float in = std::min(e, left);
                const float pop = Ease(in / 0.25f), grow = Ease((in - 0.25f) / 0.35f);
                const float txt = Clamp01((in - 0.55f) / 0.2f);
                const float h = 84.0f, full = 600.0f, pw = h + (full - h) * grow;
                const float px = (W - pw) / 2.0f, py = H - 150.0f;
                if (grow > 0.0f) {
                    Box(cv, px, py, pw, h, h / 2, SDL_Color{ 0, 0, 0, 255 }, 0.45f, 16.0f);
                    Box(cv, px, py, pw, h, h / 2, SDL_Color{ 38, 38, 40, 240 }, 1.0f);
                }
                const float ds = h * (0.6f + 0.4f * pop), dx = px + (h - ds) / 2, dy = py + (h - ds) / 2;
                if (pop > 0.0f) {
                    if (grow <= 0.0f) Box(cv, dx, dy, ds, ds, ds / 2, SDL_Color{ 0, 0, 0, 255 }, 0.4f * pop, 12.0f);
                    Box(cv, dx, dy, ds, ds, ds / 2, look.disc, pop);
                    if (av) Avatar(cv, av, dx + 6, dy + 6, ds - 12, pop);
                    else Initial(cv, f, title, dx + ds / 2, dy + ds / 2, pop);
                }
                if (txt > 0.0f) {
                    const float tx = px + h + 14, tw = pw - h - 40;
                    Text(cv, f, tx, py + 12, tw, title, white, txt);
                    Text(cv, f, tx, py + 46, tw, text, SDL_Color{ 190, 190, 190, 255 }, txt);
                }
                return in < 0.8f;
            }
            case Look::Xmb: {
                // A small smoked box that slides in from the right.
                const float bw = 420, bh = 70;
                const float slide = std::min(Ease(e / 0.3f), Ease(left / 0.3f));
                const float x = W - bw - 30 + (1.0f - slide) * (bw + 40), y = 40;
                Box(cv, x, y, bw, bh, 6, SDL_Color{ 0, 0, 0, 255 }, 0.35f, 10.0f);
                Box(cv, x, y, bw, bh, 6, SDL_Color{ 20, 20, 24, 215 }, 1.0f);
                Box(cv, x + 6, y, bw - 12, 1, 0, SDL_Color{ 255, 255, 255, 60 }, 1.0f);
                float tx = x + 16;
                if (av) { Avatar(cv, av, x + 12, y + 11, 48, 1.0f); tx = x + 72; }
                const float tw = x + bw - tx - 14;
                Text(cv, f, tx, y + 8, tw, title, white, 1.0f);
                Text(cv, f, tx, y + 38, tw, text, SDL_Color{ 180, 180, 180, 255 }, 1.0f);
                return e < 0.35f || left < 0.35f;
            }
            default: {
                // A card in the theme's colours, dropping in from the top.
                const float bw = 480, bh = 82, x = W - bw - 24;
                const float drop = std::min(Ease(e / 0.3f), Ease(left / 0.3f));
                const float y = 20 - (1.0f - drop) * (bh + 40);
                Box(cv, x, y, bw, bh, 14, SDL_Color{ 0, 0, 0, 255 }, 0.35f, 18.0f);
                Box(cv, x, y, bw, bh, 14, look.fill, 1.0f);
                Box(cv, x + 8, y + 14, 5, bh - 28, 2.5f, look.accent, 1.0f);
                float tx = x + 26;
                if (av) { Avatar(cv, av, x + 24, y + 16, 50, 1.0f); tx = x + 88; }
                const float tw = x + bw - tx - 18;
                Text(cv, f, tx, y + 12, tw, title, look.title, 1.0f);
                Text(cv, f, tx, y + 44, tw, text, look.dim, 1.0f);
                return e < 0.35f || left < 0.35f;
            }
        }
    }
}

#ifdef __SWITCH__
#include <switch.h>
#include <SDL3_image/SDL_image.h>
#include <sl/menu/net/Http.hpp>
#include <atomic>
#include <cstdarg>
#include <cstdio>

extern "C" u64 __nx_vi_layer_id;   // libnx's: which layer viCreateLayer opens
extern "C" ViDisplay *qext_vi_display(void);   // the loader's (scripts/qext/loader.patch)

namespace sl::menu::qext::toast {

    namespace {
        struct Job {
            Look look;
            std::function<bool(Item &)> next;
            std::function<void()> sound;
        };
        Thread g_thread;
        bool g_live = false;
        std::atomic<bool> g_stop{ false };
        std::atomic<u64> g_hold{ 0 };   // no pop-ups before this tick: sleep

        void Log(const char *fmt, ...) {
            FILE *fp = fopen("sdmc:/slaunch/boot.log", "a");
            if (!fp) return;
            fputs("toast: ", fp);
            va_list ap;
            va_start(ap, fmt);
            vfprintf(fp, fmt, ap);
            va_end(ap);
            fputc('\n', fp);
            fclose(fp);
        }

        bool Held() { return armGetSystemTick() < g_hold.load(); }

        void Nap(u64 ms) {
            for (u64 t = 0; t < ms && !g_stop; t += 50) svcSleepThread(50'000'000ULL);
        }

        // The loader's: "Default" is already open on this vi session, and a
        // second OpenDisplay of it fails.
        ViDisplay *Display() { return qext_vi_display(); }

        float Scale() {
            s32 lw = 0, lh = 0;
            ViDisplay *d = Display();
            if (!d || R_FAILED(viGetDisplayLogicalResolution(d, &lw, &lh)) || lw < 640 || lw > 7680) lw = 1920;
            return lw / W;
        }

        // IManagerDisplayService AddToLayerStack, which libnx does not wrap.
        Result AddToLayerStack(ViLayer *layer, ViLayerStack stack) {
            const struct { u32 stack; u32 pad; u64 layer_id; } in = { (u32)stack, 0, layer->layer_id };
            return serviceDispatchIn(viGetSession_IManagerDisplayService(), 6000, in);
        }

        void CloseFonts(Fonts &f) {
            if (f.count) TTF_ClearFallbackFonts(f.face[0]);
            for (int i = 0; i < f.count; i++) TTF_CloseFont(f.face[i]);
            f = Fonts{};
        }
        // The system fonts at FontSize::Small, the menu's banner size.
        void OpenFonts(Fonts &f, float scale) {
            static const PlSharedFontType kTypes[] = {
                PlSharedFontType_Standard, PlSharedFontType_ChineseSimplified, PlSharedFontType_ExtChineseSimplified,
                PlSharedFontType_ChineseTraditional, PlSharedFontType_KO, PlSharedFontType_NintendoExt,
            };
            for (PlSharedFontType t : kTypes) {
                PlFontData d{};
                if (R_FAILED(plGetSharedFontByType(&d, t))) continue;
                TTF_Font *ft = TTF_OpenFontIO(SDL_IOFromConstMem(d.address, d.size), true, 20.0f * scale);
                if (!ft) continue;
                TTF_SetFontHinting(ft, TTF_HINTING_LIGHT);
                TTF_SetFontKerning(ft, true);
                if (f.count) TTF_AddFallbackFont(f.face[0], ft);
                f.face[f.count++] = ft;
            }
        }

        // A picture we can decode safely: PNG, WebP or GIF (never a JPEG, see Menu_Discord).
        SDL_Surface *Fetch(const std::string &url) {
            if (url.empty()) return nullptr;
            std::string body;
            if (!net::Get(url.c_str(), body, 3) || body.size() < 12) return nullptr;
            const bool png = !memcmp(body.data(), "\x89PNG", 4), gif = !memcmp(body.data(), "GIF8", 4);
            const bool webp = !memcmp(body.data(), "RIFF", 4) && !memcmp(body.data() + 8, "WEBP", 4);
            if (!png && !gif && !webp) return nullptr;
            SDL_Surface *raw = IMG_Load_IO(SDL_IOFromConstMem(body.data(), body.size()), true);
            if (!raw) return nullptr;
            SDL_Surface *rgba = SDL_ConvertSurface(raw, SDL_PIXELFORMAT_RGBA32);
            SDL_DestroySurface(raw);
            return rgba;
        }

        // One pop-up: a layer of its own over everything, drawn for kSecs, gone.
        void Show(const Look &look, const Fonts &fonts, const Item &it, SDL_Surface *av, float s) {
            ViDisplay *disp = Display();
            if (!disp) { Log("no display"); return; }
            const SDL_Rect R = Region(look.style);
            const int bw = (int)std::ceil(R.w * s), bh = (int)std::ceil(R.h * s);
            u64 id = 0;
            // Owned by our aruid: libnx's OpenLayer below sends it, and vi refuses
            // (0x272) a layer made for another. (Tesla's 0 works only because an
            // overlay has no aruid.)
            const u64 aruid = appletGetAppletResourceUserId();
            Result rc = viCreateManagedLayer(disp, (ViLayerFlags)0, aruid, &id);
            if (R_FAILED(rc)) { Log("CreateManagedLayer aruid=0x%llx 0x%x", (unsigned long long)aruid, rc); return; }
            ViLayer layer{};
            __nx_vi_layer_id = id;           // viCreateLayer opens this one rather than making its own
            rc = viCreateLayer(disp, &layer);
            __nx_vi_layer_id = 0;
            if (R_FAILED(rc)) {
                Log("OpenLayer 0x%x", rc);
                layer.layer_id = id;
                viDestroyManagedLayer(&layer);
                return;
            }
            viSetLayerScalingMode(&layer, ViScalingMode_FitToLayer);
            s32 z = 0;
            if (R_SUCCEEDED(viGetZOrderCountMax(disp, &z)) && z > 0) viSetLayerZ(&layer, z);
            // The screen only: screenshots and recordings of the game stay clean.
            const Result r1 = AddToLayerStack(&layer, ViLayerStack_Default);
            const Result r2 = AddToLayerStack(&layer, ViLayerStack_Lcd);
            viSetLayerSize(&layer, bw, bh);
            viSetLayerPosition(&layer, R.x * s, R.y * s);
            NWindow win{};
            Framebuffer fb{};
            bool have_win = false, have_fb = false;
            rc = nwindowCreateFromLayer(&win, &layer);
            if (R_SUCCEEDED(rc)) {
                have_win = true;
                rc = framebufferCreate(&fb, &win, bw, bh, PIXEL_FORMAT_RGBA_8888, 2);
                if (R_SUCCEEDED(rc)) { have_fb = true; rc = framebufferMakeLinear(&fb); }
            }
            Log("shown z=%d stacks 0x%x 0x%x %dx%d rc=0x%x", z, r1, r2, bw, bh, rc);
            if (R_SUCCEEDED(rc)) {
                const u64 t0 = armGetSystemTick(), freq = armGetSystemTickFreq();
                bool drawn = false;
                while (!g_stop && !Held()) {
                    const float e = (float)(armGetSystemTick() - t0) / (float)freq;
                    if (e >= kSecs) break;
                    // Nothing moves in the middle: the last frame stays up by itself.
                    if (drawn && e > 0.9f && e < kSecs - 0.9f) { svcSleepThread(20'000'000ULL); continue; }
                    u32 stride = 0;
                    Uint8 *p = (Uint8 *)framebufferBegin(&fb, &stride);
                    memset(p, 0, (size_t)stride * bh);
                    Canvas cv;
                    cv.px = p; cv.w = bw; cv.h = bh; cv.pitch = (int)stride; cv.scale = s; cv.region = R;
                    Draw(cv, look, fonts, it.title, it.text, av, e);
                    framebufferEnd(&fb);
                    drawn = true;
                }
            }
            if (have_fb) framebufferClose(&fb);
            if (have_win) nwindowClose(&win);
            viDestroyManagedLayer(&layer);
        }

        void Run(void *arg) {
            Job *job = (Job *)arg;
            Fonts fonts;
            float font_scale = 0.0f;
            while (!g_stop) {
                Item it;
                if (!job->next(it)) { Nap(250); continue; }
                if (Held()) continue;   // came in around sleep: old news by now
                SDL_Surface *av = Fetch(it.avatarUrl);
                if (!g_stop && !Held()) {
                    // Docked and handheld differ: the text is drawn at the size it shows.
                    const float s = Scale();
                    if (s != font_scale) { CloseFonts(fonts); OpenFonts(fonts, s); font_scale = s; }
                    if (job->sound) job->sound();
                    Show(job->look, fonts, it, av, s);
                }
                if (av) SDL_DestroySurface(av);
            }
            CloseFonts(fonts);
            delete job;
        }
    }

    void Start(const Look &look, std::function<bool(Item &)> next, std::function<void()> sound) {
        Stop();
        g_stop = false;
        Job *job = new Job{ look, std::move(next), std::move(sound) };
        // Core 3: a game owns 0-2. Low priority: this only ever draws a few frames.
        const Result rc = threadCreate(&g_thread, Run, job, nullptr, 256 * 1024, 0x3B, 3);
        if (R_SUCCEEDED(rc) && R_SUCCEEDED(threadStart(&g_thread))) { g_live = true; return; }
        if (R_SUCCEEDED(rc)) threadClose(&g_thread);
        Log("thread 0x%x", rc);
        delete job;
    }

    void Stop() {
        if (!g_live) return;
        g_stop = true;
        threadWaitForExit(&g_thread);
        threadClose(&g_thread);
        g_live = false;
    }

    void Hold() { g_hold = armGetSystemTick() + armGetSystemTickFreq() * 10; }
}
#endif
