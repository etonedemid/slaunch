// The in-menu updater on a PC: a release zip served over file://, a fake SD card,
// and the files that should and should not change.
//   g++ -std=c++17 -I projects/sMenu/include scripts/qext/test_ota.cpp
//       projects/sMenu/source/sl/menu/net/Ota.cpp -lcurl -lz -o /tmp/test_ota && /tmp/test_ota
#include <sl/menu/net/Ota.hpp>
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

using namespace sl::menu::ota;

static void Put(const std::string &p, const std::string &s) {
    std::system(("mkdir -p \"$(dirname '" + p + "')\"").c_str());
    std::ofstream(p, std::ios::binary) << s;
}
static std::string Get(const std::string &p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream o;
    o << in.rdbuf();
    return o.str();
}
static bool Exists(const std::string &p) { return std::ifstream(p).good(); }

int main() {
    char tmpl[] = "/tmp/ota-XXXXXX";
    const std::string dir = mkdtemp(tmpl), rel = dir + "/release", sd = dir + "/sd";

    // The release: a big compressible menu, an unchanged library, a boot file,
    // a shipped setting, the installer, and a small file zip will store.
    std::string menu;
    for (int i = 0; i < 200000; i++) menu += "menu-v2 line " + std::to_string(i % 977) + "\n";
    Put(rel + "/qlaunch-ext/menus/menu.dnro", menu);
    Put(rel + "/lib/sdl3.dnro", std::string(300000, 'L'));
    Put(rel + "/atmosphere/contents/0100000000001000/exefs/main", "loader-v2");
    Put(rel + "/slaunch/config/settings.txt", "ui_mode=0\n");
    Put(rel + "/switch/sInstaller/sInstaller.nro", "installer");
    Put(rel + "/slaunch/x", "z");
    // Noise zip can't shrink, so the zip outgrows the 64 KiB read from its end
    // like a real release does.
    std::string font;
    for (unsigned v = 1, i = 0; i < 200000; i++) font += (char)((v = v * 1103515245u + 12345u) >> 24);
    Put(rel + "/slaunch/fonts/f.ttf", font);
    Put(sd + "/slaunch/fonts/f.ttf", font);
    assert(std::system(("cd '" + rel + "' && zip -qr ../r-sd.zip .").c_str()) == 0);
    const std::string url = "file://" + dir + "/r-sd.zip";

    // The card: an old menu and loader, the same library, the user's settings.
    Put(sd + "/qlaunch-ext/menus/menu.dnro", "menu-v1");
    Put(sd + "/lib/sdl3.dnro", std::string(300000, 'L'));
    Put(sd + "/atmosphere/contents/0100000000001000/exefs/main", "loader-v1");
    Put(sd + "/slaunch/config/settings.txt", "ui_mode=9\n");

    assert(ZipUrl(R"({"assets":[{"browser_download_url": "https://x/a.nro"},)"
                  R"({"browser_download_url":"https://x/slaunch-v9-sd.zip"}]})") == "https://x/slaunch-v9-sd.zip");
    assert(ZipUrl("{}").empty());

    Progress pr;
    std::vector<Entry> plan;
    std::string err;
    assert(Plan(url, sd, plan, pr, err) && err.empty());
    std::vector<std::string> names;
    for (const Entry &e : plan) names.push_back(e.name);
    std::sort(names.begin(), names.end());
    assert((names == std::vector<std::string>{ "atmosphere/contents/0100000000001000/exefs/main",
                                               "qlaunch-ext/menus/menu.dnro", "slaunch/x" }));

    assert(Apply(url, sd, plan, pr, err) && err.empty());
    assert(pr.done.load() == pr.total.load() && pr.total.load() > 0);
    assert(Get(sd + "/qlaunch-ext/menus/menu.dnro") == menu);
    assert(Get(sd + "/atmosphere/contents/0100000000001000/exefs/main") == "loader-v2");
    assert(Get(sd + "/slaunch/x") == "z");
    assert(Get(sd + "/slaunch/config/settings.txt") == "ui_mode=9\n");   // the user's, kept
    assert(!Exists(sd + "/switch/sInstaller/sInstaller.nro"));
    assert(!Exists(sd + "/qlaunch-ext/menus/menu.dnro.ota"));

    // Nothing left to fetch.
    assert(Plan(url, sd, plan, pr, err) && plan.empty());

    // A damaged download replaces nothing.
    Put(sd + "/qlaunch-ext/menus/menu.dnro", "menu-v1");
    assert(Plan(url, sd, plan, pr, err) && plan.size() == 1);
    plan[0].crc ^= 1;
    err.clear();
    assert(!Apply(url, sd, plan, pr, err) && !err.empty());
    assert(Get(sd + "/qlaunch-ext/menus/menu.dnro") == "menu-v1");
    assert(!Exists(sd + "/qlaunch-ext/menus/menu.dnro.ota"));

    // Cancelling replaces nothing either.
    plan[0].crc ^= 1;
    err.clear();
    pr.cancel = true;
    assert(!Apply(url, sd, plan, pr, err) && err == "Cancelled");
    assert(Get(sd + "/qlaunch-ext/menus/menu.dnro") == "menu-v1");

    std::system(("rm -rf '" + dir + "'").c_str());
    std::puts("ota ok");
}
