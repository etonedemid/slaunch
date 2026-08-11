#include <sl/menu/play/PlayStats.hpp>
#include <sl/menu/ui/Locale.hpp>
#include <cstdio>
#include <ctime>

namespace sl::menu::play {

    using ui::T;

    void Query(const std::vector<u64> &ids, std::vector<PlayInfo> &out) {
        out.assign(ids.size(), PlayInfo{});
        if (ids.empty()) return;
        if (R_FAILED(pdmqryInitialize())) return;   // no stats: everything stays zeroed

        for (size_t i = 0; i < ids.size(); i++) {
            if (ids[i] == 0) continue;              // homebrew and menu entries
            // libnx returns the [16.0.0+] shape on every firmware, converting the
            // older one for us: play time in nanoseconds, timestamps in POSIX time.
            PdmPlayStatistics st = {};
            if (R_FAILED(pdmqryQueryPlayStatisticsByApplicationId(ids[i], false, &st)))
                continue;                           // never launched -> no record
            out[i].seconds     = st.playtime / 1000000000ULL;
            out[i].last_played = st.last_timestamp_user;
            out[i].launches    = st.total_launches;
        }

        pdmqryExit();
    }

    std::string FormatPlaytime(u64 seconds) {
        if (seconds == 0) return "-";
        char b[32];
        const u64 mins = seconds / 60;
        // Under an hour the minutes matter; over it they are noise past the hour.
        if (mins < 60) snprintf(b, sizeof(b), "%llum", (unsigned long long)(mins ? mins : 1));
        else           snprintf(b, sizeof(b), "%lluh %llum",
                                (unsigned long long)(mins / 60),
                                (unsigned long long)(mins % 60));
        return b;
    }

    std::string FormatLastPlayed(u64 posix_time) {
        if (posix_time == 0) return "-";

        // Compare calendar days, not elapsed hours, so "yesterday evening" does
        // not read as "today" just because it was under 24 hours ago.
        const time_t then_t = (time_t)posix_time;
        const time_t now_t  = time(nullptr);
        struct tm then_tm, now_tm;
        localtime_r(&then_t, &then_tm);
        localtime_r(&now_t,  &now_tm);

        struct tm then_mid = then_tm, now_mid = now_tm;
        then_mid.tm_hour = then_mid.tm_min = then_mid.tm_sec = 0;
        now_mid.tm_hour  = now_mid.tm_min  = now_mid.tm_sec  = 0;
        const double diff = difftime(mktime(&now_mid), mktime(&then_mid));
        const int days = (int)(diff / (60 * 60 * 24) + 0.5);

        if (days <= 0) return T("Today");
        if (days == 1) return T("Yesterday");

        char b[48];
        if (days < 30)       snprintf(b, sizeof(b), "%d %s", days, T("days ago"));
        else if (days < 365) snprintf(b, sizeof(b), "%d %s", days / 30, T("months ago"));
        else                 snprintf(b, sizeof(b), "%d %s", days / 365, T("years ago"));
        return b;
    }

} // namespace sl::menu::play
