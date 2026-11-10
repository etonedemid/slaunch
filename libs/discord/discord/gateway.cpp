#include "discord/gateway.h"

#include <nxui/core/ThreadPool.hpp>

#include "core/BackgroundThread.hpp"

#include <chrono>
#include <cstring>

#include <zlib.h>

#include "discord/types.h"
#include "net/websocket.h"
#include "platform/platform.h"
#include "util/log.h"
#include "util/strings.h"

namespace {

// LAZY_USER_NOTES | NO_AFFINE_USER_IDS | DEDUPE_USER_OBJECTS | PRIORITIZED_READY_PAYLOAD:
// a smaller READY with users deduplicated and only our own member per guild.
constexpr int kCapabilities = 1 | 2 | 16 | 32;

std::atomic<bool> g_bulkSubs{true};
std::atomic<int64_t> g_lastSubMs{0};

class Inflater {
public:
    Inflater() { Reset(); }
    ~Inflater() {
        if (init_) inflateEnd(&zs_);
    }
    void Reset() {
        if (init_) inflateEnd(&zs_);
        memset(&zs_, 0, sizeof(zs_));
        init_ = inflateInit(&zs_) == Z_OK;
        buf_.clear();
    }
    // Buffers until a Z_SYNC_FLUSH suffix, then inflates the whole message.
    bool Feed(const std::string& in, std::string& out) {
        buf_ += in;
        if (buf_.size() < 4 || memcmp(buf_.data() + buf_.size() - 4, "\x00\x00\xff\xff", 4) != 0) return false;
        out.clear();
        zs_.next_in = reinterpret_cast<Bytef*>(buf_.data());
        zs_.avail_in = (uInt)buf_.size();
        char chunk[65536];
        do {
            zs_.next_out = reinterpret_cast<Bytef*>(chunk);
            zs_.avail_out = sizeof(chunk);
            int rc = inflate(&zs_, Z_SYNC_FLUSH);
            if (rc != Z_OK && rc != Z_BUF_ERROR && rc != Z_STREAM_END) {
                LOG("gateway: inflate error %d", rc);
                buf_.clear();
                return false;
            }
            out.append(chunk, sizeof(chunk) - zs_.avail_out);
            if (rc == Z_BUF_ERROR && zs_.avail_in == 0) break;
        } while (zs_.avail_in > 0 || zs_.avail_out == 0);
        buf_.clear();
        return true;
    }

private:
    z_stream zs_{};
    bool init_ = false;
    std::string buf_;
};

int64_t Now() { return Platform::NowMs(); }

}  // namespace

Gateway::~Gateway() { Stop(); }

void Gateway::Start(const std::string& token) {
    Stop();
    token_ = token;
    sessionId_.clear();
    resumeUrl_.clear();
    seq_ = -1;
    stop_ = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        in_.clear();
        out_.clear();
        error_.clear();
    }
    state_ = State::Connecting;
    done_ = false;
    thread_ = std::thread([this] {
        nxui::moveToWorkerCores(); // HomeNX: keep core 0 for the menu's frames
        Run();
        done_ = true;
    });
}

bool Gateway::StopFor(int timeoutMs) {
    stop_ = true;
    for (int t = 0; t < timeoutMs && !done_; t += 5) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    if (!thread_.joinable()) return true;
    if (!done_) {
        abandonThread(thread_); // libnx can't detach
        return false;
    }
    thread_.join();
    return true;
}

void Gateway::Stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    state_ = State::Idle;
}

void Gateway::SetPresence(const std::string& d) {
    std::lock_guard<std::mutex> lk(mu_);
    presence_ = d;
    out_.push_back("{\"op\":3,\"d\":" + d + "}");
}

void Gateway::Send(std::string payload) {
    std::lock_guard<std::mutex> lk(mu_);
    out_.push_back(std::move(payload));
}

std::vector<Gateway::Event> Gateway::Drain() {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<Event> out(std::make_move_iterator(in_.begin()), std::make_move_iterator(in_.end()));
    in_.clear();
    return out;
}

std::vector<Gateway::Event> Gateway::DrainPaused() {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<Event> out(std::make_move_iterator(pausedIn_.begin()), std::make_move_iterator(pausedIn_.end()));
    pausedIn_.clear();
    return out;
}

std::string Gateway::LastError() const {
    std::lock_guard<std::mutex> lk(mu_);
    return error_;
}

void Gateway::SetError(const std::string& e) {
    std::lock_guard<std::mutex> lk(mu_);
    error_ = e;
}

