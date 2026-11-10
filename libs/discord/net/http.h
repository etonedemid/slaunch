#pragma once

#include <string>
#include <vector>

namespace Http {

struct Request {
    std::string method = "GET";
    std::string url;
    std::vector<std::string> headers;  // "Name: value"
    std::string body;
    long timeoutMs = 20000;
    size_t maxBytes = 32u << 20;  // images and history pages are far smaller
    bool abortable = true;        // false: Discord's AbortAll() leaves it alone (HOME's Mii faces)
};

struct Response {
    long status = 0;
    std::string body;
    std::string error;       // transport error, empty on an HTTP response
    double retryAfter = 0;   // seconds, from a 429
    bool Ok() const { return status >= 200 && status < 300; }
};

void GlobalInit();
void GlobalCleanup();
// Blocking. Each thread keeps its own handle so connections are reused.
Response Perform(const Request& req);
// Frees the calling thread's handle (call before a worker thread exits).
void ThreadCleanup();
// Turns off certificate checks (a hidden setting for consoles with stale CA stores).
void SetInsecure(bool insecure);
// Makes every transfer in flight fail fast (used when quitting).
void AbortAll();
void ResetAbort();

}  // namespace Http
