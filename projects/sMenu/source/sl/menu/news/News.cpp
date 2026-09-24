#include <sl/menu/news/News.hpp>
#include <sl/menu/net/Http.hpp>
#include <sl/menu/net/ContentFilter.hpp>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <sys/stat.h>

namespace sl::menu::news {

    namespace {

        constexpr const char *kDir     = "sdmc:/slaunch/cache/news";
        constexpr const char *kNinPage = "sdmc:/slaunch/cache/news/ncom.html";

        // How long a cached list stays good. Nintendo posts a handful of stories
        // a day and Steam news moves slower than that, so these are generous:
        // the point of the cache is that opening the menu is not a network wait.
        constexpr time_t kNinTtl   = 6  * 60 * 60;
        constexpr time_t kSteamTtl = 24 * 60 * 60;

        constexpr size_t kNinMax   = 8;   // cards kept per source
        constexpr size_t kSteamMax = 6;

        void EnsureDir() {
            mkdir("sdmc:/slaunch/cache", 0777);   // may already exist; errors are fine
            mkdir(kDir, 0777);
        }

        bool Fresh(const char *path, time_t ttl) {
            struct stat st {};
            if (stat(path, &st) != 0) return false;
            const time_t now = time(nullptr);
            // A card with no clock set can report a file from the future; treat
            // that as fresh rather than refetching on every single frame.
            return (now < st.st_mtime) || (now - st.st_mtime) < ttl;
        }

        // ---- text helpers --------------------------------------------------

        void Utf8Append(std::string &out, unsigned cp) {
            if (cp < 0x80) { out += (char)cp; return; }
            if (cp < 0x800) {
                out += (char)(0xC0 | (cp >> 6));
                out += (char)(0x80 | (cp & 0x3F));
                return;
            }
            if (cp < 0x10000) {
                out += (char)(0xE0 | (cp >> 12));
                out += (char)(0x80 | ((cp >> 6) & 0x3F));
                out += (char)(0x80 | (cp & 0x3F));
                return;
            }
            out += (char)(0xF0 | (cp >> 18));
            out += (char)(0x80 | ((cp >> 12) & 0x3F));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        }

        // The string literal starting at j[s] (s points at the opening quote),
        // with JSON escapes decoded. `end` comes back one past the closing quote.
        std::string JsonUnescape(const std::string &j, size_t s, size_t *end) {
            std::string v;
            size_t i = s + 1;
            for (; i < j.size(); i++) {
                const char c = j[i];
                if (c == '"') break;
                if (c != '\\') { v += c; continue; }
                if (++i >= j.size()) break;
                switch (j[i]) {
                    case 'n': v += ' ';  break;   // cards are one paragraph
                    case 't': v += ' ';  break;
                    case 'r':            break;
                    case 'b': case 'f':  break;
                    case 'u': {
                        if (i + 4 >= j.size()) { i = j.size(); break; }
                        unsigned cp = (unsigned)strtoul(j.substr(i + 1, 4).c_str(),
                                                        nullptr, 16);
                        i += 4;
                        // Surrogate pair: take the low half too, or the codepoint
                        // is meaningless on its own.
                        if (cp >= 0xD800 && cp <= 0xDBFF &&
                            i + 6 < j.size() && j[i + 1] == '\\' && j[i + 2] == 'u') {
                            const unsigned lo = (unsigned)strtoul(
                                j.substr(i + 3, 4).c_str(), nullptr, 16);
                            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                                i += 6;
                            }
                        }
                        if (cp >= 0xD800 && cp <= 0xDFFF) cp = '?';  // lone half
                        Utf8Append(v, cp);
                        break;
                    }
                    default: v += j[i]; break;    // covers \" \\ \/
                }
            }
            if (end) *end = (i < j.size()) ? i + 1 : j.size();
            return v;
        }

        // First string value for `key` at or after `from`, bounded by `limit` so
        // a missing field cannot pick up the next record's value.
        std::string Field(const std::string &j, const char *key,
                          size_t from, size_t limit) {
            const std::string pat = std::string("\"") + key + "\"";
            const size_t p = j.find(pat, from);
            if (p == std::string::npos || p >= limit) return std::string();
            const size_t colon = j.find(':', p + pat.size());
            if (colon == std::string::npos || colon >= limit) return std::string();
            const size_t s = j.find('"', colon);
            if (s == std::string::npos || s >= limit) return std::string();
            return JsonUnescape(j, s, nullptr);
        }

