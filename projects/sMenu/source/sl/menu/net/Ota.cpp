#include <sl/menu/net/Ota.hpp>
#include <curl/curl.h>
#include <zlib.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <sys/stat.h>

namespace sl::menu::ota {

    namespace {
        const char kUserAgent[] = "sLaunch (https://github.com/etonedemid/slaunch)";

        uint16_t LE16(const unsigned char *p) { return (uint16_t)(p[0] | p[1] << 8); }
        uint32_t LE32(const unsigned char *p) {
            return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
        }
        bool Starts(const std::string &s, const char *pre) { return s.compare(0, strlen(pre), pre) == 0; }

        // Settings, added wallpapers and music, the icon cache: the installer's
        // IsUserData list. Shipped defaults arrive once and are never replaced.
        bool UserData(const std::string &name) {
            for (const char *d : { "slaunch/config/", "slaunch/themes/", "slaunch/music/", "slaunch/cache/" })
                if (Starts(name, d)) return true;
            return false;
        }
        // The files that decide what boots (sInstaller's upgrade::IsBootEntry).
        // They go last, so a failure before them leaves the console booting
        // what it had.
        bool Boot(const std::string &name) { return Starts(name, "atmosphere/contents/0100000000001000/"); }

        // A name from the zip may only point inside root.
        bool SafeName(const std::string &n) {
            if (n.empty() || n[0] == '/' || n.find('\\') != std::string::npos || n.find(':') != std::string::npos)
                return false;
            for (size_t p = 0; p <= n.size();) {
                const size_t q = std::min(n.find('/', p), n.size());
                if (n.compare(p, q - p, "..") == 0 && q - p == 2) return false;
                p = q + 1;
            }
            return true;
        }

        void MakeDirs(const std::string &file) {
            for (size_t i = 1; i < file.size(); i++)
                if (file[i] == '/') mkdir(file.substr(0, i).c_str(), 0777);
        }

        bool FileCrc(const std::string &path, uint32_t &out) {
            FILE *f = fopen(path.c_str(), "rb");
            if (!f) return false;
            std::vector<unsigned char> buf(65536);   // not thread_local: libnx gives every thread that much TLS
            uLong crc = crc32(0, nullptr, 0);
            size_t n;
            while ((n = fread(buf.data(), 1, buf.size(), f)) > 0) crc = crc32(crc, buf.data(), (uInt)n);
            const bool ok = !ferror(f);
            fclose(f);
            out = (uint32_t)crc;
            return ok;
        }

        using Sink = std::function<bool(const char *, size_t)>;
        size_t SinkCb(char *p, size_t s, size_t n, void *u) {
            return (*static_cast<Sink *>(u))(p, s * n) ? s * n : 0;
        }

