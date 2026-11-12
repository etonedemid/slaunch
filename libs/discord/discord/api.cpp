#include "discord/api.h"

#include <atomic>
#include <cstdio>
#include <chrono>
#include <mutex>
#include <thread>

#include "discord/parse.h"
#include "net/async.h"
#include "platform/platform.h"
#include "util/json.h"
#include "util/log.h"
#include "util/strings.h"

namespace Api {
namespace {

std::mutex g_mu;
std::string g_token;
std::atomic<int> g_gen{0};

// Runs fn on the main thread unless the account was switched in the meantime.
void PostIfCurrent(int gen, std::function<void()> fn) {
    Async::Post([gen, fn = std::move(fn)] {
        if (gen == g_gen.load()) fn();
    });
}

}  // namespace

void SetToken(const std::string& token) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_token = token;
    g_gen++;
}

std::string Token() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_token;
}

int Generation() { return g_gen.load(); }

Http::Response Call(const std::string& method, const std::string& path, const std::string& body,
                    const std::string& tokenOverride, const std::string& contentType, long timeoutMs) {
    Http::Request req;
    req.method = method;
    req.url = Str::StartsWith(path, "http") ? path : std::string(kBase) + path;
    req.body = body;
    req.timeoutMs = timeoutMs;
    std::string token = tokenOverride.empty() ? Token() : tokenOverride;
    if (!token.empty()) req.headers.push_back("Authorization: " + token);
    req.headers.push_back(std::string("User-Agent: ") + Client::kUserAgent);
    req.headers.push_back("X-Super-Properties: " + Client::SuperPropertiesB64());
    req.headers.push_back("X-Discord-Locale: en-US");
    req.headers.push_back("Origin: https://discord.com");
    req.headers.push_back("Referer: https://discord.com/channels/@me");
    if (!body.empty() || method == "POST" || method == "PATCH" || method == "PUT")
        req.headers.push_back("Content-Type: " + contentType);

    Http::Response res;
    for (int attempt = 0; attempt < 3; attempt++) {
        res = Http::Perform(req);
        if (res.status != 429) break;
        double wait = res.retryAfter;
        if (auto doc = J::Parse(res.body)) {
            yyjson_val* ra = J::Get(J::Root(doc), "retry_after");
            if (yyjson_is_num(ra)) wait = yyjson_get_num(ra);
        }
        if (wait <= 0) wait = 1;
        if (wait > 15) break;
        LOG("rate limited on %s, waiting %.2fs", path.c_str(), wait);
        std::this_thread::sleep_for(std::chrono::milliseconds((int)(wait * 1000) + 50));
    }
    return res;
}

std::string ErrorText(const Http::Response& r) {
    if (!r.error.empty()) return "Network error: " + r.error;
    // Discord's own text here is just "401: Unauthorized".
    if (r.status == 401)
        return "Discord didn't accept this token. Copy a fresh one from a browser that is still signed in "
               "(signing out there cancels it), or sign in with the QR code.";
    if (auto doc = J::Parse(r.body)) {
        yyjson_val* root = J::Root(doc);
        std::string msg = J::S(root, "message");
        if (J::Get(root, "captcha_key"))
            return "Discord asked for a CAPTCHA, which sLaunch cannot show. Try the QR code again later, or press Y to use a token.";
        if (!msg.empty()) return msg;
    }
    if (r.status == 403) return "You don't have permission to do that.";
    if (r.status == 429) return "You're being rate limited. Try again in a moment.";
    return Str::Format("Discord returned HTTP %ld.", r.status);
}

