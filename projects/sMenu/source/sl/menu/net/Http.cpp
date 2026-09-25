#include <sl/menu/net/Http.hpp>
#include <string>
#include <curl/curl.h>
#include <cctype>
#include <cstring>
#include <cstdio>

namespace sl::menu::net {

    namespace {
        bool g_inited = false;

        size_t WriteCb(char *ptr, size_t size, size_t nmemb, void *userdata) {
            auto *out = static_cast<std::string *>(userdata);
            const size_t n = size * nmemb;
            // Cap the buffer so a runaway response can't exhaust the applet heap.
            if (out->size() + n > 256 * 1024) return 0;
            out->append(ptr, n);
            return n;
        }
    }

    void GlobalInit() {
        if (g_inited) return;
        if (curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK)
            g_inited = true;
    }

    void GlobalExit() {
        if (!g_inited) return;
        curl_global_cleanup();
        g_inited = false;
    }

    bool Get(const char *url, std::string &out, long timeout_s,
             const char *authorization, long *out_http, int *out_curl) {
        out.clear();
        CURL *curl = curl_easy_init();
        if (!curl) return false;

        struct curl_slist *hdrs = nullptr;
        if (authorization && *authorization) {
            std::string h = std::string("Authorization: ") + authorization;
            hdrs = curl_slist_append(hdrs, h.c_str());
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
        }

        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_s);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, timeout_s);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "sLaunch/0.1");
        // No CA bundle on the Switch -> skip verification.
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

        const CURLcode rc = curl_easy_perform(curl);
        long http = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
        curl_easy_cleanup(curl);
        if (hdrs) curl_slist_free_all(hdrs);

        if (out_http) *out_http = http;
        if (out_curl) *out_curl = (int)rc;
        return rc == CURLE_OK && http >= 200 && http < 300;
    }

    bool Post(const char *url, const char *body, const char *content_type,
              const char *authorization, std::string &out, long timeout_s) {
        out.clear();
        CURL *curl = curl_easy_init();
        if (!curl) return false;

        struct curl_slist *headers = nullptr;
        if (content_type) {
            std::string h = std::string("Content-Type: ") + content_type;
            headers = curl_slist_append(headers, h.c_str());
        }
        if (authorization) {
            std::string h = std::string("Authorization: ") + authorization;
            headers = curl_slist_append(headers, h.c_str());
        }

        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body ? body : "");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)(body ? strlen(body) : 0));
        if (headers) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_s);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, timeout_s);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "sLaunch/0.1");
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

        const CURLcode rc = curl_easy_perform(curl);
        long http = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
        if (headers) curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        return rc == CURLE_OK && http >= 200 && http < 300;
    }

    namespace {
        size_t WriteFileCb(char *ptr, size_t size, size_t nmemb, void *userdata) {
            return fwrite(ptr, size, nmemb, static_cast<FILE *>(userdata)) * size;
        }
    }

    // An image that stops before its end marker. SDL_image hands a broken
    // JPEG to libjpeg, whose error path crashes the whole menu on the console
    // (SDL_image's longjmp out of it lands on garbage), so a download that is
    // not whole must never be kept. Anything that is not a JPEG or PNG passes.
    static bool ImageLooksWhole(const char *path) {
        FILE *fp = fopen(path, "rb");
        if (!fp) return false;
        unsigned char head[8] = {}, tail[64] = {};
        const size_t hn = fread(head, 1, sizeof(head), fp);
        fseek(fp, 0, SEEK_END);
        const long size = ftell(fp);
        const long tn = size < (long)sizeof(tail) ? size : (long)sizeof(tail);
        fseek(fp, size - tn, SEEK_SET);
        const size_t got = fread(tail, 1, (size_t)tn, fp);
        fclose(fp);
        if (hn >= 3 && head[0] == 0xFF && head[1] == 0xD8 && head[2] == 0xFF) {   // JPEG: FF D9
            for (size_t i = 0; i + 1 < got; i++)
                if (tail[i] == 0xFF && tail[i + 1] == 0xD9) return true;
            return false;
        }
        if (hn >= 8 && !memcmp(head, "\x89PNG", 4)) {                              // PNG: IEND
            for (size_t i = 0; i + 4 <= got; i++)
                if (!memcmp(tail + i, "IEND", 4)) return true;
            return false;
        }
        return true;
    }

    namespace {
        struct Progress { std::atomic<uint64_t> *now, *total; };
        int ProgressCb(void *p, curl_off_t dltotal, curl_off_t dlnow, curl_off_t, curl_off_t) {
            auto *pr = static_cast<Progress *>(p);
            if (pr->now)   pr->now->store((uint64_t)dlnow);
            if (pr->total) pr->total->store((uint64_t)dltotal);
            return 0;
        }
    }

    bool Download(const char *url, const char *path, long timeout_s,
                  std::atomic<uint64_t> *now, std::atomic<uint64_t> *total) {
        if (now)   now->store(0);
        if (total) total->store(0);
        // Downloaded beside the target and renamed in once it is whole, so
        // nothing ever finds a half-written file under the real name - the art
        // worker decodes whatever is there, and a truncated JPEG is a libjpeg
        // error, which takes the menu down (see ImageLooksWhole).
        const std::string part = std::string(path) + ".part";
        FILE *fp = fopen(part.c_str(), "wb");
        if (!fp) return false;

        CURL *curl = curl_easy_init();
        if (!curl) { fclose(fp); remove(part.c_str()); return false; }

        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteFileCb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_s);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, timeout_s);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "sLaunch/0.1");
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        Progress pr{ now, total };
        if (now || total) {
            curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, ProgressCb);
            curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &pr);
            curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        }

        const CURLcode rc = curl_easy_perform(curl);
        long http = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
        curl_easy_cleanup(curl);
        fclose(fp);

        bool ok = rc == CURLE_OK && http >= 200 && http < 300 && ImageLooksWhole(part.c_str());
        if (ok) {
            remove(path);                          // FAT rename will not overwrite
            ok = rename(part.c_str(), path) == 0;
        }
        if (!ok) remove(part.c_str());   // don't leave a 404 page or partial file behind
        return ok;
    }

    namespace {
        // Lowercase words, everything else a single space, (TM)/(R) dropped.
        std::string NormTitle(const std::string &in) {
            std::string out;
            bool sp = false;
            for (size_t i = 0; i < in.size(); i++) {
                const unsigned char c = (unsigned char)in[i];
                if (c >= 0x80) {                          // skip a UTF-8 sequence
                    while (i + 1 < in.size() && ((unsigned char)in[i + 1] & 0xC0) == 0x80) i++;
                    sp = true;
                    continue;
                }
                if (isalnum(c)) {
                    if (sp && !out.empty()) out += ' ';
                    out += (char)tolower(c);
                    sp = false;
                } else if (c != '\'') {
                    sp = true;                            // "Baldur's" stays one word
                }
            }
            return out;
        }
        // The title before an edition suffix: "Cyberpunk 2077: Ultimate Edition".
        std::string BaseTitle(const std::string &t) {
            size_t cut = t.find(':');
            const size_t dash = t.find(" - ");
            if (dash != std::string::npos && dash < cut) cut = dash;
            return NormTitle(cut == std::string::npos ? t : t.substr(0, cut));
        }
        std::string JsonField(const std::string &j, const char *key, size_t from, size_t to) {
            const std::string pat = std::string("\"") + key + "\":";
            size_t p = j.find(pat, from);
            if (p == std::string::npos || p >= to) return {};
            p += pat.size();
            while (p < j.size() && j[p] == ' ') p++;
            if (p >= j.size() || j[p] != '"') return {};
            std::string v;
            for (size_t i = p + 1; i < j.size() && j[i] != '"'; i++) {
                if (j[i] == '\\' && i + 1 < j.size()) i++;
                v += j[i];
            }
            return v;
        }
    }

    namespace {
        // The part after ':' or " - ", normalised; empty when there is none.
        std::string Suffix(const std::string &t) {
            size_t cut = t.find(':');
            const size_t dash = t.find(" - ");
            if (dash != std::string::npos && dash < cut) cut = dash;
            return cut == std::string::npos ? std::string() : NormTitle(t.substr(cut + 1));
        }
        // An edition, not a different game: "Ultimate Edition", "Deluxe", ...
        // "Hollow Knight: Silksong" is a sequel, and must not match "Hollow Knight".
        bool IsEditionSuffix(const std::string &norm) {
            if (norm.empty()) return false;
            for (const char *w : { "edition", "deluxe", "complete", "definitive", "remastered",
                                   "remaster", "goty", "game of the year", "directors cut",
                                   "director s cut", "anniversary", "hd", "enhanced", "bundle",
                                   "nintendo switch", "switch" })
                if (norm.find(w) != std::string::npos) return true;
            return false;
        }
        // 2 = same title, 1 = same game in another edition, 0 = not it.
        int MatchLevel(const std::string &cand, const std::string &title) {
            const std::string want = NormTitle(title);
            if (want.empty()) return 0;
            const std::string n = NormTitle(cand);
            if (n == want) return 2;
            if (n == BaseTitle(title) && IsEditionSuffix(Suffix(title))) return 1;
            if (BaseTitle(cand) == want && IsEditionSuffix(Suffix(cand))) return 1;
            return 0;
        }
    }

    bool TitlesMatch(const std::string &cand, const std::string &title) {
        return MatchLevel(cand, title) > 0;
    }

    std::string SteamAppFor(const std::string &json, const std::string &title) {
        std::string edition;   // best so far if no exact title turns up
        // One object per result: {"appid":"...","name":"...",...}
        for (size_t p = json.find('{'); p != std::string::npos; p = json.find('{', p + 1)) {
            const size_t end = json.find('}', p);
            if (end == std::string::npos) break;
            const std::string appid = JsonField(json, "appid", p, end);
            const std::string name  = JsonField(json, "name", p, end);
            if (appid.empty() || name.empty()) continue;
            const int lvl = MatchLevel(name, title);
            if (lvl == 2) return appid;
            if (lvl == 1 && edition.empty()) edition = appid;
        }
        return edition;
    }

} // namespace sl::menu::net
