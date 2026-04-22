#pragma once
#include <switch.h>
#include <vector>
#include <string>

namespace sl::os {

    // Installed application record + live view flags
    struct ApplicationRecord {
        u64    app_id;
        u8     type;        // NsApplicationRecordType
        u8     _pad[7];
    };

    // Relevant view flags from NsApplicationView
    enum class AppViewFlag : u32 {
        HasContents     = BIT(1),
        IsDownloading   = BIT(5),
        IsGameCard      = BIT(6),
        CanLaunch       = BIT(8),
        NeedsUpdate     = BIT(9),
        NeedsVerify     = BIT(13),
    };

    struct ApplicationView {
        u64    app_id;
        u32    flags;
        u64    dl_total;
        u64    dl_done;

        bool HasFlag(AppViewFlag f) const {
            return (flags & static_cast<u32>(f)) != 0;
        }
        bool CanLaunch() const { return HasFlag(AppViewFlag::CanLaunch); }
        bool IsGameCard() const { return HasFlag(AppViewFlag::IsGameCard); }
    };

    struct Application {
        ApplicationRecord record;
        ApplicationView   view;
        NsApplicationControlData control; // NACP + icon
    };

    // Fetch all installed application records (paginated via NS)
    Result ListApplicationRecords(std::vector<NsApplicationRecord> &out);

    // Fetch views for a list of app IDs (flags, download state, etc.)
    Result ListApplicationViews(const std::vector<u64> &app_ids,
                                std::vector<NsApplicationView> &out);

    // Get NACP + icon for a single application
    Result GetApplicationControl(u64 app_id, NsApplicationControlData &out);

    // Returns human-readable title name from NACP, picking the best language
    std::string GetAppName(const NacpStruct &nacp);

} // namespace sl::os
