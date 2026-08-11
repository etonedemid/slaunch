#pragma once
#include <switch.h>
#include <string>
#include <vector>

// Play statistics (total play time, last played, launch count) as the system
// records them in pdm - the same numbers the stock HOME Menu shows. The menu
// applet's NPDM grants "*" service access, so it can query pdm:qry directly
// instead of asking the daemon for them.

namespace sl::menu::play {

    struct PlayInfo {
        u64 seconds     = 0;   // total play time
        u64 last_played = 0;   // POSIX time of the most recent session (0 = never)
        u32 launches    = 0;
    };

    // Look up every id in one go. Opens (and closes) its own pdm:qry session, so
    // run it on a worker thread and only one call at a time. `out` comes back the
    // same length as `ids`; titles pdm has no record of are left zeroed.
    void Query(const std::vector<u64> &ids, std::vector<PlayInfo> &out);

    // "12h 30m" / "45m" / "-" when the game was never played.
    std::string FormatPlaytime(u64 seconds);

    // "Today" / "Yesterday" / "5 days ago" / "-" when never played.
    std::string FormatLastPlayed(u64 posix_time);

} // namespace sl::menu::play
