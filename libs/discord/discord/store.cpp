#include "discord/store.h"

#include <algorithm>
#include <unordered_set>

#include "discord/parse.h"
#include "platform/platform.h"
#include "util/log.h"
#include "util/strings.h"

namespace {

// Just enough protobuf to read guild folders out of the settings blob.
struct PbReader {
    const uint8_t* p;
    const uint8_t* end;

    bool Varint(uint64_t& v) {
        v = 0;
        for (int shift = 0; shift < 64 && p < end; shift += 7) {
            uint8_t b = *p++;
            v |= (uint64_t)(b & 0x7F) << shift;
            if (!(b & 0x80)) return true;
        }
        return false;
    }
    bool Key(uint32_t& field, uint32_t& wire) {
        uint64_t k;
        if (p >= end || !Varint(k)) return false;
        field = (uint32_t)(k >> 3);
        wire = (uint32_t)(k & 7);
        return true;
    }
    bool Bytes(PbReader& sub) {
        uint64_t len;
        if (!Varint(len) || len > (uint64_t)(end - p)) return false;
        sub = PbReader{p, p + len};
        p += len;
        return true;
    }
    bool Fixed64(uint64_t& v) {
        if (end - p < 8) return false;
        v = 0;
        for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
        p += 8;
        return true;
    }
    bool Skip(uint32_t wire) {
        uint64_t tmp;
        PbReader sub{nullptr, nullptr};
        switch (wire) {
            case 0: return Varint(tmp);
            case 1: return Fixed64(tmp);
            case 2: return Bytes(sub);
            case 5:
                if (end - p < 4) return false;
                p += 4;
                return true;
            default: return false;
        }
    }
    // Reads the `value` (field 1) of a google.protobuf wrapper message.
    bool WrapperVarint(uint64_t& v) {
        PbReader sub{nullptr, nullptr};
        if (!Bytes(sub)) return false;
        uint32_t f, w;
        while (sub.Key(f, w)) {
            if (f == 1 && w == 0) return sub.Varint(v);
            if (!sub.Skip(w)) return false;
        }
        return false;
    }
    bool WrapperString(std::string& s) {
        PbReader sub{nullptr, nullptr};
        if (!Bytes(sub)) return false;
        uint32_t f, w;
        while (sub.Key(f, w)) {
            if (f == 1 && w == 2) {
                PbReader str{nullptr, nullptr};
                if (!sub.Bytes(str)) return false;
                s.assign(reinterpret_cast<const char*>(str.p), str.end - str.p);
                return true;
            }
            if (!sub.Skip(w)) return false;
        }
        return false;
    }
};

bool ParseFolderProto(const std::vector<uint8_t>& blob, std::vector<GuildFolder>& out) {
    PbReader r{blob.data(), blob.data() + blob.size()};
    uint32_t f, w;
    while (r.Key(f, w)) {
        if (f == 14 && w == 2) {
            PbReader folders{nullptr, nullptr};
            if (!r.Bytes(folders)) return false;
            uint32_t ff, fw;
            while (folders.Key(ff, fw)) {
                if (ff == 1 && fw == 2) {
                    PbReader gf{nullptr, nullptr};
                    if (!folders.Bytes(gf)) return false;
                    GuildFolder folder;
                    uint32_t gf_f, gf_w;
                    while (gf.Key(gf_f, gf_w)) {
                        if (gf_f == 1 && gf_w == 2) {  // packed fixed64
                            PbReader ids{nullptr, nullptr};
                            if (!gf.Bytes(ids)) return false;
                            uint64_t id;
                            while (ids.Fixed64(id)) folder.guildIds.push_back(id);
                        } else if (gf_f == 1 && gf_w == 1) {
                            uint64_t id;
                            if (!gf.Fixed64(id)) return false;
                            folder.guildIds.push_back(id);
                        } else if (gf_f == 2 && gf_w == 2) {
                            uint64_t v = 0;
                            if (!gf.WrapperVarint(v)) return false;
                            folder.id = (int64_t)v;
                        } else if (gf_f == 3 && gf_w == 2) {
                            if (!gf.WrapperString(folder.name)) return false;
                        } else if (gf_f == 4 && gf_w == 2) {
                            uint64_t v = 0;
                            if (!gf.WrapperVarint(v)) return false;
                            folder.color = (uint32_t)v;
                            folder.hasColor = true;
                        } else if (!gf.Skip(gf_w)) {
                            return false;
                        }
                    }
                    out.push_back(std::move(folder));
                } else if (!folders.Skip(fw)) {
                    return false;
                }
            }
            return true;
        }
        if (!r.Skip(w)) return false;
    }
    return false;
}

bool IsTextLike(const Channel& c) { return !c.IsVoice(); }

bool ChannelOrder(const Channel* a, const Channel* b) {
    bool av = !IsTextLike(*a), bv = !IsTextLike(*b);
    if (av != bv) return !av;
    if (a->position != b->position) return a->position < b->position;
    return a->id < b->id;
}

}  // namespace

void Store::Clear() { *this = Store{}; }

void Store::RememberUser(const User& u) {
    if (!u.id) return;
    User& slot = users[u.id];
    // Partial user objects (e.g. in typing events) shouldn't wipe what we know.
    if (u.username.empty() && !slot.username.empty()) return;
    slot = u;
}

