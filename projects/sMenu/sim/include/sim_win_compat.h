// Windows compatibility for the simulator, force-included into every source of
// the Windows build only (see the `win` target in sim/Makefile).
//
// Two jobs.
//
// 1. Redirect "sdmc:/..." paths.
//
//    On Linux the simulator needs none of this: a directory can simply be named
//    "sdmc:" and every path already in the menu resolves against it. Windows
//    does not allow a colon in a filename, so that trick is impossible and the
//    paths have to be rewritten on their way into the C library instead.
//
//    The rewriting is done with macros that wrap each call rather than with
//    replacement functions, because a macro cannot recursively expand its own
//    name - `#define fopen(p, m) fopen(SimWinPath(p), m)` calls the real fopen.
//    That means no wrapper needs to know any of the SDL types involved.
//
//    The real headers are therefore included FIRST, below, before any macro is
//    defined. Defining them earlier would rewrite the library's own prototypes
//    (`SDL_Surface *IMG_Load(const char *file)` is itself a call-shaped use of
//    the name) and nothing would compile.
//
// 2. Fill in the few POSIX pieces MinGW does not have but the menu uses.
//
// Nothing in the menu changes for any of this, which is the whole point: the
// Windows build runs the same sources as the console.

#pragma once

#ifdef _WIN32

// windows.h arrives whether we ask for it or not - SDL.h pulls it in - so set
// its options before anything can include it.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

// ---- real declarations, before any macro exists ---------------------------
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <direct.h>
#include <dirent.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>
#include <SDL2/SDL_mixer.h>

// <algorithm> declares std::remove(first, last, value) - three arguments, and
// the same name as C's one-argument remove(). Pulling it in here, before the
// macro below exists, is what keeps the macro from mangling that declaration.
// The rest are included for the same reason: any header pulled in later that
// declares one of the wrapped names in call shape would break the same way.
#include <algorithm>
#include <string>
#include <vector>
#include <memory>
#include <functional>

// windows.h defines a pile of unsuffixed API names as macros selecting the A/W
// variant, and several collide with ordinary method names in this codebase -
// Gfx::LoadImage became Gfx::LoadImageA. Undefining them costs nothing here:
// none of the Win32 functions behind these names are used.
#undef LoadImage
#undef DrawText
#undef GetObject
#undef CreateWindow
#undef PlaySound
#undef SendMessage
#undef GetMessage
#undef CreateFile
#undef DeleteFile
#undef CopyFile
#undef MoveFile
#undef GetCurrentTime
#undef Rectangle
#undef Polygon
#undef ERROR
#undef near
#undef far
#undef small

#ifdef __cplusplus
extern "C" {
#endif

// Rewrites a leading "sdmc:/" to the simulated card root and returns a pointer
// valid until several more calls have been made. Any other path is returned
// unchanged, so wrapping a call is always safe even when it never sees an
// sdmc: path.
const char *SimWinPath(const char *p);

// Sets the directory that "sdmc:/" maps to. Called once at start-up.
void SimWinSetRoot(const char *root);

#ifdef __cplusplus
}
#endif

// ---- POSIX pieces MinGW lacks ---------------------------------------------
#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#endif
#ifndef S_ISREG
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#endif

// MSVCRT's localtime is already thread-local, so this is the same guarantee
// localtime_r gives, with the arguments the menu expects.
#ifndef localtime_r
#define localtime_r(t, out) (localtime_s((out), (t)) == 0 ? (out) : NULL)
#endif

// mallinfo is a glibc extension the debug overlay uses to report heap usage.
// There is no equivalent on Windows, so it reports zero and the overlay shows
// an empty heap row - the same thing it shows on a console where the call is
// unavailable. Only the two fields the overlay reads are declared.
struct mallinfo {
    int arena;
    int ordblks;
    int hblks;
    int hblkhd;
    int usmblks;
    int fsmblks;
    int uordblks;
    int fordblks;
    int keepcost;
};

static inline struct mallinfo mallinfo(void) {
    struct mallinfo m;
    memset(&m, 0, sizeof(m));
    return m;
}

// ---- path redirection ------------------------------------------------------
#define fopen(p, m)         fopen(SimWinPath(p), m)
#define opendir(p)          opendir(SimWinPath(p))
#define stat(p, b)          stat(SimWinPath(p), b)
// `remove` is deliberately NOT wrapped here. It is redirected at link time
// instead, with -Wl,--wrap=remove and __wrap_remove() in src/win_path.cpp.
//
// A macro cannot work for this name. C's remove() takes a path, but the
// standard library and sol2 both declare their own: std::remove(first, last,
// value) and stack::remove(L, index, count) take three arguments, and
// std::forward_list::remove(value) takes one - so neither a fixed-arity macro
// nor a count-dispatching one can tell a path from a list element. Dispatching
// on the symbol at link time separates them exactly, because only the C
// function has that symbol.
// MinGW's mkdir takes no mode argument, so the menu's 0777 is dropped. Windows
// has no permission bits to apply it to.
#define mkdir(p, m)         _mkdir(SimWinPath(p))
// Two rewritten paths are alive at once here, which is why SimWinPath rotates
// its buffers rather than using a single one.
#define rename(a, b)        rename(SimWinPath(a), SimWinPath(b))

#define IMG_Load(p)         IMG_Load(SimWinPath(p))
#define TTF_OpenFont(p, s)  TTF_OpenFont(SimWinPath(p), s)
#define Mix_LoadWAV(p)      Mix_LoadWAV(SimWinPath(p))
#define Mix_LoadMUS(p)      Mix_LoadMUS(SimWinPath(p))

#endif // _WIN32
