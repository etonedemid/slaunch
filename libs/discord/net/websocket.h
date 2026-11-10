#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// A minimal RFC 6455 client. The Switch's curl (7.69, libnx TLS) has no WebSocket
// support, so curl only opens the TLS connection (CONNECT_ONLY) and this class does
// the upgrade handshake and framing over curl_easy_send/recv.
class WebSocket {
public:
    struct Message {
        bool binary = false;
        std::string data;
    };

    WebSocket() = default;
    ~WebSocket();
    WebSocket(const WebSocket&) = delete;
    WebSocket& operator=(const WebSocket&) = delete;

    // url is wss://host[:port]/path?query. Extra headers are "Name: value".
    bool Connect(const std::string& url, const std::vector<std::string>& headers, std::string& err);
    bool SendText(std::string_view s);
    void SendClose(uint16_t code);
    // Non-blocking: appends complete messages. Returns false once the connection is closed.
    bool Poll(std::vector<Message>& out);
    // Blocks up to timeoutMs for at least one message (or closure).
    bool Wait(std::vector<Message>& out, int timeoutMs);
    void Close();

    bool IsOpen() const { return curl_ && open_; }
    int CloseCode() const { return closeCode_; }
    const std::string& CloseReason() const { return closeReason_; }

private:
    bool SendAll(const uint8_t* d, size_t n);
    bool SendFrame(int opcode, const uint8_t* d, size_t n);
    bool ParseFrames(std::vector<Message>& out);

    void* curl_ = nullptr;
    bool open_ = false;
    bool closeSent_ = false;
    int closeCode_ = 0;
    std::string closeReason_;
    std::string rbuf_;
    std::string frag_;
    int fragOp_ = 0;
};
