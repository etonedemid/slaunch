#include "discord/types.h"

#include <atomic>

#include "util/strings.h"

std::string User::AvatarUrl(int size) const {
    if (avatar.empty()) {
        int index = (discriminator.empty() || discriminator == "0") ? (int)((id >> 22) % 6)
                                                                     : atoi(discriminator.c_str()) % 5;
        return Str::Format("https://cdn.discordapp.com/embed/avatars/%d.png", index);
    }
    return Str::Format("https://cdn.discordapp.com/avatars/%llu/%s.png?size=%d", (unsigned long long)id,
                       avatar.c_str(), size);
}

std::string Guild::IconUrl(int size) const {
    if (icon.empty()) return "";
    return Str::Format("https://cdn.discordapp.com/icons/%llu/%s.png?size=%d", (unsigned long long)id, icon.c_str(),
                       size);
}

std::string Guild::Initials() const {
    std::string out;
    bool start = true;
    const char* p = name.data();
    const char* end = p + name.size();
    while (p < end) {
        const char* before = p;
        uint32_t cp = Str::NextCp(p, end);
        if (cp == ' ' || cp == '\t') {
            start = true;
            continue;
        }
        if (start) {
            out.append(before, p - before);
            start = false;
            if (Str::CpCount(out) >= 4) break;
        }
    }
    return out;
}

bool Attachment::IsImage() const {
    if (Str::StartsWith(contentType, "image/")) return true;
    std::string f = Str::Lower(filename);
    return Str::EndsWith(f, ".png") || Str::EndsWith(f, ".jpg") || Str::EndsWith(f, ".jpeg") ||
           Str::EndsWith(f, ".gif") || Str::EndsWith(f, ".webp");
}

bool Attachment::IsVideo() const {
    if (Str::StartsWith(contentType, "video/")) return true;
    std::string f = Str::Lower(filename);
    return Str::EndsWith(f, ".mp4") || Str::EndsWith(f, ".webm") || Str::EndsWith(f, ".mov");
}

std::string Reaction::Key() const { return emojiId ? emojiName + ":" + Str::Id(emojiId) : emojiName; }

std::string Reaction::UrlParam() const { return Str::UrlEncode(Key()); }

int64_t Message::TimeMs() const { return Str::SnowflakeMs(id); }

namespace Client {

const char* kUserAgent =
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/134.0.0.0 Safari/537.36";

namespace {
std::atomic<int> g_build{440000};
}

void SetBuildNumber(int n) {
    if (n > 0) g_build = n;
}

std::string SuperPropertiesJson() {
    return Str::Format(
        "{\"os\":\"Linux\",\"browser\":\"Chrome\",\"device\":\"\",\"system_locale\":\"en-US\","
        "\"has_client_mods\":false,\"browser_user_agent\":%s,\"browser_version\":\"134.0.0.0\","
        "\"os_version\":\"\",\"referrer\":\"\",\"referring_domain\":\"\",\"referrer_current\":\"\","
        "\"referring_domain_current\":\"\",\"release_channel\":\"stable\",\"client_build_number\":%d,"
        "\"client_event_source\":null}",
        Str::Json(kUserAgent).c_str(), g_build.load());
}

std::string SuperPropertiesB64() {
    std::string j = SuperPropertiesJson();
    return Str::Base64(reinterpret_cast<const uint8_t*>(j.data()), j.size());
}

}  // namespace Client
