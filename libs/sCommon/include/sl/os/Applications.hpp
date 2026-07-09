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

    // Same, but also reports the total bytes NS wrote (NACP + JPEG icon). Use
    // IconSize() on that value to get the icon length for caching.
    Result GetApplicationControl(u64 app_id, NsApplicationControlData &out, u64 &out_size);

    // Length of the JPEG icon inside a control blob of `control_data_size` bytes.
    // The icon follows the fixed-size NACP, so it is everything past it. Returns
    // 0 when NS reported nothing beyond the NACP (icon-less title).
    inline size_t IconSize(u64 control_data_size) {
        return control_data_size > sizeof(NacpStruct)
                   ? (size_t)(control_data_size - sizeof(NacpStruct)) : 0;
    }

    // Returns human-readable title name from NACP, picking the best language
    std::string GetAppName(const NacpStruct &nacp);

} // namespace sl::os