Store::Change Store::Apply(const std::string& t, yyjson_val* d) {
    version++;
    Change ch;
    if (t == "READY") {
        ApplyReady(d);
        ch.kind = Change::Ready;
    } else if (t == "READY_SUPPLEMENTAL") {
        yyjson_val* mp = J::Get(d, "merged_presences");
        for (yyjson_val* p : J::Arr(mp, "friends")) presence[J::Id(p, "user_id")] = J::S(p, "status");
    } else if (t == "MESSAGE_CREATE") {
        Message m = Parse::MessageObj(d);
        auto cit = channels.find(m.channelId);
        Channel* c = cit == channels.end() ? nullptr : &cit->second;
        if (!m.guildId && c) m.guildId = c->guildId;
        RememberUser(m.author);
        if (m.hasMember && m.guildId) {
            if (Guild* g = FindGuild(m.guildId)) g->members[m.author.id] = Member{m.nick, m.memberRoles};
        }
        if (c && m.id > c->lastMessageId) c->lastMessageId = m.id;
        if (c && c->IsDM()) TouchDm(c->id);
        ch.kind = Change::NewMessage;
        ch.channelId = m.channelId;
        ch.messageId = m.id;
        ch.guildId = m.guildId;
        ch.fromMe = m.author.id == me.id;
        ch.mentionsMe = !ch.fromMe && (MentionsMe(m) || (c && c->IsDM()));
        if (ch.fromMe) MarkRead(m.channelId, m.id);
        else if (ch.mentionsMe) readStates[m.channelId].mentions++;
        auto tit = typing.find(m.channelId);
        if (tit != typing.end()) tit->second.erase(m.author.id);
        auto mit = messages.find(m.channelId);
        if (mit != messages.end() && mit->second.loaded) {
            ChannelMessages& cm = mit->second;
            bool replaced = false;
            if (!m.nonce.empty()) {
                for (auto& old : cm.list) {
                    if ((old.pending || old.failed) && old.nonce == m.nonce) {
                        cm.list.erase(cm.list.begin() + (&old - cm.list.data()));
                        replaced = true;
                        break;
                    }
                }
            }
            (void)replaced;
            InsertSorted(cm, std::move(m));
        }
    } else if (t == "MESSAGE_UPDATE") {
        Snowflake cid = J::Id(d, "channel_id"), id = J::Id(d, "id");
        if (Message* m = FindMessage(cid, id)) {
            Parse::MessageUpdate(*m, d);
            m->layoutKey = -1;
            ch.kind = Change::MessageEdited;
            ch.channelId = cid;
            ch.messageId = id;
        }
    } else if (t == "MESSAGE_DELETE" || t == "MESSAGE_DELETE_BULK") {
        Snowflake cid = J::Id(d, "channel_id");
        std::unordered_set<Snowflake> gone;
        if (t == "MESSAGE_DELETE") gone.insert(J::Id(d, "id"));
        for (yyjson_val* v : J::Arr(d, "ids")) gone.insert(J::U64(v));
        auto mit = messages.find(cid);
        if (mit != messages.end()) {
            auto& list = mit->second.list;
            list.erase(std::remove_if(list.begin(), list.end(), [&](const Message& m) { return gone.count(m.id) > 0; }),
                       list.end());
            for (auto& m : list) {
                if (m.replied && gone.count(m.replied->id)) {
                    m.replied.reset();
                    m.replyMissing = true;
                    m.layoutKey = -1;
                }
            }
        }
        ch.kind = Change::MessageDeleted;
        ch.channelId = cid;
    } else if (t == "MESSAGE_REACTION_ADD" || t == "MESSAGE_REACTION_REMOVE") {
        bool add = t == "MESSAGE_REACTION_ADD";
        Snowflake cid = J::Id(d, "channel_id"), mid = J::Id(d, "message_id"), uid = J::Id(d, "user_id");
        if (Message* m = FindMessage(cid, mid)) {
            Reaction key = Parse::ReactionEmoji(J::Get(d, "emoji"));
            auto it = std::find_if(m->reactions.begin(), m->reactions.end(),
                                   [&](const Reaction& r) { return r.Key() == key.Key(); });
            bool mine = uid == me.id;
            if (add) {
                if (it == m->reactions.end()) {
                    key.count = 1;
                    key.me = mine;
                    m->reactions.push_back(key);
                } else if (!(mine && it->me)) {  // our own optimistic update already counted it
                    it->count++;
                    if (mine) it->me = true;
                }
            } else if (it != m->reactions.end() && !(mine && !it->me)) {
                it->count--;
                if (mine) it->me = false;
                if (it->count <= 0) m->reactions.erase(it);
            }
            m->layoutKey = -1;
            ch.kind = Change::MessageEdited;
            ch.channelId = cid;
        }
    } else if (t == "MESSAGE_REACTION_REMOVE_ALL" || t == "MESSAGE_REACTION_REMOVE_EMOJI") {
        Snowflake cid = J::Id(d, "channel_id"), mid = J::Id(d, "message_id");
        if (Message* m = FindMessage(cid, mid)) {
            if (t == "MESSAGE_REACTION_REMOVE_ALL") {
                m->reactions.clear();
            } else {
                std::string k = Parse::ReactionEmoji(J::Get(d, "emoji")).Key();
                m->reactions.erase(std::remove_if(m->reactions.begin(), m->reactions.end(),
                                                  [&](const Reaction& r) { return r.Key() == k; }),
                                   m->reactions.end());
            }
            m->layoutKey = -1;
        }
    } else if (t == "MESSAGE_ACK") {
        Snowflake cid = J::Id(d, "channel_id");
        ReadState& rs = readStates[cid];
        rs.lastAcked = std::max(rs.lastAcked, J::Id(d, "message_id"));
        rs.mentions = (int)J::I(d, "mention_count", 0);
    } else if (t == "TYPING_START") {
        Snowflake cid = J::Id(d, "channel_id"), uid = J::Id(d, "user_id");
        if (yyjson_val* member = J::Get(d, "member")) {
            RememberUser(Parse::UserObj(J::Get(member, "user")));
            Snowflake gid = J::Id(d, "guild_id");
            if (Guild* g = FindGuild(gid)) {
                Member mem;
                mem.nick = J::S(member, "nick");
                for (yyjson_val* r : J::Arr(member, "roles")) mem.roles.push_back(J::U64(r));
                g->members[uid] = mem;
            }
        }
        if (uid != me.id) typing[cid][uid] = Platform::NowMs() + 10000;
    } else if (t == "CHANNEL_CREATE" || t == "CHANNEL_UPDATE") {
        Channel c = Parse::ChannelObj(d, 0);
        for (yyjson_val* r : J::Arr(d, "recipients")) RememberUser(Parse::UserObj(r));
        auto old = channels.find(c.id);
        if (old != channels.end() && !c.lastMessageId) c.lastMessageId = old->second.lastMessageId;
        channels[c.id] = c;
        if (c.guildId) {
            if (Guild* g = FindGuild(c.guildId)) {
                if (std::find(g->channels.begin(), g->channels.end(), c.id) == g->channels.end())
                    g->channels.push_back(c.id);
                RecomputePermissions(c.guildId);
            }
        } else if (c.IsDM()) {
            if (std::find(dms.begin(), dms.end(), c.id) == dms.end()) dms.push_back(c.id);
            SortDms();
        }
        ch.kind = Change::ChannelsChanged;
        ch.guildId = c.guildId;
        ch.channelId = c.id;
    } else if (t == "CHANNEL_DELETE") {
        Snowflake id = J::Id(d, "id");
        auto it = channels.find(id);
        if (it != channels.end()) {
            if (Guild* g = FindGuild(it->second.guildId))
                g->channels.erase(std::remove(g->channels.begin(), g->channels.end(), id), g->channels.end());
            ch.guildId = it->second.guildId;
            channels.erase(it);
        }
        dms.erase(std::remove(dms.begin(), dms.end(), id), dms.end());
        messages.erase(id);
        ch.kind = Change::ChannelsChanged;
        ch.channelId = id;
    } else if (t == "CHANNEL_RECIPIENT_ADD" || t == "CHANNEL_RECIPIENT_REMOVE") {
        auto it = channels.find(J::Id(d, "channel_id"));
        User u = Parse::UserObj(J::Get(d, "user"));
        RememberUser(u);
        if (it != channels.end()) {
            auto& r = it->second.recipients;
            r.erase(std::remove(r.begin(), r.end(), u.id), r.end());
            if (t == "CHANNEL_RECIPIENT_ADD") r.push_back(u.id);
        }
    } else if (t == "CHANNEL_UNREAD_UPDATE") {
        for (yyjson_val* u : J::Arr(d, "channel_unread_updates")) {
            auto it = channels.find(J::Id(u, "id"));
            if (it != channels.end()) it->second.lastMessageId = std::max(it->second.lastMessageId, J::Id(u, "last_message_id"));
        }
    } else if (t == "GUILD_CREATE") {
        ApplyGuild(d, nullptr);
        Snowflake gid = J::Id(d, "id");
        bool listed = false;
        for (const auto& f : folders)
            if (std::find(f.guildIds.begin(), f.guildIds.end(), gid) != f.guildIds.end()) listed = true;
        if (!listed) {
            GuildFolder f;
            f.guildIds.push_back(gid);
            folders.insert(folders.begin(), f);
        }
        ch.kind = Change::GuildsChanged;
        ch.guildId = gid;
    } else if (t == "GUILD_UPDATE") {
        if (Guild* g = FindGuild(J::Id(d, "id"))) {
            yyjson_val* src = J::Get(d, "properties") ? J::Get(d, "properties") : d;
            if (J::Get(src, "name")) g->name = J::S(src, "name");
            if (J::Get(src, "icon")) g->icon = J::S(src, "icon");
            if (J::Get(src, "owner_id")) g->ownerId = J::Id(src, "owner_id");
            if (J::Get(d, "roles")) {
                g->roles.clear();
                for (yyjson_val* r : J::Arr(d, "roles")) {
                    Role role = Parse::RoleObj(r);
                    g->roles[role.id] = role;
                }
            }
            RecomputePermissions(g->id);
            ch.kind = Change::GuildsChanged;
        }
    } else if (t == "GUILD_DELETE") {
        Snowflake gid = J::Id(d, "id");
        if (J::Bool(d, "unavailable")) {
            if (Guild* g = FindGuild(gid)) g->unavailable = true;
        } else {
            if (Guild* g = FindGuild(gid))
                for (Snowflake cid : g->channels) channels.erase(cid);
            guilds.erase(gid);
            for (auto& f : folders) f.guildIds.erase(std::remove(f.guildIds.begin(), f.guildIds.end(), gid), f.guildIds.end());
            folders.erase(std::remove_if(folders.begin(), folders.end(), [](const GuildFolder& f) { return f.guildIds.empty(); }),
                          folders.end());
        }
        ch.kind = Change::GuildsChanged;
        ch.guildId = gid;
    } else if (t == "GUILD_ROLE_CREATE" || t == "GUILD_ROLE_UPDATE") {
        if (Guild* g = FindGuild(J::Id(d, "guild_id"))) {
            Role r = Parse::RoleObj(J::Get(d, "role"));
            g->roles[r.id] = r;
            RecomputePermissions(g->id);
            ch.kind = Change::ChannelsChanged;
            ch.guildId = g->id;
        }
    } else if (t == "GUILD_ROLE_DELETE") {
        if (Guild* g = FindGuild(J::Id(d, "guild_id"))) {
            g->roles.erase(J::Id(d, "role_id"));
            RecomputePermissions(g->id);
            ch.kind = Change::ChannelsChanged;
            ch.guildId = g->id;
        }
    } else if (t == "GUILD_MEMBER_UPDATE") {
        Snowflake gid = J::Id(d, "guild_id");
        User u = Parse::UserObj(J::Get(d, "user"));
        RememberUser(u);
        if (Guild* g = FindGuild(gid)) {
            Member mem;
            mem.nick = J::S(d, "nick");
            for (yyjson_val* r : J::Arr(d, "roles")) mem.roles.push_back(J::U64(r));
            g->members[u.id] = mem;
            if (u.id == me.id) {
                g->myRoles = mem.roles;
                g->myRolesKnown = true;
                RecomputePermissions(gid);
                ch.kind = Change::ChannelsChanged;
                ch.guildId = gid;
            }
        }
    } else if (t == "GUILD_MEMBERS_CHUNK") {
        Snowflake gid = J::Id(d, "guild_id");
        if (Guild* g = FindGuild(gid)) {
            for (yyjson_val* mv : J::Arr(d, "members")) {
                User u = Parse::UserObj(J::Get(mv, "user"));
                RememberUser(u);
                Member mem;
                mem.nick = J::S(mv, "nick");
                for (yyjson_val* r : J::Arr(mv, "roles")) mem.roles.push_back(J::U64(r));
                g->members[u.id] = mem;
            }
            ch.kind = Change::MembersLoaded;
            ch.guildId = gid;
        }
    } else if (t == "USER_UPDATE") {
        User u = Parse::UserObj(d);
        if (u.id == me.id) me = u;
        RememberUser(u);
    } else if (t == "PRESENCE_UPDATE") {
        Snowflake uid = J::Id(J::Get(d, "user"), "id");
        if (uid) presence[uid] = J::S(d, "status");
    } else if (t == "USER_GUILD_SETTINGS_UPDATE") {
        ApplyGuildSettings(d);
    }
    return ch;
}

