#pragma once

#include "discord/types.h"
#include "util/json.h"

// JSON -> model conversion. Pure functions, safe to call on worker threads.
namespace Parse {

User UserObj(yyjson_val* v);
Role RoleObj(yyjson_val* v);
Emoji EmojiObj(yyjson_val* v);
Channel ChannelObj(yyjson_val* v, Snowflake guildId);
Message MessageObj(yyjson_val* v);
// Fills fields present in a partial message (MESSAGE_UPDATE).
void MessageUpdate(Message& m, yyjson_val* v);
Reaction ReactionEmoji(yyjson_val* emoji);

}  // namespace Parse
