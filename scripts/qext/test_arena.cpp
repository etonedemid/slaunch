// g++ -std=c++17 -fsanitize=address,undefined -I ~/qlaunch-ext/loader/src scripts/qext/test_arena.cpp -o /tmp/ta && /tmp/ta
#include "arena_core.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

int main() {
    const size_t total = 64 * 1024 * 1024;
    char *mem = (char *)aligned_alloc(0x200000, total);
    arena::Insert(mem, total);
    assert(arena::FreeBytes() == total);

    // Random mix, checking alignment, no overlap (fill patterns) and full recovery.
    struct B { unsigned char *p; size_t n; unsigned char v; };
    std::vector<B> live;
    srand(1);
    for (int i = 0; i < 200000; i++) {
        if (live.empty() || rand() % 2) {
            size_t n = 1 + rand() % (1 << (rand() % 20));
            size_t align = (size_t)1 << (rand() % 13);
            auto *p = (unsigned char *)arena::Alloc(n, align);
            if (!p) continue;
            assert((uintptr_t)p % (align < 16 ? 16 : align) == 0);
            assert(p >= (unsigned char *)mem && p + n <= (unsigned char *)mem + total);
            assert(arena::Usable(p) >= n);
            unsigned char v = rand();
            memset(p, v, n);
            live.push_back({p, n, v});
        } else {
            size_t k = rand() % live.size();
            B b = live[k];
            for (size_t j = 0; j < b.n; j++) assert(b.p[j] == b.v);
            arena::Release(b.p);
            live[k] = live.back();
            live.pop_back();
        }
    }
    for (auto &b : live) {
        for (size_t j = 0; j < b.n; j++) assert(b.p[j] == b.v);
        arena::Release(b.p);
    }
    assert(arena::FreeBytes() == total);
    assert(arena::g_free && !arena::g_free->next);   // all coalesced back into one

    // Shrink: a live block near the start limits it, nothing else does.
    void *a = arena::Alloc(1000, 16);
    char *top = arena::TrimEnd(mem + total);
    assert(top > mem && top < mem + total);
    arena::Insert(top, mem + total - top);   // grow back
    arena::Release(a);
    assert(arena::TrimEnd(mem + total) == mem);
    puts("arena ok");
}