void Store::ApplyReady(yyjson_val* d) {
    Snowflake keepMe = me.id;
    (void)keepMe;
    users.clear();
    guilds.clear();
    channels.clear();
    dms.clear();
    readStates.clear();
    guildSettings.clear();
    folders.clear();
    typing.clear();

    me = Parse::UserObj(J::Get(d, "user"));
    RememberUser(me);
    for (yyjson_val* u : J::Arr(d, "users")) RememberUser(Parse::UserObj(u));

    yyjson_val* merged = J::Get(d, "merged_members");
    size_t i = 0;
    for (yyjson_val* g : J::Arr(d, "guilds")) {
        yyjson_val* mm = yyjson_is_arr(merged) ? yyjson_arr_get(merged, i) : nullptr;
        ApplyGuild(g, mm);
        i++;
    }

    for (yyjson_val* c : J::Arr(d, "private_channels")) {
        Channel ch = Parse::ChannelObj(c, 0);
        for (yyjson_val* r : J::Arr(c, "recipients")) RememberUser(Parse::UserObj(r));
        channels[ch.id] = ch;
        dms.push_back(ch.id);
    }
    SortDms();

    yyjson_val* rs = J::Get(d, "read_state");
    ApplyReadStates(yyjson_is_obj(rs) ? J::Get(rs, "entries") : rs);

    yyjson_val* ugs = J::Get(d, "user_guild_settings");
    for (yyjson_val* e : J::Arr(yyjson_is_obj(ugs) ? J::Get(ugs, "entries") : ugs)) ApplyGuildSettings(e);

    ParseFolders(d);
    ready = true;
    LOG("store: %zu guilds, %zu channels, %zu DMs, %zu users", guilds.size(), channels.size(), dms.size(), users.size());
}

