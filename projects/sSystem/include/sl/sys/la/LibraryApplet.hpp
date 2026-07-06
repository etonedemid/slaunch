#pragma once
#include <switch.h>

namespace sl::sys::la {

    // The AppletHolder for the currently-running library applet (sMenu)
    extern AppletHolder g_MenuHolder;
    extern bool         g_MenuRunning;

    // Launch sMenu as a library applet in the eShop slot via ECS redirect.
    // The status blob is pushed as the applet's input data BEFORE it starts,
    // so sMenu can read it with appletPopInData at launch.
    Result LaunchMenu(AppletId menu_applet_id, const void *status, size_t status_size);

    // Stop sMenu (graceful exit with 15s timeout, then force)
    void   StopMenu();

    // Is sMenu's holder state still alive?
    bool   IsMenuAlive();

    // Push data into sMenu's input storage
    Result PushToMenu(const void *data, size_t size);

    // Pop data from sMenu's output storage
    Result PopFromMenu(void *out, size_t size);

    // Raw storage push/pop for the SMI protocol
    Result PushStorage(AppletStorage &st);
    Result PopStorage(AppletStorage &st);

    // Open a system library applet (album, web, mii editor, ...) and block
    // until it closes. Each applet needs a specific common-args `la_version`
    // and its own input struct(s); a negative version means "push no common
    // args at all" (required by e.g. MiiEdit). Passing the wrong version or a
    // null/zero input where the applet expects one makes it fault -- which is
    // why MiiEdit was crashing. Modelled on uLaunch's la::Start.
    Result OpenSystemApplet(AppletId id, s32 la_version,
                            const void *in1 = nullptr, size_t in1_size = 0,
                            const void *in2 = nullptr, size_t in2_size = 0);

} // namespace sl::sys::la
