#pragma once

#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "util/json.h"

// The Discord gateway on its own thread: heartbeats, identify/resume, zlib-stream,
// reconnects. Dispatches are parsed there and handed to the main thread via Drain().
class Gateway {
public:
    enum class State { Idle, Connecting, Ready, Reconnecting, AuthFailed, Failed };

    struct Event {
        std::string type;  // dispatch name, e.g. MESSAGE_CREATE
        J::Doc doc;        // keeps d alive
        yyjson_val* d = nullptr;
    };

    ~Gateway();
    void Start(const std::string& token);
    void Stop();
    void RequestStop() { stop_ = true; } // Stop() then only joins
    // Stops within timeoutMs or lets the thread finish on its own (then this
    // object must outlive it). Returns whether it stopped.
    bool StopFor(int timeoutMs);
    bool Done() const { return done_; }
    void Send(std::string payload);  // raw JSON, queued until the session is ready
    std::vector<Event> Drain();
    // While a game runs nobody drains: dispatches are dropped instead of piling up,
    // and TakeMissed() then says the store needs a fresh session.
    void SetPaused(bool p) { paused_ = p; }
    bool TakeMissed() { return missed_.exchange(false); }
    // The presence "d" object (status, activities); identify sends it too, so it
    // survives a reconnect.
    void SetPresence(const std::string& d);

    State GetState() const { return state_.load(); }
    std::string LastError() const;

    // Subscribes to typing/messages for the channel being viewed in a large guild.
    void Subscribe(uint64_t guildId, uint64_t channelId);
    // Asks for the member records (nickname, roles) of these users.
    void RequestMembers(uint64_t guildId, const std::vector<uint64_t>& userIds);

private:
    enum class After { Retry, ResetSession, Fatal, AuthFailed };

    void Run();
    After Session(const std::string& url);
    void SetState(State s) { state_ = s; }
    void SetError(const std::string& e);
    bool Sleep(int ms);  // false if stopping

    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> done_{true};
    std::atomic<bool> paused_{false}, missed_{false};
    std::string presence_;  // under mu_
    std::atomic<State> state_{State::Idle};

    std::string token_;
    std::string sessionId_, resumeUrl_;
    int64_t seq_ = -1;
    bool reachedReady_ = false;

    mutable std::mutex mu_;
    std::deque<Event> in_;
    std::deque<std::string> out_;
    std::string error_;
};
