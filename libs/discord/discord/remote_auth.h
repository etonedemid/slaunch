#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

// "Scan the QR code with your phone" login (Discord's remote auth v2). The phone app
// approves the session and we receive the account token, so no password or captcha
// has to be typed on the console.
class RemoteAuth {
public:
    enum class Phase { Idle, Preparing, ShowQr, Confirm, LoggingIn, Done, Cancelled, Failed };

    struct Snapshot {
        Phase phase = Phase::Idle;
        std::string qrUrl;
        std::string userId, username, avatar;  // after the phone scans
        std::string token;                     // when Done
        std::string error;
        int64_t expiresMs = 0;
    };

    RemoteAuth();
    ~RemoteAuth();
    void Start();
    void Stop();
    void RequestStop() { stop_ = true; } // Stop() then only joins
    // Stops within timeoutMs or lets the thread finish on its own (then this
    // object must outlive it). Returns whether it stopped.
    bool StopFor(int timeoutMs);
    bool Done() const { return done_; }
    Snapshot Get() const;

private:
    struct Key;
    void Run();
    bool Session(Key& key);
    void Set(Phase p);

    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> done_{true};
    mutable std::mutex mu_;
    Snapshot snap_;
    bool hashedProof_ = true; // Discord's current revision: SHA-256 of the nonce (the raw nonce is the fallback)
};
