#include <sl/menu/hb/Shortcuts.hpp>
#include <sl/menu/cfg/UserCfg.hpp>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <dirent.h>
#include <sys/stat.h>

namespace sl::menu::hb {

    namespace {

        // RetroArch's Switch build puts everything here. The second root is what
        // you get if you unzipped it under /switch like any other homebrew.
        constexpr const char *kRetroRoots[] = {
            "sdmc:/retroarch",
            "sdmc:/switch/retroarch",
        };

        // hbloader hands argv to the NRO as one string and splits it the way
        // hbmenu writes it, so every path goes in quoted. A path containing a
        // quote of its own would split wrong, and there is no escape in that
        // convention to fix it with - such a shortcut is dropped instead.
        bool Quotable(const std::string &s) {
            return !s.empty() && s.find('"') == std::string::npos;
        }
        std::string Quote(const std::string &s) { return "\"" + s + "\""; }

        // The same FNV-1a the NRO scan uses, so a key here can never collide
        // with one there by construction of the string it hashes.
        u64 Hash(const std::string &s) {
            u64 h = 1469598103934665603ULL;
            for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; }
            return h ? h : 1;
        }

        std::string BaseName(const std::string &path) {
            size_t slash = path.find_last_of('/');
            std::string b = (slash == std::string::npos) ? path : path.substr(slash + 1);
            size_t dot = b.find_last_of('.');
            if (dot != std::string::npos) b = b.substr(0, dot);
            return b;
        }

        // Paths we open ourselves need libnx's device prefix; RetroArch stores
        // them rooted at the card ("/retroarch/cores/..."). Paths we only pass
        // through to RetroArch are left exactly as it wrote them - it is the one
        // that has to open them, and a round trip through its own file browser
        // is the only thing guaranteed to work.
        std::string Local(const std::string &p) {
            if (p.empty() || p.compare(0, 5, "sdmc:") == 0) return p;
            return p[0] == '/' ? "sdmc:" + p : p;
        }

        bool Exists(const std::string &p) {
            struct stat st;
            return stat(p.c_str(), &st) == 0;
        }

        void Utf8Append(std::string &out, unsigned cp) {
            if (cp < 0x80) { out += (char)cp; return; }
            if (cp < 0x800) {
                out += (char)(0xC0 | (cp >> 6));
                out += (char)(0x80 | (cp & 0x3F));
                return;
            }
            out += (char)(0xE0 | (cp >> 12));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        }

        // The JSON string starting at j[s] (s points at the opening quote), with
        // escapes decoded. `end` comes back one past the closing quote.
        std::string Unescape(const std::string &j, size_t s, size_t *end) {
            std::string v;
            size_t i = s + 1;
            for (; i < j.size(); i++) {
                const char c = j[i];
                if (c == '"') break;
                if (c != '\\') { v += c; continue; }
                if (++i >= j.size()) break;
                switch (j[i]) {
                    case 'n': case 't': v += ' '; break;   // labels are one line
                    case 'r': case 'b': case 'f':  break;
                    case 'u': {
                        if (i + 4 >= j.size()) { i = j.size(); break; }
                        unsigned cp = (unsigned)strtoul(j.substr(i + 1, 4).c_str(),
                                                        nullptr, 16);
                        i += 4;
                        // A surrogate half on its own is not a character; a pair
                        // would be an emoji in a game title, which no playlist
                        // has. Either way it is not worth decoding.
                        Utf8Append(v, (cp >= 0xD800 && cp <= 0xDFFF) ? '?' : cp);
                        break;
                    }
                    default: v += j[i]; break;    // covers \" \\ \/
                }
            }
            if (end) *end = (i < j.size()) ? i + 1 : j.size();
            return v;
        }

        // First string value for `key` within [from, limit). The bound is what
        // stops a field missing from one record picking up the next record's.
        std::string Field(const std::string &j, const char *key,
                          size_t from, size_t limit) {
            const std::string pat = std::string("\"") + key + "\"";
            const size_t p = j.find(pat, from);
            if (p == std::string::npos || p >= limit) return std::string();
            const size_t colon = j.find(':', p + pat.size());
            if (colon == std::string::npos || colon >= limit) return std::string();
            const size_t s = j.find('"', colon);
            if (s == std::string::npos || s >= limit) return std::string();
            return Unescape(j, s, nullptr);
        }

