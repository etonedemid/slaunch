#include <sl/menu/ui/Menu.hpp>
#include <unordered_set>
#include <sl/menu/ui/Locale.hpp>
#include <sl/menu/net/Http.hpp>
#include <sl/menu/net/ContentFilter.hpp>
#include <sl/smi/Protocol.hpp>
#include <SDL2/SDL_image.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <ctime>
#include <algorithm>
#include <dirent.h>
#include <sys/stat.h>
#include "Menu_Internal.hpp"

namespace sl::menu::ui {

    // Load the box wrap once, on first use, and remember a miss so a console
    // without one does not stat the SD every frame.
    void Menu::EnsureFlowWrap() {
        if (m_flow_wrap_tried) return;
        m_flow_wrap_tried = true;
        m_flow_wrap = m_gfx->LoadImage("sdmc:/slaunch/covers/coveroverlay.png");
    }
    // Box front art for an entry, or nullptr for a blank case.
    //
    // This is deliberately *not* the title's square icon. A 1:1 icon stretched
    // onto a 2:3 front is what made the row look like rotated icons rather than
    // boxes, which is the whole thing we are trying to get away from. Only real
    // cover art goes on a box front; everything else stays a blank case until
    // art arrives.
    //
    // covers/<titleid>.jpg is also where a SteamGridDB fetch would land, so
    // wiring that up later needs no change here.
    SDL_Texture *Menu::FlowCover(const MenuItem &it) {
        if (it.kind != ItemKind::Game || it.app_id == 0) return nullptr;
        auto f = m_covers.find(it.app_id);
        if (f != m_covers.end()) return f->second;   // nullptr is cached too
        QueueArt(Art_Cover, it.app_id);              // lands in a frame or two
        return nullptr;
    }
    SDL_Texture *Menu::GameWrap(const MenuItem &it) {
        if (it.kind != ItemKind::Game || it.app_id == 0) return nullptr;
        auto f = m_game_wraps.find(it.app_id);
        if (f != m_game_wraps.end()) return f->second;   // nullptr is cached too
        QueueArt(Art_Wrap, it.app_id);
        return nullptr;
    }

    // ---- background art loader ----------------------------------------------
    // Everything that touches the card or inflates an image runs here: the
    // stat, the texture-cache read, and on a miss the decode and the write
    // back. Only surfaces come out; textures need the renderer, so PollArt
    // uploads them on the main thread.
    void Menu::QueueArt(int kind, u64 id) {
        if (!m_art_pending.insert({kind, id}).second) return;   // already asked
        if (!m_art_started) {
            // Core 1, away from the render thread on core 0: at a lower
            // priority on the same core it only ever ran in the gaps between
            // frames, which is why art trickled in. Core 0 if 1 is refused.
            if (R_FAILED(threadCreate(&m_art_thread, &Menu::ArtTrampoline, this,
                                      nullptr, 0x20000, 0x3B, 1)) &&
                R_FAILED(threadCreate(&m_art_thread, &Menu::ArtTrampoline, this,
                                      nullptr, 0x20000, 0x3B, -2))) {
                m_art_pending.erase({kind, id});
                return;
            }
            threadStart(&m_art_thread);
            m_art_started = true;
        }
        std::lock_guard<std::mutex> lk(m_art_mx);
        m_art_q.push_back(ArtJob{ id, kind, m_art_epoch, nullptr, false });
        // Scrolling through a big library asks for far more than anyone will
        // stop on. The oldest requests are the boxes already scrolled past, so
        // they go; they are asked for again if they come back into view.
        while (m_art_q.size() > 24) {
            m_art_pending.erase({m_art_q.front().kind, m_art_q.front().id});
            m_art_q.pop_front();
        }
        m_art_cv.notify_one();
    }
    void Menu::ArtTrampoline(void *self) {
        Menu *m = static_cast<Menu *>(self);
        for (;;) {
            ArtJob j;
            {
                std::unique_lock<std::mutex> lk(m->m_art_mx);
                m->m_art_cv.wait(lk, [m] { return m->m_art_quit || !m->m_art_q.empty(); });
                if (m->m_art_quit) return;
                j = m->m_art_q.back();              // newest first: what is on screen now
                m->m_art_q.pop_back();
            }
            static const char *const kSuffix[] = { ".jpg", "_wrap.png", "_hero.jpg" };
            static const char *const kKey[]    = { "", "_wrap", "_hero" };
            static const int kW[] = { kCoverTexW, 700, kDeckHeroW };
            static const int kH[] = { kCoverTexH, 540, kDeckRowH };
            char path[96], key[32];
            snprintf(path, sizeof(path), "sdmc:/slaunch/covers/%016llX%s",
                     (unsigned long long)j.id, kSuffix[j.kind]);
            snprintf(key, sizeof(key), "%016llX%s", (unsigned long long)j.id, kKey[j.kind]);
            struct stat src {};
            if (stat(path, &src) == 0) {           // no file: a null result, remembered
                const std::string cpath = TexCachePath(key);
                const int w = kW[j.kind], h = kH[j.kind];
                j.surf = ReadCoverSurf(cpath.c_str(), src, w, h);
                if (!j.surf) {
                    // Hero art is 3.1:1 and its tile 16:9, so it is cropped;
                    // covers and box scans already have their tile's shape.
                    j.surf = j.kind == Art_Hero ? DecodeCoverSurfaceCropped(path, w, h, 0.5f)
                                                : DecodeCoverSurface(path, w, h);
                    if (j.surf) { WriteCoverTex(cpath.c_str(), src, j.surf); j.built = true; }
                }
                // SDL's GLES2 renderer has no RGB565 texture, so uploading one
                // converts it pixel by pixel on the main thread. Converting
                // here leaves the upload a plain copy.
                if (j.surf) {
                    SDL_Surface *conv = SDL_ConvertSurfaceFormat(j.surf, SDL_PIXELFORMAT_ABGR8888, 0);
                    if (conv) { SDL_FreeSurface(j.surf); j.surf = conv; }
                }
            }
            std::lock_guard<std::mutex> lk(m->m_art_mx);
            m->m_art_done.push_back(j);
        }
    }
    void Menu::PollArt() {
        std::vector<ArtJob> done;
        {
            std::lock_guard<std::mutex> lk(m_art_mx);
            if (m_art_done.empty()) return;
            // A few uploads a frame: each is a copy of a few hundred KB into
            // the GPU, cheap alone but not ten at once.
            const size_t n = std::min<size_t>(m_art_done.size(),
                                              (size_t)std::max(m_cover_budget, 0) / 2);
            done.assign(m_art_done.begin(), m_art_done.begin() + n);
            m_art_done.erase(m_art_done.begin(), m_art_done.begin() + n);
        }
        for (ArtJob &j : done) {
            m_art_pending.erase({j.kind, j.id});
            // The art on the card changed while this was in flight (a fetch or
            // the picker landed): drop it, and the next frame asks again.
            if (j.epoch != m_art_epoch) { if (j.surf) SDL_FreeSurface(j.surf); continue; }
            auto &map = j.kind == Art_Cover ? m_covers
                      : j.kind == Art_Wrap  ? m_game_wraps : m_hero_art;
            SDL_Texture *tex = nullptr;
            if (j.surf) {
                tex = SDL_CreateTextureFromSurface(m_gfx->Renderer(), j.surf);
                SDL_FreeSurface(j.surf);
            }
            auto old = map.find(j.id);
            if (old != map.end() && old->second) m_gfx->FreeImage(old->second);
            map[j.id] = tex;
            if (j.kind == Art_Cover) (j.built ? g_cover_miss : g_cover_hits)++;
            if (j.built) { m_cache_msg_tick = armGetSystemTick(); m_cache_built++; }
        }
    }
    void Menu::StopArt() {
        if (!m_art_started) return;
        {
            std::lock_guard<std::mutex> lk(m_art_mx);
            m_art_quit = true;
            m_art_q.clear();
        }
        m_art_cv.notify_one();
        threadWaitForExit(&m_art_thread);
        threadClose(&m_art_thread);
        m_art_started = false;
        for (ArtJob &j : m_art_done)
            if (j.surf) SDL_FreeSurface(j.surf);
        m_art_done.clear();
        m_art_pending.clear();
    }
    bool Menu::TdbFront(u64 app_id) {
        auto f = m_tdb_front.find(app_id);
        if (f != m_tdb_front.end()) return f->second;
        char path[96];
        snprintf(path, sizeof(path), "sdmc:/slaunch/covers/%016llX_tdb", (unsigned long long)app_id);
        struct stat st {};
        return m_tdb_front[app_id] = (stat(path, &st) == 0);
    }

    // Shelf and Deck show box art too: once the selection rests on a game
    // whose cover was looked for and not found, fetch it the way Flow does.
    void Menu::FetchArtFor(const MenuItem &it) {
        if (it.kind != ItemKind::Game || it.app_id == 0) return;
        const auto c = m_covers.find(it.app_id);
        if (c != m_covers.end() && c->second == nullptr) StartCoverFetch(it.app_id, it.name);
    }