        void Common(CURL *c, const std::string &url) {
            curl_easy_reset(c);
            curl_easy_setopt(c, CURLOPT_URL, url.c_str());
            curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
            // No cap on the whole transfer, which is a big file on a slow line;
            // half a minute without a byte is a dead one.
            curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);
            curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 30L);
            curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
            curl_easy_setopt(c, CURLOPT_USERAGENT, kUserAgent);
            // No CA bundle on the Switch. Every byte is CRC-checked against the
            // zip's own directory before it replaces anything.
            curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
        }

        // The zip's size, from a HEAD request. GitHub's asset server refuses a
        // "last n bytes" range (501), so the end has to be asked for by offset.
        bool Size(CURL *c, const std::string &url, uint64_t &out, std::string &err) {
            Common(c, url);
            curl_easy_setopt(c, CURLOPT_NOBODY, 1L);
            const CURLcode rc = curl_easy_perform(c);
            long http = 0;
            curl_off_t len = -1;
            curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http);
            curl_easy_getinfo(c, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &len);
            if (rc != CURLE_OK) { err = std::string("Network error: ") + curl_easy_strerror(rc); return false; }
            if ((http != 200 && http != 0) || len <= 0) {
                err = "The download server answered HTTP " + std::to_string(http) + ".";
                return false;
            }
            out = (uint64_t)len;
            return true;
        }

        // One byte range of url, "from-to" inclusive. A server that ignores the
        // range would send the whole zip, so only a partial answer counts
        // (file:// has no status, and honours ranges).
        bool Fetch(CURL *c, const std::string &url, uint64_t from, uint64_t to, Sink sink, std::string &err) {
            Common(c, url);
            const std::string range = std::to_string(from) + "-" + std::to_string(to);
            curl_easy_setopt(c, CURLOPT_RANGE, range.c_str());
            curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, SinkCb);
            curl_easy_setopt(c, CURLOPT_WRITEDATA, &sink);
            const CURLcode rc = curl_easy_perform(c);
            long http = 0;
            curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http);
            if (rc != CURLE_OK) {
                if (err.empty()) err = std::string("Network error: ") + curl_easy_strerror(rc);
                return false;
            }
            if (http != 206 && http != 0) {
                err = "The download server answered HTTP " + std::to_string(http) + ".";
                return false;
            }
            return true;
        }

        // Every entry of the zip's central directory.
        bool ReadIndex(CURL *c, const std::string &url, std::vector<Entry> &out, std::string &err) {
            std::string tail;
            const auto into = [&err](std::string &s, size_t cap) {
                return Sink([&s, &err, cap](const char *p, size_t n) {
                    if (s.size() + n > cap) { err = "The release's file list is too big."; return false; }
                    s.append(p, n);
                    return true;
                });
            };
            // The end record is 22 bytes plus a comment of up to 64 KiB.
            uint64_t size = 0;
            if (!Size(c, url, size, err)) return false;
            if (!Fetch(c, url, size > 65558 ? size - 65558 : 0, size - 1, into(tail, 1 << 20), err)) return false;
            size_t e = std::string::npos;
            for (size_t i = tail.size() >= 22 ? tail.size() - 22 : 0; tail.size() >= 22; i--) {
                if (LE32((const unsigned char *)tail.data() + i) == 0x06054b50) { e = i; break; }
                if (i == 0) break;
            }
            if (e == std::string::npos) { err = "The release is not a zip."; return false; }
            const unsigned char *r = (const unsigned char *)tail.data() + e;
            const uint16_t count = LE16(r + 10);
            const uint32_t cd_size = LE32(r + 12), cd_off = LE32(r + 16);
            if (count == 0xFFFF || cd_off == 0xFFFFFFFF) { err = "The release zip is too big to read."; return false; }
            // The directory sits right before the end record.
            const uint64_t tail_at = (uint64_t)cd_off + cd_size - e;
            std::string cd;
            if (cd_off >= tail_at) cd = tail.substr(cd_off - tail_at, cd_size);
            else if (!Fetch(c, url, cd_off, (uint64_t)cd_off + cd_size - 1, into(cd, 8 << 20), err)) return false;
            if (cd.size() != cd_size) { err = "The release's file list arrived cut short."; return false; }

            out.clear();
            size_t p = 0;
            for (int k = 0; k < count; k++) {
                const unsigned char *h = (const unsigned char *)cd.data() + p;
                if (p + 46 > cd.size() || LE32(h) != 0x02014b50) { err = "The release's file list is damaged."; return false; }
                const uint16_t nlen = LE16(h + 28), xlen = LE16(h + 30), clen = LE16(h + 32);
                if (p + 46 + nlen > cd.size()) { err = "The release's file list is damaged."; return false; }
                Entry en;
                en.method = LE16(h + 10);
                en.crc    = LE32(h + 16);
                en.csize  = LE32(h + 20);
                en.usize  = LE32(h + 24);
                en.offset = LE32(h + 42);
                en.name   = cd.substr(p + 46, nlen);
                if (LE16(h + 8) & 1) { err = "The release zip is encrypted."; return false; }
                out.push_back(std::move(en));
                p += 46 + nlen + xlen + clen;
            }
            return true;
        }

        // One file: its local header and compressed bytes in one range, inflated
        // into "<target>.ota" and checked against the directory's size and CRC.
        bool GetOne(CURL *c, const std::string &url, const std::string &tmp, const Entry &e,
                    Progress &pr, std::string &err) {
            MakeDirs(tmp);
            FILE *f = fopen(tmp.c_str(), "wb");
            if (!f) { err = "Could not write " + e.name + "."; return false; }
            z_stream z{};
            if (e.method == 8 && inflateInit2(&z, -MAX_WBITS) != Z_OK) {
                fclose(f);
                remove(tmp.c_str());
                err = "Out of memory.";
                return false;
            }
            unsigned char hdr[30];
            size_t hn = 0, name_left = 0, extra_left = 0;
            std::string name;
            uint32_t left = e.csize;
            uLong crc = crc32(0, nullptr, 0);
            uint64_t written = 0;
            bool ended = false;
            std::vector<unsigned char> outbuf(65536);
            unsigned char *const out = outbuf.data();
            const size_t out_n = outbuf.size();
            const auto put = [&](const unsigned char *p, size_t n) {
                crc = crc32(crc, p, (uInt)n);
                written += n;
                return fwrite(p, 1, n, f) == n;
            };
            Sink sink = [&](const char *src, size_t n) -> bool {
                const unsigned char *p = (const unsigned char *)src;
                if (pr.cancel.load()) { err = "Cancelled"; return false; }
                while (n) {
                    if (hn < 30) {                              // the local header
                        const size_t k = std::min(30 - hn, n);
                        memcpy(hdr + hn, p, k);
                        hn += k; p += k; n -= k;
                        if (hn == 30) {
                            if (LE32(hdr) != 0x04034b50) { err = e.name + " is not where the zip says."; return false; }
                            name_left  = LE16(hdr + 26);
                            extra_left = LE16(hdr + 28);
                        }
                    } else if (name_left) {
                        const size_t k = std::min(name_left, n);
                        name.append((const char *)p, k);
                        name_left -= k; p += k; n -= k;
                        if (!name_left && name != e.name) { err = e.name + " is not where the zip says."; return false; }
                    } else if (extra_left) {
                        const size_t k = std::min(extra_left, n);
                        extra_left -= k; p += k; n -= k;
                    } else if (left) {                          // the data
                        const size_t k = std::min<size_t>(left, n);
                        if (e.method == 0) {
                            if (!put(p, k)) { err = "The card is full or failing."; return false; }
                        } else {
                            z.next_in = const_cast<unsigned char *>(p);
                            z.avail_in = (uInt)k;
                            do {
                                z.next_out = out;
                                z.avail_out = (uInt)out_n;
                                const int r = inflate(&z, Z_NO_FLUSH);
                                if (r == Z_STREAM_END) ended = true;
                                else if (r != Z_OK && r != Z_BUF_ERROR) { err = e.name + " arrived damaged."; return false; }
                                if (!put(out, out_n - z.avail_out)) { err = "The card is full or failing."; return false; }
                            } while (!ended && (z.avail_in > 0 || z.avail_out == 0));
                        }
                        left -= (uint32_t)k; p += k; n -= k;
                        pr.done += k;
                    } else {
                        return true;                            // the slack after it
                    }
                }
                return true;
            };
            // The local header's extra field can differ from the directory's;
            // a KiB of slack covers what zip writes there.
            const uint64_t from = e.offset, to = from + 30 + e.name.size() + 1024 + e.csize - 1;
            bool ok = Fetch(c, url, from, to, sink, err);
            if (e.method == 8) inflateEnd(&z);
            if (fclose(f) != 0) ok = false;
            if (ok && (left || (e.method == 8 && !ended) || written != e.usize || (uint32_t)crc != e.crc)) {
                err = e.name + " arrived damaged.";
                ok = false;
            }
            if (!ok) remove(tmp.c_str());
            return ok;
        }
    }

    std::string ZipUrl(const std::string &j) {
        static const char key[] = "\"browser_download_url\"";
        for (size_t p = j.find(key); p != std::string::npos; p = j.find(key, p + 1)) {
            size_t q = j.find('"', p + sizeof(key) - 1 + 1);   // past the ':'
            if (q == std::string::npos) break;
            const size_t end = j.find('"', q + 1);
            if (end == std::string::npos) break;
            const std::string url = j.substr(q + 1, end - q - 1);
            if (url.size() > 7 && url.compare(url.size() - 7, 7, "-sd.zip") == 0) return url;
        }
        return {};
    }

    bool Plan(const std::string &url, const std::string &root, std::vector<Entry> &out,
              Progress &pr, std::string &err) {
        pr.stage = 0;
        pr.file = pr.files = 0;
        out.clear();
        CURL *c = curl_easy_init();
        if (!c) { err = "Out of memory."; return false; }
        std::vector<Entry> all;
        const bool ok = ReadIndex(c, url, all, err);
        curl_easy_cleanup(c);
        if (!ok) return false;
        pr.files = (int)all.size();
        for (const Entry &e : all) {
            pr.file++;
            if (pr.cancel.load()) { err = "Cancelled"; return false; }
            if (e.name.empty() || e.name.back() == '/') continue;   // a folder
            if (!SafeName(e.name)) { err = "The release has a file outside the card: " + e.name; return false; }
            if (Starts(e.name, "switch/sInstaller/")) continue;
            const std::string dst = root + "/" + e.name;
            struct stat st {};
            const bool have = stat(dst.c_str(), &st) == 0;
            if (have && UserData(e.name)) continue;
            uint32_t crc = 0;
            if (have && (uint64_t)st.st_size == e.usize && FileCrc(dst, crc) && crc == e.crc) continue;
            if (e.method != 0 && e.method != 8) { err = "The release zip uses a compression sLaunch can't read."; return false; }
            out.push_back(e);
        }
        return true;
    }

    bool Apply(const std::string &url, const std::string &root, const std::vector<Entry> &files,
               Progress &pr, std::string &err) {
        pr.stage = 1;
        pr.done = 0;
        uint64_t total = 0;
        for (const Entry &e : files) total += e.csize;
        pr.total = total;
        pr.files = (int)files.size();
        pr.file = 0;
        CURL *c = curl_easy_init();
        if (!c) { err = "Out of memory."; return false; }
        std::vector<std::string> made;
        for (const Entry &e : files) {
            pr.file++;
            const std::string tmp = root + "/" + e.name + ".ota";
            if (!GetOne(c, url, tmp, e, pr, err)) {
                curl_easy_cleanup(c);
                for (const std::string &m : made) remove(m.c_str());
                return false;
            }
            made.push_back(tmp);
        }
        curl_easy_cleanup(c);

        pr.stage = 2;
        for (int pass = 0; pass < 2; pass++)
            for (const Entry &e : files) {
                if (Boot(e.name) != (pass == 1)) continue;
                const std::string dst = root + "/" + e.name, tmp = dst + ".ota";
                remove(dst.c_str());                   // FAT rename will not overwrite
                if (rename(tmp.c_str(), dst.c_str()) != 0) { err = "Could not replace " + e.name + "."; return false; }
            }
        return true;
    }

}
