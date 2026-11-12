#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

// Updates sLaunch in place from a release's SD zip, fetching only what changed.
// The zip's central directory (one HTTP range request at its end) lists every
// file with its size, CRC-32 and offset; a file whose size and CRC match the
// card is skipped, and each changed one is one range request for its compressed
// bytes, inflated straight to disk. Blocking: worker threads only.
//
// Nothing here is Switch-specific: `root` is "sdmc:" on the console and a temp
// folder in scripts/qext/test_ota.cpp, which serves the zip over file://.
namespace sl::menu::ota {

    struct Entry {
        std::string name;              // path inside the zip, '/'-separated
        uint32_t crc = 0, csize = 0, usize = 0;
        uint32_t offset = 0;           // of its local header
        uint16_t method = 0;           // 0 stored, 8 deflate
    };

    struct Progress {
        std::atomic<int>      stage{0};          // 0 checking, 1 downloading, 2 installing
        std::atomic<uint64_t> done{0}, total{0}; // compressed bytes
        std::atomic<int>      file{0}, files{0};
        std::atomic<bool>     cancel{false};     // set from the UI; checked between chunks
    };

    // The release asset ending in "-sd.zip" in a GitHub releases JSON, or "".
    std::string ZipUrl(const std::string &release_json);

    // The files of the zip at `url` that differ from those under `root`. The
    // user's own files (config, themes, music, cache) are kept once they exist,
    // and the installer is left out: it is the one thing this replaces.
    bool Plan(const std::string &url, const std::string &root, std::vector<Entry> &out,
              Progress &pr, std::string &err);

    // Downloads every entry beside its target as "<name>.ota" and checks it,
    // then moves them all into place, the files that decide what boots last.
    // Nothing is replaced unless every download arrived whole.
    bool Apply(const std::string &url, const std::string &root, const std::vector<Entry> &files,
               Progress &pr, std::string &err);

}