        // Everything between tags dropped, runs of space collapsed. Steam news
        // arrives as a mix of HTML and BBCode depending on who posted it.
        std::string Plain(const std::string &in, size_t max_chars) {
            std::string out;
            bool in_tag = false;
            char bb = 0;                     // inside a [bbcode] tag
            for (size_t i = 0; i < in.size(); i++) {
                const char c = in[i];
                if (c == '<') { in_tag = true;  continue; }
                if (c == '>') { in_tag = false; out += ' '; continue; }
                if (in_tag) continue;
                if (c == '[') { bb = 1; continue; }
                if (c == ']') { bb = 0; out += ' '; continue; }
                if (bb) continue;
                if (c == '\r') continue;
                out += (c == '\n' || c == '\t') ? ' ' : c;
            }
            // Collapse whitespace so stripped markup does not leave gaps.
            std::string tidy;
            bool sp = false;
            for (char c : out) {
                if (c == ' ') { sp = true; continue; }
                if (sp && !tidy.empty()) tidy += ' ';
                sp = false;
                tidy += c;
            }
            if (tidy.size() > max_chars) {
                tidy.resize(max_chars);
                // Cut on a word so the ellipsis does not land mid-word.
                const size_t sp2 = tidy.find_last_of(' ');
                if (sp2 != std::string::npos && sp2 > max_chars / 2) tidy.resize(sp2);
                tidy += "...";
            }
            return tidy;
        }