void Gateway::Subscribe(uint64_t guildId, uint64_t channelId) {
    if (!guildId) return;
    g_lastSubMs = Now();
    if (g_bulkSubs) {
        Send(Str::Format("{\"op\":37,\"d\":{\"subscriptions\":{\"%llu\":{\"typing\":true,\"threads\":true,"
                         "\"activities\":false,\"channels\":{\"%llu\":[[0,99]]}}}}}",
                         (unsigned long long)guildId, (unsigned long long)channelId));
    } else {
        Send(Str::Format("{\"op\":14,\"d\":{\"guild_id\":\"%llu\",\"typing\":true,\"threads\":true,"
                         "\"activities\":false,\"channels\":{\"%llu\":[[0,99]]}}}",
                         (unsigned long long)guildId, (unsigned long long)channelId));
    }
}

void Gateway::RequestMembers(uint64_t guildId, const std::vector<uint64_t>& userIds) {
    if (!guildId || userIds.empty()) return;
    for (size_t i = 0; i < userIds.size(); i += 100) {
        std::string ids;
        for (size_t j = i; j < userIds.size() && j < i + 100; j++) {
            if (!ids.empty()) ids += ",";
            ids += "\"" + std::to_string(userIds[j]) + "\"";
        }
        Send(Str::Format("{\"op\":8,\"d\":{\"guild_id\":[\"%llu\"],\"user_ids\":[%s],\"presences\":false}}",
                         (unsigned long long)guildId, ids.c_str()));
    }
}

bool Gateway::Sleep(int ms) {
    for (int t = 0; t < ms && !stop_; t += 50) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    return !stop_;
}

void Gateway::Run() {
    static const int kBackoff[] = {1000, 2000, 5000, 10000, 20000, 30000};
    int attempt = 0;
    while (!stop_) {
        state_ = (sessionId_.empty() && attempt == 0) ? State::Connecting : State::Reconnecting;
        if (!Platform::NetworkUp()) {
            SetError("No internet connection");
            if (!Sleep(2000)) break;
            continue;
        }
        std::string base = (!sessionId_.empty() && !resumeUrl_.empty()) ? resumeUrl_ : "wss://gateway.discord.gg";
        if (!base.empty() && base.back() == '/') base.pop_back();
        reachedReady_ = false;
        After after = Session(base + "/?v=9&encoding=json&compress=zlib-stream");
        if (stop_) break;
        if (reachedReady_) attempt = 0;
        if (after == After::AuthFailed) {
            state_ = State::AuthFailed;
            break;
        }
        if (after == After::Fatal) {
            state_ = State::Failed;
            break;
        }
        if (after == After::ResetSession) {
            sessionId_.clear();
            seq_ = -1;
        }
        state_ = State::Reconnecting;
        int wait = kBackoff[attempt < 5 ? attempt : 5];
        uint32_t jitter = 0;
        Platform::Random(&jitter, sizeof(jitter));
        wait += (int)(jitter % 700);
        attempt++;
        LOG("gateway: reconnecting in %d ms", wait);
        if (!Sleep(wait)) break;
    }
}