void Store::ApplyGuild(yyjson_val* g, yyjson_val* mergedMembers) {
    Snowflake gid = J::Id(g, "id");
    if (!gid) return;
    if (Guild* old = FindGuild(gid))
        for (Snowflake cid : old->channels) channels.erase(cid);
    Guild guild;
    guild.id = gid;
    yyjson_val* props = J::Get(g, "properties");
    yyjson_val* src = props ? props : g;
    guild.name = J::S(src, "name");
    guild.icon = J::S(src, "icon");
    guild.ownerId = J::Id(src, "owner_id");
    guild.unavailable = J::Bool(g, "unavailable");
    guild.memberCount = (int)J::I(g, "member_count");
    for (yyjson_val* r : J::Arr(g, "roles")) {
        Role role = Parse::RoleObj(r);
        guild.roles[role.id] = role;
    }
    for (yyjson_val* e : J::Arr(g, "emojis")) {
        Emoji em = Parse::EmojiObj(e);
        guild.emojis[em.id] = em;
    }
    for (yyjson_val* c : J::Arr(g, "channels")) {
        Channel ch = Parse::ChannelObj(c, gid);
        channels[ch.id] = ch;
        guild.channels.push_back(ch.id);
    }
    auto takeMember = [&](yyjson_val* m) {
        Snowflake uid = J::Id(m, "user_id");
        yyjson_val* u = J::Get(m, "user");
        if (!uid && u) {
            uid = J::Id(u, "id");
            RememberUser(Parse::UserObj(u));
        }
        Member mem;
        mem.nick = J::S(m, "nick");
        for (yyjson_val* r : J::Arr(m, "roles")) mem.roles.push_back(J::U64(r));
        if (uid == me.id) {
            guild.myRoles = mem.roles;
            guild.myRolesKnown = true;
        }
        if (uid) guild.members[uid] = mem;
    };
    for (yyjson_val* m : J::Arr(mergedMembers)) takeMember(m);
    for (yyjson_val* m : J::Arr(g, "members")) takeMember(m);
    guilds[gid] = std::move(guild);
    RecomputePermissions(gid);
}

