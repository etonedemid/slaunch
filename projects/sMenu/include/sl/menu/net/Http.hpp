#pragma once
#include <string>

// Tiny HTTPS GET helper built on libcurl (mbedTLS backend). Blocking, so call
// it only from a background thread. Certificate verification is disabled: the
// Switch ships no CA bundle and these are low-stakes public data endpoints.

namespace sl::menu::net {

    // One-time global init/teardown (curl_global_*). Safe to call repeatedly.
    void GlobalInit();
    void GlobalExit();

    // GET url into `out`. Returns true on HTTP 2xx. `timeout_s` bounds the whole
    // transfer. `out` is cleared on entry.
    bool Get(const char *url, std::string &out, long timeout_s = 8);

    // POST `body` to url. `content_type` and `authorization` may be null. `out`
    // receives the response body; returns true on HTTP 2xx.
    bool Post(const char *url, const char *body, const char *content_type,
              const char *authorization, std::string &out, long timeout_s = 8);

    // GET url straight to a file (streamed, so it isn't size-capped like Get).
    // Returns true on HTTP 2xx; a failed/partial download leaves no file.
    bool Download(const char *url, const char *path, long timeout_s = 15);

} // namespace sl::menu::net
