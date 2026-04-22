#pragma once
#include <switch.h>

namespace sl::sys::la {

    // The AppletHolder for the currently-running library applet (sMenu)
    extern AppletHolder g_MenuHolder;
    extern bool         g_MenuRunning;

    // Launch sMenu as a library applet in the eShop slot via ECS redirect
    Result LaunchMenu(AppletId menu_applet_id);

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

    // Open a system library applet (album, web, mii editor, etc.)
    // while keeping sMenu alive in the background
    Result OpenSystemApplet(AppletId id, const void *args, size_t args_size,
                            void *out, size_t out_size);

} // namespace sl::sys::la
