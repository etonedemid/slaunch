#include <switchu/tls_roots.hpp>
#include "net/websocket.h"

#include <chrono>
#include <cstring>
#include <thread>

#include <curl/curl.h>

#include "platform/platform.h"
#include "util/log.h"
#include "util/strings.h"

namespace {

using Clock = std::chrono::steady_clock;

void SleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

bool ParseUrl(const std::string& url, std::string& host, int& port, std::string& path) {
    std::string rest;
    if (Str::StartsWith(url, "wss://")) {
        rest = url.substr(6);
        port = 443;
    } else if (Str::StartsWith(url, "ws://")) {
        rest = url.substr(5);
        port = 80;
    } else {
        return false;
    }
    size_t slash = rest.find_first_of("/?");
    std::string hostport = slash == std::string::npos ? rest : rest.substr(0, slash);
    path = slash == std::string::npos ? "/" : rest.substr(slash);
    if (!path.empty() && path[0] == '?') path = "/" + path;
    size_t colon = hostport.find(':');
    if (colon != std::string::npos) {
        host = hostport.substr(0, colon);
        port = atoi(hostport.c_str() + colon + 1);
    } else {
        host = hostport;
    }
    return !host.empty();
}

}  // namespace

WebSocket::~WebSocket() { Close(); }

void WebSocket::Close() {
    if (curl_) {
        if (open_ && !closeSent_) SendClose(1000);
        curl_easy_cleanup(static_cast<CURL*>(curl_));
        curl_ = nullptr;
    }
    open_ = false;
}

