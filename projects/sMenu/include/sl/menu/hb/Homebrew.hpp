#pragma once
#include <switch.h>
#include <string>
#include <vector>

// Homebrew (.nro) discovery + metadata. Scans sdmc:/switch (recursively) for
// .nro files and reads the name + icon out of each one's trailing ASET section
// (the same NACP + JPEG a title carries). Icons are cached to
// sdmc:/slaunch/cache/hbicons/<hash>.jpg so the UI can load them as textures.

namespace sl::menu::hb {

    struct HbEntry {
        std::string path;   // sdmc:/switch/.../foo.nro
        std::string name;   // from NACP, else the file base name
        u64         icon_key = 0;  // hash of path; icon cached as <icon_key>.jpg (0 = none)
    };

    // Recursively scan sdmc:/switch for .nro files, newest-first-ish (dir order),
    // resolving each one's name and extracting its icon to the cache.
    std::vector<HbEntry> Scan();

    // Resolve one .nro: name (NACP, else file base) + extract its icon to cache.
    // Used to give pinned homebrew their name/icon without a full scan.
    HbEntry ReadOne(const std::string &path);

    // Fill name + icon_key for a set of entries (with .path set) from the scan
    // manifest, parsing/extracting only the ones that are new or changed. Cheap
    // to call at every menu start - unchanged entries cost just a stat().
    void Resolve(std::vector<HbEntry> &entries);

    // Cache dir for extracted homebrew icons (under sdmc:/slaunch).
    constexpr const char *IconDir = "cache/hbicons";

} // namespace sl::menu::hb
