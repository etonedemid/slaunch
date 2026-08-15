#pragma once
#include <switch.h>
#include <sl/menu/gfx/Gfx.hpp>

// Developer overlay: what the menu process is actually costing at runtime.
//
// It exists mainly to answer one question that decides how far the frontend can
// be pushed - how much memory a library applet really gets. sMenu asks libnx to
// auto-size its heap because a fixed 128MB request fails outright in an applet
// slot, so the real ceiling has never been measured on hardware. The kernel
// knows; this reads it back and puts it on screen.
//
// Deliberately unthemed (fixed dark panel, fixed colours): a debug readout has
// to stay legible on every theme and wallpaper, including the ones that would
// otherwise render it as light grey on white.

namespace sl::menu::dbg {

    // Counts the menu keeps that the kernel cannot report.
    struct Counters {
        int app_icons  = 0;   // cached game icon textures
        int hb_icons   = 0;   // cached homebrew icon textures
        int sys_icons  = 0;   // cached system-entry glyph textures
        int items      = 0;   // entries in the current menu list
        int widgets    = 0;   // loaded Lua widgets
        int ui_mode    = 0;   // active UiMode, as a number
    };

    class Overlay {
    public:
        // Turning the overlay off writes a snapshot to sdmc:/slaunch/debug.log
        // (appended, timestamped). Reading these numbers off the screen and
        // retyping them loses exactly the digits that matter, and the log can be
        // pulled off the card as-is.
        void Toggle();
        bool Visible() const { return m_visible; }

        // Append the current sample to the log. Called on toggle-off; safe to
        // call at any time.
        void Dump(const Counters &c) const;

        // Once per frame, before Draw: advances the frame-time history and
        // re-reads the cheap kernel counters (svcGetInfo is a few hundred ns).
        // The expensive samples (newlib arena, SD free space) are refreshed on
        // their own slower timer inside.
        void Frame();

        void Draw(gfx::Gfx *gfx, const Counters &c);

    private:
        void Reset();
        void SampleFast();
        void SampleSlow();

        bool m_visible = false;

        // --- timing ---
        static constexpr int kHistory = 60;
        float m_frame_ms[kHistory] = {};
        int   m_frame_i   = 0;
        u64   m_last_tick = 0;
        float m_fps       = 0.0f;
        float m_ms_avg    = 0.0f;
        float m_ms_worst  = 0.0f;

        // --- memory (bytes) ---
        u64 m_mem_total    = 0;   // InfoType_TotalMemorySize: the applet budget
        u64 m_mem_used     = 0;   // InfoType_UsedMemorySize
        u64 m_mem_peak     = 0;   // highest used seen while the overlay ran
        u64 m_mem_limit    = 0;   // resource-limit ceiling for mappable memory
        u64 m_mem_rl_cur   = 0;   // resource-limit current
        u64 m_mem_rl_peak  = 0;   // resource-limit peak, tracked by the kernel
        u64 m_heap_region  = 0;   // InfoType_HeapRegionSize
        u64 m_nonsys_total = 0;   // [6.0.0+] excluding process-management memory
        u64 m_nonsys_used  = 0;
        u64 m_malloc_used  = 0;   // newlib arena in use
        u64 m_malloc_arena = 0;
        u64 m_sys_total    = 0;   // whole-console DRAM, if readable
        u64 m_sys_used     = 0;

        // --- process ---
        u64 m_program_id   = 0;
        u64 m_free_threads = 0;
        bool m_have_nonsys = false;
        bool m_have_thread = false;
        bool m_have_sysmem = false;

        // --- storage ---
        u64 m_sd_free = 0, m_sd_total = 0;

        u64 m_slow_tick = 0;
    };

} // namespace sl::menu::dbg