    // A small bar above the hints while box art is being fetched: which game,
    // what step, how far a download has got. Once the worker finishes, the
    // outcome stays up for a couple of seconds and fades.
    void Menu::DrawFetchStatus() {
        const auto stage = (FetchStage)m_fetch_stage.load();
        if (stage == FetchStage::Idle) return;
        float a = 1.0f;
        const bool done = stage >= FetchStage::Added;
        if (done) {
            const u64 ms = (armGetSystemTick() - m_fetch_end_tick) * 1000 / armGetSystemTickFreq();
            if (ms > 3000) { m_fetch_stage.store((int)FetchStage::Idle); return; }
            if (ms > 2400) a = 1.0f - (ms - 2400) / 600.0f;
        }
        const Theme &t = m_theme.Current();
        static const char *kText[] = { "", "Looking up", "Updating the game list", "Downloading box art",
                                       "Saving", "SteamGridDB", "Screenshots",
                                       "Box art added", "No box art found", "Could not download" };
        const Uint8 A = (Uint8)(255 * a);
        // In the middle of the top bar, which every main layout leaves free:
        // one line - step, percentage, game - with a thin bar under it.
        const int W = gfx::Gfx::Width, w = 460, x = (W - w) / 2, y = 14;
        std::string line = std::string(T(kText[(int)stage]));
        const u64 now = m_fetch_now.load(), total = m_fetch_total.load();
        if (!done && total > 0) {
            char pct[16];
            snprintf(pct, sizeof(pct), " %d%%", (int)(now * 100 / total));
            line += pct;
        }
        line += "  \xC2\xB7  " + m_fetch_title;
        m_gfx->TextCentered(FontSize::Small, W / 2, y, WithAlpha(t.dim, A),
                            Ellipsize(line, w, FontSize::Small).c_str());
        // Progress: real when a download reports its size, otherwise a
        // sliding block so it still reads as working.
        if (done) return;
        const int bx = x, by = y + m_gfx->LineHeight(FontSize::Small) + 4, bw = w;
        m_gfx->FillRect(bx, by, bw, 2, WithAlpha(t.dim, (Uint8)(60 * a)));
        if (total > 0) {
            m_gfx->FillRect(bx, by, (int)(bw * std::min(1.0, (double)now / total)), 2, WithAlpha(t.accent, A));
        } else {
            const float ph = fmodf((float)armGetSystemTick() / armGetSystemTickFreq() * 0.8f, 1.0f);
            const int seg = bw / 4, sx = bx + (int)((bw + seg) * ph) - seg;
            const int x0 = std::max(bx, sx), x1 = std::min(bx + bw, sx + seg);
            if (x1 > x0) m_gfx->FillRect(x0, by, x1 - x0, 2, WithAlpha(t.accent, A));
        }
    }

    // ---- GameTDB box scans --------------------------------------------------
    //
    // GameTDB keeps scans of the whole printed insert of almost every boxed
    // Switch game - back, spine and front in one image - with no key needed.
    // It knows games by its own five-letter ids, not by title id, so the link
    // is the name, matched strictly (net::TitlesMatch) against its title list.
    // The scan is kept downscaled as covers/<id>_wrap.png, and its front is
    // cropped out as the cover when the game has none yet.
    namespace {
        constexpr const char *kTdbIndex = "sdmc:/slaunch/cache/gametdb_switch.txt";
        // Where a scan's panels sit across its width.
        constexpr float kScanSpine0 = 0.476f, kScanSpine1 = 0.524f;

        std::vector<std::string> TdbFind(const std::string &name) {
            std::vector<std::string> ids;
            FILE *fp = fopen(kTdbIndex, "r");
            if (!fp) return ids;
            char line[512];
            while (fgets(line, sizeof(line), fp) && ids.size() < 4) {
                char *eq = strstr(line, " = ");
                if (!eq || eq - line != 5) continue;           // also skips the header
                *eq = '\0';
                std::string title = eq + 3;
                while (!title.empty() && (title.back() == '\n' || title.back() == '\r'))
                    title.pop_back();
                if (net::TitlesMatch(title, name)) ids.emplace_back(line);
            }
            fclose(fp);
            return ids;
        }

        // Returns true when a wrap was written; `cover_made` when the front
        // was also saved as the game's cover.
        bool FetchGameTdb(u64 app_id, const std::string &name, bool need_cover,
                          int region, bool &cover_made, std::atomic<int> &stage,
                          std::atomic<uint64_t> &dl_now, std::atomic<uint64_t> &dl_total) {
            using Stage = FetchStage;
            cover_made = false;
            struct stat st {};
            const time_t now = time(nullptr);
            if (stat(kTdbIndex, &st) != 0 || (now > st.st_mtime && now - st.st_mtime > 14 * 86400)) {
                mkdir("sdmc:/slaunch/cache", 0777);
                stage.store((int)Stage::Index);
                net::Download("https://www.gametdb.com/switchtdb.txt?LANG=EN", kTdbIndex, 40, &dl_now, &dl_total);
                stage.store((int)Stage::Lookup);
            }
            const std::vector<std::string> ids = TdbFind(name);
            if (ids.empty()) return false;

            std::vector<std::string> regions{ kTdbRegions[std::clamp(region, 0, kTdbRegionCount - 1)] };
            for (const char *r : { "US", "EN", "JA" })
                if (regions[0] != r) regions.emplace_back(r);

            const char *tmp = "sdmc:/slaunch/cache/gametdb_dl.jpg";
            bool got = false;
            for (const auto &id : ids) {
                for (const auto &r : regions) {
                    const std::string url = "https://art.gametdb.com/switch/coverfullHQ/" + r + "/" + id + ".jpg";
                    stage.store((int)Stage::Scan);
                    if (net::Download(url.c_str(), tmp, 40, &dl_now, &dl_total)) { got = true; break; }
                }
                if (got) break;
            }
            if (!got) return false;

            stage.store((int)Stage::Save);
            SDL_Surface *raw = IMG_Load(tmp);
            remove(tmp);
            if (!raw) return false;
            SDL_Surface *src = SDL_ConvertSurfaceFormat(raw, SDL_PIXELFORMAT_RGB24, 0);
            SDL_FreeSurface(raw);
            if (!src || src->w < 100 || src->h < 100) { if (src) SDL_FreeSurface(src); return false; }

            auto scaled = [&](const SDL_Rect *from, int w, int h) -> SDL_Surface * {
                SDL_Surface *d = SDL_CreateRGBSurfaceWithFormat(0, w, h, 24, SDL_PIXELFORMAT_RGB24);
                if (d) SDL_BlitScaled(src, from, d, nullptr);
                return d;
            };
            char path[96];
            bool ok = false;
            const int ww = std::min(src->w, 1400), wh = src->h * ww / src->w;
            if (SDL_Surface *w = scaled(nullptr, ww, wh)) {
                // PNG, never IMG_SaveJPG: libjpeg's compressor errors on the
                // console, and SDL_image's escape from a libjpeg error takes
                // the whole menu down (see ImageLooksWhole in Http.cpp).
                snprintf(path, sizeof(path), "sdmc:/slaunch/covers/%016llX_wrap.png", (unsigned long long)app_id);
                ok = IMG_SavePNG(w, path) == 0;
                SDL_FreeSurface(w);
            }
            if (ok && need_cover) {
                const int fx = (int)(src->w * kScanSpine1);
                const SDL_Rect front{ fx, 0, src->w - fx, src->h };
                if (SDL_Surface *c = scaled(&front, 600, 900)) {
                    // Named .jpg because that is where every layout looks for a
                    // cover; the loader goes by the file's contents, not its name.
                    snprintf(path, sizeof(path), "sdmc:/slaunch/covers/%016llX.jpg", (unsigned long long)app_id);
                    cover_made = IMG_SavePNG(c, path) == 0;
                    SDL_FreeSurface(c);
                    if (cover_made) {
                        snprintf(path, sizeof(path), "sdmc:/slaunch/covers/%016llX_tdb", (unsigned long long)app_id);
                        if (FILE *f = fopen(path, "w")) fclose(f);
                    }
                }
            }
            SDL_FreeSurface(src);
            return ok;
        }
    }