void Store::ApplyGuildSettings(yyjson_val* e) {
    Snowflake gid = J::Id(e, "guild_id");
    GuildSettings& s = guildSettings[gid];
    s.muted = J::Bool(e, "muted");
    s.mutedChannels.clear();
    for (yyjson_val* o : J::Arr(e, "channel_overrides"))
        if (J::Bool(o, "muted")) s.mutedChannels.push_back(J::Id(o, "channel_id"));
}

void Store::ApplyReadStates(yyjson_val* arr) {
    for (yyjson_val* r : J::Arr(arr)) {
        if (J::I(r, "read_state_type", 0) != 0) continue;
        ReadState rs;
        rs.lastAcked = J::Id(r, "last_message_id");
        rs.mentions = (int)J::I(r, "mention_count");
        readStates[J::Id(r, "id")] = rs;
    }
}

void Store::ParseFolders(yyjson_val* d) {
    folders.clear();
    std::string proto = J::S(d, "user_settings_proto");
    if (!proto.empty()) {
        std::vector<uint8_t> blob = Str::Base64Decode(proto);
        if (!ParseFolderProto(blob, folders)) folders.clear();
    }
    if (folders.empty()) {
        yyjson_val* us = J::Get(d, "user_settings");
        for (yyjson_val* f : J::Arr(us, "guild_folders")) {
            GuildFolder folder;
            folder.id = J::I(f, "id", 0);
            folder.name = J::S(f, "name");
            yyjson_val* color = J::Get(f, "color");
            if (color && !yyjson_is_null(color)) {
                folder.color = (uint32_t)J::Int(color);
                folder.hasColor = true;
            }
            for (yyjson_val* g : J::Arr(f, "guild_ids")) folder.guildIds.push_back(J::U64(g));
            folders.push_back(std::move(folder));
        }
    }
    // Drop guilds we're no longer in, add ones the settings don't know about yet (at the top, like Discord).
    std::unordered_set<Snowflake> seen;
    for (auto& f : folders) {
        f.guildIds.erase(std::remove_if(f.guildIds.begin(), f.guildIds.end(),
                                        [&](Snowflake id) { return !guilds.count(id) || !seen.insert(id).second; }),
                         f.guildIds.end());
    }
    folders.erase(std::remove_if(folders.begin(), folders.end(), [](const GuildFolder& f) { return f.guildIds.empty(); }),
                  folders.end());
    std::vector<Snowflake> missing;
    for (const auto& kv : guilds)
        if (!seen.count(kv.first)) missing.push_back(kv.first);
    std::sort(missing.begin(), missing.end(), std::greater<Snowflake>());
    std::vector<GuildFolder> front;
    for (Snowflake id : missing) {
        GuildFolder f;
        f.guildIds.push_back(id);
        front.push_back(f);
    }
    folders.insert(folders.begin(), front.begin(), front.end());
}

void Store::SortDms() {
    std::sort(dms.begin(), dms.end(), [&](Snowflake a, Snowflake b) {
        auto ca = channels.find(a), cb = channels.find(b);
        Snowflake la = ca != channels.end() && ca->second.lastMessageId ? ca->second.lastMessageId : a;
        Snowflake lb = cb != channels.end() && cb->second.lastMessageId ? cb->second.lastMessageId : b;
        return la > lb;
    });
}

