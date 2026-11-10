#include <switchu/tls_roots.hpp>
#include "net/http.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <strings.h>

#include <curl/curl.h>

#include "util/log.h"

namespace Http {
namespace {

std::atomic<bool> g_insecure{false};
std::atomic<bool> g_abort{false};

struct Sink {
    std::string* body;
    size_t max;
    bool overflow = false;
};

size_t OnBody(char* ptr, size_t size, size_t nmemb, void* user) {
    Sink* s = static_cast<Sink*>(user);
    size_t n = size * nmemb;
    if (s->body->size() + n > s->max) {
        s->overflow = true;
        return 0;
    }
    try { // an exception must never unwind through curl (it leaves the handle unusable)
        s->body->append(ptr, n);
    } catch (...) {
        s->overflow = true;
        return 0;
    }
    return n;
}

size_t OnHeader(char* buf, size_t size, size_t nitems, void* user) {
    size_t n = size * nitems;
    Response* r = static_cast<Response*>(user);
    static const char kRetry[] = "retry-after:";
    if (n > sizeof(kRetry) - 1 && strncasecmp(buf, kRetry, sizeof(kRetry) - 1) == 0) {
        r->retryAfter = atof(buf + sizeof(kRetry) - 1);
    }
    return n;
}

int OnProgress(void*, curl_off_t, curl_off_t, curl_off_t, curl_off_t) { return g_abort.load() ? 1 : 0; }

}  // namespace

void AbortAll() { g_abort = true; }
void ResetAbort() { g_abort = false; }

void GlobalInit() { curl_global_init(CURL_GLOBAL_DEFAULT); }
void GlobalCleanup() { curl_global_cleanup(); }
void SetInsecure(bool insecure) { g_insecure = insecure; }

// Modules get no TLS block, so no per-thread handle: every request has its own.
// ponytail: a new connection per request; a small per-pool handle cache if it drags.
void ThreadCleanup() {}

Response Perform(const Request& req) {
    Response res;
    CURL* c = curl_easy_init();
    if (!c) {
        res.error = "curl init failed";
        return res;
    }
    Sink sink{&res.body, req.maxBytes};
    struct curl_slist* hdrs = nullptr;
    for (const auto& h : req.headers) hdrs = curl_slist_append(hdrs, h.c_str());

    curl_easy_setopt(c, CURLOPT_URL, req.url.c_str());
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, req.timeoutMs);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, OnBody);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, OnHeader);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, &res);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, req.abortable ? 0L : 1L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, OnProgress);
    if (hdrs) curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
    if (g_insecure) {
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
    }

    if (req.method == "GET") {
        curl_easy_setopt(c, CURLOPT_HTTPGET, 1L);
    } else {
        curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, req.method.c_str());
        if (!req.body.empty() || req.method == "POST" || req.method == "PUT" || req.method == "PATCH") {
            curl_easy_setopt(c, CURLOPT_POSTFIELDS, req.body.c_str());
            curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)req.body.size());
        }
    }

    // A server the console's older store refuses: HomeNX's roots (switchu/tls_roots.hpp).
    CURLcode rc = switchu::tls::perform(c, req.url, switchu::tls::kHomeCerts, [&res]() { res.body.clear(); });
    if (rc != CURLE_OK && !sink.overflow) {
        res.error = curl_easy_strerror(rc);
        LOG("http %s %s failed: %s", req.method.c_str(), req.url.c_str(), res.error.c_str());
    } else if (sink.overflow) {
        res.error = "response too large";
    }
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &res.status);
    if (hdrs) curl_slist_free_all(hdrs);
    curl_easy_cleanup(c);
    return res;
}

}  // namespace Http
