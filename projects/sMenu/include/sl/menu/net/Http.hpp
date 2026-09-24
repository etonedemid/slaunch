#pragma once
#include <string>
#include <atomic>
#include <cstdint>

// Tiny HTTPS GET helper built on libcurl (mbedTLS backend). Blocking, so call
// it only from a background thread. Certificate verification is disabled: the
// Switch ships no CA bundle and these are low-stakes public data endpoints.

namespace sl::menu::net {

    // One-time global init/teardown (curl_global_*). Safe to call repeatedly.
    void GlobalInit();
    void GlobalExit();

    // GET url into `out`. Returns true on HTTP 2xx. `timeout_s` bounds the whole
    // transfer. `out` is cleared on entry. `authorization`, when given, is sent
    // as the Authorization header - SteamGridDB wants a bearer token there.
    bool Get(const char *url, std::string &out, long timeout_s = 8,
             const char *authorization = nullptr,
             // Optional diagnostics: the HTTP status and the raw CURLcode. A
             // rejected API key and an unreachable host both just return false,
             // and telling them apart matters when something is failing in the
             // field where no debugger can reach.
             long *out_http = nullptr, int *out_curl = nullptr);

    // POST `body` to url. `content_type` and `authorization` may be null. `out`
    // receives the response body; returns true on HTTP 2xx.
    bool Post(const char *url, const char *body, const char *content_type,
              const char *authorization, std::string &out, long timeout_s = 8);

    // GET url straight to a file (streamed, so it isn't size-capped like Get).
    // Returns true on HTTP 2xx; a failed/partial download leaves no file.
    // `now` / `total`, when given, follow the transfer in bytes (total stays
    // 0 until the server says how big it is) - for progress bars.
    bool Download(const char *url, const char *path, long timeout_s = 15,
                  std::atomic<uint64_t> *now = nullptr, std::atomic<uint64_t> *total = nullptr);

    // The appid of the entry in a steamcommunity SearchApps response whose
    // name really is `title`, or "" when none is. Steam's search is fuzzy and
    // always returns something - searching "Super Smash Bros. Ultimate", which
    // is not on Steam, returns "Super Smash Gals" - so taking the first hit
    // put another game's news and screenshots on the box. A match is the same
    // words ignoring case, punctuation and (TM)/(R), with an edition suffix
    // after ':' or " - " allowed on either side.
    std::string SteamAppFor(const std::string &search_json, const std::string &title);

    // The same rule on its own: is `candidate` the game called `title`?
    bool TitlesMatch(const std::string &candidate, const std::string &title);

} // namespace sl::menu::net
