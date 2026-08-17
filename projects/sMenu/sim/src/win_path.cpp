// Implementation of the Windows path redirection declared in
// sim_win_compat.h. Built only for the Windows cross-build.

#ifdef _WIN32

#include <sim_win_compat.h>
#include <sys/statvfs.h>


namespace {

    // A small ring, because more than one rewritten path can be alive at the
    // same time - rename() takes two, and a caller is free to hold one across
    // another call. Four is comfortably more than anything the menu does.
    //
    // Per thread, and that is not optional. The menu rewrites paths from every
    // worker it runs - the screenshot decoder calls IMG_Load, the fetch worker
    // calls fopen and rename - while the main thread is doing stat and opendir
    // for the screen it is drawing. Sharing one ring meant two threads writing
    // the same buffer, which corrupts the heap and surfaces later as a
    // std::bad_alloc somewhere entirely unrelated.
    //
    // Plain char arrays rather than std::string: a thread_local with a
    // non-trivial destructor needs TLS teardown support that MinGW does not
    // handle reliably for the detached threads the menu uses, and made every
    // run fault on exit. Nothing here needs to allocate.
    constexpr int    kSlots   = 4;
    constexpr size_t kMaxPath = 1024;   // FS_MAX_PATH is 0x301; this is ample
    thread_local char     g_slot[kSlots][kMaxPath];
    thread_local unsigned g_next = 0;

    // Written once at start-up, before any worker exists, then only read.
    char g_root[kMaxPath] = "sdmc";

}   // namespace

extern "C" void SimWinSetRoot(const char *root) {
    if (root && *root) snprintf(g_root, sizeof(g_root), "%s", root);
}

// remove(), redirected at link time rather than by macro - see the note in
// sim_win_compat.h for why this one cannot be done with the preprocessor.
extern "C" int __real_remove(const char *p);
extern "C" int __wrap_remove(const char *p) { return __real_remove(SimWinPath(p)); }

// Free space on the volume holding the simulated card. Declared in
// include-win/sys/statvfs.h; only the fields the debug overlay reads are set.
extern "C" int statvfs(const char *path, struct statvfs *buf) {
    if (!buf) return -1;
    memset(buf, 0, sizeof(*buf));

    ULARGE_INTEGER avail{}, total{}, free_bytes{};
    if (!GetDiskFreeSpaceExA(SimWinPath(path), &avail, &total, &free_bytes))
        return -1;

    // A block size of 1 reports the counts directly in bytes, which is what the
    // overlay multiplies back out anyway.
    buf->f_bsize  = 1;
    buf->f_frsize = 1;
    buf->f_blocks = total.QuadPart;
    buf->f_bfree  = free_bytes.QuadPart;
    buf->f_bavail = avail.QuadPart;
    return 0;
}

extern "C" const char *SimWinPath(const char *p) {
    if (!p) return p;

    // Everything the menu opens on the card starts with this. Anything else -
    // the simulator's own screenshot directory, an absolute Windows path from
    // a command-line flag - is passed straight through.
    static const char kPrefix[] = "sdmc:/";
    static const size_t kPrefixLen = sizeof(kPrefix) - 1;
    if (strncmp(p, kPrefix, kPrefixLen) != 0) return p;

    char *out = g_slot[g_next];
    g_next = (g_next + 1) % kSlots;
    snprintf(out, kMaxPath, "%s/%s", g_root, p + kPrefixLen);
    return out;
}

#endif // _WIN32
