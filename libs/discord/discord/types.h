#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

using Snowflake = uint64_t;

namespace Perm {
constexpr uint64_t ADD_REACTIONS = 1ull << 6;
constexpr uint64_t ADMINISTRATOR = 1ull << 3;
constexpr uint64_t VIEW_CHANNEL = 1ull << 10;
constexpr uint64_t SEND_MESSAGES = 1ull << 11;
constexpr uint64_t MANAGE_MESSAGES = 1ull << 13;
constexpr uint64_t READ_MESSAGE_HISTORY = 1ull << 16;
constexpr uint64_t CONNECT = 1ull << 20;
constexpr uint64_t SEND_MESSAGES_IN_THREADS = 1ull << 38;
constexpr uint64_t ALL = ~0ull;
}  // namespace Perm

enum ChannelType {
    CH_GUILD_TEXT = 0,
    CH_DM = 1,
    CH_GUILD_VOICE = 2,
    CH_GROUP_DM = 3,
    CH_GUILD_CATEGORY = 4,
    CH_GUILD_ANNOUNCEMENT = 5,
    CH_ANNOUNCEMENT_THREAD = 10,
    CH_PUBLIC_THREAD = 11,
    CH_PRIVATE_THREAD = 12,
    CH_GUILD_STAGE_VOICE = 13,
    CH_GUILD_DIRECTORY = 14,
    CH_GUILD_FORUM = 15,
    CH_GUILD_MEDIA = 16,
};

struct User {
    Snowflake id = 0;
    std::string username, globalName, avatar, discriminator;
    bool bot = false;
    int premiumType = 0;  // 0 = no Nitro

    std::string DisplayName() const { return !globalName.empty() ? globalName : username; }
    std::string AvatarUrl(int size = 64) const;
};

struct Member {
    std::string nick;
    std::vector<Snowflake> roles;
};

struct Role {
    Snowflake id = 0;
    std::string name;
    uint32_t color = 0;
    int position = 0;
    uint64_t permissions = 0;
};

struct Overwrite {
    Snowflake id = 0;
    int type = 0;  // 0 role, 1 member
    uint64_t allow = 0, deny = 0;
};

struct Emoji {
    Snowflake id = 0;
    std::string name;
    bool animated = false;
};

struct Channel {
    Snowflake id = 0, guildId = 0, parentId = 0, lastMessageId = 0, ownerId = 0;
    int type = 0, position = 0;
    std::string name, topic, icon;
    bool nsfw = false;
    std::vector<Overwrite> overwrites;
    std::vector<Snowflake> recipients;
    // Derived from roles and overwrites.
    bool canView = true, canSend = true, canReact = true, canManage = false;

    bool IsDM() const { return type == CH_DM || type == CH_GROUP_DM; }
    bool IsThread() const { return type == CH_ANNOUNCEMENT_THREAD || type == CH_PUBLIC_THREAD || type == CH_PRIVATE_THREAD; }
    bool IsVoice() const { return type == CH_GUILD_VOICE || type == CH_GUILD_STAGE_VOICE; }
    bool IsCategory() const { return type == CH_GUILD_CATEGORY; }
    bool IsForum() const { return type == CH_GUILD_FORUM || type == CH_GUILD_MEDIA; }
    // Channels whose messages we can show (voice channels have a text chat too).
    bool HasMessages() const { return !IsCategory() && !IsForum() && type != CH_GUILD_DIRECTORY; }
};

struct Guild {
    Snowflake id = 0, ownerId = 0;
    std::string name, icon;
    int memberCount = 0;
    bool unavailable = false;
    std::unordered_map<Snowflake, Role> roles;
    std::vector<Snowflake> channels;  // unsorted; the store sorts for display
    std::unordered_map<Snowflake, Emoji> emojis;
    std::vector<Snowflake> myRoles;
    bool myRolesKnown = false;
    std::unordered_map<Snowflake, Member> members;  // filled on demand (op 8)

    std::string IconUrl(int size = 64) const;
    std::string Initials() const;
};

struct GuildFolder {
    int64_t id = 0;  // 0 = a lone guild (not a real folder)
    std::string name;
    uint32_t color = 0;
    bool hasColor = false;
    std::vector<Snowflake> guildIds;
};

struct Attachment {
    Snowflake id = 0;
    std::string filename, url, proxyUrl, contentType, description;
    int width = 0, height = 0;
    uint64_t size = 0;

    bool IsImage() const;
    bool IsVideo() const;
    bool Spoiler() const { return filename.rfind("SPOILER_", 0) == 0; }
};

struct EmbedField {
    std::string name, value;
    bool isInline = false;
};

struct Embed {
    std::string type, title, url, description, authorName, footer, provider;
    uint32_t color = 0;
    bool hasColor = false;
    std::string imageUrl, thumbUrl;        // Discord's media proxy
    std::string imageOrig, thumbOrig, videoUrl;  // the source files (animated GIF/WebP live here)
    int imageW = 0, imageH = 0, thumbW = 0, thumbH = 0;
    std::vector<EmbedField> fields;
};

struct Reaction {
    Snowflake emojiId = 0;
    std::string emojiName;
    bool animated = false;
    int count = 0;
    bool me = false;

    std::string Key() const;       // "name:id" or the unicode emoji
    std::string UrlParam() const;  // for the reactions endpoint
};

struct StickerItem {
    Snowflake id = 0;
    std::string name;
    int format = 1;  // 1 png, 2 apng, 3 lottie, 4 gif
};

struct MsgLayout;  // UI cache, defined by the chat view

struct Message {
    Snowflake id = 0, channelId = 0, guildId = 0;
    int type = 0;
    std::string content;
    User author;
    std::string nick;
    std::vector<Snowflake> memberRoles;
    bool hasMember = false;
    bool edited = false, pinned = false, mentionEveryone = false, forwarded = false;
    int flags = 0;
    std::vector<Snowflake> mentionIds, mentionRoles;
    std::vector<Attachment> attachments;
    std::vector<Embed> embeds;
    std::vector<Reaction> reactions;
    std::vector<StickerItem> stickers;
    std::string systemText;  // for join/pin/boost/call messages
    std::string commandName;  // "/name" for slash command replies
    Snowflake replyId = 0;
    std::shared_ptr<Message> replied;
    bool replyMissing = false;
    std::string nonce;
    bool pending = false, failed = false;  // our own message before the server confirms it

    mutable std::shared_ptr<MsgLayout> layout;
    mutable int layoutKey = -1;

    int64_t TimeMs() const;
    bool IsSystem() const { return !systemText.empty(); }
};

struct ChannelMessages {
    std::vector<Message> list;  // oldest first
    bool loaded = false, loading = false, loadingOlder = false, reachedStart = false, error = false;
    int64_t lastUsedMs = 0;
};

struct ReadState {
    Snowflake lastAcked = 0;
    int mentions = 0;
};

struct GuildSettings {
    bool muted = false;
    std::vector<Snowflake> mutedChannels;
};

// Identity sent with every request and the gateway identify.
namespace Client {
extern const char* kUserAgent;
std::string SuperPropertiesJson();
std::string SuperPropertiesB64();
void SetBuildNumber(int n);
}  // namespace Client
