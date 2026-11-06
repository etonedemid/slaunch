#pragma once
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

// Going from the daemon (sSystem + a separate sMenu) to the qlaunch-ext build. Both
// live at the same exefs path, so installing overwrites the daemon; this is the
// part around that. Nothing here is Switch-specific: `root` is "sdmc:" on the
// console, and scripts/qext/test_upgrade_installer.cpp runs it on a PC.
namespace upgrade {

    inline bool Exists(const std::string &p) { struct stat st; return stat(p.c_str(), &st) == 0; }

    inline constexpr const char *kExefs = "/atmosphere/contents/0100000000001000/exefs";
    inline constexpr const char *kMenu  = "/qlaunch-ext/menus/menu.dnro";

    // Running sSystem, disabled or not, as opposed to nothing or the new build.
    inline bool HasOldInstall(const std::string &root) {
        return !Exists(root + kMenu) &&
               (Exists(root + kExefs + "/main") ||
                Exists(root + "/slaunch/backup/0100000000001000.disabled/exefs/main"));
    }

    // The files that decide what boots. They go last: if anything before them fails,
    // the console still boots whatever it had, instead of a loader with no modules.
    inline bool IsBootEntry(const std::string &zip_name) {
        return zip_name.compare(0, 37, "atmosphere/contents/0100000000001000/") == 0;
    }

    inline void RemoveTree(const std::string &path) {
        if (DIR *d = opendir(path.c_str())) {
            while (struct dirent *e = readdir(d)) {
                const std::string name = e->d_name;
                if (name != "." && name != "..") RemoveTree(path + "/" + name);
            }
            closedir(d);
            rmdir(path.c_str());
        } else {
            remove(path.c_str());
        }
    }

    // After the new files are in. Only what sLaunch itself put there and nothing can
    // use any more; the user's config, themes, music and cache are not touched. Does
    // nothing unless the new menu really is on the card, so an old release installed
    // through this keeps its own menu.
    inline void RemoveLeftovers(const std::string &root) {
        if (!Exists(root + kMenu)) return;
        const std::string c = root + "/atmosphere/contents/0100000000001000";
        RemoveTree(c + "/exefs.nsp");            // would lose to the loose files anyway
        RemoveTree(c + "/exefs/main.bak");
        RemoveTree(c + "/exefs.sslaunch");       // a test build's manual step
        RemoveTree(root + "/slaunch/bin/sMenu");
        RemoveTree(root + "/slaunch/bin/sMenu-vk");
        // The daemon's mailboxes: the menu reads these differently or not at all.
        RemoveTree(root + "/slaunch/hbtarget.txt");
        RemoveTree(root + "/slaunch/config/restart_menu.request");
        RemoveTree(root + "/slaunch/config/menu_crashes");
    }

}