bool WebSocket::Connect(const std::string& url, const std::vector<std::string>& headers, std::string& err) {
    Close();
    rbuf_.clear();
    frag_.clear();
    closeCode_ = 0;
    closeReason_.clear();
    closeSent_ = false;

    std::string host, path;
    int port = 443;
    if (!ParseUrl(url, host, port, path)) {
        err = "bad websocket url";
        return false;
    }
    CURL* c = curl_easy_init();
    if (!c) {
        err = "curl init failed";
        return false;
    }
    curl_ = c;
    std::string target = Str::Format("%s://%s:%d/", port == 80 ? "http" : "https", host.c_str(), port);
    curl_easy_setopt(c, CURLOPT_URL, target.c_str());
    curl_easy_setopt(c, CURLOPT_CONNECT_ONLY, 1L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, 12000L);
    // WebSockets need HTTP/1.1; never let ALPN pick HTTP/2.
    curl_easy_setopt(c, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(c, CURLOPT_SSL_ENABLE_ALPN, 0L);
    CURLcode rc = switchu::tls::perform(c, target, switchu::tls::kHomeCerts); // (refused: HomeNX's roots)
    if (rc != CURLE_OK) {
        // Which stage failed: ip=none means before any address was found (name lookup), errno is the socket's.
        const char* ip = nullptr;
        long osErr = 0;
        curl_easy_getinfo(c, CURLINFO_PRIMARY_IP, &ip);
        curl_easy_getinfo(c, CURLINFO_OS_ERRNO, &osErr);
        err = Str::Format("connect: %s (rc=%d ip=%s errno=%ld)", curl_easy_strerror(rc), static_cast<int>(rc),
                          ip && *ip ? ip : "none", osErr);
        Close();
        return false;
    }
    open_ = true;

    uint8_t key[16];
    Platform::Random(key, sizeof(key));
    std::string req = "GET " + path + " HTTP/1.1\r\n";
    req += "Host: " + host + "\r\n";
    req += "Upgrade: websocket\r\nConnection: Upgrade\r\n";
    req += "Sec-WebSocket-Key: " + Str::Base64(key, sizeof(key)) + "\r\n";
    req += "Sec-WebSocket-Version: 13\r\n";
    for (const auto& h : headers) req += h + "\r\n";
    req += "\r\n";
    if (!SendAll(reinterpret_cast<const uint8_t*>(req.data()), req.size())) {
        err = "handshake send failed";
        Close();
        return false;
    }

    // Read the HTTP response head; anything after it is already WebSocket data.
    auto deadline = Clock::now() + std::chrono::seconds(12);
    char buf[4096];
    while (true) {
        size_t n = 0;
        rc = curl_easy_recv(c, buf, sizeof(buf), &n);
        if (rc == CURLE_OK && n > 0) {
            rbuf_.append(buf, n);
            size_t end = rbuf_.find("\r\n\r\n");
            if (end != std::string::npos) {
                std::string head = rbuf_.substr(0, end);
                rbuf_.erase(0, end + 4);
                size_t sp = head.find(' ');
                int status = sp == std::string::npos ? 0 : atoi(head.c_str() + sp + 1);
                if (status != 101) {
                    err = Str::Format("handshake rejected (HTTP %d)", status);
                    LOG("ws: %s", head.substr(0, 300).c_str());
                    Close();
                    return false;
                }
                return true;
            }
            if (rbuf_.size() > 65536) break;
            continue;
        }
        if (rc == CURLE_AGAIN) {
            if (Clock::now() > deadline) break;
            SleepMs(5);
            continue;
        }
        break;
    }
    err = "handshake failed";
    Close();
    return false;
}

bool WebSocket::SendAll(const uint8_t* d, size_t n) {
    CURL* c = static_cast<CURL*>(curl_);
    if (!c) return false;
    auto deadline = Clock::now() + std::chrono::seconds(10);
    while (n > 0) {
        size_t sent = 0;
        CURLcode rc = curl_easy_send(c, d, n, &sent);
        if (rc == CURLE_OK) {
            d += sent;
            n -= sent;
        } else if (rc == CURLE_AGAIN) {
            if (Clock::now() > deadline) return false;
            SleepMs(2);
        } else {
            LOG("ws send failed: %s", curl_easy_strerror(rc));
            open_ = false;
            return false;
        }
    }
    return true;
}

bool WebSocket::SendFrame(int opcode, const uint8_t* d, size_t n) {
    if (!IsOpen()) return false;
    std::string f;
    f.reserve(n + 14);
    f += (char)(0x80 | opcode);
    if (n < 126) {
        f += (char)(0x80 | n);
    } else if (n < 65536) {
        f += (char)(0x80 | 126);
        f += (char)((n >> 8) & 0xFF);
        f += (char)(n & 0xFF);
    } else {
        f += (char)(0x80 | 127);
        for (int i = 7; i >= 0; i--) f += (char)(((uint64_t)n >> (i * 8)) & 0xFF);
    }
    uint8_t mask[4];
    Platform::Random(mask, 4);
    f.append(reinterpret_cast<char*>(mask), 4);
    size_t base = f.size();
    f.append(reinterpret_cast<const char*>(d), n);
    for (size_t i = 0; i < n; i++) f[base + i] = (char)(f[base + i] ^ mask[i & 3]);
    return SendAll(reinterpret_cast<const uint8_t*>(f.data()), f.size());
}

bool WebSocket::SendText(std::string_view s) {
    return SendFrame(0x1, reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

void WebSocket::SendClose(uint16_t code) {
    if (closeSent_ || !IsOpen()) return;
    closeSent_ = true;
    uint8_t p[2] = {(uint8_t)(code >> 8), (uint8_t)(code & 0xFF)};
    SendFrame(0x8, p, 2);
}

bool WebSocket::ParseFrames(std::vector<Message>& out) {
    while (true) {
        if (rbuf_.size() < 2) return true;
        const uint8_t* b = reinterpret_cast<const uint8_t*>(rbuf_.data());
        bool fin = b[0] & 0x80;
        int op = b[0] & 0x0F;
        bool masked = b[1] & 0x80;
        uint64_t len = b[1] & 0x7F;
        size_t pos = 2;
        if (len == 126) {
            if (rbuf_.size() < 4) return true;
            len = ((uint64_t)b[2] << 8) | b[3];
            pos = 4;
        } else if (len == 127) {
            if (rbuf_.size() < 10) return true;
            len = 0;
            for (int i = 0; i < 8; i++) len = (len << 8) | b[2 + i];
            pos = 10;
        }
        uint8_t mask[4] = {0, 0, 0, 0};
        if (masked) {
            if (rbuf_.size() < pos + 4) return true;
            memcpy(mask, b + pos, 4);
            pos += 4;
        }
        if (len > (256u << 20)) {
            LOG("ws: frame too large (%llu)", (unsigned long long)len);
            open_ = false;
            return false;
        }
        if (rbuf_.size() < pos + len) return true;
        std::string payload = rbuf_.substr(pos, (size_t)len);
        rbuf_.erase(0, pos + (size_t)len);
        if (masked)
            for (size_t i = 0; i < payload.size(); i++) payload[i] = (char)(payload[i] ^ mask[i & 3]);

        switch (op) {
            case 0x0:  // continuation
                frag_ += payload;
                if (fin) {
                    out.push_back({fragOp_ == 0x2, std::move(frag_)});
                    frag_.clear();
                }
                break;
            case 0x1:
            case 0x2:
                if (fin) {
                    out.push_back({op == 0x2, std::move(payload)});
                } else {
                    fragOp_ = op;
                    frag_ = std::move(payload);
                }
                break;
            case 0x8: {
                closeCode_ = payload.size() >= 2 ? (((uint8_t)payload[0] << 8) | (uint8_t)payload[1]) : 1005;
                closeReason_ = payload.size() > 2 ? payload.substr(2) : "";
                if (!closeSent_) {
                    closeSent_ = true;
                    uint8_t p[2] = {(uint8_t)(closeCode_ >> 8), (uint8_t)(closeCode_ & 0xFF)};
                    SendFrame(0x8, p, 2);
                }
                open_ = false;
                return false;
            }
            case 0x9:
                SendFrame(0xA, reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
                break;
            default:
                break;
        }
    }
}

bool WebSocket::Poll(std::vector<Message>& out) {
    CURL* c = static_cast<CURL*>(curl_);
    if (!c || !open_) return false;
    char buf[16384];
    while (true) {
        size_t n = 0;
        CURLcode rc = curl_easy_recv(c, buf, sizeof(buf), &n);
        if (rc == CURLE_OK) {
            if (n == 0) {  // orderly shutdown without a close frame
                if (!closeCode_) closeCode_ = 1006;
                open_ = false;
                break;
            }
            rbuf_.append(buf, n);
            continue;
        }
        if (rc == CURLE_AGAIN) break;
        LOG("ws recv failed: %s", curl_easy_strerror(rc));
        if (!closeCode_) closeCode_ = 1006;
        open_ = false;
        break;
    }
    bool alive = ParseFrames(out);
    return alive && open_;
}

bool WebSocket::Wait(std::vector<Message>& out, int timeoutMs) {
    auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
    while (true) {
        size_t before = out.size();
        if (!Poll(out)) return false;
        if (out.size() > before || Clock::now() >= deadline) return true;
        SleepMs(10);
    }
}
