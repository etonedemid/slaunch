#include "discord/remote_auth.h"

#include <nxui/core/ThreadPool.hpp>

#include "core/BackgroundThread.hpp"

#include <chrono>
#include <cstring>
#include <vector>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/pk.h>
#include <mbedtls/rsa.h>
#include <mbedtls/sha256.h>
#include <mbedtls/version.h>

#include "discord/api.h"
#include "discord/types.h"
#include "net/websocket.h"
#include "platform/platform.h"
#include "util/json.h"
#include "util/log.h"
#include "util/strings.h"

namespace {

int Entropy(void*, unsigned char* out, size_t len) {
    Platform::Random(out, len);
    return 0;
}

int64_t Now() { return Platform::NowMs(); }

}  // namespace

struct RemoteAuth::Key {
    mbedtls_pk_context pk;
    mbedtls_ctr_drbg_context drbg;
    bool ok = false;
    std::string spkiB64;

    Key() {
        mbedtls_pk_init(&pk);
        mbedtls_ctr_drbg_init(&drbg);
    }
    ~Key() {
        mbedtls_pk_free(&pk);
        mbedtls_ctr_drbg_free(&drbg);
    }

    bool Generate() {
        static const unsigned char kPers[] = "switchcord-remote-auth";
        if (mbedtls_ctr_drbg_seed(&drbg, Entropy, nullptr, kPers, sizeof(kPers) - 1) != 0) return false;
        if (mbedtls_pk_setup(&pk, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA)) != 0) return false;
        mbedtls_rsa_context* rsa = mbedtls_pk_rsa(pk);
        if (mbedtls_rsa_gen_key(rsa, mbedtls_ctr_drbg_random, &drbg, 2048, 65537) != 0) return false;
        mbedtls_rsa_set_padding(rsa, MBEDTLS_RSA_PKCS_V21, MBEDTLS_MD_SHA256);
        unsigned char der[1024];
        int len = mbedtls_pk_write_pubkey_der(&pk, der, sizeof(der));
        if (len <= 0) return false;
        spkiB64 = Str::Base64(der + sizeof(der) - len, (size_t)len);
        ok = true;
        return true;
    }

    bool Decrypt(const std::string& b64, std::vector<uint8_t>& out) {
        std::vector<uint8_t> in = Str::Base64Decode(b64);
        mbedtls_rsa_context* rsa = mbedtls_pk_rsa(pk);
        if (in.size() != mbedtls_rsa_get_len(rsa)) return false;
        out.assign(512, 0);
        size_t olen = 0;
#if MBEDTLS_VERSION_MAJOR >= 3
        int rc = mbedtls_rsa_rsaes_oaep_decrypt(rsa, mbedtls_ctr_drbg_random, &drbg, nullptr, 0, &olen, in.data(),
                                                out.data(), out.size());
#else
        int rc = mbedtls_rsa_rsaes_oaep_decrypt(rsa, mbedtls_ctr_drbg_random, &drbg, MBEDTLS_RSA_PRIVATE, nullptr, 0,
                                                &olen, in.data(), out.data(), out.size());
#endif
        if (rc != 0) {
            LOG("remote auth: decrypt failed (-0x%04x)", -rc);
            return false;
        }
        out.resize(olen);
        return true;
    }
};

RemoteAuth::RemoteAuth() = default;
RemoteAuth::~RemoteAuth() { Stop(); }

void RemoteAuth::Start() {
    Stop();
    stop_ = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        snap_ = Snapshot{};
        snap_.phase = Phase::Preparing;
    }
    done_ = false;
    thread_ = std::thread([this] {
        nxui::moveToWorkerCores(); // HomeNX: keep core 0 for the menu's frames
        Run();
        done_ = true;
    });
}

bool RemoteAuth::StopFor(int timeoutMs) {
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

void RemoteAuth::Stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
}

RemoteAuth::Snapshot RemoteAuth::Get() const {
    std::lock_guard<std::mutex> lk(mu_);
    return snap_;
}

void RemoteAuth::Set(Phase p) {
    std::lock_guard<std::mutex> lk(mu_);
    snap_.phase = p;
}