void Store::TouchDm(Snowflake channelId) {
    auto it = std::find(dms.begin(), dms.end(), channelId);
    if (it == dms.end()) {
        dms.insert(dms.begin(), channelId);
    } else {
        dms.erase(it);
        dms.insert(dms.begin(), channelId);
    }
}

uint64_t Store::ComputePerms(const Guild& g, const Channel& c) const {
    if (g.ownerId && g.ownerId == me.id) return Perm::ALL;
    uint64_t base = 0;
    auto ev = g.roles.find(g.id);
    if (ev != g.roles.end()) base = ev->second.permissions;
    for (Snowflake rid : g.myRoles) {
        auto r = g.roles.find(rid);
        if (r != g.roles.end()) base |= r->second.permissions;
    }
    if (base & Perm::ADMINISTRATOR) return Perm::ALL;
    const Channel* src = &c;
    if (c.IsThread() && c.parentId) {
        auto p = channels.find(c.parentId);
        if (p != channels.end()) src = &p->second;
    }
    uint64_t p = base;
    for (const auto& ow : src->overwrites) {
        if (ow.id == g.id) {
            p &= ~ow.deny;
            p |= ow.allow;
        }
    }
    uint64_t allow = 0, deny = 0;
    for (const auto& ow : src->overwrites) {
        if (ow.type == 0 && ow.id != g.id && std::find(g.myRoles.begin(), g.myRoles.end(), ow.id) != g.myRoles.end()) {
            allow |= ow.allow;
            deny |= ow.deny;
        }
    }
    p &= ~deny;
    p |= allow;
    for (const auto& ow : src->overwrites) {
        if (ow.type == 1 && ow.id == me.id) {
            p &= ~ow.deny;
            p |= ow.allow;
        }
    }
    return p;
}

void Store::RecomputePermissions(Snowflake guildId) {
    Guild* g = FindGuild(guildId);
    if (!g) return;
    for (Snowflake cid : g->channels) {
        auto it = channels.find(cid);
        if (it == channels.end()) continue;
        Channel& c = it->second;
        if (!g->myRolesKnown && g->ownerId != me.id) {
            c.canView = c.canSend = c.canReact = true;
            c.canManage = false;
            continue;
        }
        uint64_t p = ComputePerms(*g, c);
        c.canView = (p & Perm::VIEW_CHANNEL) != 0;
        c.canSend = c.IsThread() ? (p & Perm::SEND_MESSAGES_IN_THREADS) != 0 : (p & Perm::SEND_MESSAGES) != 0;
        c.canReact = (p & Perm::ADD_REACTIONS) != 0;
        c.canManage = (p & Perm::MANAGE_MESSAGES) != 0;
    }
}

std::vector<Store::Row> Store::ChannelList(Snowflake guildId) const {
    std::vector<Row> rows;
    const Guild* g = FindGuild(guildId);
    if (!g) return rows;
    std::vector<const Channel*> top, cats;
    std::unordered_map<Snowflake, std::vector<const Channel*>> children;
    for (Snowflake cid : g->channels) {
        auto it = channels.find(cid);
        if (it == channels.end()) continue;
        const Channel& c = it->second;
        if (c.IsThread() || c.type == CH_GUILD_DIRECTORY) continue;
        if (c.IsCategory()) {
            cats.push_back(&c);
            continue;
        }
        if (!c.canView) continue;
        if (c.parentId && channels.count(c.parentId)) children[c.parentId].push_back(&c);
        else top.push_back(&c);
    }
    std::sort(top.begin(), top.end(), ChannelOrder);
    std::sort(cats.begin(), cats.end(), [](const Channel* a, const Channel* b) {
        return a->position != b->position ? a->position < b->position : a->id < b->id;
    });
    for (const Channel* c : top) rows.push_back({c->id, false});
    for (const Channel* cat : cats) {
        auto it = children.find(cat->id);
        if (it == children.end() || it->second.empty()) continue;
        rows.push_back({cat->id, true});
        std::sort(it->second.begin(), it->second.end(), ChannelOrder);
        for (const Channel* c : it->second) rows.push_back({c->id, false});
    }
    return rows;
}

std::string Store::ChannelName(const Channel& c) const {
    if (!c.IsDM()) return c.name;
    if (!c.name.empty()) return c.name;
    std::string out;
    for (Snowflake uid : c.recipients) {
        const User* u = FindUser(uid);
        if (!out.empty()) out += ", ";
        out += u ? u->DisplayName() : "Unknown";
    }
    return out.empty() ? "Unnamed" : out;
}

std::string Store::ChannelName(Snowflake id) const {
    const Channel* c = FindChannel(id);
    return c ? ChannelName(*c) : "unknown-channel";
}

std::string Store::DmAvatarUrl(const Channel& c) const {
    if (c.type == CH_DM && !c.recipients.empty()) {
        const User* u = FindUser(c.recipients[0]);
        if (u) return u->AvatarUrl(64);
        return "";
    }
    if (!c.icon.empty())
        return Str::Format("https://cdn.discordapp.com/channel-icons/%llu/%s.png?size=64", (unsigned long long)c.id,
                           c.icon.c_str());
    return "";
}

const User* Store::FindUser(Snowflake id) const {
    auto it = users.find(id);
    return it == users.end() ? nullptr : &it->second;
}

