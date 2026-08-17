// statvfs on Windows, for the cross-build only.
//
// The debug overlay reports free space on the card with statvfs("sdmc:/"),
// which MinGW does not provide. GetDiskFreeSpaceEx answers the same question,
// so the overlay shows the free space of whatever volume holds the simulated
// card - which is the closest true answer available.

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

typedef unsigned long long fsblkcnt_t;
typedef unsigned long long fsfilcnt_t;

struct statvfs {
    unsigned long f_bsize;
    unsigned long f_frsize;
    fsblkcnt_t    f_blocks;
    fsblkcnt_t    f_bfree;
    fsblkcnt_t    f_bavail;
    fsfilcnt_t    f_files;
    fsfilcnt_t    f_ffree;
    fsfilcnt_t    f_favail;
    unsigned long f_fsid;
    unsigned long f_flag;
    unsigned long f_namemax;
};

#ifdef __cplusplus
extern "C" {
#endif

// Declared here and defined in sim/src/win_path.cpp, where the sdmc: prefix can
// be resolved to the real directory first.
int statvfs(const char *path, struct statvfs *buf);

#ifdef __cplusplus
}
#endif
