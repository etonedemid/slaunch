// g++ -std=c++17 -fsanitize=address,undefined -I projects/sInstaller/include scripts/qext/test_upgrade_installer.cpp -o /tmp/tu && /tmp/tu
#include "Upgrade.hpp"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
namespace fs = std::filesystem;
using namespace upgrade;

static void Touch(const std::string &p) {
    fs::create_directories(fs::path(p).parent_path());
    std::ofstream(p) << "x";
}

int main() {
    char tmpl[] = "/tmp/sl-upgrade-XXXXXX";
    const std::string sd = mkdtemp(tmpl);
    const std::string c = sd + "/atmosphere/contents/0100000000001000";

    // Empty card, then live and disabled daemons.
    assert(!HasOldInstall(sd));
    Touch(c + "/exefs/main");
    assert(HasOldInstall(sd));
    fs::create_directories(sd + "/slaunch/backup");
    fs::rename(c, sd + "/slaunch/backup/0100000000001000.disabled");
    assert(HasOldInstall(sd));
    fs::rename(sd + "/slaunch/backup/0100000000001000.disabled", c);

    // A full old install, plus things that must survive.
    for (const char *f : { "/exefs/main.bak", "/exefs.nsp", "/exefs.sslaunch/main" }) Touch(c + f);
    for (const char *f : { "/slaunch/bin/sMenu/main", "/slaunch/bin/sMenu-vk/main", "/slaunch/bin/hbloader_app/main",
                           "/slaunch/hbtarget.txt", "/slaunch/config/menu_crashes",
                           "/slaunch/config/restart_menu.request",
                           "/slaunch/config/users/a/settings.txt", "/slaunch/config/hb_donor.txt",
                           "/slaunch/themes/mine.jpg", "/slaunch/music/mine.ogg", "/slaunch/cache/applist.txt",
                           "/slaunch/bin/hbloader/main", "/slaunch/bin/sMenu-sdl2-main.bak" })
        Touch(sd + f);

    // Not the new build: nothing may go, or an old release would lose its menu.
    RemoveLeftovers(sd);
    assert(Exists(sd + "/slaunch/bin/sMenu/main") && Exists(c + "/exefs.nsp"));

    Touch(sd + kMenu);
    assert(!HasOldInstall(sd));
    RemoveLeftovers(sd);
    for (const char *f : { "/slaunch/bin/sMenu", "/slaunch/bin/sMenu-vk",
                           "/slaunch/hbtarget.txt", "/slaunch/config/menu_crashes",
                           "/slaunch/config/restart_menu.request" })
        assert(!Exists(sd + f));
    assert(!Exists(c + "/exefs.nsp") && !Exists(c + "/exefs/main.bak") && !Exists(c + "/exefs.sslaunch"));
    assert(Exists(c + "/exefs/main"));
    assert(Exists(sd + "/slaunch/bin/hbloader_app"));   // the donor launch needs it
    for (const char *f : { "/slaunch/config/users/a/settings.txt", "/slaunch/config/hb_donor.txt",
                           "/slaunch/themes/mine.jpg", "/slaunch/music/mine.ogg", "/slaunch/cache/applist.txt",
                           "/slaunch/bin/hbloader/main", "/slaunch/bin/sMenu-sdl2-main.bak" })
        assert(Exists(sd + f));

    assert(IsBootEntry("atmosphere/contents/0100000000001000/exefs/main"));
    assert(!IsBootEntry("atmosphere/contents/0100000000001000x"));
    assert(!IsBootEntry("atmosphere/config/system_settings.ini"));
    assert(!IsBootEntry("slaunch/bin/hbloader/main"));

    fs::remove_all(sd);
    puts("installer upgrade ok");
}
