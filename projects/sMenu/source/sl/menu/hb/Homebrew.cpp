#include <sl/menu/hb/Homebrew.hpp>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <dirent.h>
#include <sys/stat.h>
#include <algorithm>
#include <unordered_map>
#include <mutex>

namespace sl::menu::hb {

    namespace {
        // NRO header: "NRO0" magic at 0x10, total NRO size (u32) at 0x18. Right
        // after the NRO (at file offset == size) sits an optional asset blob:
        //   struct AssetHeader { u32 magic("ASET"); u32 version;
        //                        AssetSection icon, nacp, romfs; };
        //   struct AssetSection { u64 offset; u64 size; };  // offsets from ASET start
        struct AssetSection { u64 offset; u64 size; };
        struct AssetHeader {
            u32 magic;
            u32 version;
            AssetSection icon;
            AssetSection nacp;
            AssetSection romfs;
        };
        constexpr u32 NroMagic = 0x304F524E; // "NRO0"
        constexpr u32 AsetMagic = 0x54455341; // "ASET"

        // Small FNV-1a hash of the path -> stable icon cache key.
        u64 HashPath(const std::string &s) {
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

        // Pull the display name (NACP) + extract the icon JPEG for one NRO.
        void ReadMeta(HbEntry &e) {
            FILE *fp = fopen(e.path.c_str(), "rb");
            if (!fp) return;

            u32 magic = 0, nro_size = 0;
            if (fseek(fp, 0x10, SEEK_SET) != 0 || fread(&magic, 4, 1, fp) != 1 ||
                magic != NroMagic ||
                fseek(fp, 0x18, SEEK_SET) != 0 || fread(&nro_size, 4, 1, fp) != 1) {
                fclose(fp); return;
            }

            AssetHeader ah = {};
            if (fseek(fp, nro_size, SEEK_SET) != 0 || fread(&ah, sizeof(ah), 1, fp) != 1 ||
                ah.magic != AsetMagic) {
                fclose(fp); return;   // no assets -> keep the file-base name
            }

            // NACP name: American English (index 0), else the first non-empty.
            if (ah.nacp.size >= sizeof(NacpStruct)) {
                static NacpStruct nacp;   // 0x4000 bytes - keep off the stack
                if (fseek(fp, nro_size + ah.nacp.offset, SEEK_SET) == 0 &&
                    fread(&nacp, sizeof(nacp), 1, fp) == 1) {
                    const NacpLanguageEntry *pick = nullptr;
                    if (nacp.lang[0].name[0]) pick = &nacp.lang[0];
                    else for (int i = 1; i < 16; i++)
                        if (nacp.lang[i].name[0]) { pick = &nacp.lang[i]; break; }
                    if (pick) {
                        char nm[0x201] = {};
                        memcpy(nm, pick->name, sizeof(pick->name));
                        if (nm[0]) e.name = nm;
                    }
                }
            }

            // Icon: extract the JPEG to the cache so the UI can load it.
            if (ah.icon.size > 0 && ah.icon.size < 512 * 1024) {
                e.icon_key = HashPath(e.path);
                char out[96];
                snprintf(out, sizeof(out), "sdmc:/slaunch/%s/%016llX.jpg",
                         IconDir, (unsigned long long)e.icon_key);
                struct stat st;
                if (stat(out, &st) != 0 || (u64)st.st_size != ah.icon.size) {
                    std::vector<u8> buf(ah.icon.size);
                    if (fseek(fp, nro_size + ah.icon.offset, SEEK_SET) == 0 &&
                        fread(buf.data(), 1, buf.size(), fp) == buf.size()) {
                        FILE *of = fopen(out, "wb");
                        if (of) { fwrite(buf.data(), 1, buf.size(), of); fclose(of); }
                    }
                }
            }
            fclose(fp);
        }

        // Manifest: one line per .nro we've already parsed, so we never re-read an
        // NRO we've seen before. Keyed by path only - we deliberately do NOT check
        // whether the file changed. Its icon (cache/hbicons/<key>.jpg) and name are
        // cosmetic; if a .nro is replaced, the stale name/icon is harmless and gets
        // refreshed if the cache is cleared. Line format: key\tpath\tname
        constexpr const char *ManifestPath = "sdmc:/slaunch/cache/hb_manifest.txt";

        // Serialises manifest read-modify-write: Scan() (browser thread) and
        // Resolve() (pin-resolve thread) can run at once and must not clobber it.
        std::mutex g_manifest_mutex;

        struct CacheRec { u64 icon_key; std::string name; };

        void LoadManifest(std::unordered_map<std::string, CacheRec> &m) {
            FILE *fp = fopen(ManifestPath, "r");
            if (!fp) return;
            char line[FS_MAX_PATH + 320];
            while (fgets(line, sizeof(line), fp)) {
                line[strcspn(line, "\r\n")] = '\0';
                char *p = line, *end = nullptr;
                u64 key = strtoull(p, &end, 10); if (*end != '\t') continue; p = end + 1;
                char *tab = strchr(p, '\t'); if (!tab) continue;  // path \t name
                *tab = '\0';
                if (p[0]) m[p] = CacheRec{ key, std::string(tab + 1) };
            }
            fclose(fp);
        }

        void SaveManifest(const std::unordered_map<std::string, CacheRec> &m) {
            FILE *fp = fopen(ManifestPath, "w");
            if (!fp) return;
            for (auto &kv : m) {
                // Names never contain tab/newline in practice; guard anyway.
                std::string nm = kv.second.name;
                for (char &c : nm) if (c == '\t' || c == '\n' || c == '\r') c = ' ';
                fprintf(fp, "%llu\t%s\t%s\n",
                        (unsigned long long)kv.second.icon_key, kv.first.c_str(), nm.c_str());
            }
            fclose(fp);
        }

        void ScanDir(const std::string &dir, std::vector<HbEntry> &out, int depth,
                     std::unordered_map<std::string, CacheRec> &old_m,
                     std::unordered_map<std::string, CacheRec> &new_m, bool &changed) {
            if (depth > 6) return;   // guard against symlink loops / deep trees
            DIR *d = opendir(dir.c_str());
            if (!d) return;
            struct dirent *ent;
            while ((ent = readdir(d)) != nullptr) {
                if (ent->d_name[0] == '.') continue;
                std::string p = dir + "/" + ent->d_name;
                if (ent->d_type == DT_DIR) {
                    ScanDir(p, out, depth + 1, old_m, new_m, changed);
                } else {
                    size_t n = strlen(ent->d_name);
                    if (n <= 4 || strcasecmp(ent->d_name + n - 4, ".nro") != 0) continue;

                    HbEntry e;
                    e.path = p;
                    auto it = old_m.find(p);
                    if (it != old_m.end()) {
                        e.name     = it->second.name;   // seen before: from manifest, no read
                        e.icon_key = it->second.icon_key;
                    } else {
                        // First time we've seen this .nro: parse NACP + extract icon.
                        e.name = BaseName(p);
                        ReadMeta(e);
                        changed = true;
                    }
                    new_m[p] = CacheRec{ e.icon_key, e.name };
                    out.push_back(std::move(e));
                }
            }
            closedir(d);
        }
    }

