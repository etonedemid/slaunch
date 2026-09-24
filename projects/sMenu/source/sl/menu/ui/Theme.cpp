#include <sl/menu/ui/Theme.hpp>
#include <sl/menu/cfg/UserCfg.hpp>
#include <string>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <sys/stat.h>

namespace sl::menu::ui {

    bool g_sd_ok = false;

    // The theme a person picked, and any they built themselves, belong to
    // that person - so the file lives in their config folder. The wallpapers
    // and theme packs it points at stay shared under sdmc:/slaunch.
    static std::string ThemeCfgPath() { return cfg::Path("theme.cfg"); }

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

        // The built-ins get their background palette from their own accent and
        // title, so a fresh install looks the same as it did before the
        // backgrounds had colours of their own.
        for (auto &b : m_builtin) { b.fx_a = b.accent; b.fx_b = b.title; }

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
        // The applied theme, not m_current: browsing the Themes list previews
        // each entry live (Select() on every cursor move), so m_current is
        // often just whatever the cursor is passing over, not what's actually
        // in effect. Basing a new theme on that meant "New Theme" could copy
        // a theme you only glanced at on the way to the bottom of the list.
        Theme t = At(m_applied);
        snprintf(t.name, sizeof(t.name), "Custom %d", (int)m_custom.size() + 1);
        m_custom.push_back(t);
        return BuiltinThemeCount + (int)m_custom.size() - 1;
    }

    void ThemeManager::DeleteCustom(int i) {
        if (!IsCustom(i)) return;
        m_custom.erase(m_custom.begin() + (i - BuiltinThemeCount));
        // Erasing shifts every later custom theme's index down by one, so
        // just clamping left m_current silently pointing at whichever theme
        // slid into its old slot instead of the one it was on.
        auto reindex = [&](int &idx) {
            if (idx == i)      idx -= 1; // it was the one just deleted
            else if (idx > i)  idx -= 1; // it shifted down
            if (idx >= Count()) idx = Count() - 1;
            if (idx < 0)        idx = 0;
        };
        reindex(m_current);
        reindex(m_applied);
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

        FILE *fp = fopen(ThemeCfgPath().c_str(), "r");
        if (!fp) return;

        int want_current = m_current;
        bool seen_fx_a = false, seen_fx_b = false;
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
                else if (!strcmp(field, "icon_bg")) ParseColor(val, c.icon_bg);
                else if (!strcmp(field, "icon_fg")) ParseColor(val, c.icon_fg);
                else if (!strcmp(field, "fx_a")) { ParseColor(val, c.fx_a); seen_fx_a = true; }
                else if (!strcmp(field, "fx_b")) { ParseColor(val, c.fx_b); seen_fx_b = true; }
                // One line per background: fx<style>=lines,thickness,amplitude,
                // seed,layers,y,x,cam
                else if (!strncmp(field, "fx", 2) && isdigit((unsigned char)field[2])) {
                    const int st = atoi(field + 2);
                    if (st >= 0 && st < (int)BackgroundStyle_Count) {
                        Theme::FxParams &f = c.fx[st];
                        sscanf(val, "%d,%d,%d,%d,%d,%d,%d,%d",
                               &f.lines, &f.thickness, &f.amplitude, &f.seed,
                               &f.layers, &f.y, &f.x, &f.cam);
                    }
                }
                // Pre-per-background files: one shared set, which was the
                // ribbon's. Land it on both ribbons and leave the rest at the
                // defaults that suit them.
                else if (!strcmp(field, "fx_x")) {
                    const int v = atoi(val);
                    c.fx[BackgroundStyle_Ribbon].x   = (v < 0) ? 0 : (v > 100 ? 100 : v);
                    c.fx[BackgroundStyle_Grid].x     = c.fx[BackgroundStyle_Ribbon].x;
                }
                else if (!strcmp(field, "icon_bg_alpha")) {
                    int a = atoi(val);
                    c.icon_bg_alpha = (a < 0) ? 0 : (a > 255 ? 255 : a);
                }
                else if (!strcmp(field, "wallpaper")) { strncpy(c.wallpaper, val, sizeof(c.wallpaper) - 1); c.wallpaper[sizeof(c.wallpaper) - 1] = '\0'; }
                else if (!strcmp(field, "background_style")) c.background_style = atoi(val);
                else if (!strcmp(field, "ribbon_lines")) {
                    c.fx[BackgroundStyle_Ribbon].lines = atoi(val);
                    c.fx[BackgroundStyle_RibbonHD].lines = c.fx[BackgroundStyle_Ribbon].lines;
                }
                else if (!strcmp(field, "ribbon_thickness")) {
                    c.fx[BackgroundStyle_Ribbon].thickness = atoi(val);
                    c.fx[BackgroundStyle_RibbonHD].thickness = c.fx[BackgroundStyle_Ribbon].thickness;
                }
                else if (!strcmp(field, "ribbon_amplitude")) {
                    c.fx[BackgroundStyle_Ribbon].amplitude = atoi(val);
                    c.fx[BackgroundStyle_RibbonHD].amplitude = c.fx[BackgroundStyle_Ribbon].amplitude;
                }
                else if (!strcmp(field, "ribbon_seed")) {
                    c.fx[BackgroundStyle_Ribbon].seed = atoi(val);
                    c.fx[BackgroundStyle_RibbonHD].seed = c.fx[BackgroundStyle_Ribbon].seed;
                }
                else if (!strcmp(field, "ribbon_layers")) {
                    c.fx[BackgroundStyle_Ribbon].layers = atoi(val);
                    c.fx[BackgroundStyle_RibbonHD].layers = c.fx[BackgroundStyle_Ribbon].layers;
                }
                else if (!strcmp(field, "ribbon_y_center")) {
                    c.fx[BackgroundStyle_Ribbon].y = atoi(val);
                    c.fx[BackgroundStyle_RibbonHD].y = c.fx[BackgroundStyle_Ribbon].y;
                }
                else if (!strcmp(field, "wallpaper_effect")) {
                    // Backward compat: old single enum -> new toggles.
                    int eff = atoi(val);
                    if (eff == 1) { c.wallpaper_dim = 1; }
                    else if (eff == 2) { c.wallpaper_blur = 1; c.wallpaper_dim = 0; }
                    else if (eff == 3) { c.wallpaper_snow = 1; }
                    // eff==0 -> all off (default)
                }
                else if (!strcmp(field, "wallpaper_dim"))           c.wallpaper_dim           = std::max(0, std::min(1, atoi(val)));
                else if (!strcmp(field, "wallpaper_blur"))          c.wallpaper_blur          = std::max(0, std::min(1, atoi(val)));
                else if (!strcmp(field, "wallpaper_blur_radius"))   c.wallpaper_blur_radius   = std::max(2, std::min(32, atoi(val)));
                else if (!strcmp(field, "wallpaper_snow"))          c.wallpaper_snow          = std::max(0, std::min(1, atoi(val)));
            }
        }
        fclose(fp);
        // Older theme.cfg files have no fx colours. Seeding them from the
        // theme's own accent and title is what keeps those themes looking
        // exactly as they did before the backgrounds gained their own palette.
        for (auto &c : m_custom) {
            if (!seen_fx_a) c.fx_a = c.accent;
            if (!seen_fx_b) c.fx_b = c.title;
        }
        Select(want_current);
        m_applied = m_current;
    }

    void ThemeManager::Save() const {
        m_applied = m_current;   // this call is what makes m_current official
        if (!g_sd_ok) return;
        cfg::EnsureDir();

        FILE *fp = fopen(ThemeCfgPath().c_str(), "w");
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
            snprintf(k, sizeof(k), "c%d_icon_bg", i); WriteColor(fp, k, c.icon_bg);
            snprintf(k, sizeof(k), "c%d_icon_fg", i); WriteColor(fp, k, c.icon_fg);
            snprintf(k, sizeof(k), "c%d_fx_a", i);    WriteColor(fp, k, c.fx_a);
            snprintf(k, sizeof(k), "c%d_fx_b", i);    WriteColor(fp, k, c.fx_b);
            fprintf(fp, "c%d_icon_bg_alpha=%d\n", i, c.icon_bg_alpha);

            fprintf(fp, "c%d_wallpaper=%s\n", i, c.wallpaper);
            fprintf(fp, "c%d_background_style=%d\n", i, c.background_style);
            for (int st = 0; st < (int)BackgroundStyle_Count; st++) {
                const Theme::FxParams &f = c.fx[st];
                fprintf(fp, "c%d_fx%d=%d,%d,%d,%d,%d,%d,%d,%d\n", i, st,
                        f.lines, f.thickness, f.amplitude, f.seed,
                        f.layers, f.y, f.x, f.cam);
            }
            fprintf(fp, "c%d_wallpaper_dim=%d\n",           i, c.wallpaper_dim);
            fprintf(fp, "c%d_wallpaper_blur=%d\n",          i, c.wallpaper_blur);
            fprintf(fp, "c%d_wallpaper_blur_radius=%d\n",   i, c.wallpaper_blur_radius);
            fprintf(fp, "c%d_wallpaper_snow=%d\n",          i, c.wallpaper_snow);
        }
        fclose(fp);
    }

} // namespace sl::menu::ui