    // ---- SteamGridDB cover fetch --------------------------------------------
    //
    // Switch control data carries a square icon and nothing else, so box art has
    // to come from outside. SteamGridDB's grids are 600x900 - the same 2:3 as a
    // case front - which is why it fits here.
    //
    // Two requests per title: search the name to get a game id, then ask for that
    // game's grids and download the first. Matching is by name because a title id
    // means nothing to them, so this will occasionally pick the wrong edition of
    // something; the file it writes is an ordinary cover in covers/, so a bad
    // match is fixed by replacing that one file.
    //
    // Deliberately one title at a time and only for whatever the cursor rests on:
    // walking a whole library at boot would be hundreds of requests, and this way
    // the covers you actually look at arrive first.
    void Menu::CoverFetchTrampoline(void *self) {
        Menu *m = static_cast<Menu *>(self);
        // The widget worker normally does this, but a cover fetch can start
        // before that has finished; curl_global_init is a no-op once done.
        net::GlobalInit();
        m->m_cover_ok = false;
        m->m_shots_ok = false;
        m->m_hero_ok  = false;

        // Fetch only what is actually absent.
        //
        // The screenshots used to be fetched inside the cover fetch, and the
        // cover fetch only ran when the COVER was missing - so a title that
        // already had its cover never got screenshots at all, however long you
        // sat on it. The two are looked for independently now. Hero art is a
        // third, same treatment.
        bool need_cover, need_shots, need_hero;
        {
            char probe[96];
            struct stat st {};
            snprintf(probe, sizeof(probe), "sdmc:/slaunch/covers/%016llX.jpg",
                     (unsigned long long)m->m_cover_id);
            need_cover = (stat(probe, &st) != 0);
            snprintf(probe, sizeof(probe), "sdmc:/slaunch/covers/%016llX_s0.jpg",
                     (unsigned long long)m->m_cover_id);
            need_shots = (stat(probe, &st) != 0);
            snprintf(probe, sizeof(probe), "sdmc:/slaunch/covers/%016llX_hero.jpg",
                     (unsigned long long)m->m_cover_id);
            need_hero = (stat(probe, &st) != 0);
        }
        m->m_cover_state.store((int)CoverState::Searching, std::memory_order_release);
        CoverState end = CoverState::Failed;

        // GameTDB first: keyless, and a real box scan beats everything else.
        // With one, the back of the case is printed already, so Steam's
        // screenshots are not needed either.
        m->m_wrap_ok = false;
        {
            char probe[96];
            struct stat st {};
            snprintf(probe, sizeof(probe), "sdmc:/slaunch/covers/%016llX_wrap.png",
                     (unsigned long long)m->m_cover_id);
            bool have_wrap = (stat(probe, &st) == 0);
            if (!have_wrap && !net::ContentFilter::ShouldFilterGameByName(m->m_cover_name)) {
                bool cover_made = false;
                have_wrap = FetchGameTdb(m->m_cover_id, m->m_cover_name, need_cover,
                                         m->m_tdb_region, cover_made, m->m_fetch_stage,
                                         m->m_fetch_now, m->m_fetch_total);
                m->m_wrap_ok = have_wrap;
                if (cover_made) { m->m_cover_ok = true; need_cover = false; end = CoverState::Got; }
            }
            if (have_wrap) need_shots = false;
        }
        // SteamGridDB only when the user turned it on and gave it a key;
        // otherwise only the keyless Steam screenshots remain to look for.
        if (!m->m_sgdb_enabled || m->m_sgdb_key.empty()) { need_cover = false; need_hero = false; }
        if (need_cover || need_hero) m->m_fetch_stage.store((int)FetchStage::Sgdb);

        do {
            const std::string auth = "Bearer " + m->m_sgdb_key;

            // URL-encode the title for the search path.
            std::string q;
            for (unsigned char c : m->m_cover_name) {
                if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') q += (char)c;
                else if (c == ' ') q += "%20";
                else {
                    char b[4]; snprintf(b, sizeof(b), "%%%02X", c); q += b;
                }
            }
            if (q.empty()) break;

            // Log every request with its codes. "Fetch failed" on its own
            // cannot distinguish a rejected key from an unreachable host, and
            // this is the only way to see which it is on a console.
            auto logline = [&](const char *what, long http, int rc, size_t len) {
                if (FILE *lf = fopen("sdmc:/slaunch/covers.log", "a")) {
                    fprintf(lf, "%s http=%ld curl=%d bytes=%u name=%s\n",
                            what, http, rc, (unsigned)len, m->m_cover_name.c_str());
                    fclose(lf);
                }
            };

            // Check if this game should be filtered due to adult content
            if (net::ContentFilter::ShouldFilterGameByName(m->m_cover_name)) {
                end = CoverState::Filtered;
                logline("filtered", 0, 0, m->m_cover_name.size());
                break;
            }

            std::string body;
            long http = 0; int rc = 0;
            char dst[96];

            if (!need_cover && !need_hero) {
                end = CoverState::Got;      // already on the card
                goto shots;
            }

            {
            std::string url = "https://www.steamgriddb.com/api/v2/search/autocomplete/" + q;
            const bool okq = net::Get(url.c_str(), body, 12, auth.c_str(), &http, &rc);
            logline("search", http, rc, body.size());
            if (!okq) {
                // 401/403 is the key being refused, which is worth saying out
                // loud rather than reporting as a generic failure.
                end = (http == 401 || http == 403) ? CoverState::BadKey
                                                   : CoverState::Failed;
                break;
            }

            // First "id" inside the data array is the best-ranked match.
            end = CoverState::NoMatch;
            const size_t d = body.find("\"data\"");
            if (d == std::string::npos) break;
            const size_t idp = body.find("\"id\"", d);
            if (idp == std::string::npos) break;
            const size_t colon = body.find(':', idp);
            if (colon == std::string::npos) break;
            const long long gid = strtoll(body.c_str() + colon + 1, nullptr, 10);
            if (gid <= 0) break;

            if (need_cover) {
                char grid[160];
                snprintf(grid, sizeof(grid),
                         "https://www.steamgriddb.com/api/v2/grids/game/%lld"
                         "?dimensions=600x900&types=static&limit=1", gid);
                body.clear();
                end = CoverState::NoArt;
                http = 0; rc = 0;
                const bool okg = net::Get(grid, body, 12, auth.c_str(), &http, &rc);
                logline("grids", http, rc, body.size());
                if (!okg) break;

                const std::string img = JsonStr(body, "url");
                if (img.empty()) break;

                // Download beside the covers the user may have added by hand; the
                // extension stays .jpg because that is what FlowCover looks for and
                // SDL_image sniffs the actual format anyway.
                mkdir("sdmc:/slaunch/covers", 0777);
                snprintf(dst, sizeof(dst), "sdmc:/slaunch/covers/%016llX.jpg",
                         (unsigned long long)m->m_cover_id);
                m->m_cover_ok = net::Download(img.c_str(), dst, 25);
                logline(m->m_cover_ok ? "download-ok" : "download-fail", 0, 0, img.size());
                end = m->m_cover_ok ? CoverState::Got : CoverState::Failed;
                if (!m->m_cover_ok) break;
            } else {
                end = CoverState::Got;      // cover already on the card
            }

            // Hero art for Deck's wide tile - wide key art, deliberately not
            // gated on the cover fetch above: a title that already has its
            // cover but not yet a hero still needs this to run, and a hero
            // miss must never turn a cover success into a reported failure,
            // hence its own m_hero_ok rather than folding into `end`.
            if (need_hero) {
                char hero[160];
                snprintf(hero, sizeof(hero),
                         "https://www.steamgriddb.com/api/v2/heroes/game/%lld"
                         "?dimensions=1920x620&types=static&limit=1", gid);
                body.clear();
                http = 0; rc = 0;
                const bool okh = net::Get(hero, body, 12, auth.c_str(), &http, &rc);
                logline("heroes", http, rc, body.size());
                if (okh) {
                    const std::string himg = JsonStr(body, "url");
                    if (!himg.empty()) {
                        mkdir("sdmc:/slaunch/covers", 0777);
                        snprintf(dst, sizeof(dst), "sdmc:/slaunch/covers/%016llX_hero.jpg",
                                 (unsigned long long)m->m_cover_id);
                        m->m_hero_ok = net::Download(himg.c_str(), dst, 25);
                        logline(m->m_hero_ok ? "hero-download-ok" : "hero-download-fail",
                                0, 0, himg.size());
                    }
                }
            }
            }

        shots:
            if (!need_shots) break;
            m->m_fetch_stage.store((int)FetchStage::Shots);

            // The two panels on the back of the case.
            //
            // These come from Steam rather than SteamGridDB, which has no
            // screenshots at all - grids, heroes, logos and icons is its whole
            // catalogue, and its "heroes" are wide key art rather than anything
            // from the game. Steam is also keyless, and serves JPEG where a
            // hero was PNG, so the panels are far cheaper to decode.
            int shots = 0;
            if (!m->m_steam_dead) {
                // Steam, in two keyless steps: search the name for an appid,
                // then ask the store for that app's screenshots. No key, no
                // sign-up, and the shots are actual gameplay rather than the
                // wide key art SteamGridDB calls a hero.
                char url_s[288];
                snprintf(url_s, sizeof(url_s),
                         "https://steamcommunity.com/actions/SearchApps/%s", q.c_str());
                body.clear();
                http = 0; rc = 0;
                // Shorter than the SteamGridDB calls: there is a fallback
                // waiting behind this, so waiting a long time to find out it is
                // not coming only delays the panels that would have worked.
                const bool oka = net::Get(url_s, body, 8, nullptr, &http, &rc);
                logline("steam-search", http, rc, body.size());
                if (!oka && (http == 0 || http >= 500)) m->m_steam_dead = true;

                // Only a result that is really this game: Steam's search always
                // returns something, and the first hit for a title that is not
                // on Steam is some other game (see net::SteamAppFor).
                const std::string appid = oka ? net::SteamAppFor(body, m->m_cover_name) : std::string();

                if (!appid.empty()) {
                    snprintf(url_s, sizeof(url_s),
                             "https://store.steampowered.com/api/appdetails"
                             "?appids=%s&filters=screenshots", appid.c_str());
                    body.clear();
                    http = 0; rc = 0;
                    const bool okd = net::Get(url_s, body, 8, nullptr, &http, &rc);
                    logline("steam-shots", http, rc, body.size());
                    if (!okd && (http == 0 || http >= 500)) m->m_steam_dead = true;

                    if (okd) {
                        // Check if Steam store page indicates adult content
                        if (net::ContentFilter::ShouldFilterBySteamTags(body)) {
                            logline("steam-filtered", 0, 0, body.size());
                            shots = 0;  // Don't download filtered screenshots
                        } else {
                            // Each screenshot carries a thumbnail and a full-size
                            // image; the panels are drawn a few hundred pixels wide,
                            // so the full one is what is wanted.
                            const std::vector<std::string> imgs =
                                JsonStrAll(body, "path_full", 2);
                            for (size_t k = 0; k < imgs.size(); k++) {
                                snprintf(dst, sizeof(dst),
                                         "sdmc:/slaunch/covers/%016llX_s%u.jpg",
                                         (unsigned long long)m->m_cover_id, (unsigned)k);
                                const bool sok = net::Download(imgs[k].c_str(), dst, 25);
                                logline(sok ? "shot-ok" : "shot-fail", 0, 0, imgs[k].size());
                                if (sok) { shots++; m->m_shots_ok = true; }
                            }
                        }
                    }
                }
            }

            // No hero fallback. SteamGridDB's heroes are wide key art, not
            // anything from the game, and printing marketing art where a case
            // prints screenshots was only ever a stand-in for having none. A
            // title Steam has never heard of now gets a plain back rather than
            // a misleading one.
        } while (false);

        m->m_cover_state.store((int)end, std::memory_order_release);
        // The outcome, for the status bar: something new landed, nothing was
        // found, or the network let us down.
        m->m_fetch_stage.store((int)(m->m_cover_ok || m->m_wrap_ok || m->m_shots_ok || m->m_hero_ok
                                     ? FetchStage::Added
                                     : (end == CoverState::Failed ? FetchStage::Failed : FetchStage::NotFound)));
        m->m_cover_done.store(true, std::memory_order_release);
    }
    void Menu::StartCoverFetch(u64 app_id, const std::string &name) {
        if (m_cover_running || app_id == 0 || name.empty()) return;

        // No key is no longer the end of it: GameTDB and Steam need none.
        SgdbKeyPresent();                  // loads the key, if there is one
        if (m_cover_tried.count(app_id))  return;

        m_cover_tried[app_id] = true;
        m_cover_id   = app_id;
        m_cover_name = name;
        m_fetch_title = name;
        m_fetch_now.store(0); m_fetch_total.store(0);
        m_fetch_stage.store((int)FetchStage::Lookup);
        m_cover_done.store(false, std::memory_order_release);
        if (R_SUCCEEDED(threadCreate(&m_cover_thread, &Menu::CoverFetchTrampoline,
                                     this, nullptr, 0x20000, 0x3B, -2))) {
            threadStart(&m_cover_thread);
            m_cover_running = true;
        }
    }
    void Menu::PollCoverFetch() {
        if (!m_cover_running || !m_cover_done.load(std::memory_order_acquire)) return;
        threadWaitForExit(&m_cover_thread);
        threadClose(&m_cover_thread);
        m_cover_running = false;
        m_fetch_end_tick = armGetSystemTick();   // the outcome shows for a moment

        // Screenshots arrived: drop the recorded "none" so they decode.
        if (m_shots_ok) {
            auto g = m_shots.find(m_cover_id);
            if (g != m_shots.end()) {
                if (g->second.a) m_gfx->FreeImage(g->second.a);
                if (g->second.b) m_gfx->FreeImage(g->second.b);
                m_shots.erase(g);
            }
        }

        if (m_wrap_ok || m_hero_ok || m_cover_ok) m_art_epoch++;
        // A box scan arrived: drop what was cached for this game so the wrap
        // (and a cover cut from it) are picked up.
        if (m_wrap_ok) {
            auto w = m_game_wraps.find(m_cover_id);
            if (w != m_game_wraps.end()) {
                if (w->second) m_gfx->FreeImage(w->second);
                m_game_wraps.erase(w);
            }
            m_tdb_front.erase(m_cover_id);
        }

        // Hero art arrived: same, for HeroArt's own cache.
        if (m_hero_ok) {
            auto h = m_hero_art.find(m_cover_id);
            if (h != m_hero_art.end()) {
                if (h->second) m_gfx->FreeImage(h->second);
                m_hero_art.erase(h);
            }
        }

        // Drop the cached miss so FlowCover picks the new file up next frame.
        if (m_cover_ok) {
            m_cover_ok_count++;
            auto f = m_covers.find(m_cover_id);
            if (f != m_covers.end()) {
                if (f->second) m_gfx->FreeImage(f->second);
                m_covers.erase(f);
            }
            auto g = m_shots.find(m_cover_id);
            if (g != m_shots.end()) {
                if (g->second.a) m_gfx->FreeImage(g->second.a);
                if (g->second.b) m_gfx->FreeImage(g->second.b);
                m_shots.erase(g);
            }
        }
    }
    // Decoded on a worker; see the note on m_shot_thread. Returns nothing until
    // the panels are ready, so the back of a case is briefly blank rather than
    // the whole menu stopping to inflate two 1920x620 images.
    const Menu::FlowShots &Menu::FlowBackShots(const MenuItem &it) {
        static const FlowShots none;
        if (it.kind != ItemKind::Game || it.app_id == 0) return none;

        auto f = m_shots.find(it.app_id);
        if (f != m_shots.end()) return f->second;

        // One at a time: two of these already saturate a core, and only the box
        // you have stopped on ever asks.
        if (!m_shot_running) StartShotDecode(it.app_id);
        return none;
    }
    void Menu::StartShotDecode(u64 app_id) {
        snprintf(m_shot_path_a, sizeof(m_shot_path_a),
                 "sdmc:/slaunch/covers/%016llX_s0.jpg", (unsigned long long)app_id);
        snprintf(m_shot_path_b, sizeof(m_shot_path_b),
                 "sdmc:/slaunch/covers/%016llX_s1.jpg", (unsigned long long)app_id);
        m_shot_job_id = app_id;
        m_shot_surf_a = nullptr;
        m_shot_surf_b = nullptr;
        m_shot_done.store(false, std::memory_order_release);
        if (R_SUCCEEDED(threadCreate(&m_shot_thread, &Menu::ShotDecodeTrampoline,
                                     this, nullptr, 0x20000, 0x3B, -2))) {
            threadStart(&m_shot_thread);
            m_shot_running = true;
        } else {
            // No worker available: record the miss so it is not retried every
            // frame for the rest of the session.
            m_shots[app_id] = FlowShots{};
            m_shot_job_id = 0;
        }
    }
    // Surfaces only - IMG_Load, a format convert and a scaled blit are all CPU
    // work on ordinary memory. Nothing here touches the renderer, which is what
    // makes it safe off the main thread.
    void Menu::ShotDecodeTrampoline(void *self) {
        Menu *m = static_cast<Menu *>(self);
        m->m_shot_surf_a = DecodeCoverSurface(m->m_shot_path_a, kShotTexW, kShotTexH);
        m->m_shot_surf_b = DecodeCoverSurface(m->m_shot_path_b, kShotTexW, kShotTexH);
        m->m_shot_done.store(true, std::memory_order_release);
    }
    void Menu::PollShotDecode() {
        if (!m_shot_running || !m_shot_done.load(std::memory_order_acquire)) return;
        threadWaitForExit(&m_shot_thread);
        threadClose(&m_shot_thread);
        m_shot_running = false;

        FlowShots s;
        if (m_shot_surf_a) {
            s.a = SDL_CreateTextureFromSurface(m_gfx->Renderer(), m_shot_surf_a);
            SDL_FreeSurface(m_shot_surf_a);
            m_shot_surf_a = nullptr;
        }
        if (m_shot_surf_b) {
            s.b = SDL_CreateTextureFromSurface(m_gfx->Renderer(), m_shot_surf_b);
            SDL_FreeSurface(m_shot_surf_b);
            m_shot_surf_b = nullptr;
        }
        // Recorded even when both are null, so a title with no hero art is not
        // asked for again every frame.
        if (m_shot_job_id != 0) m_shots[m_shot_job_id] = s;
        m_shot_job_id = 0;
    }
    // Flow shows launchable content only. Everything else - Settings, Album,
    // Power and friends - is not a game and has no box, so it lives behind Minus.
    void Menu::FlowRebuild() {
        m_flow_items.clear();
        for (int i = 0; i < (int)m_items.size(); i++) {
            const ItemKind k = m_items[i].kind;
            if (k == ItemKind::Game || k == ItemKind::Homebrew)
                m_flow_items.push_back(i);
        }
    }