const Channel* Store::FindChannel(Snowflake id) const {
    auto it = channels.find(id);
    return it == channels.end() ? nullptr : &it->second;
}

Guild* Store::FindGuild(Snowflake id) {
    auto it = guilds.find(id);
    return it == guilds.end() ? nullptr : &it->second;
}

const Guild* Store::FindGuild(Snowflake id) const {
    auto it = guilds.find(id);
    return it == guilds.end() ? nullptr : &it->second;
}

std::string Store::MemberName(Snowflake guildId, Snowflake userId) const {
    if (const Guild* g = FindGuild(guildId)) {
        auto it = g->members.find(userId);
        if (it != g->members.end() && !it->second.nick.empty()) return it->second.nick;
    }
    if (const User* u = FindUser(userId)) return u->DisplayName();
    return "Unknown User";
}

std::string Store::AuthorName(const Message& m) const {
    if (m.hasMember && !m.nick.empty()) return m.nick;
    if (m.guildId) {
        if (const Guild* g = FindGuild(m.guildId)) {
            auto it = g->members.find(m.author.id);
            if (it != g->members.end() && !it->second.nick.empty()) return it->second.nick;
        }
    }
    std::string n = m.author.DisplayName();
    return n.empty() ? "Unknown User" : n;
}

uint32_t Store::AuthorColor(const Message& m) const {
    if (!m.guildId) return 0;
    const Guild* g = FindGuild(m.guildId);
    if (!g) return 0;
    const std::vector<Snowflake>* roles = nullptr;
    if (m.hasMember) {
        roles = &m.memberRoles;
    } else {
        auto it = g->members.find(m.author.id);
        if (it != g->members.end()) roles = &it->second.roles;
    }
    if (!roles) return 0;
    int best = -1;
    uint32_t color = 0;
    for (Snowflake rid : *roles) {
        auto r = g->roles.find(rid);
        if (r != g->roles.end() && r->second.color && r->second.position > best) {
            best = r->second.position;
            color = r->second.color;
        }
    }
    return color;
}

std::string Store::RoleName(Snowflake guildId, Snowflake roleId) const {
    if (const Guild* g = FindGuild(guildId)) {
        auto it = g->roles.find(roleId);
        if (it != g->roles.end()) return it->second.name;
    }
    return "deleted-role";
}

uint32_t Store::RoleColor(Snowflake guildId, Snowflake roleId) const {
    if (const Guild* g = FindGuild(guildId)) {
        auto it = g->roles.find(roleId);
        if (it != g->roles.end()) return it->second.color;
    }
    return 0;
}

bool Store::MentionsMe(const Message& m) const {
    if (std::find(m.mentionIds.begin(), m.mentionIds.end(), me.id) != m.mentionIds.end()) return true;
    if (m.mentionEveryone) return true;
    if (!m.mentionRoles.empty()) {
        if (const Guild* g = FindGuild(m.guildId))
            for (Snowflake r : m.mentionRoles)
                if (std::find(g->myRoles.begin(), g->myRoles.end(), r) != g->myRoles.end()) return true;
    }
    return false;
}

bool Store::ChannelMuted(const Channel& c) const {
    auto it = guildSettings.find(c.guildId);
    if (it == guildSettings.end()) return false;
    const auto& mc = it->second.mutedChannels;
    return std::find(mc.begin(), mc.end(), c.id) != mc.end() ||
           (c.parentId && std::find(mc.begin(), mc.end(), c.parentId) != mc.end());
}

bool Store::GuildMuted(Snowflake guildId) const {
    auto it = guildSettings.find(guildId);
    return it != guildSettings.end() && it->second.muted;
}

bool Store::ChannelUnread(Snowflake id) const {
    const Channel* c = FindChannel(id);
    if (!c || !c->HasMessages() || !c->canView || !c->lastMessageId) return false;
    auto rs = readStates.find(id);
    if (rs == readStates.end()) return false;
    return c->lastMessageId > rs->second.lastAcked;
}

int Store::ChannelMentions(Snowflake id) const {
    auto rs = readStates.find(id);
    return rs == readStates.end() ? 0 : rs->second.mentions;
}

bool Store::GuildUnread(Snowflake guildId) const {
    const Guild* g = FindGuild(guildId);
    if (!g || GuildMuted(guildId)) return false;
    for (Snowflake cid : g->channels) {
        const Channel* c = FindChannel(cid);
        if (c && !c->IsThread() && !ChannelMuted(*c) && ChannelUnread(cid)) return true;
    }
    return false;
}

int Store::GuildMentions(Snowflake guildId) const {
    const Guild* g = FindGuild(guildId);
    if (!g) return 0;
    int n = 0;
    for (Snowflake cid : g->channels) {
        const Channel* c = FindChannel(cid);
        if (c && c->canView) n += ChannelMentions(cid);
    }
    return n;
}

int Store::DmMentions() const {
    int n = 0;
    for (Snowflake cid : dms) n += ChannelMentions(cid);
    return n;
}

std::vector<Snowflake> Store::TypingUsers(Snowflake channelId, int64_t nowMs) const {
    std::vector<Snowflake> out;
    auto it = typing.find(channelId);
    if (it == typing.end()) return out;
    for (const auto& kv : it->second)
        if (kv.second > nowMs && kv.first != me.id) out.push_back(kv.first);
    return out;
}

