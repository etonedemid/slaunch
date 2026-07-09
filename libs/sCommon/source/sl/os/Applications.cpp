#include <sl/os/Applications.hpp>
#include <sl/Result.hpp>
#include <cstring>

namespace sl::os {

    Result ListApplicationRecords(std::vector<NsApplicationRecord> &out) {
        out.clear();
        constexpr s32 PageSize = 30;
        s32 offset = 0;

        while (true) {
            NsApplicationRecord buf[PageSize];
            s32 count = 0;
            Result rc = nsListApplicationRecord(buf, PageSize, offset, &count);
            if (rc != 0) return rc;
            if (count == 0) break;
            for (s32 i = 0; i < count; i++)
                out.push_back(buf[i]);
            offset += count;
            if (count < PageSize) break;
        }
        return 0;
    }

    Result ListApplicationViews(const std::vector<u64> &app_ids,
                                std::vector<NsApplicationView> &out) {
        out.clear();
        if (app_ids.empty()) return 0;

        out.resize(app_ids.size());
        return nsGetApplicationView(
            reinterpret_cast<NsApplicationView*>(out.data()),
            app_ids.data(),
            static_cast<s32>(app_ids.size())
        );
    }

    Result GetApplicationControl(u64 app_id, NsApplicationControlData &out) {
        u64 dummy = 0;
        return GetApplicationControl(app_id, out, dummy);
    }

    Result GetApplicationControl(u64 app_id, NsApplicationControlData &out, u64 &out_size) {
        out_size = 0;
        memset(&out, 0, sizeof(out));
        return nsGetApplicationControlData(
            NsApplicationControlSource_Storage,
            app_id, &out, sizeof(out), &out_size
        );
    }

    std::string GetAppName(const NacpStruct &nacp) {
        // Priority order: English, then any non-empty entry
        auto pick = [](const NacpStruct &n) -> const NacpLanguageEntry* {
            // English (AmericanEnglish = index 0)
            if (n.lang[0].name[0] != '\0') return &n.lang[0];
            for (int i = 1; i < 16; i++)
                if (n.lang[i].name[0] != '\0') return &n.lang[i];
            return nullptr;
        };
        const auto *e = pick(nacp);
        if (!e) return "Unknown";
        // Truncate to null terminator
        char name[129] = {};
        strncpy(name, e->name, 128);
        return std::string(name);
    }

} // namespace sl::os