    // ---- Flow's Minus menu --------------------------------------------------
    // Flow's shelf is games. Everything else the menu can do - Settings, Album,
    // Music, Power, the homebrew browser - is reached from here instead, so the
    // row never has to pretend a settings entry is a boxed game.
    //
    // The entries are the same m_items the other layouts show, minus the ones
    // already on the shelf, so nothing needs a second definition and hiding an
    // entry under Theming still hides it here (FlowMenuItem is declared up with
    // the layout constants, where the touch code can see it too).
    // Where B goes from a screen the Flow menu opened. Without this every one of
    // those handlers sends you to Screen::Main, which in Flow is the shelf - so
    // opening Album from the Minus menu and backing out dumped you on the boxes
    // instead of the menu you came from.
    Menu::Screen Menu::BackTarget() {
        if (m_from_flow_menu) { m_from_flow_menu = false; return Screen::FlowMenu; }
        if (m_from_deck_menu) { m_from_deck_menu = false; return Screen::DeckMenu; }
        return Screen::Main;
    }
    void Menu::DrawFlowMenu() {
        const Theme &t = m_theme.Current();
        DrawTopBar(nullptr);

        std::vector<std::string> labels, values;
        for (const auto &it : m_items)
            if (FlowMenuItem(it)) labels.push_back(it.name);
        values.resize(labels.size());

        if (labels.empty()) {
            m_gfx->TextCentered(FontSize::Normal, gfx::Gfx::Width / 2,
                                gfx::Gfx::Height / 2, t.dim, T("No apps found"));
        } else {
            if (m_flow_menu_cursor >= (int)labels.size())
                m_flow_menu_cursor = (int)labels.size() - 1;
            DrawCarousel(labels, values, m_flow_menu_cursor, m_sub_scroll);
        }
        DrawHint({ {{"up","down"}, "Select"}, {{"a"}, "Open"}, {{"b"}, "Back"} });
    }
    Menu::Action Menu::OnButtonFlowMenu(Btn b, u64 &out_app_id) {
        // Map the visible row back to the item it came from.
        std::vector<int> idx;
        for (int i = 0; i < (int)m_items.size(); i++)
            if (FlowMenuItem(m_items[i])) idx.push_back(i);

        const int n = (int)idx.size();
        if (b == Btn::B) { m_screen = Screen::Main; return Action::None; }
        if (n == 0) return Action::None;

        if (m_flow_menu_cursor >= n) m_flow_menu_cursor = n - 1;
        if (b == Btn::Down) m_flow_menu_cursor = (m_flow_menu_cursor + 1) % n;
        if (b == Btn::Up)   m_flow_menu_cursor = (m_flow_menu_cursor + n - 1) % n;

        if (b == Btn::A) {
            // Hand the selection to the main handler, which already knows what
            // every kind does; this screen only decides *which* entry.
            m_cursor = idx[m_flow_menu_cursor];
            m_screen = Screen::Main;
            const Action a = ActivateSelected(out_app_id);
            // If that opened another screen rather than launching something,
            // remember where it was opened from so B comes back here instead of
            // dropping onto the shelf.
            m_from_flow_menu = (m_screen != Screen::Main);
            return a;
        }
        return Action::None;
    }
    // Which box is under a touch in Flow, as an index into m_items.
    //
    // The row is perspective projected and unevenly spaced, so there is no pitch
    // to divide by - the boxes are walked and each one's projected span is
    // tested, nearest-to-centre first so an overlap resolves to the box actually
    // on top. Without this Flow fell through to the flat-list hit-test and taps
    // landed on whatever that arithmetic happened to produce.
    int Menu::FlowItemAt(int px, int py) const {
        if (m_flow_items.empty()) return -1;
        if (py < 90 || py > gfx::Gfx::Height - 150) return -1;   // title / hint rows

        // Same virtual range the renderer walks, so a box drawn past either end
        // of an endless row is tappable rather than being ignored because its
        // index is out of bounds.
        const int n      = (int)m_flow_items.size();
        const int centre = (int)lroundf(m_flow_scroll);
        int first = centre - kFlowVisible;
        int last  = centre + kFlowVisible;
        if (!m_wrap_nav) {
            if (first < 0)     first = 0;
            if (last  > n - 1) last  = n - 1;
        }

        int    best = -1;
        float  best_d = 1e9f;
        for (int i = first; i <= last; i++) {
            const float p = (float)i - m_flow_scroll;
            float fx, fz, fang;
            FlowPlace(p, fx, fz, fang);
            fz += m_flow_dolly;

            float c[4][3];
            FlowCorners(fx, fz, fang, gFlowHalfW, kFlowHalf, c);

            float lo = 1e9f, hi = -1e9f, top = 1e9f, bot = -1e9f;
            for (int k = 0; k < 4; k++) {
                float sx, sy;
                m_gfx->Project3D(c[k], sx, sy);
                lo = std::min(lo, sx); hi = std::max(hi, sx);
                top = std::min(top, sy); bot = std::max(bot, sy);
            }
            if (px < lo || px > hi || py < top || py > bot) continue;

            // Overlapping boxes: the one nearest the centre is drawn last and is
            // therefore the one you can see and meant to hit.
            const float d = std::abs(p);
            if (d < best_d) { best_d = d; best = i; }
        }
        return (best < 0) ? -1 : m_flow_items[FlowWrap(best, n)];
    }

