#include <sl/menu/ui/Locale.hpp>
#include <switch.h>
#include <unordered_map>
#include <string>
#include <cstdio>
#include <cstring>
#include <cctype>

namespace sl::menu::ui {

    namespace {
        std::unordered_map<std::string, std::string> g_map;
        char g_code[8] = "en";      // two-letter language code
        char g_full[12] = "en";     // full code as reported ("en-US", "pt-BR")

        bool LoadFile(const char *code) {
            char path[80];
            snprintf(path, sizeof(path), "sdmc:/slaunch/lang/%s.txt", code);
            FILE *fp = fopen(path, "r");
            if (!fp) return false;
            char line[512];
            while (fgets(line, sizeof(line), fp)) {
                line[strcspn(line, "\r\n")] = '\0';
                if (line[0] == '\0' || line[0] == '#') continue; // blank / comment
                char *eq = strchr(line, '=');                    // first '=' splits
                if (!eq) continue;
                *eq = '\0';
                g_map[line] = eq + 1;                            // English -> localized
            }
            fclose(fp);
            return true;
        }
    }

    void LocaleInit() {
        g_map.clear();
        strcpy(g_code, "en");
        strcpy(g_full, "en");

        // The system language code is an ASCII string ("en-US", "fr-FR", ...)
        // packed into a u64. Split off the two-letter language part.
        u64 lc = 0;
        if (R_SUCCEEDED(setGetSystemLanguage(&lc))) {
            char buf[9] = {};
            memcpy(buf, &lc, 8);
            if (isalpha((unsigned char)buf[0])) {
                strncpy(g_full, buf, sizeof(g_full) - 1);
                g_full[sizeof(g_full) - 1] = '\0';
                g_code[0] = (char)tolower((unsigned char)buf[0]);
                g_code[1] = (buf[1] && buf[1] != '-')
                                ? (char)tolower((unsigned char)buf[1]) : '\0';
                g_code[2] = '\0';
            }
        }

        if (strcmp(g_code, "en") == 0) return; // English is the built-in fallback

        // Prefer an exact regional file (pt-BR.txt), else the language file (pt.txt).
        if (!LoadFile(g_full)) LoadFile(g_code);
    }

    const char *T(const char *english) {
        if (!english) return "";
        if (g_map.empty()) return english;
        auto it = g_map.find(english);
        return it != g_map.end() ? it->second.c_str() : english;
    }

    const char *LocaleCode() { return g_code; }

} // namespace sl::menu::ui
