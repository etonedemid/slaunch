#pragma once

#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "discord/types.h"
#include "util/json.h"

// Everything we know about the signed-in account. Main thread only.
class Store {
public:
    // What the UI may want to react to after an event.
    struct Change {
        enum Kind { None, NewMessage, MessageEdited, MessageDeleted, ChannelsChanged, GuildsChanged, MembersLoaded, Ready };
        Kind kind = None;
        Snowflake channelId = 0, messageId = 0, guildId = 0;
        bool mentionsMe = false;
        bool fromMe = false;
    };

    struct Row {  // one line of a guild's channel list
        Snowflake id = 0;
        bool category = false;
    };

    User me;
    bool ready = false;
    std::unordered_map<Snowflake, User> users;
    std::unordered_map<Snowflake, Guild> guilds;
    std::vector<GuildFolder> folders;  // sidebar order
    std::unordered_map<Snowflake, Channel> channels;
    std::vector<Snowflake> dms;  // most recent first
    std::unordered_map<Snowflake, ReadState> readStates;
    std::unordered_map<Snowflake, GuildSettings> guildSettings;
    std::unordered_map<Snowflake, std::string> presence;
    std::unordered_map<Snowflake, ChannelMessages> messages;
    std::unordered_map<Snowflake, std::map<Snowflake, int64_t>> typing;  // channel -> user -> expiry
    uint64_t version = 0;  // bumps on every change, so the UI knows to redraw

    void Clear();
    Change Apply(const std::string& type, yyjson_val* d);

    // Queries.
    std::vector<Row> ChannelList(Snowflake guildId) const;
    std::string ChannelName(const Channel& c) const;
    std::string ChannelName(Snowflake id) const;
    std::string DmAvatarUrl(const Channel& c) const;
    const User* FindUser(Snowflake id) const;
    const Channel* FindChannel(Snowflake id) const;
    Guild* FindGuild(Snowflake id);
    const Guild* FindGuild(Snowflake id) const;
    std::string MemberName(Snowflake guildId, Snowflake userId) const;
    std::string AuthorName(const Message& m) const;
    uint32_t AuthorColor(const Message& m) const;  // 0 = default
    std::string RoleName(Snowflake guildId, Snowflake roleId) const;
    uint32_t RoleColor(Snowflake guildId, Snowflake roleId) const;
    bool MentionsMe(const Message& m) const;

    bool ChannelMuted(const Channel& c) const;
    bool GuildMuted(Snowflake guildId) const;
    bool ChannelUnread(Snowflake id) const;
    int ChannelMentions(Snowflake id) const;
    bool GuildUnread(Snowflake guildId) const;
    int GuildMentions(Snowflake guildId) const;
    int DmMentions() const;
    std::vector<Snowflake> TypingUsers(Snowflake channelId, int64_t nowMs) const;

    // Messages.
    ChannelMessages& Messages(Snowflake channelId);
    void AddHistory(Snowflake channelId, std::vector<Message> page, bool older, int requested);
    void AddLocalEcho(Message m);
    void ConfirmSent(const std::string& nonce, Message m);
    void MarkFailed(Snowflake channelId, const std::string& nonce);
    Message* FindMessage(Snowflake channelId, Snowflake id);
    void MarkRead(Snowflake channelId, Snowflake messageId);
    void TrimCaches(Snowflake keepChannel);
    // Members we still need names/colours for (from a page of messages).
    std::vector<Snowflake> UnknownMembers(Snowflake guildId, const std::vector<Message>& msgs) const;

    void RecomputePermissions(Snowflake guildId);

private:
    void ApplyReady(yyjson_val* d);
    void ApplyGuild(yyjson_val* g, yyjson_val* mergedMembers);
    void ApplyGuildSettings(yyjson_val* entry);
    void ApplyReadStates(yyjson_val* arr);
    void ParseFolders(yyjson_val* d);
    void SortDms();
    void TouchDm(Snowflake channelId);
    uint64_t ComputePerms(const Guild& g, const Channel& c) const;
    void InsertSorted(ChannelMessages& cm, Message m);
    void RememberUser(const User& u);
};
