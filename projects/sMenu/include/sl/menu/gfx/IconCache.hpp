#pragma once
#include <switch.h>
#include <SDL2/SDL.h>
#include <sl/menu/gfx/Gfx.hpp>
#include <unordered_map>
#include <cstdint>

// Bounded texture cache for application icons. The JPEGs are extracted to
// slaunch/cache/icons/<id>.jpg by the menu process (see sMenu/main.cpp); this
// turns them into GPU textures on demand for the icon-based UI modes.
//
// Only a screenful of icons is ever visible at once, so the cache keeps at most
// Capacity textures and evicts the least-recently-used when full - this bounds
// applet-heap use no matter how many games are installed. A title that has no
// cached icon file is remembered as a miss so we don't stat() it every frame.

namespace sl::menu::gfx {

    class IconCache {
    public:
        // subdir is under sdmc:/slaunch/ (e.g. "cache/icons" or "cache/boxart").
        void Init(Gfx *gfx, const char *subdir = "cache/icons") { m_gfx = gfx; m_dir = subdir; }
        void Exit();                       // free every texture (call before Gfx::Exit)

        // Returns the icon texture for app_id, loading it if needed, or nullptr
        // when the title has no cached icon. Touching an entry marks it recent.
        SDL_Texture *Get(u64 app_id);

        // Load resolution for subsequent Get()s. 0 = original (used by Line mode,
        // which draws few large covers); a positive value downscales once at load
        // (Grid/List, many small tiles). Changing it drops cached textures so
        // they reload at the new size.
        void SetScale(int px);
        static constexpr int GridScale = 192;   // default downscale for grid/list

    private:
        static constexpr int Capacity = 48;

        struct Entry {
            SDL_Texture *tex = nullptr; // null = known miss (no file)
            uint64_t     used = 0;      // last-access tick for LRU
        };

        Gfx        *m_gfx  = nullptr;
        const char *m_dir  = "cache/icons";
        uint64_t    m_clock = 0;
        int         m_scale = GridScale;   // current load size (0 = original)
        std::unordered_map<u64, Entry> m_map;

        void EvictOldest();
    };

} // namespace sl::menu::gfx
