// Host run of the qlaunch-ext loader's heap code (lifted from loader/src/main.cpp):
// boot, grow to 256 MB, a menu's big blocks all freed by its unload, release for
// the browser (must end at 64 MB or less), grow back for the reload.
//   awk '/^static constexpr size_t kHeapFloor/{on=1} /^static Result heap_set_floor\(\)/{on=0} on' ~/qlaunch-ext/loader/src/main.cpp > /tmp/heap_slice.inc
//   g++ -std=gnu++17 -w -fsanitize=address,undefined -I ~/qlaunch-ext/loader/src -I /tmp scripts/qext/test_heap.cpp ~/qlaunch-ext/loader/src/arena.cpp -o /tmp/th && /tmp/th
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include "arena.hpp"
typedef uint32_t Result; typedef uint32_t u32;
#define R_SUCCEEDED(r) ((r) == 0)
#define R_FAILED(r) ((r) != 0)
#define MAKERESULT(m, d) (1u)
enum { Module_Libnx, LibnxError_OutOfMemory };
struct Mutex { int x; };
static void mutexLock(Mutex *) {}
static void mutexUnlock(Mutex *) {}
namespace logging { template <class... A> void LogLine(const char *f, A... a) { printf("  log: "); printf(f, a...); printf("\n"); } }
static char *g_base; static size_t g_kheap;
static Result svcSetHeapSize(void **addr, size_t size) { if (size > (512u << 20)) return 1; *addr = g_base; g_kheap = size; return 0; }
char *fake_heap_start, *fake_heap_end;
static size_t g_used = 10u << 20;
static void *sbrk(long) { return fake_heap_start + g_used; }
#include "heap_slice.inc"
int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    g_base = (char *)aligned_alloc(0x200000, 512u << 20);
    __libnx_initheap();
    assert(g_heapSize == 60u << 20);
    assert(heap_set_total(256u << 20) == 0);
    printf("grown: heap %zu MB, arena %zu MB\n", g_heapSize >> 20, (size_t)(s_arenaEnd - s_arenaStart) >> 20);
    void *p[20];
    for (int i = 0; i < 20; i++) { p[i] = ArenaAlloc((i + 1) << 20, 16); assert(p[i]); }
    for (int i = 0; i < 20; i++) arena::Release(p[i]);
    unsigned left = heap_release_arena(nullptr);
    printf("released: %u MB, kernel %zu MB, arena %zu MB\n", left, g_kheap >> 20, (size_t)(s_arenaEnd - s_arenaStart) >> 20);
    assert(left <= 64 && g_kheap == g_heapSize);
    assert(heap_set_total(256u << 20) == 0);
    printf("regrown: heap %zu MB, arena %zu MB free %zu MB\n", g_heapSize >> 20, (size_t)(s_arenaEnd - s_arenaStart) >> 20, arena::FreeBytes() >> 20);
    void *big = ArenaAlloc(150u << 20, 16);
    assert(big && (char *)big >= s_arenaStart && (char *)big < s_arenaEnd);
    printf("heap sim ok\n");
}
