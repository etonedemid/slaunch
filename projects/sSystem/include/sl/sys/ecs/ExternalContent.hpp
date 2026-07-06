#pragma once
#include <switch.h>

// Atmosphere External Content Storage (ECS).
// Registers a folder on the SD card (containing the applet's `main` +
// `main.npdm`) as the external code for an applet program id, so when that
// applet slot is launched Atmosphere's loader reads our menu instead.
//
// The register + filesystem-serve mechanism is adapted from uLaunch
// (https://github.com/Xortroll/uLaunch, GPLv2, (C) Xortroll & contributors).

namespace sl::sys::ecs {

    // We take over the "shop" applet slot. On current firmware libnx's
    // AppletId_LibraryAppletShop launches program 0x…1042 (systemWeb, the
    // web-based shop) -- confirmed empirically -- so the ECS program id and the
    // applet's NPDM (sMenu.json) must both be 0x…1042 for our menu to be served.
    constexpr AppletId MenuAppletId  = AppletId_LibraryAppletShop;
    constexpr u64      MenuProgramId = 0x0100000000001042ULL;
    // SD path (relative to the SD root) of sMenu's exefs folder.
    constexpr const char *MenuExefsDir = "/slaunch/bin/sMenu";

    // Homebrew menu: nx-hbloader served into the album (PhotoViewer) applet
    // slot -- the classic "hbmenu over album" mechanism. Launched with no
    // target, nx-hbloader loads sdmc:/hbmenu.nro. We unregister afterwards so
    // the real Album shortcut keeps working.
    constexpr AppletId HbloaderAppletId  = AppletId_LibraryAppletPhotoViewer;
    constexpr u64      HbloaderProgramId = 0x010000000000100DULL; // album applet
    constexpr const char *HbloaderExefsDir = "/slaunch/bin/hbloader";

    // Start the sf server thread that serves ECS filesystem sessions. Idempotent.
    Result InitializeServer();

    // Register an SD exefs folder as external code for program_id (ldr:shel cmd
    // 65000 + an in-process fs server). exefs_path is relative to the SD root.
    Result RegisterExternalContent(u64 program_id, const char *exefs_path);

    // Stop overriding program_id (ldr:shel cmd 65001). Used to restore the real
    // album applet after the homebrew menu closes.
    Result UnregisterExternalContent(u64 program_id);

} // namespace sl::sys::ecs
