#pragma once
#include <switch.h>
#include <cstdio>

// What sSystem does for the menu as a separate process, for the build that runs
// inside qlaunch-ext's loader instead (SL_QEXT). See QextBackend.cpp.

// The loader's exports (loader/src/qext.hpp), resolved when the menu is loaded.
extern "C" {
    int    qext_heap_setup(unsigned total_mb, unsigned small_mb);   // scripts/qext/loader.patch
    unsigned long qext_arena_free_bytes(void);                      // scripts/qext/loader.patch
    unsigned long qext_arena_size(void);                            // scripts/qext/loader.patch
    unsigned qext_heap_release(FILE *also);  // scripts/qext/loader.patch
    int    qext_album_refresh(void);
    u64    qext_suspended_title(void);
    int    qext_game_running(void);
    void   qext_display_size(int *w, int *h);

    int    qext_refresh_titles(void);
    int    qext_title_count(void);
    u64    qext_title_id(int index);
    int    qext_title_name(int index, char *out, unsigned cap);
    int    qext_title_original_name(int index, char *out, unsigned cap);
    int    qext_title_icon_size(int index);
    int    qext_title_icon(int index, void *out, unsigned cap);
    int    qext_title_is_virtual(u64 tid);
    int    qext_title_rename(u64 tid, const char *name);

    int    qext_battery_percent(void);
    int    qext_battery_charging(void);
    int    qext_airplane_mode(void);
    int    qext_account_list(AccountUid *out, int cap);
    int    qext_account_image_size(AccountUid uid);
    int    qext_account_image(AccountUid uid, void *out, unsigned cap);

    typedef struct SwkbdOptions {
        const char *initialText;
        const char *headerText;
        unsigned minLength;
        unsigned maxLength;
        int blurBackground;
    } SwkbdOptions;
    int    qext_swkbd_show_ex(const SwkbdOptions *options, char *out, unsigned cap);
}
int qext_titles_changed(void);   // not extern "C" in the loader

namespace sl::menu::qext {

    // Removes a homebrew redirect a crash left behind in the album slot.
    void CleanUpLeftovers();
    // The screen came back to the menu: end the album redirect, run a queued homebrew.
    void OnBackInMenu();
    // A RestartMenu command is pending. Clears it.
    bool TakeRestart();
    // The browser was asked for. Clears it. The menu has to be gone before StartWeb.
    bool TakeWebRequest();
    void StartWeb(unsigned heap_mb_left);
    // A user rename, into the loader's registry. Empty restores the original name.
    void RenameTitle(u64 tid, const char *name);
}
