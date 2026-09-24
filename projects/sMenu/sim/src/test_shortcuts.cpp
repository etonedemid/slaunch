// Self-check for the launcher-shortcut scan (hb/Shortcuts.cpp).
//
// Builds against the simulator's switch.h, which leaves "sdmc:/..." paths
// alone - so a directory literally named "sdmc:" stands in for the card. The
// fixtures are written here rather than checked in, so the test is one file
// and cannot drift from a tree someone edited by hand.
//
//   make -C projects/sMenu/sim test
#include <sl/menu/hb/Shortcuts.hpp>
#include <sl/menu/cfg/UserCfg.hpp>
#include <sys/stat.h>
#include <unistd.h>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace sl::menu::hb;
namespace cfg = sl::menu::cfg;

// The one libnx symbol UserCfg needs. Defined here rather than linking the
// simulator's stub, which drags in SDL for a test that draws nothing.
extern "C" bool accountUidIsValid(const AccountUid *uid) {
    return uid && (uid->uid[0] || uid->uid[1]);
}

static void Write(const std::string &path, const char *body) {
    FILE *fp = fopen(path.c_str(), "w");
    assert(fp && "fixture could not be written");
    fputs(body, fp);
    fclose(fp);
}

static const Shortcut *Find(const std::vector<Shortcut> &v, const char *name) {
    for (const auto &s : v) if (s.name == name) return &s;
    return nullptr;
}

int main() {
    char dir[] = "/tmp/sl_shortcuts_XXXXXX";
    assert(mkdtemp(dir) && "no temp dir");
    assert(chdir(dir) == 0);
    for (const char *d : { "sdmc:", "sdmc:/retroarch", "sdmc:/retroarch/playlists",
                           "sdmc:/slaunch", "sdmc:/slaunch/config",
                           "sdmc:/slaunch/config/users",
                           "sdmc:/slaunch/config/users/default" })
        mkdir(d, 0777);

    // Modern JSON playlist. "DETECT" on the first entry means "use the
    // playlist default core"; the second names its own and carries an escape.
    Write("sdmc:/retroarch/playlists/Nintendo - Super Nintendo Entertainment System.lpl",
R"JSON({
  "version": "1.5",
  "default_core_path": "/retroarch/cores/snes9x_libretro_libnx.nro",
  "default_core_name": "Nintendo - SNES / SFC (Snes9x)",
  "items": [
    {
      "path": "/roms/snes/Super Mario World.sfc",
      "label": "Super Mario World",
      "core_path": "DETECT",
      "core_name": "DETECT",
      "crc32": "B19ED489|crc",
      "db_name": "Nintendo - Super Nintendo Entertainment System.lpl"
    },
    {
      "path": "/roms/snes/Chrono Trigger.sfc",
      "label": "Chrono Trigger \/ Edition",
      "core_path": "/retroarch/cores/bsnes_libretro_libnx.nro",
      "db_name": "Nintendo - Super Nintendo Entertainment System.lpl"
    }
  ]
})JSON");

    // No core on the entry and none on the playlist either.
    Write("sdmc:/retroarch/playlists/Unassigned.lpl",
R"JSON({
  "version": "1.5",
  "default_core_path": "DETECT",
  "items": [
    { "path": "/roms/x/orphan.bin", "label": "No Core Here", "core_path": "DETECT" }
  ]
})JSON");

    // Pre-1.7.6: six plain lines per entry, no JSON.
    Write("sdmc:/retroarch/playlists/Sega - Mega Drive.lpl",
          "/roms/md/Sonic.bin\n"
          "Sonic the Hedgehog\n"
          "/retroarch/cores/genesis_plus_gx_libretro_libnx.nro\n"
          "Sega - MS/GG/MD/CD\n"
          "DETECT\n"
          "Sega - Mega Drive - Genesis.lpl\n");

    Write("sdmc:/slaunch/config/users/default/shortcuts.txt",
          "/switch/moonlight/moonlight.nro\tMoonlight\t\tStreaming\n"
          "#comment\n"
          "/switch/plain.nro\n");

    AccountUid none{};
    cfg::SetUser(none);                       // -> config/users/default
    const std::vector<Shortcut> v = ScanShortcuts();

    const Shortcut *smw = Find(v, "Super Mario World");
    assert(smw && "JSON playlist entry missing");
    // "DETECT" inherits the playlist's default core rather than being dropped.
    assert(smw->nro == "sdmc:/retroarch/cores/snes9x_libretro_libnx.nro");
    assert(smw->category == "Nintendo - Super Nintendo Entertainment System");
    // The core path is ours to open, so it gains the device prefix; the ROM
    // path is RetroArch's own and goes back to it exactly as written.
    assert(smw->argv == "\"sdmc:/retroarch/cores/snes9x_libretro_libnx.nro\""
                        " \"/roms/snes/Super Mario World.sfc\"");
    assert(smw->icon_path == "sdmc:/retroarch/thumbnails/"
                             "Nintendo - Super Nintendo Entertainment System/"
                             "Named_Boxarts/Super Mario World.png");

    // Per-entry core beats the default; "\/" decodes, and the "/" it decodes to
    // is then sanitised out of the thumbnail file name.
    const Shortcut *ct = Find(v, "Chrono Trigger / Edition");
    assert(ct && "JSON escape not decoded");
    assert(ct->nro == "sdmc:/retroarch/cores/bsnes_libretro_libnx.nro");
    assert(ct->icon_path.find("Chrono Trigger _ Edition.png") != std::string::npos);

    // Nothing to launch and no way to guess one: left out rather than listed.
    assert(!Find(v, "No Core Here"));

    // Legacy playlist parsed, and db_name beating the file name.
    const Shortcut *sonic = Find(v, "Sonic the Hedgehog");
    assert(sonic && "legacy playlist not parsed");
    assert(sonic->category == "Sega - Mega Drive - Genesis");

    // shortcuts.txt: named and categorised, with no argv of its own.
    const Shortcut *ml = Find(v, "Moonlight");
    assert(ml && "shortcuts.txt entry missing");
    assert(ml->category == "Streaming");
    assert(ml->argv == "\"sdmc:/switch/moonlight/moonlight.nro\"");
    // Path only: name falls back to the file base, no category, runs plain.
    const Shortcut *plain = Find(v, "plain");
    assert(plain && plain->category.empty());
    assert(plain->argv == "\"sdmc:/switch/plain.nro\"");
    assert(!Find(v, "comment"));               // '#' is a comment, not a path

    // Grouped by category: XmbRebuild makes a column per category by walking
    // this list once, so a category appearing twice would be two columns.
    for (size_t i = 1; i < v.size(); i++)
        assert(v[i - 1].category <= v[i].category);

    printf("ok: %zu shortcuts\n", v.size());
    return 0;
}
