#pragma once
#include <switch.h>

// Atmosphere External Content Storage:
// We register sMenu's NSO path so that when the system tries to load
// the library applet slot (eShop, AppletId 0x10), it loads our binary instead.

namespace sl::sys::ecs {

    constexpr AppletId MenuAppletId = AppletId_LibraryAppletShop; // 0x10, eShop slot

    // Register sMenu's NSO with ldr:shel so the next LA launch in this slot
    // runs our code. Must be called before LaunchMenu().
    Result RegisterMenu(const char *nso_path);

    // Un-register (called on shutdown)
    void   UnregisterMenu();

} // namespace sl::sys::ecs
