#pragma once
#include <switch.h>
#include <SDL2/SDL.h>
#include <sl/menu/gfx/Gfx.hpp>
#include <unordered_map>
#include <vector>
#include <functional>
#include <string>
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

        // Decoding budget for the current frame.
        //
        // Get() decodes synchronously, and the layouts call it once per visible
        // entry while drawing, so the first frame after the menu opens used to
        // decode an image for every entry on screen before anything appeared -
        // most of the wait between pressing HOME and seeing the menu. With a
        // budget, a frame decodes a few and returns nullptr for the rest; the
        // layouts already draw a placeholder for a missing icon, so the art
        // streams in over the next handful of frames instead of gating the
        // first one.
        void BeginFrame(int budget) { m_budget = budget; }
        static constexpr int GridScale = 192;   // default downscale for grid/list

        // Live textures held (misses excluded), for the debug overlay.
        int Live() const;

        // Where a key's image lives, when it is not the cache directory. Set by
        // the menu for launcher shortcuts, whose art is RetroArch's own
        // thumbnail folder rather than anything sLaunch extracted - a .png,
        // under a name, outside slaunch/. Returning an empty string (or leaving
        // this unset) keeps the built-in "<dir>/<key>.jpg" layout, so the NRO
        // and title icons sharing a cache with them are unaffected.
        void SetPathFn(std::function<std::string(u64)> fn) { m_path_fn = std::move(fn); }

    private:
        static constexpr int Capacity = 48;
        // Ceiling on remembered misses (see Get). Generous next to the ~48 live
        // textures, small next to a ROM library.
        static constexpr size_t MissCapacity = 512;

        struct Entry {
            SDL_Texture *tex = nullptr; // null = known miss (no file)
            uint64_t     used = 0;      // last-access tick for LRU
        };

        // Textures of evicted entries, kept for reuse. At a fixed scale every
        // icon is the same size, so a new one is uploaded into an old texture
        // instead of destroying one and creating another. Scrolling a ROM
        // library used to do that up to twice a frame, for as long as the
        // stick was held - the same GPU allocation churn that once took the
        // text cache down (see Gfx::EvictText) - and the pool caps it at
        // Capacity allocations for the life of the cache.
        std::vector<SDL_Texture *> m_pool;

        Gfx        *m_gfx  = nullptr;
        const char *m_dir  = "cache/icons";
        uint64_t    m_clock = 0;
        int         m_scale = GridScale;   // current load size (0 = original)
        int         m_budget = 9999;       // decodes left this frame
        int         m_live   = 0;          // live textures, tracked not recounted
        std::function<std::string(u64)> m_path_fn;
        std::unordered_map<u64, Entry> m_map;

        void EvictOldest();
        void FreeAll();
        SDL_Texture *Load(const std::string &path);
    };

} // namespace sl::menu::gfx
