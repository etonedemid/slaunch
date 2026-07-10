#include <sl/menu/hb/Homebrew.hpp>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <algorithm>

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

        void ScanDir(const std::string &dir, std::vector<HbEntry> &out, int depth) {
            if (depth > 6) return;   // guard against symlink loops / deep trees
            DIR *d = opendir(dir.c_str());
            if (!d) return;
            struct dirent *ent;
            while ((ent = readdir(d)) != nullptr) {
                if (ent->d_name[0] == '.') continue;
                std::string p = dir + "/" + ent->d_name;
                if (ent->d_type == DT_DIR) {
                    ScanDir(p, out, depth + 1);
                } else {
                    size_t n = strlen(ent->d_name);
                    if (n > 4 && strcasecmp(ent->d_name + n - 4, ".nro") == 0) {
                        HbEntry e;
                        e.path = p;
                        e.name = BaseName(p);
                        ReadMeta(e);
                        out.push_back(std::move(e));
                    }
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

    std::vector<HbEntry> Scan() {
        mkdir("sdmc:/slaunch", 0777);
        mkdir("sdmc:/slaunch/cache", 0777);
        mkdir("sdmc:/slaunch/cache/hbicons", 0777);

        std::vector<HbEntry> out;
        ScanDir("sdmc:/switch", out, 0);
        std::sort(out.begin(), out.end(), [](const HbEntry &a, const HbEntry &b) {
            return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
        });
        return out;
    }

} // namespace sl::menu::hb
