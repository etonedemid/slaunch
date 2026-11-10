#pragma once
#include <curl/curl.h>

#include <cstdio>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <sys/stat.h>

// HomeNX's root certificates (CardsNX's certs/, from Mozilla's list), one per
// file because the console's TLS reads one CA from a file: Let's Encrypt (ISRG
// X1, X2) and Google Trust Services (Cloudflare's sites: nlib.cc, SteamGridDB,
// Discord). The console's own store is older and refuses some chains. perform()
// tries a refused host with each of them once (still verified), keeps the one
// that worked for that host, and drops it again if it stops working, so a
// request is never worse off than with the console's store alone.
namespace switchu::tls {

// HOME's copy (the frontend installs CardsNX's files there).
inline constexpr const char* kHomeCerts = "sdmc:/slaunch/certs/";
inline constexpr const char* kRootFiles[] = {"gts-root-r4.pem",  "gts-root-r1.pem", "isrg-root-x1.pem",
                                             "isrg-root-x2.pem", "gts-root-r3.pem", "globalsign-root-r3.pem"};

inline bool certRefused(CURLcode rc) {
    return rc == CURLE_PEER_FAILED_VERIFICATION || rc == CURLE_SSL_CONNECT_ERROR || rc == CURLE_SSL_ISSUER_ERROR
        || rc == CURLE_SSL_INVALIDCERTSTATUS || rc == CURLE_SSL_CACERT_BADFILE;
}

inline std::string hostOf(const std::string& url) {
    size_t at = url.find("://");
    at = at == std::string::npos ? 0 : at + 3;
    const size_t end = url.find_first_of(":/?#", at);
    return url.substr(at, end == std::string::npos ? std::string::npos : end - at);
}

struct Host {
    std::string file;   // the root that works for it ("": the console's store)
    bool tried = false; // its roots were tried this session
};
inline std::mutex& hostsMutex() {
    static std::mutex mu;
    return mu;
}
inline std::map<std::string, Host>& hosts() {
    static std::map<std::string, Host> h;
    return h;
}

inline bool exists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

// A host known to need one of them (GameTDB: Let's Encrypt), straight away.
inline void preferRoot(const std::string& url, const char* file) {
    std::lock_guard<std::mutex> lk(hostsMutex());
    Host& h = hosts()[hostOf(url)];
    if (h.file.empty() && !h.tried) h.file = file;
}

// curl_easy_perform, with the roots for a host the console's store refuses.
// `again` empties what a failed try wrote, before the next one.
inline CURLcode perform(CURL* c, const std::string& url, const std::string& certDir,
                        const std::function<void()>& again = nullptr) {
    const std::string host = hostOf(url);
    std::string known;
    {
        std::lock_guard<std::mutex> lk(hostsMutex());
        known = hosts()[host].file;
    }
    auto use = [&](const std::string& file) {
        if (file.empty()) curl_easy_setopt(c, CURLOPT_CAINFO, static_cast<const char*>(nullptr));
        else curl_easy_setopt(c, CURLOPT_CAINFO, (certDir + file).c_str());
    };
    if (!known.empty()) use(known);
    CURLcode rc = curl_easy_perform(c);
    if (rc == CURLE_OK || !certRefused(rc)) return rc;
    bool tryRoots = false;
    {
        std::lock_guard<std::mutex> lk(hostsMutex());
        Host& h = hosts()[host];
        if (!known.empty()) h.file.clear(); // it stopped working
        tryRoots = !h.tried;
        h.tried = true;
    }
    if (!known.empty()) { // the console's store, then
        use({});
        if (again) again();
        rc = curl_easy_perform(c);
        if (rc == CURLE_OK || !certRefused(rc)) return rc;
    }
    if (tryRoots) {
        for (const char* file : kRootFiles) {
            if (known == file || !exists(certDir + file)) continue;
            use(file);
            if (again) again();
            rc = curl_easy_perform(c);
            if (rc == CURLE_OK) {
                std::lock_guard<std::mutex> lk(hostsMutex());
                hosts()[host].file = file;
                return rc;
            }
            if (!certRefused(rc)) break; // not the certificate (the network): no use trying more
        }
    }
    use({});
    return rc;
}

// The first failure per host, kept for the next bug report (a few lines at most).
inline void noteFailure(const std::string& url, CURLcode rc, const char* logPath) {
    static std::mutex mu;
    static std::set<std::string> noted;
    const std::string key = hostOf(url) + "#" + std::to_string(static_cast<int>(rc));
    std::lock_guard<std::mutex> lk(mu);
    if (!noted.insert(key).second) return;
    if (std::FILE* f = std::fopen(logPath, "a")) {
        std::fprintf(f, "%s: curl error %d (%s)\n", hostOf(url).c_str(), static_cast<int>(rc), curl_easy_strerror(rc));
        std::fclose(f);
    }
}

} // namespace switchu::tls