        std::string UrlEncode(const std::string &s, const char *keep) {
            static const char *hex = "0123456789ABCDEF";
            std::string out;
            for (unsigned char c : s) {
                const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                  (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                                  c == '.' || c == '~' ||
                                  (keep && strchr(keep, (int)c) != nullptr);
                if (safe) { out += (char)c; continue; }
                out += '%';
                out += hex[c >> 4];
                out += hex[c & 0xF];
            }
            return out;
        }

        // "2026-08-27T16:00:00.000Z" -> "27 Aug".
        std::string DateIso(const std::string &iso) {
            if (iso.size() < 10) return std::string();
            static const char *mon[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                           "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
            const int m = atoi(iso.substr(5, 2).c_str());
            const int d = atoi(iso.substr(8, 2).c_str());
            if (m < 1 || m > 12 || d < 1 || d > 31) return std::string();
            char buf[16];
            snprintf(buf, sizeof(buf), "%d %s", d, mon[m - 1]);
            return buf;
        }

        std::string DateUnix(const std::string &secs) {
            const time_t t = (time_t)strtoull(secs.c_str(), nullptr, 10);
            if (t <= 0) return std::string();
            struct tm tmv {};
            if (!localtime_r(&t, &tmv)) return std::string();
            char buf[16];
            strftime(buf, sizeof(buf), "%d %b", &tmv);
            // Leading zero on the day looks wrong next to a two-digit one.
            return (buf[0] == '0') ? std::string(buf + 1) : std::string(buf);
        }

        // ---- cache ---------------------------------------------------------

        std::string CachePath(Source s, u64 app_id) {
            char p[96];
            if (s == Source::Steam)
                snprintf(p, sizeof(p), "%s/steam_%016llX.txt", kDir,
                         (unsigned long long)app_id);
            else
                snprintf(p, sizeof(p), "%s/nintendo.txt", kDir);
            return p;
        }

        void Save(const std::vector<Item> &items, const std::string &path) {
            EnsureDir();
            FILE *fp = fopen(path.c_str(), "w");
            if (!fp) return;
            for (const Item &it : items)
                fprintf(fp, "%s\t%s\t%s\t%s\t%s\t%s\n",
                        it.kind.c_str(), it.date.c_str(), it.img.c_str(),
                        it.link.c_str(), it.title.c_str(), it.summary.c_str());
            fclose(fp);
        }

        std::vector<Item> Load(const std::string &path) {
            std::vector<Item> out;
            FILE *fp = fopen(path.c_str(), "r");
            if (!fp) return out;
            // Long enough for a card's worth of summary plus a Cloudinary url.
            std::vector<char> line(2048);
            while (fgets(line.data(), (int)line.size(), fp)) {
                std::string s(line.data());
                while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
                Item it;
                std::string *dst[6] = { &it.kind, &it.date, &it.img,
                                        &it.link, &it.title, &it.summary };
                size_t start = 0;
                int f = 0;
                for (; f < 6; f++) {
                    if (start > s.size()) break;
                    const size_t tab = (f == 5) ? std::string::npos : s.find('\t', start);
                    *dst[f] = s.substr(start, tab == std::string::npos
                                              ? std::string::npos : tab - start);
                    if (tab == std::string::npos) { f++; break; }
                    start = tab + 1;
                }
                if (f == 6 && !it.title.empty()) out.push_back(std::move(it));
            }
            fclose(fp);
            return out;
        }

    } // namespace

    Feed::~Feed() {
        if (m_running) {
            threadWaitForExit(&m_thread);
            threadClose(&m_thread);
            m_running = false;
        }
    }

    void Feed::Init() {
        m_nintendo = Load(CachePath(Source::Nintendo, 0));
    }

    void Feed::Request(Source s, u64 app_id, const char *name) {
        if (m_running) return;

        if (s == Source::Steam) {
            if (app_id == 0 || !name || !*name) return;
            if (app_id == m_steam_of) return;                    // already showing
            if (std::find(m_steam_asked.begin(), m_steam_asked.end(), app_id)
                != m_steam_asked.end()) {
                // Asked before and it came back empty: show the cache (if any)
                // and do not go back out for it.
                if (app_id != m_steam_of) {
                    m_steam    = Load(CachePath(Source::Steam, app_id));
                    m_steam_of = app_id;
                }
                return;
            }
            const std::string cache = CachePath(Source::Steam, app_id);
            if (Fresh(cache.c_str(), kSteamTtl)) {
                m_steam    = Load(cache);
                m_steam_of = app_id;
                m_steam_tried = true;
                return;
            }
            m_job_id   = app_id;
            m_job_name = name;
        } else {
            if (!m_nintendo.empty() &&
                Fresh(CachePath(Source::Nintendo, 0).c_str(), kNinTtl))
                return;
        }

        m_job = s;
        m_pending.clear();
        m_done.store(false, std::memory_order_release);
        // Same shape as the cover fetch: a low-priority worker off the render
        // thread, reaped by Poll. The stack is generous because the Nintendo
        // page is parsed in one string.
        if (R_SUCCEEDED(threadCreate(&m_thread, &Feed::Trampoline, this,
                                     nullptr, 0x20000, 0x3B, -2))) {
            threadStart(&m_thread);
            m_running = true;
        }
    }

    bool Feed::Poll() {
        if (!m_running || !m_done.load(std::memory_order_acquire)) return false;
        threadWaitForExit(&m_thread);
        threadClose(&m_thread);
        m_running = false;

        if (m_job == Source::Steam) {
            m_steam       = m_pending;
            m_steam_of    = m_job_id;
            m_steam_tried = true;
            if (std::find(m_steam_asked.begin(), m_steam_asked.end(), m_job_id)
                == m_steam_asked.end())
                m_steam_asked.push_back(m_job_id);
        } else {
            // A failed refetch keeps whatever was on the card: stale news beats
            // an empty row.
            if (!m_pending.empty()) m_nintendo = m_pending;
            m_nin_tried = true;
        }
        m_pending.clear();
        return true;
    }

    void Feed::Trampoline(void *self) { static_cast<Feed *>(self)->Work(); }

    void Feed::Work() {
        if (m_job == Source::Steam) FetchSteam();
        else                        FetchNintendo();
        m_done.store(true, std::memory_order_release);
    }

    // ---- Nintendo ----------------------------------------------------------

    void Feed::FetchNintendo() {
        EnsureDir();

        // Streamed to a file rather than held in memory: the page is ~350KB,
        // which is over the cap Get() keeps on a response body.
        if (!net::Download("https://www.nintendo.com/us/whatsnew/", kNinPage, 25))
            return;

        std::string page;
        if (FILE *fp = fopen(kNinPage, "rb")) {
            fseek(fp, 0, SEEK_END);
            const long sz = ftell(fp);
            fseek(fp, 0, SEEK_SET);
            if (sz > 0 && sz < 4 * 1024 * 1024) {
                page.resize((size_t)sz);
                if (fread(&page[0], 1, (size_t)sz, fp) != (size_t)sz) page.clear();
            }
            fclose(fp);
        }
        remove(kNinPage);
        if (page.empty()) return;

        // The page ships its Apollo cache inline, one object per article. Each
        // record is walked from its own "__typename":"NewsArticle" up to the
        // next one, so a field the page has stopped emitting reads as missing
        // instead of borrowing the following article's value.
        const std::string mark = "\"__typename\":\"NewsArticle\"";
        size_t p = page.find(mark);
        std::vector<Item> out;
        while (p != std::string::npos && out.size() < kNinMax) {
            const size_t next = page.find(mark, p + mark.size());
            const size_t lim  = (next == std::string::npos) ? page.size() : next;

            Item it;
            it.title   = Field(page, "title", p, lim);
            it.date    = DateIso(Field(page, "publishDate", p, lim));
            it.link    = "nintendo.com" + Field(page, "url({\\\"relative\\\":true})", p, lim);
            // The body is keyed by the call that produced it, character limit
            // and all. Matching on the stable head of that key rather than the
            // whole thing keeps this working if the limit is retuned.
            {
                const size_t b = page.find("\"text({", p);
                if (b != std::string::npos && b < lim) {
                    const size_t colon = page.find(':', page.find("})\"", b));
                    const size_t s     = page.find('"', colon);
                    if (colon != std::string::npos && s != std::string::npos && s < lim)
                        it.summary = Plain(JsonUnescape(page, s, nullptr), 220);
                }
            }

            // Category tag -> the small label across the top of the card.
            it.kind = "NEWS";
            {
                const size_t t = page.find("ContentTag:articleCategory", p);
                if (t != std::string::npos && t < lim) {
                    const std::string cat =
                        page.substr(t + strlen("ContentTag:articleCategory"), 24);
                    if      (cat.rfind("Promotions", 0) == 0) it.kind = "PROMOTION";
                    else if (cat.rfind("Updates", 0)    == 0) it.kind = "UPDATE";
                    else if (cat.rfind("Events", 0)     == 0) it.kind = "EVENT";
                    else if (cat.rfind("Releases", 0)   == 0) it.kind = "NEW RELEASE";
                }
            }

            if (!it.title.empty()) {
                // Art. The page names a Cloudinary asset; the delivery url is
                // built from it. f_jpg rather than f_auto: the console decodes
                // JPEG everywhere and this way the format cannot change under us.
                const std::string pid = Field(page, "publicId", p, lim);
                if (!pid.empty()) {
                    const std::string url =
                        "https://assets.nintendo.com/image/upload/"
                        "c_fill,f_jpg,q_auto,w_480/" + UrlEncode(pid, "/");
                    char dst[96];
                    snprintf(dst, sizeof(dst), "%s/n%u.jpg", kDir,
                             (unsigned)out.size());
                    if (net::Download(url.c_str(), dst, 20)) it.img = dst;
                }
                // Filter adult content before adding to output
                if (!net::ContentFilter::ShouldFilterNewsArticle(it.title, it.summary, it.kind)) {
                    out.push_back(std::move(it));
                }
            }
            p = next;
        }

        if (out.empty()) return;
        Save(out, CachePath(Source::Nintendo, 0));
        m_pending = std::move(out);
    }

    // ---- Steam -------------------------------------------------------------

    void Feed::FetchSteam() {
        EnsureDir();

        // Trim the decorations a Switch title carries that Steam's index does
        // not: an edition suffix is the usual reason a match is missed.
        std::string q = m_job_name;
        for (const char *cut : { "\xe2\x84\xa2", "\xc2\xae" }) {   // (TM), (R)
            size_t at;
            while ((at = q.find(cut)) != std::string::npos) q.erase(at, strlen(cut));
        }
        std::string body;
        std::string url = "https://steamcommunity.com/actions/SearchApps/" +
                          UrlEncode(q, "");
        if (!net::Get(url.c_str(), body, 10)) return;

        const std::string appid = net::SteamAppFor(body, m_job_name);
        if (appid.empty()) return;   // not on Steam: no news beats someone else's

        char nurl[192];
        snprintf(nurl, sizeof(nurl),
                 "https://api.steampowered.com/ISteamNews/GetNewsForApp/v2/"
                 // Long enough that a post's first inline image is inside the
                 // slice Steam returns: the summary is trimmed locally anyway,
                 // and at 600 the art was usually cut off with the tail.
                 "?appid=%s&count=%u&maxlength=1200",
                 appid.c_str(), (unsigned)kSteamMax);
        body.clear();
        if (!net::Get(nurl, body, 10)) return;

        // The store header, used for any story that carries no art of its own -
        // a row where only some cards have a picture reads as broken rather than
        // sparse.
        std::string img;
        {
            const std::string hurl =
                "https://cdn.cloudflare.steamstatic.com/steam/apps/" + appid +
                "/header.jpg";
            char dst[96];
            snprintf(dst, sizeof(dst), "%s/s%016llX.jpg", kDir,
                     (unsigned long long)m_job_id);
            if (net::Download(hurl.c_str(), dst, 20)) img = dst;
        }

        // Each story starts at its own "gid", which is what separates the
        // records; every field is then read inside that story's span.
        std::vector<Item> out;
        size_t p = body.find("\"gid\"");
        while (p != std::string::npos && out.size() < kSteamMax) {
            const size_t next = body.find("\"gid\"", p + 5);
            const size_t lim  = (next == std::string::npos) ? body.size() : next;

            Item it;
            it.title   = Plain(Field(body, "title", p, lim), 120);
            it.link    = Field(body, "url", p, lim);
            it.summary = Plain(Field(body, "contents", p, lim), 220);
            it.kind    = Field(body, "feedlabel", p, lim);
            if (it.kind.empty()) it.kind = "STEAM";
            // Art of the story's own, when it has any. Steam writes these as a
            // {STEAM_CLAN_IMAGE} placeholder that the client expands; the store
            // header is the fallback for everything else.
            it.img = img;
            {
                const std::string body_span = body.substr(p, lim - p);
                const char *tag = "{STEAM_CLAN_IMAGE}";
                const size_t ph = body_span.find(tag);
                if (ph != std::string::npos) {
                    size_t e = ph + strlen(tag);
                    std::string rel;
                    // Up to the quote, the escape, or whatever whitespace the
                    // post put after it.
                    for (; e < body_span.size(); e++) {
                        const char c = body_span[e];
                        if (c == '"' || c == '\\' || c == ' ' || c == '\n' ||
                            c == '[' || c == ')') break;
                        rel += c;
                    }
                    if (rel.size() > 4) {
                        const std::string iurl =
                            "https://clan.cloudflare.steamstatic.com/images" + rel;
                        // .jpg whatever the source is - Steam serves PNG here
                        // too - because the decoder sniffs content, not names,
                        // and the rest of the cache is named this way.
                        char dst[112];
                        snprintf(dst, sizeof(dst), "%s/s%016llX_%u.jpg", kDir,
                                 (unsigned long long)m_job_id,
                                 (unsigned)out.size());
                        if (net::Download(iurl.c_str(), dst, 20)) it.img = dst;
                    }
                }
            }
            // date is a number, not a string, so it is read off the raw text.
            {
                const size_t d = body.find("\"date\":", p);
                if (d != std::string::npos && d < lim)
                    it.date = DateUnix(body.substr(d + 7, 20));
            }
            // Filter adult content before adding to output
            if (!it.title.empty() && !net::ContentFilter::ShouldFilterSteamNews(it.title, it.summary)) {
                out.push_back(std::move(it));
            }
            p = next;
        }

        if (out.empty()) return;
        Save(out, CachePath(Source::Steam, m_job_id));
        m_pending = std::move(out);
    }

} // namespace sl::menu::news