Gateway::After Gateway::Session(const std::string& url) {
    WebSocket ws;
    std::string err;
    std::vector<std::string> headers = {std::string("User-Agent: ") + Client::kUserAgent,
                                        "Origin: https://discord.com"};
    LOG("gateway: connecting to %s", url.c_str());
    if (!ws.Connect(url, headers, err)) {
        LOG("gateway: %s", err.c_str());
        SetError(err);
        return After::Retry;
    }

    Inflater z;
    int64_t hbInterval = 41250;
    int64_t nextHb = 0;
    int64_t identifyAt = 0;
    bool ackPending = false;
    bool ready = false;
    bool helloSeen = false;

    auto sendIdentify = [&] {
        std::string props = Client::SuperPropertiesJson();
        std::string presence;
        {
            std::lock_guard<std::mutex> lk(mu_);
            presence = presence_.empty() ? "{\"status\":\"unknown\",\"since\":0,\"activities\":[],\"afk\":false}" : presence_;
        }
        ws.SendText(Str::Format("{\"op\":2,\"d\":{\"token\":%s,\"capabilities\":%d,\"properties\":%s,"
                                "\"presence\":%s,"
                                "\"compress\":false,\"client_state\":{\"guild_versions\":{}}}}",
                                Str::Json(token_).c_str(), kCapabilities, props.c_str(), presence.c_str()));
    };

    std::vector<WebSocket::Message> msgs;
    while (!stop_) {
        msgs.clear();
        bool alive = ws.Poll(msgs);
        for (auto& m : msgs) {
            std::string text;
            if (m.binary) {
                if (!z.Feed(m.data, text)) continue;
            } else {
                text = std::move(m.data);
            }
            J::Doc doc = J::Parse(text);
            yyjson_val* root = J::Root(doc);
            if (!root) {
                LOG("gateway: unparseable payload (%zu bytes)", text.size());
                continue;
            }
            int op = (int)J::I(root, "op", -1);
            yyjson_val* d = J::Get(root, "d");
            switch (op) {
                case 10: {  // HELLO
                    helloSeen = true;
                    hbInterval = J::I(d, "heartbeat_interval", 41250);
                    uint32_t r = 0;
                    Platform::Random(&r, sizeof(r));
                    nextHb = Now() + (int64_t)(hbInterval * ((r % 1000) / 1000.0));
                    if (!sessionId_.empty() && seq_ >= 0) {
                        LOG("gateway: resuming session");
                        ws.SendText(Str::Format("{\"op\":6,\"d\":{\"token\":%s,\"session_id\":%s,\"seq\":%lld}}",
                                                Str::Json(token_).c_str(), Str::Json(sessionId_).c_str(),
                                                (long long)seq_));
                    } else {
                        sendIdentify();
                    }
                    break;
                }
                case 11:  // HEARTBEAT ACK
                    ackPending = false;
                    break;
                case 1:  // server asks for a heartbeat now
                    nextHb = 0;
                    break;
                case 7:  // RECONNECT
                    LOG("gateway: server asked us to reconnect");
                    ws.SendClose(4000);
                    return After::Retry;
                case 9: {  // INVALID SESSION
                    bool resumable = yyjson_is_true(d);
                    LOG("gateway: invalid session (resumable=%d)", resumable);
                    if (resumable) {
                        ws.SendClose(4000);
                        return After::Retry;
                    }
                    sessionId_.clear();
                    seq_ = -1;
                    uint32_t r = 0;
                    Platform::Random(&r, sizeof(r));
                    identifyAt = Now() + 1000 + (r % 4000);
                    break;
                }
                case 0: {  // DISPATCH
                    yyjson_val* s = J::Get(root, "s");
                    if (yyjson_is_int(s)) seq_ = J::Int(s);
                    std::string t = J::S(root, "t");
                    if (t == "READY") {
                        sessionId_ = J::S(d, "session_id");
                        resumeUrl_ = J::S(d, "resume_gateway_url");
                        ready = true;
                        reachedReady_ = true;
                        state_ = State::Ready;
                        SetError("");
                        LOG("gateway: READY (%zu bytes)", text.size());
                    } else if (t == "RESUMED") {
                        ready = true;
                        reachedReady_ = true;
                        state_ = State::Ready;
                        SetError("");
                        LOG("gateway: resumed");
                    }
                    if (paused_) {
                        missed_ = true;
                        // READY too: a game launched before it came has an empty
                        // store, and every message would be from an unknown chat.
                        if (t == "MESSAGE_CREATE" || t == "READY") {
                            std::lock_guard<std::mutex> lk(mu_);
                            if (pausedIn_.size() >= 8) pausedIn_.pop_front();
                            pausedIn_.push_back(Event{std::move(t), doc, d});
                        }
                        break;
                    }
                    std::lock_guard<std::mutex> lk(mu_);
                    in_.push_back(Event{std::move(t), doc, d});
                    break;
                }
                default:
                    break;
            }
        }

        if (!alive) {
            int code = ws.CloseCode();
            LOG("gateway: closed with %d %s", code, ws.CloseReason().c_str());
            SetError(Str::Format("Disconnected (%d)", code));
            if (code == 4004) {
                SetError("This account's token is no longer valid. Sign in again.");
                return After::AuthFailed;
            }
            if ((code == 4001 || code == 4002) && g_bulkSubs && Now() - g_lastSubMs.load() < 5000) {
                LOG("gateway: bulk subscriptions rejected, falling back to op 14");
                g_bulkSubs = false;
            }
            if (code >= 4010 && code <= 4014) return After::Fatal;
            if (code == 4007 || code == 4009 || !helloSeen) return After::ResetSession;
            return After::Retry;
        }

        const int64_t now = Now();
        if (identifyAt && now >= identifyAt) {
            identifyAt = 0;
            sendIdentify();
        }
        if (helloSeen && now >= nextHb) {
            if (ackPending) {
                LOG("gateway: no heartbeat ack, reconnecting");
                ws.SendClose(4000);
                return After::Retry;
            }
            ws.SendText(seq_ >= 0 ? Str::Format("{\"op\":1,\"d\":%lld}", (long long)seq_)
                                  : std::string("{\"op\":1,\"d\":null}"));
            ackPending = true;
            nextHb = now + hbInterval;
        }
        if (ready) {
            std::deque<std::string> out;
            {
                std::lock_guard<std::mutex> lk(mu_);
                out.swap(out_);
            }
            for (auto& p : out) ws.SendText(p);
        }
        if (msgs.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }
    ws.SendClose(1000);
    return After::Retry;
}
