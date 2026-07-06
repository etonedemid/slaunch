#include <sl/menu/widgets/Widgets.hpp>
#include <sl/menu/net/Http.hpp>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <string>
#include <sys/stat.h>

namespace sl::menu::widgets {

    using gfx::FontSize;

    namespace {
        SDL_Color WithAlpha(SDL_Color c, Uint8 a) { return SDL_Color{ c.r, c.g, c.b, a }; }

        std::string UrlEncode(const char *s) {
            std::string o;
            for (; *s; ++s) {
                unsigned char c = (unsigned char)*s;
                if (isalnum(c) || c == '-' || c == '_' || c == '.') o += (char)c;
                else { char b[4]; snprintf(b, sizeof(b), "%%%02X", c); o += b; }
            }
            return o;
        }

        // Parse the number that follows "key": in `s`, searching from `from`.
        bool ExtractNumber(const std::string &s, const char *key, double &out, size_t from = 0) {
            std::string k = std::string("\"") + key + "\"";
            size_t p = s.find(k, from);
            if (p == std::string::npos) return false;
            p = s.find(':', p + k.size());
            if (p == std::string::npos) return false;
            p++;
            while (p < s.size() && (s[p] == ' ' || s[p] == '"')) p++;
            char *end = nullptr;
            double v = strtod(s.c_str() + p, &end);
            if (end == s.c_str() + p) return false;
            out = v;
            return true;
        }

        bool ExtractString(const std::string &s, const char *key, char *out, size_t out_sz, size_t from = 0) {
            std::string k = std::string("\"") + key + "\"";
            size_t p = s.find(k, from);
            if (p == std::string::npos) return false;
            p = s.find(':', p + k.size());
            if (p == std::string::npos) return false;
            p = s.find('"', p);
            if (p == std::string::npos) return false;
            size_t e = s.find('"', p + 1);
            if (e == std::string::npos) return false;
            size_t n = e - p - 1;
            if (n >= out_sz) n = out_sz - 1;
            memcpy(out, s.c_str() + p + 1, n);
            out[n] = '\0';
            return true;
        }

        // WMO weather-code -> short description.
        const char *WeatherText(int code) {
            switch (code) {
                case 0:  return "Clear";
                case 1:  return "Mainly clear";
                case 2:  return "Partly cloudy";
                case 3:  return "Overcast";
                case 45: case 48: return "Fog";
                case 51: case 53: case 55: return "Drizzle";
                case 56: case 57: return "Freezing drizzle";
                case 61: case 63: case 65: return "Rain";
                case 66: case 67: return "Freezing rain";
                case 71: case 73: case 75: return "Snow";
                case 77: return "Snow grains";
                case 80: case 81: case 82: return "Showers";
                case 85: case 86: return "Snow showers";
                case 95: return "Thunderstorm";
                case 96: case 99: return "Thunderstorm, hail";
                default: return "--";
            }
        }
    }

    void Widgets::Init() {
        mutexInit(&m_lock);
        LoadConfig();
        net::GlobalInit();
        m_run = true;
        if (R_SUCCEEDED(threadCreate(&m_thread, &Widgets::ThreadTrampoline, this,
                                     nullptr, 0x8000, 0x3B, -2))) {
            threadStart(&m_thread);
            m_started = true;
        }
    }

    void Widgets::Exit() {
        m_run = false;
        if (m_started) {
            threadWaitForExit(&m_thread);
            threadClose(&m_thread);
            m_started = false;
        }
        net::GlobalExit();
    }

    void Widgets::ThreadTrampoline(void *arg) { static_cast<Widgets *>(arg)->ThreadLoop(); }

