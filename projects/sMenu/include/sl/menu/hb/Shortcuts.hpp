#pragma once
#include <switch.h>
#include <string>
#include <vector>

// Launchable shortcuts: a .nro plus the argv to hand it.
//
// Everything sLaunch launched before this was a bare .nro told only its own
// path, which is all hbmenu ever passes. But the argv line has been plumbed
// end to end for a while (Commands.hpp -> sSystem -> hbtarget.txt line 2 ->
// hbloader), and an emulator core given a ROM path is exactly that: one .nro,
// one argument. So a "RetroArch shortcut" needs no new launch machinery - only
// somewhere for the argv and a display name to come from.
//
// Two sources, both optional, merged into one list:
//
//   1. RetroArch's own playlists (sdmc:/retroarch/playlists/*.lpl). These are
//      already what the user curated in RetroArch: one file per system, every
//      entry naming its ROM, its core and its label. Nothing to configure -
//      scan your ROMs in RetroArch once and the columns appear.
//
//   2. sdmc:/slaunch/config/users/<id>/shortcuts.txt, a tab-separated line per
//      shortcut. For emulators that are not RetroArch, for a core/ROM pair the
//      scanner missed, and for any .nro that takes arguments at all.
//
// `category` is what turns these into XMB columns: entries sharing one get a
// column of their own, named after it. Playlists set it to the system name, so
// "Nintendo - Super Nintendo Entertainment System" becomes an SNES column.

namespace sl::menu::hb {

    struct Shortcut {
        std::string nro;        // the .nro to run (an emulator core, usually)
        std::string argv;       // full argv line for hbloader, already quoted
        std::string name;       // display name
        std::string category;   // XMB column; empty = the plain Homebrew column
        std::string icon_path;  // box art on the card, or empty
        u64         icon_key = 0;  // hash of the shortcut, for the texture cache
    };

    // Read both sources. Pure file I/O and no NRO parsing, but it walks the
    // playlists directory and every entry in it, so callers run it off the
    // main thread the same way they run Scan().
    std::vector<Shortcut> ScanShortcuts();

} // namespace sl::menu::hb