    HbEntry ReadOne(const std::string &path) {
        mkdir("sdmc:/slaunch", 0777);
        mkdir("sdmc:/slaunch/cache", 0777);
        mkdir("sdmc:/slaunch/cache/hbicons", 0777);
        HbEntry e;
        e.path = path;
        e.name = BaseName(path);
        ReadMeta(e);
        return e;
    }

    void Resolve(std::vector<HbEntry> &entries) {
        mkdir("sdmc:/slaunch", 0777);
        mkdir("sdmc:/slaunch/cache", 0777);
        mkdir("sdmc:/slaunch/cache/hbicons", 0777);

        std::lock_guard<std::mutex> lk(g_manifest_mutex);
        std::unordered_map<std::string, CacheRec> m;
        LoadManifest(m);
        bool changed = false;
        for (auto &e : entries) {
            auto it = m.find(e.path);
            if (it != m.end()) {
                e.name     = it->second.name;      // seen before: from manifest, no read
                e.icon_key = it->second.icon_key;
            } else {
                if (e.name.empty()) e.name = BaseName(e.path);
                ReadMeta(e);                        // first time: parse + extract icon
                m[e.path] = CacheRec{ e.icon_key, e.name };
                changed = true;
            }
        }
        if (changed) SaveManifest(m);   // merge-only (never prunes other entries)
    }

    std::vector<HbEntry> Scan() {
        mkdir("sdmc:/slaunch", 0777);
        mkdir("sdmc:/slaunch/cache", 0777);
        mkdir("sdmc:/slaunch/cache/hbicons", 0777);

        // Reuse manifest names/icons for any .nro we've seen before (icons already
        // on disk); parse only ones new to the manifest. Rewrite only if it changed.
        std::lock_guard<std::mutex> lk(g_manifest_mutex);
        std::unordered_map<std::string, CacheRec> old_m, new_m;
        LoadManifest(old_m);

        std::vector<HbEntry> out;
        bool changed = false;
        ScanDir("sdmc:/switch", out, 0, old_m, new_m, changed);

        if (changed || new_m.size() != old_m.size())   // adds, edits, or removals
            SaveManifest(new_m);

        std::sort(out.begin(), out.end(), [](const HbEntry &a, const HbEntry &b) {
            return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
        });
        return out;
    }

} // namespace sl::menu::hb