void FetchMessages(Snowflake channel, Snowflake before, int limit,
                   std::function<void(bool, std::vector<Message>, const std::string&)> cb) {
    int gen = g_gen.load();
    Async::Run(Async::Pool::Api, [=] {
        std::string path = Str::Format("/channels/%llu/messages?limit=%d", (unsigned long long)channel, limit);
        if (before) path += Str::Format("&before=%llu", (unsigned long long)before);
        Http::Response r = Call("GET", path);
        std::vector<Message> msgs;
        bool ok = r.Ok();
        std::string err;
        if (ok) {
            auto doc = J::Parse(r.body);
            for (yyjson_val* v : J::Arr(J::Root(doc))) msgs.push_back(Parse::MessageObj(v));
        } else {
            err = ErrorText(r);
        }
        PostIfCurrent(gen, [cb, ok, msgs = std::move(msgs), err]() mutable { cb(ok, std::move(msgs), err); });
    });
}

void SendMessage(Snowflake channel, const std::string& content, const std::string& nonce, Snowflake replyTo,
                 bool pingReply, std::function<void(bool, Message, const std::string&)> cb) {
    int gen = g_gen.load();
    std::string body = "{\"content\":" + Str::Json(content) + ",\"nonce\":" + Str::Json(nonce) +
                       ",\"tts\":false,\"flags\":0,\"mobile_network_type\":\"unknown\"";
    if (replyTo) {
        body += Str::Format(
            ",\"message_reference\":{\"message_id\":\"%llu\",\"channel_id\":\"%llu\",\"fail_if_not_exists\":false}",
            (unsigned long long)replyTo, (unsigned long long)channel);
        body += Str::Format(",\"allowed_mentions\":{\"parse\":[\"users\",\"roles\",\"everyone\"],\"replied_user\":%s}",
                            pingReply ? "true" : "false");
    }
    body += "}";
    Async::Run(Async::Pool::Api, [=] {
        Http::Response r = Call("POST", Str::Format("/channels/%llu/messages", (unsigned long long)channel), body);
        Message m;
        bool ok = r.Ok();
        std::string err;
        if (ok) {
            auto doc = J::Parse(r.body);
            m = Parse::MessageObj(J::Root(doc));
        } else {
            err = ErrorText(r);
        }
        PostIfCurrent(gen, [cb, ok, m = std::move(m), err]() mutable { cb(ok, std::move(m), err); });
    });
}

void UploadFile(Snowflake channel, const std::string& path, const std::string& content, const std::string& nonce,
                std::function<void(bool, Message, const std::string&)> cb) {
    int gen = g_gen.load();
    Async::Run(Async::Pool::Api, [=] {
        std::string data, err;
        if (FILE* f = fopen(path.c_str(), "rb")) {
            char buf[64 * 1024];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, n);
            fclose(f);
        } else {
            err = "Could not read the file.";
        }
        std::string name = path.substr(path.find_last_of('/') + 1);
        for (char& c : name) if (c == '"' || c == '\\' || c < 0x20) c = '_';
        const std::string ext = Str::Lower(name.substr(name.find_last_of('.') + 1));
        const char* type = ext == "jpg" || ext == "jpeg" ? "image/jpeg" : ext == "png" ? "image/png"
                         : ext == "gif" ? "image/gif" : ext == "mp4" ? "video/mp4" : ext == "txt" ? "text/plain"
                         : "application/octet-stream";
        const std::string b = "----sLaunch" + nonce;
        std::string body = "--" + b + "\r\nContent-Disposition: form-data; name=\"payload_json\"\r\n"
                           "Content-Type: application/json\r\n\r\n{\"content\":" + Str::Json(content) +
                           ",\"nonce\":" + Str::Json(nonce) + ",\"attachments\":[{\"id\":\"0\",\"filename\":" +
                           Str::Json(name) + "}]}\r\n--" + b +
                           "\r\nContent-Disposition: form-data; name=\"files[0]\"; filename=\"" + name + "\"\r\n"
                           "Content-Type: " + type + "\r\n\r\n";
        body.reserve(body.size() + data.size() + b.size() + 8);
        body += data;
        std::string().swap(data);
        body += "\r\n--" + b + "--\r\n";
        Message m;
        bool ok = false;
        if (err.empty()) {
            // 2 minutes: a 10 MB clip over the console's Wi-Fi.
            Http::Response r = Call("POST", Str::Format("/channels/%llu/messages", (unsigned long long)channel), body,
                                    "", "multipart/form-data; boundary=" + b, 120000);
            ok = r.Ok();
            if (ok) {
                auto doc = J::Parse(r.body);
                m = Parse::MessageObj(J::Root(doc));
            } else {
                err = ErrorText(r);
            }
        }
        LOG("upload %s: %s", name.c_str(), ok ? "sent" : err.c_str());
        PostIfCurrent(gen, [cb, ok, m = std::move(m), err]() mutable { cb(ok, std::move(m), err); });
    });
}