    void Widgets::ThreadLoop() {
        while (m_run) {
            bool need;
            mutexLock(&m_lock);
            need = m_weatherEnabled && (m_wDirty || !m_wValid);
            mutexUnlock(&m_lock);

            if (need) FetchWeather();

            // Wait ~15 min before refreshing, but wake early on config change
            // or shutdown.
            for (int i = 0; i < 900 && m_run; i++) {
                svcSleepThread(1'000'000'000ULL);
                mutexLock(&m_lock);
                bool dirty = m_wDirty;
                mutexUnlock(&m_lock);
                if (dirty) break;
            }
        }
    }

    bool Widgets::FetchWeather() {
        char city[64];
        bool enabled;
        mutexLock(&m_lock);
        enabled = m_weatherEnabled;
        strncpy(city, m_weatherCity, sizeof(city));
        city[sizeof(city) - 1] = '\0';
        m_wDirty = false;
        mutexUnlock(&m_lock);

        if (!enabled || !city[0]) return false;

        // 1) Geocode the city name to coordinates.
        std::string url = "https://geocoding-api.open-meteo.com/v1/search?count=1&language=en&name=";
        url += UrlEncode(city);
        std::string resp;
        if (!net::Get(url.c_str(), resp)) return false;
        double lat, lon;
        if (!ExtractNumber(resp, "latitude", lat)) return false;
        if (!ExtractNumber(resp, "longitude", lon)) return false;
        char place[64] = "";
        ExtractString(resp, "name", place, sizeof(place));

        // 2) Current conditions for those coordinates.
        char furl[256];
        snprintf(furl, sizeof(furl),
                 "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
                 "&current=temperature_2m,weather_code,wind_speed_10m", lat, lon);
        if (!net::Get(furl, resp)) return false;

        // Start after the "current" object so we don't read the "current_units"
        // strings that appear earlier.
        size_t cur = resp.find("\"current\"");
        if (cur == std::string::npos) cur = 0;
        double temp = 0, code = 0, wind = 0;
        if (!ExtractNumber(resp, "temperature_2m", temp, cur)) return false;
        ExtractNumber(resp, "weather_code", code, cur);
        ExtractNumber(resp, "wind_speed_10m", wind, cur);

        mutexLock(&m_lock);
        m_wTemp = (float)temp;
        m_wCode = (int)code;
        m_wWind = (float)wind;
        strncpy(m_wPlace, place[0] ? place : city, sizeof(m_wPlace));
        m_wPlace[sizeof(m_wPlace) - 1] = '\0';
        m_wValid = true;
        mutexUnlock(&m_lock);
        return true;
    }

int Widgets::Render(gfx::Gfx *gfx, const ui::Theme &t, int x, int y, int w) {
        if (!m_weatherEnabled) return 0;

        bool valid; float temp, wind; int code; char place[64];
        mutexLock(&m_lock);
        valid = m_wValid; temp = m_wTemp; wind = m_wWind; code = m_wCode;
        strncpy(place, m_wPlace, sizeof(place)); place[sizeof(place) - 1] = '\0';
        mutexUnlock(&m_lock);

        // Increased height to accommodate the extra vertical line
        const int h = 175; 
        gfx->FillRect(x, y, w, h, WithAlpha(t.bg_bottom, 205));
        gfx->FillRect(x, y, w, 3, t.accent);
        gfx->Text(FontSize::Small, x + 18, y + 14, t.dim, "WEATHER");

        if (!valid) {
            gfx->Text(FontSize::Normal, x + 18, y + 60, t.dim, "Loading...");
            return h;
        }

        char tb[16];  snprintf(tb, sizeof(tb), "%.0f\xC2\xB0""C", temp);   // °C
        char wb[32];  snprintf(wb, sizeof(wb), "Wind %.0f km/h", wind);
        
        gfx->Text(FontSize::Title,  x + 18, y + 40,  t.fg,  tb);
        gfx->Text(FontSize::Normal, x + 18, y + 96,  t.accent, WeatherText(code));
        gfx->Text(FontSize::Small,  x + 18, y + 128, t.dim, place);
        gfx->Text(FontSize::Small,  x + 18, y + 152, t.dim, wb); 
        
        return h;
    }

    // ---- config ------------------------------------------------------------
    void Widgets::SetWeatherEnabled(bool v) {
        mutexLock(&m_lock); m_weatherEnabled = v; m_wDirty = true; mutexUnlock(&m_lock);
        SaveConfig();
    }
    void Widgets::SetWeatherCity(const char *city) {
        mutexLock(&m_lock);
        strncpy(m_weatherCity, city ? city : "", sizeof(m_weatherCity));
        m_weatherCity[sizeof(m_weatherCity) - 1] = '\0';
        m_wValid = false; m_wDirty = true;
        mutexUnlock(&m_lock);
        SaveConfig();
    }

    void Widgets::LoadConfig() {
        FILE *fp = fopen("sdmc:/slaunch/config/widgets.txt", "r");
        if (!fp) return;
        char line[128];
        while (fgets(line, sizeof(line), fp)) {
            line[strcspn(line, "\r\n")] = '\0';
            char *eq = strchr(line, '=');
            if (!eq) continue;
            *eq = '\0';
            const char *k = line, *v = eq + 1;
            if (!strcmp(k, "weather_enabled")) m_weatherEnabled = atoi(v) != 0;
            else if (!strcmp(k, "weather_city")) {
                strncpy(m_weatherCity, v, sizeof(m_weatherCity));
                m_weatherCity[sizeof(m_weatherCity) - 1] = '\0';
            }
        }
        fclose(fp);
    }

    void Widgets::SaveConfig() {
        mkdir("sdmc:/slaunch", 0777);
        mkdir("sdmc:/slaunch/config", 0777);
        FILE *fp = fopen("sdmc:/slaunch/config/widgets.txt", "w");
        if (!fp) return;
        fprintf(fp, "weather_enabled=%d\n", m_weatherEnabled ? 1 : 0);
        fprintf(fp, "weather_city=%s\n", m_weatherCity);
        fclose(fp);
    }

} // namespace sl::menu::widgets
