#include <sl/menu/ui/Theme.hpp>
#include <cstdio>
#include <cstring>
#include <cstdlib>
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

        // Default custom = AMOLED base with a cyan accent, gradient background.
        m_custom = { "Custom",
            C(0, 0, 0), C(0, 0, 0), C(240, 240, 240), C(90, 200, 220),
            C(120, 120, 120), C(160, 220, 235), "" };
    }

    const Theme &ThemeManager::At(int i) const {
        if (i == CustomThemeIndex) return m_custom;
        if (i < 0) i = 0;
        if (i >= BuiltinThemeCount) i = BuiltinThemeCount - 1;
        return m_builtin[i];
    }

    void ThemeManager::Select(int i) {
        if (i < 0) i = 0;
        if (i >= TotalThemeCount) i = TotalThemeCount - 1;
        m_current = i;
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
        if (!g_sd_ok) return; // no SD -> defaults only

        FILE *fp = fopen(ThemeCfg, "r");
        if (!fp) return;

        char line[160];
        while (fgets(line, sizeof(line), fp)) {
            line[strcspn(line, "\r\n")] = '\0';
            char *eq = strchr(line, '=');
            if (!eq) continue;
            *eq = '\0';
            const char *key = line;
            const char *val = eq + 1;

            if      (strcmp(key, "current") == 0)        Select(atoi(val));
            else if (strcmp(key, "custom_bg_top") == 0)  ParseColor(val, m_custom.bg_top);
            else if (strcmp(key, "custom_bg_bot") == 0)  ParseColor(val, m_custom.bg_bottom);
            else if (strcmp(key, "custom_fg") == 0)      ParseColor(val, m_custom.fg);
            else if (strcmp(key, "custom_accent") == 0)  ParseColor(val, m_custom.accent);
            else if (strcmp(key, "custom_dim") == 0)     ParseColor(val, m_custom.dim);
            else if (strcmp(key, "custom_title") == 0)   ParseColor(val, m_custom.title);
            else if (strcmp(key, "custom_wallpaper") == 0) {
                strncpy(m_custom.wallpaper, val, sizeof(m_custom.wallpaper) - 1);
                m_custom.wallpaper[sizeof(m_custom.wallpaper) - 1] = '\0';
            }
        }
        fclose(fp);
    }

    void ThemeManager::Save() const {
        if (!g_sd_ok) return;
        mkdir("sdmc:/slaunch", 0777);
        mkdir(ConfigDir, 0777);

        FILE *fp = fopen(ThemeCfg, "w");
        if (!fp) return;
        fprintf(fp, "current=%d\n", m_current);
        WriteColor(fp, "custom_bg_top", m_custom.bg_top);
        WriteColor(fp, "custom_bg_bot", m_custom.bg_bottom);
        WriteColor(fp, "custom_fg",     m_custom.fg);
        WriteColor(fp, "custom_accent", m_custom.accent);
        WriteColor(fp, "custom_dim",    m_custom.dim);
        WriteColor(fp, "custom_title",  m_custom.title);
        fprintf(fp, "custom_wallpaper=%s\n", m_custom.wallpaper);
        fclose(fp);
    }

} // namespace sl::menu::ui
