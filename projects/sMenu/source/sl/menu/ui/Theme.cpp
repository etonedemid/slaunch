#include <sl/menu/ui/Theme.hpp>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <sys/stat.h>

namespace sl::menu::ui {

    bool g_sd_ok = false;

    static constexpr const char *ConfigDir = "sdmc:/slaunch/config";
    static constexpr const char *ThemeCfg  = "sdmc:/slaunch/config/theme.cfg";

    static constexpr SDL_Color C(Uint8 r, Uint8 g, Uint8 b) { return SDL_Color{ r, g, b, 255 }; }

    void ThemeManager::InitBuiltins() {
        // Dark Blue - deep navy gradient, cyan accents.
        m_builtin[0] = { "Dark Blue",
            C(10, 18, 40), C(4, 8, 20), C(230, 235, 245), C(90, 170, 255),
            C(120, 135, 165), C(150, 195, 255), "" };
        // Midnight Purple - near-black to violet, magenta accents.
        m_builtin[1] = { "Midnight Purple",
            C(24, 12, 36), C(8, 4, 14), C(235, 230, 245), C(200, 120, 255),
            C(140, 120, 160), C(210, 160, 255), "" };
        // AMOLED - pure black, white accents.
        m_builtin[2] = { "AMOLED",
            C(0, 0, 0), C(0, 0, 0), C(240, 240, 240), C(255, 255, 255),
            C(120, 120, 120), C(255, 255, 255), "" };
        // Warm Dark - dark warm brown, amber accents; loads a wallpaper if present.
        m_builtin[3] = { "Warm Dark",
            C(40, 26, 16), C(16, 10, 6), C(245, 235, 220), C(255, 190, 90),
            C(170, 140, 110), C(255, 160, 90), "sdmc:/slaunch/warm.jpg" };
        // Forest - green gradient, bright lime accents; loads a wallpaper if present.
        m_builtin[4] = { "Forest",
            C(30, 70, 34), C(10, 30, 14), C(240, 245, 235), C(180, 240, 120),
            C(150, 180, 150), C(235, 250, 220), "sdmc:/slaunch/forest.jpg" };

        // No custom themes exist until the user creates one.
        m_custom.clear();
    }

    const Theme &ThemeManager::At(int i) const {
        if (i >= BuiltinThemeCount && i < Count())
            return m_custom[i - BuiltinThemeCount];
        if (i < 0) i = 0;
        if (i >= BuiltinThemeCount) i = BuiltinThemeCount - 1;
        return m_builtin[i];
    }

    void ThemeManager::Select(int i) {
        if (i < 0) i = 0;
        if (i >= Count()) i = Count() - 1;
        m_current = i;
    }

    int ThemeManager::AddCustom() {
        Theme t = At(m_current); // start from the current look
        snprintf(t.name, sizeof(t.name), "Custom %d", (int)m_custom.size() + 1);
        m_custom.push_back(t);
        return BuiltinThemeCount + (int)m_custom.size() - 1;
    }

    void ThemeManager::DeleteCustom(int i) {
        if (!IsCustom(i)) return;
        m_custom.erase(m_custom.begin() + (i - BuiltinThemeCount));
        if (m_current >= Count()) m_current = Count() - 1;
        if (m_current < 0) m_current = 0;
    }

    // ---- persistence --------------------------------------------------------
    static void WriteColor(FILE *fp, const char *key, SDL_Color c) {
        fprintf(fp, "%s=%u,%u,%u\n", key, c.r, c.g, c.b);
    }
    static bool ParseColor(const char *val, SDL_Color &out) {
        int r = 0, g = 0, b = 0;
        if (sscanf(val, "%d,%d,%d", &r, &g, &b) != 3) return false;
        out = SDL_Color{ (Uint8)r, (Uint8)g, (Uint8)b, 255 };
        return true;
    }

    void ThemeManager::Load() {
        InitBuiltins();
        m_custom.clear();
        if (!g_sd_ok) return; // no SD -> defaults only

        FILE *fp = fopen(ThemeCfg, "r");
        if (!fp) return;

        int want_current = m_current;
        char line[160];
        while (fgets(line, sizeof(line), fp)) {
            line[strcspn(line, "\r\n")] = '\0';
            char *eq = strchr(line, '=');
            if (!eq) continue;
            *eq = '\0';
            const char *key = line;
            const char *val = eq + 1;

            if (strcmp(key, "current") == 0) { want_current = atoi(val); continue; }
            if (strcmp(key, "custom_count") == 0) {
                int n = atoi(val);
                if (n < 0) n = 0; if (n > 64) n = 64;
                m_custom.assign(n, m_builtin[2]); // AMOLED base
                for (auto &c : m_custom) { strncpy(c.name, "Custom", sizeof(c.name)); c.wallpaper[0] = '\0'; }
                continue;
            }
            // Per-custom keys: cN_field
            if (key[0] == 'c' && isdigit((unsigned char)key[1])) {
                int idx = atoi(key + 1);
                const char *us = strchr(key, '_');
                if (!us || idx < 0 || idx >= (int)m_custom.size()) continue;
                const char *field = us + 1;
                Theme &c = m_custom[idx];
                if      (!strcmp(field, "name"))   { strncpy(c.name, val, sizeof(c.name) - 1); c.name[sizeof(c.name) - 1] = '\0'; }
                else if (!strcmp(field, "bg_top")) ParseColor(val, c.bg_top);
                else if (!strcmp(field, "bg_bot")) ParseColor(val, c.bg_bottom);
                else if (!strcmp(field, "fg"))     ParseColor(val, c.fg);
                else if (!strcmp(field, "accent")) ParseColor(val, c.accent);
                else if (!strcmp(field, "dim"))    ParseColor(val, c.dim);
                else if (!strcmp(field, "title"))  ParseColor(val, c.title);
                else if (!strcmp(field, "wallpaper")) { strncpy(c.wallpaper, val, sizeof(c.wallpaper) - 1); c.wallpaper[sizeof(c.wallpaper) - 1] = '\0'; }
            }
        }
        fclose(fp);
        Select(want_current);
    }

    void ThemeManager::Save() const {
        if (!g_sd_ok) return;
        mkdir("sdmc:/slaunch", 0777);
        mkdir(ConfigDir, 0777);

        FILE *fp = fopen(ThemeCfg, "w");
        if (!fp) return;
        fprintf(fp, "current=%d\n", m_current);
        fprintf(fp, "custom_count=%d\n", (int)m_custom.size());
        for (int i = 0; i < (int)m_custom.size(); i++) {
            const Theme &c = m_custom[i];
            char k[32];
            fprintf(fp, "c%d_name=%s\n", i, c.name);
            snprintf(k, sizeof(k), "c%d_bg_top", i); WriteColor(fp, k, c.bg_top);
            snprintf(k, sizeof(k), "c%d_bg_bot", i); WriteColor(fp, k, c.bg_bottom);
            snprintf(k, sizeof(k), "c%d_fg", i);     WriteColor(fp, k, c.fg);
            snprintf(k, sizeof(k), "c%d_accent", i); WriteColor(fp, k, c.accent);
            snprintf(k, sizeof(k), "c%d_dim", i);    WriteColor(fp, k, c.dim);
            snprintf(k, sizeof(k), "c%d_title", i);  WriteColor(fp, k, c.title);
            fprintf(fp, "c%d_wallpaper=%s\n", i, c.wallpaper);
        }
        fclose(fp);
    }

} // namespace sl::menu::ui
