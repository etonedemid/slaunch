#include "discord/parse.h"

#include "util/strings.h"

namespace Parse {

User UserObj(yyjson_val* v) {
    User u;
    u.id = J::Id(v, "id");
    u.username = J::S(v, "username");
    u.globalName = J::S(v, "global_name");
    u.avatar = J::S(v, "avatar");
    u.discriminator = J::S(v, "discriminator");
    u.bot = J::Bool(v, "bot");
    u.premiumType = (int)J::I(v, "premium_type");
    return u;
}

Role RoleObj(yyjson_val* v) {
    Role r;
    r.id = J::Id(v, "id");
    r.name = J::S(v, "name");
    r.color = (uint32_t)J::I(v, "color");
    if (!r.color) {
        yyjson_val* colors = J::Get(v, "colors");
        r.color = (uint32_t)J::I(colors, "primary_color");
    }
    r.position = (int)J::I(v, "position");
    r.permissions = J::U64(J::Get(v, "permissions"));
    return r;
}

Emoji EmojiObj(yyjson_val* v) {
    Emoji e;
    e.id = J::Id(v, "id");
    e.name = J::S(v, "name");
    e.animated = J::Bool(v, "animated");
    return e;
}

Channel ChannelObj(yyjson_val* v, Snowflake guildId) {
    Channel c;
    c.id = J::Id(v, "id");
    c.guildId = J::Id(v, "guild_id");
    if (!c.guildId) c.guildId = guildId;
    c.parentId = J::Id(v, "parent_id");
    c.lastMessageId = J::Id(v, "last_message_id");
    c.ownerId = J::Id(v, "owner_id");
    c.type = (int)J::I(v, "type");
    c.position = (int)J::I(v, "position");
    c.name = J::S(v, "name");
    c.topic = J::S(v, "topic");
    c.icon = J::S(v, "icon");
    c.nsfw = J::Bool(v, "nsfw");
    for (yyjson_val* o : J::Arr(v, "permission_overwrites")) {
        Overwrite ow;
        ow.id = J::Id(o, "id");
        yyjson_val* t = J::Get(o, "type");
        ow.type = yyjson_is_str(t) ? (J::Str(t) == "member" ? 1 : 0) : (int)J::Int(t);
        ow.allow = J::U64(J::Get(o, "allow"));
        ow.deny = J::U64(J::Get(o, "deny"));
        c.overwrites.push_back(ow);
    }
    for (yyjson_val* r : J::Arr(v, "recipient_ids")) c.recipients.push_back(J::U64(r));
    for (yyjson_val* r : J::Arr(v, "recipients")) c.recipients.push_back(J::Id(r, "id"));
    return c;
}

Reaction ReactionEmoji(yyjson_val* emoji) {
    Reaction r;
    r.emojiId = J::Id(emoji, "id");
    r.emojiName = J::S(emoji, "name");
    r.animated = J::Bool(emoji, "animated");
    return r;
}

namespace {

void ParseMedia(yyjson_val* media, std::string& url, std::string& orig, int& w, int& h) {
    if (!media) return;
    orig = J::S(media, "url");
    url = J::S(media, "proxy_url");
    if (url.empty()) url = orig;
    w = (int)J::I(media, "width");
    h = (int)J::I(media, "height");
}

Attachment AttachmentObj(yyjson_val* a) {
    Attachment at;
    at.id = J::Id(a, "id");
    at.filename = J::S(a, "filename");
    at.url = J::S(a, "url");
    at.proxyUrl = J::S(a, "proxy_url");
    at.contentType = J::S(a, "content_type");
    at.description = J::S(a, "description");
    at.width = (int)J::I(a, "width");
    at.height = (int)J::I(a, "height");
    at.size = (uint64_t)J::I(a, "size");
    return at;
}

Embed EmbedObj(yyjson_val* e) {
    Embed em;
    em.type = J::S(e, "type");
    em.title = J::S(e, "title");
    em.url = J::S(e, "url");
    em.description = J::S(e, "description");
    yyjson_val* color = J::Get(e, "color");
    if (color && !yyjson_is_null(color)) {
        em.color = (uint32_t)J::Int(color);
        em.hasColor = true;
    }
    em.authorName = J::S(J::Get(e, "author"), "name");
    em.footer = J::S(J::Get(e, "footer"), "text");
    em.provider = J::S(J::Get(e, "provider"), "name");
    ParseMedia(J::Get(e, "image"), em.imageUrl, em.imageOrig, em.imageW, em.imageH);
    ParseMedia(J::Get(e, "thumbnail"), em.thumbUrl, em.thumbOrig, em.thumbW, em.thumbH);
    em.videoUrl = J::S(J::Get(e, "video"), "url");
    for (yyjson_val* f : J::Arr(e, "fields")) {
        EmbedField ef;
        ef.name = J::S(f, "name");
        ef.value = J::S(f, "value");
        ef.isInline = J::Bool(f, "inline");
        em.fields.push_back(std::move(ef));
    }
    return em;
}

std::string SystemText(const Message& m, yyjson_val* v) {
    auto firstMention = [&]() -> std::string {
        yyjson_val* arr = J::Get(v, "mentions");
        if (J::Len(arr) == 0) return "someone";
        User u = UserObj(yyjson_arr_get_first(arr));
        return u.DisplayName();
    };
    switch (m.type) {
        case 1: return "added " + firstMention() + " to the group.";
        case 2: {
            std::string who = firstMention();
            return who == m.author.DisplayName() ? "left the group." : "removed " + who + " from the group.";
        }
        case 3: return "started a call.";
        case 4: return "changed the channel name: " + m.content;
        case 5: return "changed the channel icon.";
        case 6: return "pinned a message to this channel.";
        case 7: return "joined the server.";
        case 8: return "just boosted the server!";
        case 9: return "just boosted the server! It reached Level 1!";
        case 10: return "just boosted the server! It reached Level 2!";
        case 11: return "just boosted the server! It reached Level 3!";
        case 12: return "added " + m.content + " to this channel.";
        case 18: return "started a thread: " + m.content;
        case 24: return "AutoMod blocked a message.";
        case 46: return "a poll has closed.";
        default: return "";
    }
}

}  // namespace

void MessageUpdate(Message& m, yyjson_val* v) {
    if (J::Get(v, "content")) m.content = J::S(v, "content");
    if (J::Get(v, "edited_timestamp") && !J::IsNull(v, "edited_timestamp")) m.edited = true;
    if (J::Get(v, "pinned")) m.pinned = J::Bool(v, "pinned");
    if (J::Get(v, "attachments")) {
        m.attachments.clear();
        for (yyjson_val* a : J::Arr(v, "attachments")) m.attachments.push_back(AttachmentObj(a));
    }
    if (J::Get(v, "embeds")) {
        m.embeds.clear();
        for (yyjson_val* e : J::Arr(v, "embeds")) m.embeds.push_back(EmbedObj(e));
    }
    if (J::Get(v, "mentions")) {
        m.mentionIds.clear();
        for (yyjson_val* u : J::Arr(v, "mentions")) m.mentionIds.push_back(J::Id(u, "id"));
    }
    if (J::Get(v, "mention_roles")) {
        m.mentionRoles.clear();
        for (yyjson_val* r : J::Arr(v, "mention_roles")) m.mentionRoles.push_back(J::U64(r));
    }
    if (J::Get(v, "mention_everyone")) m.mentionEveryone = J::Bool(v, "mention_everyone");
    if (J::Get(v, "flags")) m.flags = (int)J::I(v, "flags");
}

Message MessageObj(yyjson_val* v) {
    Message m;
    m.id = J::Id(v, "id");
    m.channelId = J::Id(v, "channel_id");
    m.guildId = J::Id(v, "guild_id");
    m.type = (int)J::I(v, "type");
    m.content = J::S(v, "content");
    m.author = UserObj(J::Get(v, "author"));
    if (yyjson_val* member = J::Get(v, "member")) {
        m.hasMember = true;
        m.nick = J::S(member, "nick");
        for (yyjson_val* r : J::Arr(member, "roles")) m.memberRoles.push_back(J::U64(r));
    }
    m.edited = !J::IsNull(v, "edited_timestamp");
    m.pinned = J::Bool(v, "pinned");
    m.mentionEveryone = J::Bool(v, "mention_everyone");
    m.flags = (int)J::I(v, "flags");
    for (yyjson_val* u : J::Arr(v, "mentions")) m.mentionIds.push_back(J::Id(u, "id"));
    for (yyjson_val* r : J::Arr(v, "mention_roles")) m.mentionRoles.push_back(J::U64(r));
    for (yyjson_val* a : J::Arr(v, "attachments")) m.attachments.push_back(AttachmentObj(a));
    for (yyjson_val* e : J::Arr(v, "embeds")) m.embeds.push_back(EmbedObj(e));
    for (yyjson_val* r : J::Arr(v, "reactions")) {
        Reaction re = ReactionEmoji(J::Get(r, "emoji"));
        re.count = (int)J::I(r, "count");
        re.me = J::Bool(r, "me");
        m.reactions.push_back(std::move(re));
    }
    for (yyjson_val* s : J::Arr(v, "sticker_items")) {
        StickerItem st;
        st.id = J::Id(s, "id");
        st.name = J::S(s, "name");
        st.format = (int)J::I(s, "format_type", 1);
        m.stickers.push_back(std::move(st));
    }
    yyjson_val* nonce = J::Get(v, "nonce");
    if (yyjson_is_str(nonce)) m.nonce = J::Str(nonce);
    else if (nonce && !yyjson_is_null(nonce)) m.nonce = std::to_string(J::U64(nonce));

    yyjson_val* ref = J::Get(v, "message_reference");
    if (ref) {
        int refType = (int)J::I(ref, "type");
        yyjson_val* snaps = J::Get(v, "message_snapshots");
        if (refType == 1 && J::Len(snaps) > 0) {
            // A forward: show the snapshot's content as this message's body.
            yyjson_val* snap = J::Get(yyjson_arr_get_first(snaps), "message");
            m.forwarded = true;
            m.content = J::S(snap, "content");
            for (yyjson_val* a : J::Arr(snap, "attachments")) m.attachments.push_back(AttachmentObj(a));
            for (yyjson_val* e : J::Arr(snap, "embeds")) m.embeds.push_back(EmbedObj(e));
        } else if (m.type == 19 || m.type == 21 || J::Get(v, "referenced_message")) {
            m.replyId = J::Id(ref, "message_id");
            yyjson_val* rm = J::Get(v, "referenced_message");
            if (rm && yyjson_is_obj(rm)) {
                m.replied = std::make_shared<Message>(MessageObj(rm));
            } else if (m.type == 19) {
                m.replyMissing = true;
            }
        }
    }

    if (yyjson_val* meta = J::Get(v, "interaction_metadata")) {
        std::string name = J::S(meta, "name");
        if (!name.empty()) m.commandName = "/" + name;
    } else if (yyjson_val* inter = J::Get(v, "interaction")) {
        std::string name = J::S(inter, "name");
        if (!name.empty()) m.commandName = "/" + name;
    }

    if (yyjson_val* poll = J::Get(v, "poll")) {
        std::string text = "\xF0\x9F\x93\x8A **" + J::S(J::Get(poll, "question"), "text") + "**";
        for (yyjson_val* a : J::Arr(poll, "answers")) {
            yyjson_val* media = J::Get(a, "poll_media");
            text += "\n\xE2\x80\xA2 " + J::S(media, "text");
        }
        if (!m.content.empty()) m.content += "\n";
        m.content += text;
    }

    m.systemText = SystemText(m, v);
    return m;
}

}  // namespace Parse
