#include <sl/menu/dbg/Debug.hpp>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <malloc.h>
#include <sys/statvfs.h>

namespace sl::menu::dbg {

    namespace {
        constexpr SDL_Color kPanel = { 8, 10, 14, 225 };
        constexpr SDL_Color kEdge  = { 90, 220, 140, 200 };
        constexpr SDL_Color kKey   = { 150, 160, 175, 255 };
        constexpr SDL_Color kVal   = { 235, 240, 245, 255 };
        constexpr SDL_Color kHot   = { 255, 120, 110, 255 };
        constexpr SDL_Color kGood  = { 120, 230, 150, 255 };
        constexpr SDL_Color kHead  = { 90, 220, 140, 255 };

        // Sizes here span kilobytes (a texture) to gigabytes (console DRAM), so
        // pick the unit per value and keep one decimal - "3.4 MB" reads faster
        // than "3567616" when the number is changing every frame.
        void FormatBytes(char *out, size_t n, u64 v) {
            if (v >= 1024ULL * 1024 * 1024)
                snprintf(out, n, "%.2f GB", (double)v / (1024.0 * 1024 * 1024));
            else if (v >= 1024 * 1024)
                snprintf(out, n, "%.1f MB", (double)v / (1024.0 * 1024));
            else if (v >= 1024)
                snprintf(out, n, "%.1f KB", (double)v / 1024.0);
            else
                snprintf(out, n, "%llu B", (unsigned long long)v);
        }

        u64 GetInfo(u32 type) {
            u64 v = 0;
            return R_SUCCEEDED(svcGetInfo(&v, type, CUR_PROCESS_HANDLE, 0)) ? v : 0;
        }
        bool TryInfo(u32 type, u64 *out) {
            return R_SUCCEEDED(svcGetInfo(out, type, CUR_PROCESS_HANDLE, 0));
        }
    }

    void Overlay::Toggle() {
        m_visible = !m_visible;
        if (m_visible) Reset();
    }

    void Overlay::Dump(const Counters &c) const {
        FILE *fp = fopen("sdmc:/slaunch/debug.log", "a");
        if (!fp) return;

        const time_t now = time(nullptr);
        struct tm tmv;
        localtime_r(&now, &tmv);
        char when[32];
        strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tmv);