void RemoteAuth::Run() {
    LOG("remote auth: key generation start");
    Platform::SetCpuBoost(true);
    Key key;
    bool generated = key.Generate();
    Platform::SetCpuBoost(false);
    LOG("remote auth: key generation done (ok=%d)", generated ? 1 : 0);
    if (!generated) {
        std::lock_guard<std::mutex> lk(mu_);
        snap_.phase = Phase::Failed;
        snap_.error = "Couldn't create the login key.";
        return;
    }
    // A QR code only lives for a couple of minutes; keep offering fresh ones.
    int failures = 0;
    while (!stop_) {
        bool again = Session(key);
        Phase p = Get().phase;
        if (!again || p == Phase::Done || p == Phase::Failed) break;
        if (p == Phase::Cancelled) {
            // Show "cancelled" briefly, then a new code.
            for (int i = 0; i < 30 && !stop_; i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (++failures > 20) break;
        std::lock_guard<std::mutex> lk(mu_);
        snap_ = Snapshot{};
        snap_.phase = Phase::Preparing;
    }
}

bool RemoteAuth::Session(Key& key) {
    WebSocket ws;
    std::string err;
    std::vector<std::string> headers = {"Origin: https://discord.com",
                                        std::string("User-Agent: ") + Client::kUserAgent};
    LOG("remote auth: connecting");
    if (!ws.Connect("wss://remote-auth-gateway.discord.gg/?v=2", headers, err)) {
        LOG("remote auth: %s", err.c_str());
        std::lock_guard<std::mutex> lk(mu_);
        snap_.error = "Can't reach Discord (" + err + ")";
        // Retry after a pause; the network may still be coming up.
        for (int i = 0; i < 30 && !stop_; i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        return true;
    }

    int64_t hbInterval = 41250, nextHb = 0;
    bool proofSent = false;
    std::vector<WebSocket::Message> msgs;
    while (!stop_) {
        msgs.clear();
        bool alive = ws.Poll(msgs);
        for (auto& m : msgs) {
            J::Doc doc = J::Parse(m.data);
            yyjson_val* root = J::Root(doc);
            std::string op = J::S(root, "op");
            if (op != "heartbeat_ack") LOG("remote auth: op %s", op.c_str());
            if (op == "hello") {
                hbInterval = J::I(root, "heartbeat_interval", 41250);
                nextHb = Now() + hbInterval;
                int64_t timeout = J::I(root, "timeout_ms", 120000);
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    snap_.expiresMs = Now() + timeout;
                }
                ws.SendText("{\"op\":\"init\",\"encoded_public_key\":\"" + key.spkiB64 + "\"}");
            } else if (op == "nonce_proof") {
                std::vector<uint8_t> nonce;
                if (!key.Decrypt(J::S(root, "encrypted_nonce"), nonce)) {
                    std::lock_guard<std::mutex> lk(mu_);
                    snap_.phase = Phase::Failed;
                    snap_.error = "Login handshake failed.";
                    return false;
                }
                std::string nonceB64 = Str::Base64Url(nonce.data(), nonce.size());
                if (hashedProof_) {
                    // Older protocol revision: SHA-256 of the nonce as "proof".
                    unsigned char digest[32];
#if MBEDTLS_VERSION_MAJOR >= 3
                    mbedtls_sha256(nonce.data(), nonce.size(), digest, 0);
#else
                    mbedtls_sha256_ret(nonce.data(), nonce.size(), digest, 0);
#endif
                    ws.SendText("{\"op\":\"nonce_proof\",\"proof\":\"" + Str::Base64Url(digest, 32) + "\"}");
                } else {
                    ws.SendText("{\"op\":\"nonce_proof\",\"nonce\":\"" + nonceB64 + "\"}");
                }
                proofSent = true;
            } else if (op == "pending_remote_init") {
                std::lock_guard<std::mutex> lk(mu_);
                snap_.qrUrl = "https://discord.com/ra/" + J::S(root, "fingerprint");
                snap_.phase = Phase::ShowQr;
                snap_.error.clear();
            } else if (op == "pending_ticket") {
                std::vector<uint8_t> payload;
                if (key.Decrypt(J::S(root, "encrypted_user_payload"), payload)) {
                    std::string s(payload.begin(), payload.end());
                    auto parts = Str::Split(s, ':');
                    std::lock_guard<std::mutex> lk(mu_);
                    if (parts.size() >= 4) {
                        snap_.userId = parts[0];
                        snap_.avatar = parts[2] == "0" ? "" : parts[2];
                        // Usernames can't contain ':', but join the rest just in case.
                        std::string name = parts[3];
                        for (size_t i = 4; i < parts.size(); i++) name += ":" + parts[i];
                        snap_.username = name;
                    }
                    snap_.phase = Phase::Confirm;
                } else {
                    LOG("remote auth: could not read the user payload");
                }
            } else if (op == "pending_login") {
                Set(Phase::LoggingIn);
                std::string ticket = J::S(root, "ticket");
                LOG("remote auth: exchanging the ticket (%zu chars)", ticket.size());
                Http::Response r =
                    Api::Call("POST", "/users/@me/remote-auth/login", "{\"ticket\":" + Str::Json(ticket) + "}", "");
                std::string encrypted;
                if (r.Ok()) {
                    auto rdoc = J::Parse(r.body);
                    encrypted = J::S(J::Root(rdoc), "encrypted_token");
                }
                std::vector<uint8_t> token;
                std::lock_guard<std::mutex> lk(mu_);
                LOG("remote auth: ticket answer HTTP %ld, %s, encrypted %zu chars", r.status, r.error.c_str(), encrypted.size());
                if (!encrypted.empty() && key.Decrypt(encrypted, token)) {
                    snap_.token.assign(token.begin(), token.end());
                    snap_.phase = Phase::Done;
                    LOG("remote auth: logged in as %s", snap_.username.c_str());
                } else {
                    snap_.phase = Phase::Failed;
                    snap_.error = r.Ok() ? "Discord sent an unreadable token." : Api::ErrorText(r);
                    LOG("remote auth: ticket exchange failed: HTTP %ld %s", r.status, r.body.substr(0, 200).c_str());
                }
                ws.Close();
                return false;
            } else if (op == "cancel") {
                LOG("remote auth: cancelled on the phone");
                Set(Phase::Cancelled);
                ws.Close();
                return true;
            }
        }
        if (!alive) {
            int code = ws.CloseCode();
            LOG("remote auth: closed with %d", code);
            if (proofSent && Get().phase == Phase::Preparing) {
                // Closed after the proof and before a QR code: the proof format was
                // rejected (whatever the close code), so the other revision next time.
                hashedProof_ = !hashedProof_;
            }
            return true;  // timeout (4003) or network drop: new code
        }
        if (nextHb && Now() >= nextHb) {
            ws.SendText("{\"op\":\"heartbeat\"}");
            nextHb = Now() + hbInterval;
        }
        if (msgs.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ws.Close();
    return false;
}
