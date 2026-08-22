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

    // We take over the "shop" applet slot. Which *program* that slot launches
    // is decided by a table inside `am`, and that table is not the same on
    // every firmware: the shop applet is 0x...100B ("ShopN") up to [16.x] and
    // 0x...1042 ("systemWeb", the web-based eShop) on newer firmware -- the
    // latter confirmed empirically on 18.x. Nothing lets us read that table, so
    // we register our exefs for *every* program id the slot is known to resolve
    // to and let the firmware launch whichever one it believes in. Registering
    // an id this firmware never launches costs one ldr:shel session and nothing
    // else. (The NPDM's own program id does not have to match -- uLaunch serves
    // uMenu, whose NPDM says 0x...FFFF, into whatever slot it is configured for.)
    struct MenuSlot {
        AppletId    applet_id;
        const char *name;
        u64         program_ids[2];
        u8          program_id_count;
    };

    // Tried in order, first to last. The shop slot is preferred because web
    // applets get the largest library-applet heap, which is what the menu's
    // wallpapers and covers live in; offlineWeb is another web-applet slot with
    // a comparable reservation, and unlike album/myPage the menu has no entry
    // that opens it, so taking it over costs the user nothing. The daemon falls
    // through to it only when the shop slot never brings the menu up.
    constexpr MenuSlot MenuSlots[] = {
        { AppletId_LibraryAppletShop,       "shop",
          { 0x0100000000001042ULL, 0x010000000000100BULL }, 2 },
        { AppletId_LibraryAppletOfflineWeb, "offlineWeb",
          { 0x010000000000100FULL, 0 },                     1 },
    };
    constexpr size_t MenuSlotCount = sizeof(MenuSlots) / sizeof(MenuSlots[0]);

    // One line holding a program id ("0x010000000000100B"), for a console whose
    // firmware maps the slots differently again: the daemon serves the menu into
    // that program's slot first, and only drops back to the list above if that
    // slot never brings the menu up (a typo here must not cost a console).
    constexpr const char *MenuSlotOverridePath = "sdmc:/slaunch/config/takeover.txt";

    // The applet slot `am` launches program_id from, or AppletId_None if it is
    // not a library-applet program.
    AppletId AppletIdForProgramId(u64 program_id);

    // SD path (relative to the SD root) of sMenu's exefs folder.
    constexpr const char *MenuExefsDir = "/slaunch/bin/sMenu";

    // Homebrew menu: nx-hbloader served into the album (PhotoViewer) applet
    // slot -- the classic "hbmenu over album" mechanism. Launched with no
    // target, nx-hbloader loads sdmc:/hbmenu.nro. We unregister afterwards so
    // the real Album shortcut keeps working.
    constexpr AppletId HbloaderAppletId  = AppletId_LibraryAppletPhotoViewer;
    constexpr u64      HbloaderProgramId = 0x010000000000100DULL; // album applet
    constexpr const char *HbloaderExefsDir = "/slaunch/bin/hbloader";
    // Same loader with an application_type=1 npdm: when served into a donor game's
    // slot the process becomes a real application, so the loaded .nro's libnx can
    // open an application proxy (am) and run with full RAM instead of failing
    // InitFail_AM. Used only for "run homebrew as an application".
    constexpr const char *HbloaderAppExefsDir = "/slaunch/bin/hbloader_app";

    // Start the sf server thread that serves ECS filesystem sessions. Idempotent.
    Result InitializeServer();

    // Register an SD exefs folder as external code for program_id (ldr:shel cmd
    // 65000 + an in-process fs server). exefs_path is relative to the SD root.
    Result RegisterExternalContent(u64 program_id, const char *exefs_path);

    // Stop overriding program_id (ldr:shel cmd 65001). Used to restore the real
    // album applet after the homebrew menu closes.
    Result UnregisterExternalContent(u64 program_id);

} // namespace sl::sys::ecs