namespace {
void Simple(const std::string& method, const std::string& path, const std::string& body, Done cb) {
    int gen = g_gen.load();
    Async::Run(Async::Pool::Api, [=] {
        Http::Response r = Call(method, path, body);
        bool ok = r.Ok();
        std::string err = ok ? "" : ErrorText(r);
        if (cb) PostIfCurrent(gen, [cb, ok, err] { cb(ok, err); });
    });
}
}  // namespace

void EditMessage(Snowflake channel, Snowflake id, const std::string& content, Done cb) {
    Simple("PATCH", Str::Format("/channels/%llu/messages/%llu", (unsigned long long)channel, (unsigned long long)id),
           "{\"content\":" + Str::Json(content) + "}", std::move(cb));
}

void DeleteMessage(Snowflake channel, Snowflake id, Done cb) {
    Simple("DELETE", Str::Format("/channels/%llu/messages/%llu", (unsigned long long)channel, (unsigned long long)id),
           "", std::move(cb));
}

void React(Snowflake channel, Snowflake id, const std::string& emojiParam, bool add, Done cb) {
    Simple(add ? "PUT" : "DELETE",
           Str::Format("/channels/%llu/messages/%llu/reactions/%s/@me?location=Message&type=0",
                       (unsigned long long)channel, (unsigned long long)id, emojiParam.c_str()),
           "", std::move(cb));
}

void Ack(Snowflake channel, Snowflake message) {
    Simple("POST",
           Str::Format("/channels/%llu/messages/%llu/ack", (unsigned long long)channel, (unsigned long long)message),
           "{\"token\":null}", nullptr);
}

void Typing(Snowflake channel) {
    Simple("POST", Str::Format("/channels/%llu/typing", (unsigned long long)channel), "", nullptr);
}

void OpenDM(Snowflake userId, std::function<void(bool, Channel)> cb) {
    int gen = g_gen.load();
    Async::Run(Async::Pool::Api, [=] {
        Http::Response r =
            Call("POST", "/users/@me/channels", Str::Format("{\"recipients\":[\"%llu\"]}", (unsigned long long)userId));
        Channel c;
        bool ok = r.Ok();
        if (ok) {
            auto doc = J::Parse(r.body);
            c = Parse::ChannelObj(J::Root(doc), 0);
        }
        PostIfCurrent(gen, [cb, ok, c] { cb(ok, c); });
    });
}

void FetchMe(const std::string& token, std::function<void(bool, User, const std::string&)> cb) {
    Async::Run(Async::Pool::Api, [=] {
        Http::Response r = Call("GET", "/users/@me", "", token);
        User u;
        bool ok = r.Ok();
        std::string err;
        if (ok) {
            auto doc = J::Parse(r.body);
            u = Parse::UserObj(J::Root(doc));
            ok = u.id != 0;
        }
        if (!ok) err = ErrorText(r);
        Async::Post([cb, ok, u, err] { cb(ok, u, err); });
    });
}

std::string NewNonce() {
    uint32_t rnd = 0;
    Platform::Random(&rnd, sizeof(rnd));
    return std::to_string(Str::SnowflakeFromMs(Platform::NowMs()) | (rnd & 0x3FFFFF));
}

}  // namespace Api
