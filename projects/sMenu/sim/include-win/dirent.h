// A dirent implementation with d_type, for the Windows cross-build only.
//
// MinGW does ship a <dirent.h>, but its struct has no d_type field - and the
// menu relies on d_type to tell files from directories while scanning homebrew,
// themes, icon packs and the album. Falling back to a stat() per entry would
// work but would be far slower over a large card, so this fills d_type in
// directly from the Win32 find data, which already carries it.
//
// Only the parts the menu uses are implemented: opendir, readdir, closedir.

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <stdlib.h>
#include <string.h>

#define DT_UNKNOWN 0
#define DT_REG     8
#define DT_DIR     4

struct dirent {
    unsigned char d_type;
    char          d_name[MAX_PATH];
};

typedef struct DIR {
    HANDLE          h;
    WIN32_FIND_DATAA fd;
    int             first;     // FindFirstFile already produced an entry
    struct dirent   ent;
} DIR;

#ifdef __cplusplus
extern "C" {
#endif

static inline DIR *opendir(const char *path) {
    if (!path || !*path) return NULL;

    // FindFirstFile wants a wildcard, and tolerates either separator.
    size_t n = strlen(path);
    char  *pat = (char *)malloc(n + 3);
    if (!pat) return NULL;
    memcpy(pat, path, n);
    if (n && (path[n - 1] == '/' || path[n - 1] == '\\')) n--;
    pat[n] = '\\';
    pat[n + 1] = '*';
    pat[n + 2] = '\0';

    DIR *d = (DIR *)calloc(1, sizeof(DIR));
    if (!d) { free(pat); return NULL; }

    d->h = FindFirstFileA(pat, &d->fd);
    free(pat);
    if (d->h == INVALID_HANDLE_VALUE) { free(d); return NULL; }
    d->first = 1;
    return d;
}

static inline struct dirent *readdir(DIR *d) {
    if (!d) return NULL;
    if (d->first) {
        d->first = 0;
    } else if (!FindNextFileA(d->h, &d->fd)) {
        return NULL;
    }
    d->ent.d_type = (d->fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                        ? DT_DIR : DT_REG;
    strncpy(d->ent.d_name, d->fd.cFileName, sizeof(d->ent.d_name) - 1);
    d->ent.d_name[sizeof(d->ent.d_name) - 1] = '\0';
    return &d->ent;
}

static inline int closedir(DIR *d) {
    if (!d) return -1;
    if (d->h != INVALID_HANDLE_VALUE) FindClose(d->h);
    free(d);
    return 0;
}

#ifdef __cplusplus
}
#endif
