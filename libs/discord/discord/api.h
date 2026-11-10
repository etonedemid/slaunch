#pragma once

#include <functional>
#include <string>
#include <vector>

#include "discord/types.h"
#include "net/http.h"

// Discord REST. Blocking calls for worker threads, plus async wrappers whose
// callbacks run on the main thread and are dropped if the account changed meanwhile.
namespace Api {

constexpr const char* kBase = "https://discord.com/api/v9";

void SetToken(const std::string& token);
std::string Token();
int Generation();

Http::Response Call(const std::string& method, const std::string& path, const std::string& body = "",
                    const std::string& token = "", const std::string& contentType = "application/json",
                    long timeoutMs = 20000);
std::string ErrorText(const Http::Response& r);

using Done = std::function<void(bool ok, const std::string& err)>;

void FetchMessages(Snowflake channel, Snowflake before, int limit,
                   std::function<void(bool ok, std::vector<Message> msgs, const std::string& err)> cb);
void SendMessage(Snowflake channel, const std::string& content, const std::string& nonce, Snowflake replyTo,
                 bool pingReply, std::function<void(bool ok, Message m, const std::string& err)> cb);
// Sends a file from the SD card as a message, with optional text. Reads it on the worker.
void UploadFile(Snowflake channel, const std::string& path, const std::string& content, const std::string& nonce,
                std::function<void(bool ok, Message m, const std::string& err)> cb);
void EditMessage(Snowflake channel, Snowflake id, const std::string& content, Done cb);
void DeleteMessage(Snowflake channel, Snowflake id, Done cb);
void React(Snowflake channel, Snowflake id, const std::string& emojiParam, bool add, Done cb);
void Ack(Snowflake channel, Snowflake message);
void Typing(Snowflake channel);
void OpenDM(Snowflake userId, std::function<void(bool ok, Channel c)> cb);
// Validates a token and returns the account it belongs to.
void FetchMe(const std::string& token, std::function<void(bool ok, User u, const std::string& err)> cb);

std::string NewNonce();

}  // namespace Api
