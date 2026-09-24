#include <sl/menu/gfx/IconCache.hpp>
#include <cstdio>
#include <string>

namespace sl::menu::gfx {

    void IconCache::FreeAll() {
        for (auto &kv : m_map)
            if (kv.second.tex && m_gfx) m_gfx->FreeImage(kv.second.tex);
        for (SDL_Texture *t : m_pool)
            if (m_gfx) m_gfx->FreeImage(t);
        m_map.clear();
        m_pool.clear();
        m_live = 0;
    }

    void IconCache::SetScale(int px) {
        if (px == m_scale) return;
        // Resolution changed: drop cached textures (and the pool, which is
        // sized for the old scale) so they reload at the new size.
        FreeAll();
        m_scale = px;
    }

    int IconCache::Live() const { return m_live; }

    void IconCache::Exit() { FreeAll(); }

    void IconCache::EvictOldest() {
        auto oldest = m_map.end();
        for (auto it = m_map.begin(); it != m_map.end(); ++it) {
            // Only real textures cost memory; drop the least-recently-used one.
            if (it->second.tex == nullptr) continue;
            if (oldest == m_map.end() || it->second.used < oldest->second.used)
                oldest = it;
        }
        if (oldest != m_map.end()) {
            // Only exactly-m_scale textures are pooled: Load overwrites them
            // whole, so an odd-sized one (see Load's fallback) must go.
            int w = 0, h = 0;
            SDL_QueryTexture(oldest->second.tex, nullptr, nullptr, &w, &h);
            if (m_scale > 0 && w == m_scale && h == m_scale) m_pool.push_back(oldest->second.tex);
            else if (m_gfx) m_gfx->FreeImage(oldest->second.tex);
            m_map.erase(oldest);
            m_live--;
        }
    }

    SDL_Texture *IconCache::Get(u64 app_id) {
        if (!m_gfx) return nullptr;

        auto it = m_map.find(app_id);
        if (it != m_map.end()) {
            it->second.used = ++m_clock;
            return it->second.tex; // may be nullptr (known miss)
        }

        // Out of decoding budget for this frame: report a miss without
        // recording one, so the next frame tries again rather than caching a
        // permanent nullptr for a perfectly good icon.
        if (m_budget <= 0) return nullptr;
        m_budget--;

        // Evict before adding so we never exceed Capacity live textures. The
        // live count is tracked rather than recounted: this used to walk the
        // whole map on every lookup, which was nothing against a few dozen
        // games but thousands of iterations per icon per frame once a scanned
        // ROM library put an entry in here for every ROM.
        if (m_live >= Capacity) EvictOldest();

        // Remembered misses cost no GPU memory but the map still grows one
        // entry per key ever asked for, and a ROM library asks for thousands.
        // They are only an optimisation - dropping them all costs one retried
        // load apiece - so the map gets a ceiling too.
        if (m_map.size() >= MissCapacity) {
            for (auto it = m_map.begin(); it != m_map.end(); ) {
                if (it->second.tex) { ++it; continue; }   // keep live textures
                it = m_map.erase(it);
            }
        }

        std::string path = m_path_fn ? m_path_fn(app_id) : std::string();
        if (path.empty()) {
            char buf[80];
            snprintf(buf, sizeof(buf), "sdmc:/slaunch/%s/%016llX.jpg",
                     m_dir, (unsigned long long)app_id);
            path = buf;
        }
        SDL_Texture *tex = Load(path);

        m_map[app_id] = Entry{ tex, ++m_clock };
        if (tex) m_live++;
        return tex;
    }

    SDL_Texture *IconCache::Load(const std::string &path) {
        if (m_scale <= 0) return m_gfx->LoadImage(path.c_str());   // full resolution

        SDL_Surface *surf = m_gfx->LoadSurfaceScaled(path.c_str(), m_scale, m_scale);
        if (!surf) return nullptr;
        SDL_Texture *tex = nullptr;
        if (surf->w == m_scale && surf->h == m_scale) {
            if (!m_pool.empty()) { tex = m_pool.back(); m_pool.pop_back(); }
            else {
                tex = SDL_CreateTexture(m_gfx->Renderer(), SDL_PIXELFORMAT_RGBA8888,
                                        SDL_TEXTUREACCESS_STATIC, m_scale, m_scale);
                if (tex) SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
            }
            if (tex && SDL_UpdateTexture(tex, nullptr, surf->pixels, surf->pitch) != 0) {
                m_gfx->FreeImage(tex);
                tex = nullptr;
            }
        } else {
            // Scaling fell back to the source size (out of memory): an odd-sized
            // texture that must not enter the pool, which assumes m_scale.
            tex = SDL_CreateTextureFromSurface(m_gfx->Renderer(), surf);
        }
        SDL_FreeSurface(surf);
        return tex;
    }

} // namespace sl::menu::gfx