ChannelMessages& Store::Messages(Snowflake channelId) {
    ChannelMessages& cm = messages[channelId];
    cm.lastUsedMs = Platform::NowMs();
    return cm;
}

void Store::InsertSorted(ChannelMessages& cm, Message m) {
    auto& list = cm.list;
    if (list.empty() || m.id > list.back().id) {
        list.push_back(std::move(m));
        return;
    }
    auto it = std::lower_bound(list.begin(), list.end(), m.id, [](const Message& a, Snowflake id) { return a.id < id; });
    if (it != list.end() && it->id == m.id) {
        *it = std::move(m);
        return;
    }
    list.insert(it, std::move(m));
}

void Store::AddHistory(Snowflake channelId, std::vector<Message> page, bool older, int requested) {
    version++;
    ChannelMessages& cm = messages[channelId];
    std::reverse(page.begin(), page.end());  // the API returns newest first
    for (const auto& m : page) RememberUser(m.author);
    if (const Channel* c = FindChannel(channelId)) {
        if (c->guildId) {
            if (Guild* g = FindGuild(c->guildId)) {
                for (auto& m : page) {
                    m.guildId = c->guildId;
                    if (m.hasMember) g->members[m.author.id] = Member{m.nick, m.memberRoles};
                }
            }
        }
    }
    if (!older) {
        // Keep anything that arrived over the gateway while the page loaded.
        Snowflake newest = page.empty() ? 0 : page.back().id;
        std::vector<Message> keep;
        for (auto& m : cm.list)
            if (m.id > newest || m.pending || m.failed) keep.push_back(std::move(m));
        cm.list = std::move(page);
        for (auto& m : keep) InsertSorted(cm, std::move(m));
        cm.loaded = true;
        cm.loading = false;
        cm.error = false;
    } else {
        cm.loadingOlder = false;
        Snowflake oldest = cm.list.empty() ? ~0ull : cm.list.front().id;
        std::vector<Message> merged;
        for (auto& m : page)
            if (m.id < oldest) merged.push_back(std::move(m));
        merged.insert(merged.end(), std::make_move_iterator(cm.list.begin()), std::make_move_iterator(cm.list.end()));
        cm.list = std::move(merged);
    }
    if ((int)page.size() < requested) cm.reachedStart = true;
}

void Store::AddLocalEcho(Message m) {
    version++;
    ChannelMessages& cm = messages[m.channelId];
    m.pending = true;
    cm.list.push_back(std::move(m));
}

void Store::ConfirmSent(const std::string& nonce, Message m) {
    version++;
    ChannelMessages& cm = messages[m.channelId];
    for (size_t i = 0; i < cm.list.size(); i++) {
        if (cm.list[i].pending && cm.list[i].nonce == nonce) {
            cm.list.erase(cm.list.begin() + i);
            break;
        }
    }
    for (const auto& existing : cm.list)
        if (existing.id == m.id) return;  // the gateway beat us to it
    InsertSorted(cm, std::move(m));
}

void Store::MarkFailed(Snowflake channelId, const std::string& nonce) {
    version++;
    for (auto& m : messages[channelId].list) {
        if (m.nonce == nonce && m.pending) {
            m.pending = false;
            m.failed = true;
            m.layoutKey = -1;
        }
    }
}

Message* Store::FindMessage(Snowflake channelId, Snowflake id) {
    auto it = messages.find(channelId);
    if (it == messages.end()) return nullptr;
    auto& list = it->second.list;
    auto m = std::lower_bound(list.begin(), list.end(), id, [](const Message& a, Snowflake v) { return a.id < v; });
    if (m != list.end() && m->id == id) return &*m;
    for (auto& x : list)  // local echoes aren't in id order
        if (x.id == id) return &x;
    return nullptr;
}

void Store::MarkRead(Snowflake channelId, Snowflake messageId) {
    version++;
    ReadState& rs = readStates[channelId];
    rs.lastAcked = std::max(rs.lastAcked, messageId);
    rs.mentions = 0;
}

void Store::TrimCaches(Snowflake keepChannel) {
    std::vector<std::pair<int64_t, Snowflake>> used;
    for (auto& kv : messages)
        if (kv.first != keepChannel) used.push_back({kv.second.lastUsedMs, kv.first});
    std::sort(used.begin(), used.end(), std::greater<>());
    for (size_t i = 8; i < used.size(); i++) messages.erase(used[i].second);
    for (auto& kv : messages) {
        auto& list = kv.second.list;
        if (kv.first != keepChannel && list.size() > 150) {
            list.erase(list.begin(), list.end() - 100);
            kv.second.reachedStart = false;
        }
    }
}

std::vector<Snowflake> Store::UnknownMembers(Snowflake guildId, const std::vector<Message>& msgs) const {
    std::vector<Snowflake> out;
    const Guild* g = FindGuild(guildId);
    if (!g) return out;
    std::unordered_set<Snowflake> seen;
    for (const auto& m : msgs) {
        Snowflake uid = m.author.id;
        if (!uid || m.hasMember || m.author.discriminator == "0000") continue;
        if (g->members.count(uid) || !seen.insert(uid).second) continue;
        out.push_back(uid);
    }
    return out;
}