    // ---- Flow layout tuning -------------------------------------------------
    // The shelf is drawn live behind this screen, so every change is visible as
    // it is made - which is the only sane way to tune numbers like these.
    void Menu::LoadFlowConfig() {
        FILE *fp = fopen(GetUserConfigPath("flow.txt").c_str(), "r");
        if (!fp) return;
        char line[64];
        while (fgets(line, sizeof(line), fp)) {
            for (int i = 0; i < kFlowParamN; i++) {
                char pat[32];
                snprintf(pat, sizeof(pat), "%s=%%f", kFlowKeys[i]);
                float v = 0.0f;
                if (sscanf(line, pat, &v) == 1) {
                    const FlowParam &pr = kFlowParams[i];
                    *pr.value = (v < pr.lo) ? pr.lo : (v > pr.hi ? pr.hi : v);
                    break;
                }
            }
        }
        fclose(fp);
    }
    void Menu::SaveFlowConfig() {
        EnsureUserConfigDir();
        FILE *fp = fopen(GetUserConfigPath("flow.txt").c_str(), "w");
        if (!fp) return;
        for (int i = 0; i < kFlowParamN; i++)
            fprintf(fp, "%s=%.4f\n", kFlowKeys[i], *kFlowParams[i].value);
        fclose(fp);
    }
    void Menu::DrawFlowSettings() {
        // Draw the shelf first, then the panel over it: the whole point is
        // seeing what the number does.
        DrawMainFlow();

        const Theme &t = m_theme.Current();
        const int W = gfx::Gfx::Width, H = gfx::Gfx::Height;
        const int panel_w = 520, panel_x = W - panel_w - 40;
        const int row_h = 40, top = 96;
        const int rows = kFlowParamN + 3;   // Layout + params + Reset + Back

        m_gfx->FillRect(panel_x - 16, top - 24, panel_w + 32,
                        rows * row_h + 56, SDL_Color{ 0, 0, 0, 190 });

        for (int i = 0; i < rows; i++) {
            const bool sel = (i == m_flowset_cursor);
            const int  y   = top + i * row_h;
            const SDL_Color c = sel ? t.accent : t.fg;

            const int   pi    = i - 1;   // index into the parameter table
            const char *label = (i == 0) ? T("Layout")
                              : (pi < kFlowParamN) ? T(kFlowParams[pi].label)
                              : (pi == kFlowParamN ? T("Reset to defaults") : T("Back"));
            if (sel) m_gfx->Text(FontSize::Normal, panel_x - 22, y, t.accent, ">");
            m_gfx->Text(FontSize::Normal, panel_x, y, c, label);

            char val[32];
            val[0] = '\0';
            if (i == 0) {
                const int m = FlowLayoutMatch();
                snprintf(val, sizeof(val), "%s",
                         (m >= 0) ? T(kFlowLayouts[m].name) : T("Custom"));
            } else if (pi < kFlowParamN) {
                const FlowParam &pr = kFlowParams[pi];
                if (pr.degrees)
                    snprintf(val, sizeof(val), "%.0f deg", *pr.value * 57.2958f);
                else
                    snprintf(val, sizeof(val), "%.3f", *pr.value);
            }
            if (val[0]) {
                const int vw = m_gfx->TextWidth(FontSize::Normal, val);
                m_gfx->Text(FontSize::Normal, panel_x + panel_w - vw, y, c, val);
            }
        }
        DrawStatusHint({ {{"left","right"}, "Adjust"}, {{"a"}, "Select"}, {{"b"}, "Back"} });
        (void)H;
    }
    Menu::Action Menu::OnButtonFlowSettings(Btn b) {
        const int rows = kFlowParamN + 3;
        if (b == Btn::B) { SaveFlowConfig(); m_screen = Screen::Main; return Action::None; }
        if (b == Btn::Down) m_flowset_cursor = (m_flowset_cursor + 1) % rows;
        if (b == Btn::Up)   m_flowset_cursor = (m_flowset_cursor + rows - 1) % rows;

        // Row 0 is the layout; the parameter table starts one below it.
        auto applyLayout = [&](int dir) {
            int m = FlowLayoutMatch();
            // Tuned away from all of them: step onto the first or the last
            // rather than jumping somewhere arbitrary.
            if (m < 0) m = (dir > 0) ? -1 : 0;
            m = (m + dir + kFlowLayoutN) % kFlowLayoutN;
            for (int k = 0; k < kFlowParamN; k++)
                *kFlowParams[k].value = kFlowLayouts[m].v[k];
            SetStatus(kFlowLayouts[m].name);
        };

        if (m_flowset_cursor == 0 && (b == Btn::Left || b == Btn::Right))
            applyLayout(b == Btn::Right ? +1 : -1);

        const int pi = m_flowset_cursor - 1;
        if (pi >= 0 && pi < kFlowParamN && (b == Btn::Left || b == Btn::Right)) {
            const FlowParam &pr = kFlowParams[pi];
            float v = *pr.value + ((b == Btn::Right) ? pr.step : -pr.step);
            // Clamped, not wrapped: sliding off one end straight to the other is
            // never what you meant while you are tuning by eye.
            *pr.value = (v < pr.lo) ? pr.lo : (v > pr.hi ? pr.hi : v);
        }
        if (b == Btn::A) {
            if (m_flowset_cursor == 0) {
                applyLayout(+1);
            } else if (pi == kFlowParamN) {
                for (int i = 0; i < kFlowParamN; i++)
                    *kFlowParams[i].value = kFlowDefaults[i];
                SetStatus("Reset");
            } else if (pi == kFlowParamN + 1) {
                SaveFlowConfig();
                m_screen = Screen::Main;
            }
        }
        return Action::None;
    }
    void Menu::DrawMainFlow() {
        const Theme &t = m_theme.Current();
        // The box wrap is a 165 KB PNG loaded on first use and never budgeted,
        // so it landed squarely on the frame the menu was trying to show. One
        // frame of plain cases costs nothing next to holding the whole menu
        // back for it.
        if (g_phase_done) EnsureFlowWrap();
        const int    W = gfx::Gfx::Width;
        const int    H = gfx::Gfx::Height;

        // Covers are drawn large here, so the grid's 192px downscale would be
        // visibly soft on the centre item.
        m_icons.SetScale(0);
        DrawTopBar(nullptr);

        if (m_flow_items.empty()) { DrawMainEmpty(); return; }

        const int n = (int)m_flow_items.size();

        // m_cursor indexes m_items; the row indexes m_flow_items. Find where the
        // shared cursor sits in the filtered row, so switching in from another
        // layout keeps your place on the same game.
        // m_cursor indexes m_items; the row indexes m_flow_items. Find where the
        // shared cursor sits in the filtered row, so switching in from another
        // layout keeps your place on the same game.
        int sel = 0;
        for (int i = 0; i < n; i++)
            if (m_flow_items[i] == m_cursor) { sel = i; break; }

        // The fling itself runs centrally in StepFling, for every layout. All
        // that is left here is the settle, and it must not run while a finger or
        // a throw owns the row - that ease is what snapped a flick back to
        // wherever you let go.
        if (!ScrollBusy()) {
            // With an endless row, settle toward whichever copy of the selected
            // item is nearest rather than the one at index sel: after wrapping
            // past the end those can be a whole library apart, and easing to the
            // literal index would rewind the row.
            float target = (float)sel;
            if (m_wrap_nav && n > 0) {
                while (target - m_flow_scroll >  n * 0.5f) target -= (float)n;
                while (target - m_flow_scroll < -n * 0.5f) target += (float)n;
            }
            m_flow_scroll += (target - m_flow_scroll) * 0.22f;
            if (std::abs(target - m_flow_scroll) < 0.002f) m_flow_scroll = target;
        }

        // Has the row come to rest on the selected item?
        //
        // With an endless row this is NOT the same question as
        // "m_flow_scroll == sel". The settle above eases toward whichever copy
        // of the item is nearest, so the resting position is often sel - n or
        // sel + n; the row is showing the right game while the number differs
        // from sel by a whole library.
        //
        // Comparing against the literal index therefore never matched for
        // anything the row reached by wrapping backwards - and a newly
        // installed game always is, because an unknown id sorts to the end of
        // the arrangement and the shortest way there is backwards past zero.
        // That is why a new game never fetched its box art and never loaded its
        // back panels, while games in the middle of the library did both.
        const bool flow_settled =
            (m_flow_scroll == std::floor(m_flow_scroll)) &&
            (FlowWrap((int)m_flow_scroll, n) == sel);

        // Right stick. X yaws the camera and, held past halfway, starts spinning
        // the selected box so you can read the back of the case; Y dollies in and
        // out. Everything springs back to rest when the stick is released, so the
        // shelf cannot be left parked at a strange angle.
        // X turns the selected box, Y dollies. X used to also yaw the camera, so
        // turning a box swung the whole shelf at the same time and the box you
        // were trying to look at slid away from you while it turned.
        const float rx = m_rstick_x, ry = m_rstick_y;
        m_flow_spin  += ((rx * 3.14159f) - m_flow_spin)  * 0.12f;
        m_flow_dolly += ((ry * 0.80f)    - m_flow_dolly) * 0.10f;

        // Wall clock for the running game's idle rotation, read once so every
        // box in the row is placed against the same instant.
        const float flow_now = (float)armGetSystemTick() / (float)armGetSystemTickFreq();

        // The drawn range is expressed in *virtual* positions - they may run
        // below zero or past the end - so the row is continuous across a wrap.
        // Each one is folded to a real item only when its content is needed.
        const int centre = (int)lroundf(m_flow_scroll);
        int first = centre - kFlowVisible;
        int last  = centre + kFlowVisible;
        if (!m_wrap_nav) {                 // finite row: stop at the ends
            if (first < 0)     first = 0;
            if (last  > n - 1) last  = n - 1;
        }

        // Outside-in: the two ends are farthest, the centre is nearest and must
        // land last so it covers its neighbours.
        std::vector<int> order;
        order.reserve(last - first + 1);
        for (int i = first; i <= last; i++) order.push_back(i);
        std::sort(order.begin(), order.end(), [&](int a, int b) {
            return std::abs((float)a - m_flow_scroll) > std::abs((float)b - m_flow_scroll);
        });

        // Load everything this frame needs *before* drawing any of it.
        //
        // Gfx::LoadImageScaled downscales by switching the render target, which
        // flushes whatever render pass is in flight. Called from inside the draw
        // loop that meant a flush between boxes, several times a frame, which is
        // what made scrolling crawl. Doing the loads up front costs the same
        // decode but leaves the draw pass unbroken.
        {
            // Cached covers keep loading while you scroll - they are cheap - but
            // decoding a new one is not, so that waits until the row has come to
            // rest. Building the cache mid-scroll is what made scrolling stall.
            for (int row : order)
                FlowCover(m_items[m_flow_items[FlowWrap(row, n)]]);
            // Back panels are far more expensive - a hero is 1920x620 - and are
            // only wanted once you have stopped somewhere. Loading them as the
            // selection swept past during a scroll was two full decodes every
            // few frames.
            if (flow_settled) FlowBackShots(m_items[m_flow_items[sel]]);
            // The running game turns by itself, so its back swings into view
            // whether or not it is the entry you are on. Without this the only
            // box that is guaranteed to show you its back is the only one whose
            // back art was never decoded, and it came round blank every time.
            if (flow_settled && m_suspended != 0) {
                for (int idx : m_flow_items) {
                    const MenuItem &ri = m_items[idx];
                    if (ri.app_id == m_suspended) { FlowBackShots(ri); break; }
                }
            }
        }

        // Box scans are big: keep only those of the boxes near the selection.
        if (m_game_wraps.size() > 8) {
            std::unordered_set<u64> keep;
            for (int d = -2; d <= 2; d++)
                keep.insert(m_items[m_flow_items[FlowWrap(sel + d, n)]].app_id);
            for (auto w = m_game_wraps.begin(); w != m_game_wraps.end(); ) {
                if (keep.count(w->first)) { ++w; continue; }
                if (w->second) m_gfx->FreeImage(w->second);
                w = m_game_wraps.erase(w);
            }
        }

        for (int row : order) {
            // row is a virtual position; the item it shows is that position
            // folded back into the list.
            const MenuItem &it = m_items[m_flow_items[FlowWrap(row, n)]];
            const float p = (float)row - m_flow_scroll;

            float x, z, angle;
            FlowPlace(p, x, z, angle);

            // Dolly moves the row in depth; nothing slides it sideways.
            z += m_flow_dolly;

            // The selected box carries the extra spin, so only it turns around.
            const bool is_sel = (FlowWrap(row, n) == sel);
            if (is_sel) angle += m_flow_spin;

            // The game currently running turns slowly on its own, so you can
            // pick it out of the row at a glance without reading anything. It
            // is added on top of any other rotation, so the running game still
            // responds to the stick if it happens to be the selected one.
            if (it.app_id != 0 && it.app_id == m_suspended)
                angle += flow_now * kFlowRunSpin;


            // Launch bounce: the chosen case dips and then springs toward you.
            // Scaling the corners in world space lets the perspective do the
            // rest, so it grows the way it would if it were really coming
            // forward rather than just being drawn bigger.
            //
            // All three dimensions scale, and every face is built from these
            // rather than from the globals. Scaling only the front panel grew
            // the picture while the case it is printed on stayed put, so the
            // box came apart as it sprang.
            float half_w = gFlowHalfW, half_h = kFlowHalf, depth = gFlowDepth;
            if (is_sel) {
                const float lt = LaunchAnimT();
                if (lt >= 0.0f) {
                    const float sc = LaunchScale(lt);
                    half_w *= sc;
                    half_h *= sc;
                    depth  *= sc;
                }
            }

            // The box front, 2:3 rather than square.
            float corners[4][3];
            FlowCorners(x, z, angle, half_w, half_h, corners);

            SDL_Texture *cover = FlowCover(it);
            // No cover yet: fall back to the title's own icon, centred on the
            // case rather than stretched across it - a 1:1 icon pulled onto a 2:3
            // front is what made this look like rotated tiles before.
            SDL_Texture *icon = cover ? nullptr
                              : ((it.kind == ItemKind::Homebrew)
                                     ? m_hb_icons.Get(it.hb_icon)
                                     : m_icons.Get(it.app_id));

            const float prox = std::max(0.0f, 1.0f - std::abs(p));
            const Uint8 lit  = (Uint8)(150 + 105 * prox);
            const SDL_Color tint { lit, lit, lit, 255 };
            // The bare case, under whatever is printed on it.
            const Uint8 bl = (Uint8)(18 + 14 * prox);
            const SDL_Color blank { bl, bl, bl, 255 };
            // The side faces are plain grey plastic and are lit independently of
            // the case front - they were derived from it for a moment, which tied
            // the edges to a colour that has nothing to do with them.
            const Uint8 side_lit = (Uint8)(70 + 60 * prox);

            // The case is a solid object, so every face gets an opaque backing
            // before anything is printed on it. The wrap is 96% clear across the
            // front panel and 67% clear across the back, so a face drawn as the
            // wrap alone is mostly a hole - which is what made the box see-
            // through even once the faces were all being drawn.
            const SDL_Color side_col { side_lit, side_lit, side_lit, 255 };
            const Uint8 backg = (Uint8)(side_lit * 0.9f);
            const SDL_Color back_col { backg, backg, backg, 255 };

            auto side_face = [&](float lx, float out[4][3]) {
                FlowFace(x, z, angle, lx, 0.0f, lx, 2.0f * depth, half_h, out);
            };

            float back[4][3];
            FlowFace(x, z, angle,
                     half_w, 2.0f * depth,
                    -half_w, 2.0f * depth,
                     half_h, back);

            auto mid_z = [](const float f[4][3]) {
                return 0.25f * (f[0][2] + f[1][2] + f[2][2] + f[3][2]);
            };

            // Which way round the case is, taken from the geometry rather than
            // from cosf(angle).
            //
            // The face sort below was moved off the angle for exactly this
            // reason and this test was left behind on it: the boxes sit well off
            // to either side, so perspective swings a face toward or away from
            // the camera at rotations the cosine knows nothing about. Off-centre,
            // the angle would call the back "showing" while the front was still
            // the nearer face - and the front's printing is skipped when that is
            // set, so the case went blank while you were looking straight at its
            // cover. The nearer of the two faces is the one you are looking at,
            // which is the same rule the sort uses and agrees with it by
            // construction.
            const bool showing_back = mid_z(back) < mid_z(corners);

            // Takes the face already built, because the draw order below needs
            // every face's geometry before it can decide what to paint first.
            // This game's own box scan, when GameTDB had one: loaded for the
            // boxes by the selection, used from cache for the rest.
            SDL_Texture *gw = nullptr;
            if (std::abs(p) <= 2.5f) gw = GameWrap(it);
            else if (auto g = m_game_wraps.find(it.app_id); g != m_game_wraps.end()) gw = g->second;
            const bool scan_front = gw && TdbFront(it.app_id);

            auto draw_side = [&](const float face[4][3], bool spine) {
                m_gfx->DrawQuad3D(nullptr, face, side_col, 255, 255, false, 4);
                if (spine && gw) {
                    const float uv_spine[4] = { kScanSpine0, 0.0f, kScanSpine1, 1.0f };
                    m_gfx->DrawQuad3D(gw, face, tint, 255, 255, false, 4, uv_spine);
                } else if (spine && m_flow_wrap) {
                    const float uv_spine[4] = { kWrapSpine0, 0.0f, kWrapSpine1, 1.0f };
                    m_gfx->DrawQuad3D(m_flow_wrap, face, tint, 255, 255,
                                      false, 4, uv_spine);
                }
            };
            auto draw_back = [&]() {
                m_gfx->DrawQuad3D(nullptr, back, back_col, 255, 255, false, 4);
                if (gw) {                   // the real back of the case
                    const float uv_back[4] = { 0.0f, 0.0f, kScanSpine0, 1.0f };
                    m_gfx->DrawQuad3D(gw, back, tint, 255, 255, false, 12, uv_back);
                    return;
                }

                // Two panels across the top of the back, where a real case
                // prints its screenshots. Drawn before the wrap so the printed
                // furniture - the legal block, the barcode - sits over them.
                //
                // The back face runs from +halfW on the left to -halfW on the
                // right so its texture is not mirrored; these follow the same
                // sense, hence the descending x.
                // Only decode these for a box actually showing its back. Doing
                // it for every visible box held ~118 MB of textures for images
                // nobody could see, which is what starved the wallpaper of
                // memory and left other screens on a bare gradient.
                // Loaded for the selected box whether or not it is turned
                // round: this is the only box that can be rotated, and decoding
                // on the frame it passes ninety degrees is precisely the hitch
                // you see when turning one over.
                // Read-only here: anything not already cached by the load pass
                // above simply is not drawn this frame.
                static const FlowShots kNoShots;
                auto shit = m_shots.find(it.app_id);
                const FlowShots &sh = (shit != m_shots.end()) ? shit->second : kNoShots;
                if (sh.a || sh.b) {
                    // Two 16:9 panels stacked flush: the full width of the case,
                    // starting at its top edge, with nothing between them. The
                    // height still follows from the width (h = w * 9/16) so they
                    // keep their aspect rather than being stretched to fill.
                    //
                    // Two of them come to 0.765 of the case height, which leaves
                    // the bottom quarter for the printed legal block - the same
                    // place a real case puts it.
                    const float h      = (2.0f * half_w) * 9.0f / 16.0f;
                    const float gap    = 0.0f;
                    const float top    = half_h;           // flush with the top edge

                    // The back face runs +halfW on the left to -halfW on the
                    // right so its texture is not mirrored; these follow suit.
                    float panel[4][3];
                    if (sh.a) {
                        FlowPanel(x, z, angle, half_w, -half_w,
                                  top - h, top, 2.0f * depth, panel);
                        m_gfx->DrawQuad3D(sh.a, panel, tint, 255, 255, false, 8);
                    }
                    if (sh.b) {
                        const float t2 = top - h - gap;
                        FlowPanel(x, z, angle, half_w, -half_w,
                                  t2 - h, t2, 2.0f * depth, panel);
                        m_gfx->DrawQuad3D(sh.b, panel, tint, 255, 255, false, 8);
                    }
                }

                if (m_flow_wrap) {
                    const float uv_back[4] = { 0.0f, 0.0f, kWrapSpine0, 1.0f };
                    m_gfx->DrawQuad3D(m_flow_wrap, back, tint, 255, 255,
                                      false, 12, uv_back);
                }
            };
            auto draw_front = [&]() {
                m_gfx->DrawQuad3D(nullptr, corners, blank, 255, 255);
                if (showing_back) return;   // its printing faces away from us
                if (cover) {
                    m_gfx->DrawQuad3D(cover, corners, tint, 255, 255);
                } else if (gw) {
                    const float uv_front[4] = { kScanSpine1, 0.0f, 1.0f, 1.0f };
                    m_gfx->DrawQuad3D(gw, corners, tint, 255, 255, false, 12, uv_front);
                    return;                 // printed already
                } else if (icon) {
                    // Square icon inset on the face, leaving case above and below.
                    float inset[4][3];
                    FlowCorners(x, z, angle, half_w * 0.82f, half_w * 0.82f, inset);
                    m_gfx->DrawQuad3D(icon, inset, tint, 255, 255);
                }
                // The printed wrap over the art is what makes this a boxed game
                // rather than a picture on a slab - unless the art is a scan of
                // the real thing, which has its own.
                if (m_flow_wrap && !scan_front) {
                    const float uv_front[4] = { kWrapFront0, 0.0f, kWrapFront1, 1.0f };
                    m_gfx->DrawQuad3D(m_flow_wrap, corners, tint, 255, 255,
                                      false, 12, uv_front);
                }
            };

            // Painted back to front. There is no depth buffer, so draw order is
            // the only depth information there is.
            //
            // The four faces are sorted by their actual depth rather than by
            // rules read off the angle. The angle on its own is not enough: the
            // boxes sit well off to either side, so perspective slides the faces
            // past one another at rotations the sine and cosine know nothing
            // about, and a face could be painted over one genuinely in front of
            // it. That is what made sides go missing and the order look wrong at
            // certain angles - the old rules were right near the centre of the
            // row and drifted further out.
            //
            // Sorting a convex box's faces far-to-near is correct at every angle
            // and every position, and it needs no winding convention: the ones
            // pointing away are simply painted over by the ones in front.
            //
            // The spine is the wrap's left edge, so it lives on the box's left;
            // the plain opening edge is on the right.
            float face_l[4][3], face_r[4][3];
            side_face(-half_w, face_l);
            side_face( half_w, face_r);

            struct FaceOrder { float z; int which; };   // 0 front 1 back 2 left 3 right
            FaceOrder faces[4] = {
                { mid_z(corners), 0 },
                { mid_z(back),    1 },
                { mid_z(face_l),  2 },
                { mid_z(face_r),  3 },
            };
            for (int a = 1; a < 4; a++) {          // insertion sort, farthest first
                const FaceOrder key = faces[a];
                int b = a - 1;
                while (b >= 0 && faces[b].z < key.z) { faces[b + 1] = faces[b]; b--; }
                faces[b + 1] = key;
            }
            for (int k = 0; k < 4; k++) {
                switch (faces[k].which) {
                    case 0:  draw_front(); break;
                    case 1:  draw_back();  break;
                    case 2:  draw_side(face_l, true);  break;
                    default: draw_side(face_r, false); break;
                }
            }

            // Reflection. Mirroring only the front face left the box floating on
            // a reflection narrower than itself; every face the box is built from
            // gets mirrored, so the reflection has the same silhouette.
            //
            // Mirrored about the box's OWN bottom edge, not about a fixed floor.
            // kFlowFloor is the bottom of an unscaled case, so while the launch
            // bounce grows the selected one its base hung below that line and the
            // reflection - still folded about the old one - rode up over the box
            // it was supposed to be sitting on. Taking the plane from the scaled
            // half-height keeps the two exactly edge to edge at every size, and
            // is identical to the constant for every box that is not bouncing.
            const float floor_y = gFlowY - half_h;
            auto mirror = [&](const float src[4][3], float out[4][3]) {
                for (int k = 0; k < 4; k++) {
                    const int j = (k == 0) ? 3 : (k == 1) ? 2 : (k == 2) ? 1 : 0;
                    out[k][0] = src[j][0];
                    out[k][2] = src[j][2];
                    out[k][1] = 2.0f * floor_y - src[j][1];
                }
            };
            {
                float m[4][3], face[4][3];
                // Sides first, then the face you are actually looking at, so the
                // reflection stacks the same way the box does.
                side_face(half_w, face);  mirror(face, m);
                m_gfx->DrawQuad3D(nullptr, m, blank, 70, 0, true, 4);
                side_face(-half_w, face); mirror(face, m);
                m_gfx->DrawQuad3D(nullptr, m, blank, 70, 0, true, 4);

                mirror(showing_back ? back : corners, m);
                if (showing_back) {
                    m_gfx->DrawQuad3D(nullptr, m, blank, 80, 0, true, 4);
                } else {
                    m_gfx->DrawQuad3D(cover ? cover : nullptr, m,
                                      cover ? tint : blank, 90, 0, true);
                }
            }
        }

        // Keep a wide margin of covers either side of what is on screen, and
        // fill it a few at a time.
        //
        // The window used to be four boxes and did nothing but evict - art was
        // only ever decoded for a box being drawn, so scrolling quickly meant
        // decoding on the frame each new box appeared, which is the stutter.
        // Loading ahead moves that work to frames with nothing else to do, and
        // the per-frame cap stops the filling itself becoming a hitch.
        {
            // The drawn range is virtual and may run past either end, so the
            // keep window has to be folded the same way the draw loop folds it.
            // Clamping it to [0, n-1] instead meant that once the row wrapped,
            // the boxes actually on screen had indices outside the window: they
            // were evicted and re-decoded every single frame, which is the
            // four-frames-a-second you hit on the second lap.
            //
            // The membership test is a set rather than a scan per cached entry.
            // The old nested loop was the window size times the cache size every
            // frame, and both grew with the preload margin.
            const int lo = first - kFlowPreload;
            const int hi = last  + kFlowPreload;

            auto row_item = [&](int i) -> const MenuItem * {
                const int idx = m_wrap_nav ? FlowWrap(i, n) : i;
                if (idx < 0 || idx >= n) return nullptr;
                return &m_items[m_flow_items[idx]];
            };

            std::unordered_set<u64> keep_ids;
            keep_ids.reserve((size_t)(hi - lo + 1) * 2);
            for (int i = lo; i <= hi; i++)
                if (const MenuItem *it2 = row_item(i)) keep_ids.insert(it2->app_id);

            int budget = m_cover_budget;
            for (int i = lo; i <= hi && budget > 0; i++) {
                const MenuItem *pit = row_item(i);
                if (!pit || pit->kind != ItemKind::Game || pit->app_id == 0) continue;
                if (m_covers.count(pit->app_id)) continue;   // cached, hit or miss
                FlowCover(*pit);
                budget--;
            }
            for (auto it2 = m_covers.begin(); it2 != m_covers.end(); ) {
                if (keep_ids.count(it2->first)) { ++it2; continue; }
                if (it2->second) m_gfx->FreeImage(it2->second);
                it2 = m_covers.erase(it2);
            }

            // Back panels are held a couple either side of the selection rather
            // than for the selected box alone. Only the selected box loads them,
            // but dropping them the instant the cursor moves meant stepping one
            // across and back re-decoded a megabyte each time.
            std::unordered_set<u64> keep_shots;
            for (int i = sel - 2; i <= sel + 2; i++)
                if (const MenuItem *it2 = row_item(i)) keep_shots.insert(it2->app_id);
            for (auto it2 = m_shots.begin(); it2 != m_shots.end(); ) {
                if (keep_shots.count(it2->first)) { ++it2; continue; }
                if (it2->second.a) m_gfx->FreeImage(it2->second.a);
                if (it2->second.b) m_gfx->FreeImage(it2->second.b);
                it2 = m_shots.erase(it2);
            }
        }

        // Title of the centred item, under the shelf.
        {
            const MenuItem &cur = m_items[m_flow_items[sel]];

            // Fetch art for whatever you have actually stopped on. Gating on the
            // row being settled means scrolling through a big library queues one
            // lookup, not one per game you passed.
            //
            // "No cover" has to mean a cover that was looked for and not found,
            // not merely one that has not been decoded yet. FlowCover also
            // returns null when the per-frame decode budget is spent, and
            // treating that as missing sent a SteamGridDB search for a title
            // whose art was already sitting on the card - once per launch,
            // which is what filled covers.log with the same game over and over.
            const bool cover_missing = m_covers.count(cur.app_id) &&
                                       m_covers[cur.app_id] == nullptr;
            // Same rule for the back panels: "no screenshots" means the decoder
            // looked and found nothing, not that it has yet to run. Without this
            // a title whose cover was already on the card never fetched any,
            // because the fetch was only ever triggered by a missing cover.
            const auto sit = m_shots.find(cur.app_id);
            const bool shots_missing = (sit != m_shots.end()) &&
                                       !sit->second.a && !sit->second.b;
            // ...and for a box scan: GameWrap looked and found none.
            const auto wit = m_game_wraps.find(cur.app_id);
            const bool wrap_missing = (wit != m_game_wraps.end()) && !wit->second;
            if (flow_settled && cur.kind == ItemKind::Game &&
                ((!FlowCover(cur) && cover_missing) || shots_missing || wrap_missing))
                StartCoverFetch(cur.app_id, cur.name);
            const int ty = H - 132;
            m_gfx->TextCentered(FontSize::Large, W / 2, ty, t.title,
                                Ellipsize(cur.name, W - 160, FontSize::Large).c_str());
            char pos[32];
            snprintf(pos, sizeof(pos), "%d / %d", sel + 1, n);
            // Position counters are optional; blanking the string here keeps
            // the layout arithmetic below untouched.
            if (!m_show_counter) pos[0] = '\0';
            m_gfx->TextCentered(FontSize::Small, W / 2,
                                ty + m_gfx->LineHeight(FontSize::Large) + 4, t.dim, pos);
        }

        DrawFetchStatus();
        DrawStatusHint({ {{"a"}, "Launch"}, {{"x"}, "Options"}, {{"minus"}, "Menu"}, {{"rstick"}, "Look/Turn"} });
    }
} // namespace sl::menu::ui