        // Raw byte counts, not the formatted strings: the whole reason for the
        // log is to avoid a lossy round trip through "448.7 MB".
        fprintf(fp, "\n--- sLaunch debug snapshot %s (v%s) ---\n", when, SL_VERSION);
        fprintf(fp, "mem.total_available   = %llu\n", (unsigned long long)m_mem_total);
        fprintf(fp, "mem.used              = %llu\n", (unsigned long long)m_mem_used);
        fprintf(fp, "mem.peak_session      = %llu\n", (unsigned long long)m_mem_peak);
        fprintf(fp, "mem.heap_region       = %llu\n", (unsigned long long)m_heap_region);
        if (m_have_nonsys) {
            fprintf(fp, "mem.nonsystem_total   = %llu\n", (unsigned long long)m_nonsys_total);
            fprintf(fp, "mem.nonsystem_used    = %llu\n", (unsigned long long)m_nonsys_used);
        }
        fprintf(fp, "rlimit.ceiling        = %llu\n", (unsigned long long)m_mem_limit);
        fprintf(fp, "rlimit.current        = %llu\n", (unsigned long long)m_mem_rl_cur);
        fprintf(fp, "rlimit.peak           = %llu\n", (unsigned long long)m_mem_rl_peak);
        fprintf(fp, "malloc.in_use         = %llu\n", (unsigned long long)m_malloc_used);
        fprintf(fp, "malloc.arena          = %llu\n", (unsigned long long)m_malloc_arena);
        fprintf(fp, "frame.fps             = %.2f\n", m_fps);
        fprintf(fp, "frame.avg_ms          = %.3f\n", m_ms_avg);
        fprintf(fp, "frame.worst_ms        = %.3f\n", m_ms_worst);
        fprintf(fp, "cache.game_icons      = %d\n", c.app_icons);
        fprintf(fp, "cache.hb_icons        = %d\n", c.hb_icons);
        fprintf(fp, "cache.sys_icons       = %d\n", c.sys_icons);
        fprintf(fp, "menu.entries          = %d\n", c.items);
        fprintf(fp, "menu.widgets          = %d\n", c.widgets);
        fprintf(fp, "menu.ui_mode          = %d\n", c.ui_mode);
        fprintf(fp, "sys.program_id        = %016llX\n", (unsigned long long)m_program_id);
        fprintf(fp, "sys.firmware          = %u.%u.%u\n",
                HOSVER_MAJOR(hosversionGet()), HOSVER_MINOR(hosversionGet()),
                HOSVER_MICRO(hosversionGet()));
        if (m_have_thread)
            fprintf(fp, "sys.free_threads      = %llu\n", (unsigned long long)m_free_threads);
        if (m_have_sysmem) {
            fprintf(fp, "sys.dram_total        = %llu\n", (unsigned long long)m_sys_total);
            fprintf(fp, "sys.dram_used         = %llu\n", (unsigned long long)m_sys_used);
        }
        fprintf(fp, "sd.free               = %llu\n", (unsigned long long)m_sd_free);
        fprintf(fp, "sd.total              = %llu\n", (unsigned long long)m_sd_total);
        fclose(fp);
    }

    void Overlay::Reset() {
        memset(m_frame_ms, 0, sizeof(m_frame_ms));
        m_frame_i = 0;
        m_last_tick = 0;
        m_mem_peak = 0;
        m_slow_tick = 0;
    }

    void Overlay::SampleFast() {
        m_mem_total   = GetInfo(InfoType_TotalMemorySize);
        m_mem_used    = GetInfo(InfoType_UsedMemorySize);
        m_heap_region = GetInfo(InfoType_HeapRegionSize);
        m_program_id  = GetInfo(InfoType_ProgramId);
        if (m_mem_used > m_mem_peak) m_mem_peak = m_mem_used;

        // [6.0.0+]: the same figures minus the slab the kernel reserves for
        // managing this process, which is the number that actually bounds our
        // allocations.
        m_have_nonsys = TryInfo(InfoType_TotalNonSystemMemorySize, &m_nonsys_total) &&
                        TryInfo(InfoType_UsedNonSystemMemorySize,  &m_nonsys_used);
        m_have_thread = TryInfo(InfoType_FreeThreadCount, &m_free_threads);

        // The resource limit is the ceiling the *system* enforces on this
        // process, and the kernel tracks its own peak - more trustworthy than
        // our per-frame sampling, which only sees the frames we looked at.
        u64 rl_handle = 0;
        if (TryInfo(InfoType_ResourceLimit, &rl_handle) && rl_handle) {
            const Handle rl = (Handle)rl_handle;
            s64 v = 0;
            if (R_SUCCEEDED(svcGetResourceLimitLimitValue(&v, rl, LimitableResource_Memory)))
                m_mem_limit = (u64)v;
            if (R_SUCCEEDED(svcGetResourceLimitCurrentValue(&v, rl, LimitableResource_Memory)))
                m_mem_rl_cur = (u64)v;
            if (R_SUCCEEDED(svcGetResourceLimitPeakValue(&v, rl, LimitableResource_Memory)))
                m_mem_rl_peak = (u64)v;
            svcCloseHandle(rl);
        }
    }

    void Overlay::SampleSlow() {
        struct mallinfo mi = mallinfo();
        m_malloc_used  = (u64)(unsigned)mi.uordblks;
        m_malloc_arena = (u64)(unsigned)mi.arena;

        struct statvfs st;
        if (statvfs("sdmc:/", &st) == 0) {
            m_sd_free  = (u64)st.f_bfree  * st.f_frsize;
            m_sd_total = (u64)st.f_blocks * st.f_frsize;
        }

        // Whole-console DRAM. Not every firmware lets an applet ask, so this is
        // best-effort and the rows are hidden when it fails.
        u64 a = 0, b = 0;
        m_have_sysmem =
            R_SUCCEEDED(svcGetSystemInfo(&a, SystemInfoType_TotalPhysicalMemorySize, INVALID_HANDLE, 0)) &&
            R_SUCCEEDED(svcGetSystemInfo(&b, SystemInfoType_UsedPhysicalMemorySize,  INVALID_HANDLE, 0));
        if (m_have_sysmem) { m_sys_total = a; m_sys_used = b; }
    }

    void Overlay::Frame() {
        if (!m_visible) return;

        const u64 now = armGetSystemTick();
        const u64 hz  = armGetSystemTickFreq();
        if (m_last_tick != 0) {
            const float ms = (float)((double)(now - m_last_tick) * 1000.0 / (double)hz);
            m_frame_ms[m_frame_i++ % kHistory] = ms;
        }
        m_last_tick = now;

        float sum = 0, worst = 0;
        int n = (m_frame_i < kHistory) ? m_frame_i : kHistory;
        for (int i = 0; i < n; i++) {
            sum += m_frame_ms[i];
            if (m_frame_ms[i] > worst) worst = m_frame_ms[i];
        }
        m_ms_avg   = n ? sum / n : 0.0f;
        m_ms_worst = worst;
        m_fps      = (m_ms_avg > 0.0001f) ? 1000.0f / m_ms_avg : 0.0f;

        SampleFast();
        if (now - m_slow_tick > hz) { m_slow_tick = now; SampleSlow(); }
    }

    void Overlay::Draw(gfx::Gfx *gfx, const Counters &c) {
        if (!m_visible || !gfx) return;

        // Two columns. In one column this runs past the bottom of a 720p screen
        // once every optional row is present, and squeezing the line height to
        // make it fit overlaps the glyphs - the small font is 20pt, so its line
        // box is taller than the row spacing that would be needed.
        const int lh   = gfx->LineHeight(gfx::FontSize::Small);
        const int pad  = 14;
        const int gap  = 22;
        const int colW = 306;
        const int W    = pad * 2 + colW * 2 + gap;
        const int X    = gfx::Gfx::Width - W - 20;
        const int Y    = 20;

        // Rows per column, counted the way they are emitted below: a heading
        // costs a line plus its leading, a value row costs a line.
        const int leftRows  = 5 + (m_have_nonsys ? 1 : 0) + 3 + 2;
        const int rightRows = 3 + 5 + 2 + (m_have_thread ? 1 : 0)
                                        + (m_have_sysmem ? 1 : 0) + 1;
        const int headH  = lh + 6;
        const int leftH  = 3 * headH + leftRows  * lh;
        const int rightH = 3 * headH + rightRows * lh;
        const int titleH = gfx->LineHeight(gfx::FontSize::Normal) + 8;
        const int H = titleH + (leftH > rightH ? leftH : rightH) + pad * 2;

        gfx->FillRect(X, Y, W, H, kPanel);
        gfx->FillRect(X, Y, W, 2, kEdge);

        char buf[96], buf2[96], both[192];

        // Column cursor: cx is the left edge, y walks down, and values are
        // right-aligned to the column's own right edge.
        int cx = X + pad;
        int y  = Y + pad;
        auto head = [&](const char *t) {
            y += 6;
            gfx->Text(gfx::FontSize::Small, cx, y, kHead, t);
            y += lh;
        };
        auto row = [&](const char *k, const char *v, SDL_Color vc = kVal) {
            gfx->Text(gfx::FontSize::Small, cx, y, kKey, k);
            const int vw = gfx->TextWidth(gfx::FontSize::Small, v);
            gfx->Text(gfx::FontSize::Small, cx + colW - vw, y, vc, v);
            y += lh;
        };
        auto rowBytes = [&](const char *k, u64 v, SDL_Color vc = kVal) {
            FormatBytes(buf, sizeof(buf), v);
            row(k, buf, vc);
        };
        auto pair = [&](const char *k, u64 a, u64 b, const char *sep = " / ") {
            FormatBytes(buf,  sizeof(buf),  a);
            FormatBytes(buf2, sizeof(buf2), b);
            snprintf(both, sizeof(both), "%s%s%s", buf, sep, buf2);
            row(k, both);
        };

        gfx->Text(gfx::FontSize::Normal, cx, y, kHead, "sLaunch debug");
        snprintf(buf, sizeof(buf), "v%s  L+R+Minus", SL_VERSION);
        {
            const int vw = gfx->TextWidth(gfx::FontSize::Small, buf);
            gfx->Text(gfx::FontSize::Small, X + W - pad - vw, y + 6, kKey, buf);
        }
        y += titleH;
        const int bodyTop = y;

        // ---- left column: memory, the reason this overlay exists ----
        head("MEMORY (this applet)");
        rowBytes("Total available", m_mem_total, kGood);
        {
            const double pct = m_mem_total ? (double)m_mem_used * 100.0 / (double)m_mem_total : 0.0;
            FormatBytes(buf2, sizeof(buf2), m_mem_used);
            snprintf(buf, sizeof(buf), "%s (%.0f%%)", buf2, pct);
            row("Used", buf, pct > 85.0 ? kHot : kVal);
        }
        rowBytes("Peak this session", m_mem_peak);
        rowBytes("Free", m_mem_total > m_mem_used ? m_mem_total - m_mem_used : 0);
        rowBytes("Heap region", m_heap_region);
        if (m_have_nonsys) pair("Non-system", m_nonsys_used, m_nonsys_total);

        head("RESOURCE LIMIT");
        rowBytes("Ceiling", m_mem_limit, kGood);
        rowBytes("Current", m_mem_rl_cur);
        rowBytes("Peak (kernel)", m_mem_rl_peak);

        head("MALLOC (newlib)");
        rowBytes("In use", m_malloc_used);
        rowBytes("Arena", m_malloc_arena);

        // ---- right column ----
        cx = X + pad + colW + gap;
        y  = bodyTop;

        head("FRAME");
        snprintf(buf, sizeof(buf), "%.1f", m_fps);
        row("FPS", buf, m_fps < 50.0f ? kHot : kGood);
        snprintf(buf, sizeof(buf), "%.2f ms", m_ms_avg);
        row("Avg", buf);
        snprintf(buf, sizeof(buf), "%.2f ms", m_ms_worst);
        row("Worst of 60", buf, m_ms_worst > 20.0f ? kHot : kVal);

        head("CACHES");
        snprintf(buf, sizeof(buf), "%d", c.app_icons);  row("Game icons", buf);
        snprintf(buf, sizeof(buf), "%d", c.hb_icons);   row("Homebrew icons", buf);
        snprintf(buf, sizeof(buf), "%d", c.sys_icons);  row("System icons", buf);
        snprintf(buf, sizeof(buf), "%d", c.items);      row("Menu entries", buf);
        snprintf(buf, sizeof(buf), "%d", c.widgets);    row("Widgets", buf);

        head("SYSTEM");
        snprintf(buf, sizeof(buf), "%016llX", (unsigned long long)m_program_id);
        row("Program id", buf);
        {
            const u32 v = hosversionGet();
            snprintf(buf, sizeof(buf), "%u.%u.%u",
                     HOSVER_MAJOR(v), HOSVER_MINOR(v), HOSVER_MICRO(v));
            row("Firmware", buf);
        }
        if (m_have_thread) {
            snprintf(buf, sizeof(buf), "%llu", (unsigned long long)m_free_threads);
            row("Free threads", buf);
        }
        if (m_have_sysmem) pair("Console DRAM", m_sys_used, m_sys_total);
        pair("SD free", m_sd_free, m_sd_total);
    }

} // namespace sl::menu::dbg
