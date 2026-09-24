#include <sl/menu/cfg/UserCfg.hpp>
#include <sys/stat.h>
#include <dirent.h>
#include <cstdio>
#include <cstring>

namespace sl::menu::cfg {

    namespace {

        constexpr const char *kRoot     = "sdmc:/slaunch";
        constexpr const char *kConfig   = "sdmc:/slaunch/config";
        constexpr const char *kUsers    = "sdmc:/slaunch/config/users";
        // Written once the first account has taken over the console-wide files,
        // so the second account starts from defaults instead of inheriting the
        // first one's menu.
        constexpr const char *kMigrated = "sdmc:/slaunch/config/users/migrated.txt";

        AccountUid  g_uid = {};
        std::string g_dir;      // resolved once by SetUser

        // What a person owns, as opposed to what the console owns. Also the
        // migration list: these are exactly the files that used to sit in
        // sdmc:/slaunch/config and now belong to an account.
        const char *kOwned[] = {
            "settings.txt",       // menu options (layout, welcome, anti-alias...)
            "theme.cfg",          // selected theme + any custom ones
            "font.cfg",           // selected font
            "icon_pack.txt",      // selected icon pack
            "flow.txt",           // Flow layout preset + tuning
            "tiles.txt",          // per-entry tile size/colour on the wall
            "sysentries.txt",     // which system entries are shown
            "order.txt",          // manual entry order
            "names.txt",          // renamed entries
            "sort.txt",           // sort mode
            "favourites.txt",
            "hb_favourites.txt",
            "homebrew.txt",       // pinned .nro
            "shortcuts.txt",      // launcher shortcuts (.nro + argv)
            "music.txt",          // menu music: on/off, volume, track
            "widget_pos.txt",     // where the floating widgets sit
            "widget_enabled.txt", // which of them are on
            "setup_done",         // this account has been through first-run setup
        };

        bool Exists(const std::string &path) {
            struct stat st;
            return stat(path.c_str(), &st) == 0;
        }

        // Byte copy. Used only by the migration, so a plain 4K loop is plenty.
        bool CopyFile(const std::string &from, const std::string &to) {
            FILE *in = fopen(from.c_str(), "rb");
            if (!in) return false;
            FILE *out = fopen(to.c_str(), "wb");
            if (!out) { fclose(in); return false; }
            char buf[4096];
            size_t n;
            bool ok = true;
            while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
                if (fwrite(buf, 1, n, out) != n) { ok = false; break; }
            fclose(in);
            fclose(out);
            if (!ok) remove(to.c_str());
            return ok;
        }

    } // namespace

    void SetUser(AccountUid uid) {
        g_uid = uid;
        if (accountUidIsValid(&g_uid)) {
            char id[64];
            snprintf(id, sizeof(id), "%s/%016llX%016llX", kUsers,
                     (unsigned long long)g_uid.uid[0],
                     (unsigned long long)g_uid.uid[1]);
            g_dir = id;
        } else {
            // No account to key on. One shared folder rather than the old
            // console-wide files, so a setting written here is still read back
            // from the same place next boot.
            g_dir = std::string(kUsers) + "/default";
        }
    }

    AccountUid User() { return g_uid; }

    std::string Dir() {
        if (g_dir.empty()) SetUser(g_uid);   // never used before SetUser, but be safe
        return g_dir;
    }

    std::string Path(const char *filename) {
        return Dir() + "/" + filename;
    }

    void EnsureDir() {
        mkdir(kRoot,   0777);
        mkdir(kConfig, 0777);
        mkdir(kUsers,  0777);
        mkdir(Dir().c_str(), 0777);
    }

    void EnsureSubdir(const char *sub) {
        EnsureDir();
        mkdir((Dir() + "/" + sub).c_str(), 0777);
    }

    void NoteNickname(const char *nick) {
        if (!nick || !nick[0]) return;
        const std::string path = Path("name.txt");
        // Only write when it would change something: this runs every boot and
        // the card is the slowest thing the menu touches.
        char have[64] = {};
        if (FILE *fp = fopen(path.c_str(), "r")) {
            if (fgets(have, sizeof(have), fp)) have[strcspn(have, "\r\n")] = '\0';
            fclose(fp);
            if (strcmp(have, nick) == 0) return;
        }
        EnsureDir();
        if (FILE *fp = fopen(path.c_str(), "w")) {
            fprintf(fp, "%s\n", nick);
            fclose(fp);
        }
    }

    void MigrateLegacy() {
        if (Exists(kMigrated)) return;      // an account already took them over

        EnsureDir();

        for (const char *name : kOwned) {
            const std::string dst = Path(name);
            if (Exists(dst)) continue;      // this account already has its own
            const std::string src = std::string(kConfig) + "/" + name;
            if (!Exists(src)) continue;
            CopyFile(src, dst);
        }

        // Widget options live one folder down, one file per script.
        if (DIR *d = opendir("sdmc:/slaunch/config/widgets")) {
            bool made = false;
            while (dirent *e = readdir(d)) {
                const size_t n = strlen(e->d_name);
                if (n < 5 || strcmp(e->d_name + n - 4, ".cfg") != 0) continue;
                if (!made) { EnsureSubdir("widgets"); made = true; }
                const std::string dst = Path("widgets") + "/" + e->d_name;
                if (Exists(dst)) continue;
                CopyFile(std::string("sdmc:/slaunch/config/widgets/") + e->d_name, dst);
            }
            closedir(d);
        }

        // The originals stay put - this is a copy, not a move, so an older
        // build (or a downgrade) still finds everything it wrote.
        if (FILE *fp = fopen(kMigrated, "w")) {
            fprintf(fp, "%s\n", Dir().c_str());
            fclose(fp);
        }
    }

} // namespace sl::menu::cfg
