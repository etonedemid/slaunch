#include <sl/menu/dbg/Debug.hpp>
#include <switch.h>
#include <cstdio>
#include <unordered_map>
#include <vector>
#include <algorithm>

// Sampling profiler for the menu's main thread, on the console only.
//
// Runs when sdmc:/slaunch/config/profile exists: from three seconds in, a
// second thread pauses the main thread about every 2 ms, reads its registers,
// walks its frame-pointer chain and lets it go again. After twenty seconds
// the counts are appended to sdmc:/slaunch/perf.log (a file that already
// exists, so the menu's MTP server lists it) as offsets into the NSO -
// "self" is where the thread was, "total" is every function on its stack.
// Symbolize with addr2line against build(-vk)/sMenu.elf.
namespace sl::menu::dbg {

#ifndef SL_SIMULATOR
    namespace {
        Handle g_main = INVALID_HANDLE;
        Thread g_thread;

        u64 Base() { return (u64)(uintptr_t)&StartProfiler; }

        void Run(void *) {
            svcSleepThread(3'000'000'000ULL);
            std::unordered_map<u64, u32> self, total;
            u64 samples = 0;
            MemoryInfo stack{};
            const u64 end = armGetSystemTick() + armNsToTicks(20'000'000'000ULL);
            while (armGetSystemTick() < end) {
                svcSleepThread(2'000'000);
                if (R_FAILED(svcSetThreadActivity(g_main, ThreadActivity_Paused))) break;
                ThreadContext ctx{};
                const bool ok = R_SUCCEEDED(svcGetThreadContext3(&ctx, g_main));
                u64 pcs[24];
                int n = 0;
                if (ok) {
                    pcs[n++] = ctx.pc.x;
                    if (!stack.size || ctx.sp < stack.addr || ctx.sp >= stack.addr + stack.size) {
                        u32 pi;
                        svcQueryMemory(&stack, &pi, ctx.sp);
                    }
                    // Frame records: {previous fp, return address}. Anything
                    // outside the stack ends the walk.
                    u64 fp = ctx.fp;
                    while (n < 24 && fp >= stack.addr && fp + 16 <= stack.addr + stack.size && !(fp & 7)) {
                        const u64 *rec = (const u64 *)fp;
                        if (!rec[1]) break;
                        pcs[n++] = rec[1];
                        if (rec[0] <= fp) break;
                        fp = rec[0];
                    }
                }
                svcSetThreadActivity(g_main, ThreadActivity_Runnable);
                if (!ok) continue;
                samples++;
                self[pcs[0]]++;
                for (int i = 0; i < n; i++) {
                    bool dup = false;   // recursion counts once per sample
                    for (int j = 0; j < i; j++) dup |= pcs[j] == pcs[i];
                    if (!dup) total[pcs[i]]++;
                }
            }

            FILE *fp = fopen("sdmc:/slaunch/perf.log", "a");
            if (!fp) return;
            fprintf(fp, "samples %llu\nbase %llx (StartProfiler)\n",
                    (unsigned long long)samples, (unsigned long long)Base());
            auto dump = [&](const char *what, const std::unordered_map<u64, u32> &m) {
                std::vector<std::pair<u32, u64>> v;
                for (auto &kv : m) v.push_back({ kv.second, kv.first });
                std::sort(v.rbegin(), v.rend());
                if (v.size() > 400) v.resize(400);
                for (auto &e : v)
                    fprintf(fp, "%s %u %llx\n", what, e.first, (unsigned long long)(e.second - Base()));
            };
            dump("self", self);
            dump("total", total);
            fclose(fp);
        }
    }

    void StartProfiler() {
        FILE *f = fopen("sdmc:/slaunch/config/profile", "r");
        if (!f) return;
        fclose(f);
        g_main = threadGetCurHandle();
        if (R_SUCCEEDED(threadCreate(&g_thread, Run, nullptr, nullptr, 0x10000, 0x2B, -2)))
            threadStart(&g_thread);
    }
#else
    void StartProfiler() {}
#endif

} // namespace sl::menu::dbg
