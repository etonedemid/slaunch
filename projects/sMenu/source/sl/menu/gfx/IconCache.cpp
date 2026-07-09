#include <sl/menu/gfx/IconCache.hpp>
#include <cstdio>

namespace sl::menu::gfx {

    void IconCache::SetScale(int px) {
        if (px == m_scale) return;
        // Resolution changed: drop cached textures so they reload at the new size.
        for (auto &kv : m_map)
            if (kv.second.tex && m_gfx) m_gfx->FreeImage(kv.second.tex);
        m_map.clear();
        m_scale = px;
    }

    void IconCache::Exit() {
        for (auto &kv : m_map) {
            if (kv.second.tex && m_gfx) m_gfx->FreeImage(kv.second.tex);
        }
        m_map.clear();
    }

    void IconCache::EvictOldest() {
        auto oldest = m_map.end();
        for (auto it = m_map.begin(); it != m_map.end(); ++it) {
            // Only real textures cost memory; drop the least-recently-used one.
            if (it->second.tex == nullptr) continue;
            if (oldest == m_map.end() || it->second.used < oldest->second.used)
                oldest = it;
        }
        if (oldest != m_map.end()) {
            if (m_gfx) m_gfx->FreeImage(oldest->second.tex);
            m_map.erase(oldest);
        }
    }

    SDL_Texture *IconCache::Get(u64 app_id) {
        if (!m_gfx) return nullptr;

        auto it = m_map.find(app_id);
        if (it != m_map.end()) {
            it->second.used = ++m_clock;
            return it->second.tex; // may be nullptr (known miss)
        }

        // Count how many real textures are resident; evict before adding a new
        // one so we never exceed Capacity live textures.
        int live = 0;
        for (auto &kv : m_map) if (kv.second.tex) live++;
        if (live >= Capacity) EvictOldest();

        char path[64];
        snprintf(path, sizeof(path), "sdmc:/slaunch/cache/icons/%016llX.jpg",
                 (unsigned long long)app_id);
        SDL_Texture *tex = (m_scale > 0)
                               ? m_gfx->LoadImageScaled(path, m_scale, m_scale)
                               : m_gfx->LoadImage(path);   // full resolution

        m_map[app_id] = Entry{ tex, ++m_clock };
        return tex;
    }

} // namespace sl::menu::gfx