        std::string ReadFile(const std::string &path) {
            FILE *fp = fopen(path.c_str(), "rb");
            if (!fp) return std::string();
            std::string out;
            char buf[8192];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) out.append(buf, n);
            fclose(fp);
            return out;
        }

        // RetroArch replaces these in a label before using it as a file name, so
        // a thumbnail is found by doing the same thing to the same label.
        std::string SanitizeLabel(const std::string &label) {
            static const char *bad = "&*/:`<>?\\|\"";
            std::string out = label;
            for (char &c : out) if (strchr(bad, c)) c = '_';
            return out;
        }

        // Boxart only. Titles and snaps are the other two folders RetroArch
        // fills, but trying each would be a stat per ROM per folder on a card
        // that may hold thousands - and the icon cache already treats a path
        // that will not load as a miss and draws the placeholder, at no cost
        // until the entry is actually on screen.
        std::string ThumbPath(const std::string &root, const std::string &system,
                              const std::string &label) {
            if (system.empty() || label.empty()) return std::string();
            return root + "/thumbnails/" + system + "/Named_Boxarts/" +
                   SanitizeLabel(label) + ".png";
        }

        // "DETECT" is RetroArch's "ask me later" - a playlist entry that has not
        // been assigned a core. We cannot guess one, so such an entry is skipped
        // rather than launched into nothing.
        bool CoreUsable(const std::string &c) {
            return !c.empty() && c != "DETECT";
        }

        void AddEntry(std::vector<Shortcut> &out, const std::string &root,
                      const std::string &system, const std::string &rom,
                      const std::string &label, const std::string &core) {
            if (rom.empty() || !CoreUsable(core)) return;
            const std::string nro = Local(core);
            if (!Quotable(nro) || !Quotable(rom)) return;

            Shortcut s;
            s.nro      = nro;
            s.argv     = Quote(nro) + " " + Quote(rom);
            // The protocol carries argv in a fixed 512-byte field. Truncating it
            // would hand RetroArch half a ROM path and an error screen, so a
            // shortcut that does not fit is left out of the list entirely.
            if (s.argv.size() >= 512) return;
            s.name     = label.empty() ? BaseName(rom) : label;
            s.category = system;
            s.icon_path = ThumbPath(root, system, s.name);
            s.icon_key  = Hash(rom);
            out.push_back(std::move(s));
        }

        // Modern .lpl: one JSON object with an "items" array. Records are walked
        // by brace depth rather than by searching for the next "path", so a
        // field missing from one entry cannot be filled in from the next.
        bool ParseJsonPlaylist(const std::string &j, const std::string &root,
                               const std::string &fallback_system,
                               std::vector<Shortcut> &out) {
            const size_t items = j.find("\"items\"");
            if (items == std::string::npos) return false;
            const size_t arr = j.find('[', items);
            if (arr == std::string::npos) return false;

            const std::string def_core = Field(j, "default_core_path", 0, items);

            size_t i = arr + 1;
            while (i < j.size()) {
                if (j[i] == ']') break;
                if (j[i] != '{') { i++; continue; }

                // Find this record's closing brace, ignoring braces and quotes
                // inside string values.
                size_t k = i, depth = 0;
                bool in_str = false;
                for (; k < j.size(); k++) {
                    const char c = j[k];
                    if (in_str) {
                        if (c == '\\') k++;
                        else if (c == '"') in_str = false;
                        continue;
                    }
                    if (c == '"') { in_str = true; continue; }
                    if (c == '{') depth++;
                    else if (c == '}' && --depth == 0) break;
                }
                if (k >= j.size()) break;

                const std::string rom   = Field(j, "path",      i, k);
                const std::string label = Field(j, "label",     i, k);
                std::string       core  = Field(j, "core_path", i, k);
                std::string       db    = Field(j, "db_name",   i, k);
                if (!CoreUsable(core)) core = def_core;
                if (db.size() > 4 && db.compare(db.size() - 4, 4, ".lpl") == 0)
                    db = db.substr(0, db.size() - 4);
                AddEntry(out, root, db.empty() ? fallback_system : db, rom, label, core);

                i = k + 1;
            }
            return true;
        }

        // Pre-1.7.6 .lpl: six plain lines per entry, no JSON at all. RetroArch
        // rewrites these the first time it touches the playlist, so a card can
        // easily be carrying both formats at once.
        void ParseLegacyPlaylist(const std::string &body, const std::string &root,
                                 const std::string &fallback_system,
                                 std::vector<Shortcut> &out) {
            // Process line-by-line without keeping the whole split vector in memory.
            const char *data = body.c_str();
            size_t pos = 0, len = body.size();
            std::string buf[6];
            int idx = 0;
            auto flush = [&]() {
                if (idx == 6) {
                    std::string db = buf[5];
                    if (db.size() > 4 && db.compare(db.size() - 4, 4, ".lpl") == 0)
                        db = db.substr(0, db.size() - 4);
                    AddEntry(out, root, db.empty() ? fallback_system : db,
                             buf[0], buf[1], buf[2]);
                    idx = 0;
                }
            };
            while (pos <= len) {
                size_t e = body.find('\n', pos);
                if (e == std::string::npos) e = len;
                std::string line(data + pos, e - pos);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (idx < 6) buf[idx++] = std::move(line);
                if (idx == 6) flush();
                if (e == len) break;
                pos = e + 1;
            }
            // Incomplete tail is ignored.
        }

        void ScanPlaylists(const std::string &root, std::vector<Shortcut> &out) {
            const std::string dir = root + "/playlists";
            DIR *d = opendir(dir.c_str());
            if (!d) return;
            while (struct dirent *e = readdir(d)) {
                if (e->d_name[0] == '.') continue;
                const size_t n = strlen(e->d_name);
                if (n <= 4 || strcasecmp(e->d_name + n - 4, ".lpl") != 0) continue;
                // RetroArch's own bookkeeping lists, not systems.
                if (strncasecmp(e->d_name, "content_", 8) == 0) continue;

                const std::string body = ReadFile(dir + "/" + e->d_name);
                if (body.empty()) continue;
                const std::string system(e->d_name, n - 4);
                if (!ParseJsonPlaylist(body, root, system, out))
                    ParseLegacyPlaylist(body, root, system, out);
            }
            closedir(d);
        }

        // shortcuts.txt: nro <tab> name <tab> argv <tab> category <tab> icon.
        // Every field after the first is optional, so the shortest useful line
        // is just a path - which is a pinned .nro by another name, and behaves
        // like one.
        void ScanFile(std::vector<Shortcut> &out) {
            FILE *fp = fopen(cfg::Path("shortcuts.txt").c_str(), "r");
            if (!fp) return;
            char line[1024];
            while (fgets(line, sizeof(line), fp)) {
                line[strcspn(line, "\r\n")] = '\0';
                if (!line[0] || line[0] == '#') continue;

                std::string f[5];
                int n = 0;
                for (char *p = line; n < 5; ) {
                    char *tab = strchr(p, '\t');
                    if (!tab) { f[n++] = p; break; }
                    *tab = '\0';
                    f[n++] = p;
                    p = tab + 1;
                }

                Shortcut s;
                s.nro = Local(f[0]);
                if (s.nro.empty() || !Quotable(s.nro)) continue;
                s.name      = f[1].empty() ? BaseName(s.nro) : f[1];
                s.argv      = f[2].empty() ? Quote(s.nro) : f[2];
                if (s.argv.size() >= 512) continue;
                s.category  = f[3];
                s.icon_path = Local(f[4]);
                s.icon_key  = Hash(s.argv);
                out.push_back(std::move(s));
            }
            fclose(fp);
        }

    } // namespace

    std::vector<Shortcut> ScanShortcuts() {
        std::vector<Shortcut> out;
        out.reserve(4096);
        for (const char *root : kRetroRoots)
            if (Exists(root)) ScanPlaylists(root, out);
        ScanFile(out);

        // Sorted by category, then name: the XMB grouping walks this list once
        // and takes the order it finds, so sorting here is what puts the columns
        // in a predictable order and each one in alphabetical order.
        std::sort(out.begin(), out.end(), [](const Shortcut &a, const Shortcut &b) {
            const int c = strcasecmp(a.category.c_str(), b.category.c_str());
            if (c != 0) return c < 0;
            return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
        });
        return out;
    }

} // namespace sl::menu::hb
