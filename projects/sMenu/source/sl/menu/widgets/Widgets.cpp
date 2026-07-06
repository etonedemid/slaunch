#include <sl/menu/widgets/Widgets.hpp>
#include <sl/menu/net/Http.hpp>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <string>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/ioctl.h>

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
        // Poll ~6x/second so AuroraChat feels live; weather refreshes on a much
        // slower cadence (~15 min) tracked by a loop counter.
        int weatherTicks = 100000; // force an immediate first fetch
        while (m_run) {
            bool wneed;
            mutexLock(&m_lock);
            wneed = m_weatherEnabled && (m_wDirty || !m_wValid);
            mutexUnlock(&m_lock);
            if (wneed || weatherTicks >= 5400) { // 5400 * 166ms ~= 15 min
                FetchWeather();
                weatherTicks = 0;
            }
            weatherTicks++;

            AuroraStep();

            svcSleepThread(166'000'000ULL); // ~6 Hz
        }
        AuroraDisconnect();
    }

    void Widgets::AddAuroraLine(const char *user, const char *msg) {
        char line[256];
        snprintf(line, sizeof(line), "%s: %s", user, msg);
        mutexLock(&m_lock);
        if (m_aCount < AuroraMaxLines) {
            m_aLines[m_aCount++] = line;
        } else {
            for (int i = 1; i < AuroraMaxLines; i++) m_aLines[i - 1] = m_aLines[i];
            m_aLines[AuroraMaxLines - 1] = line;
        }
        mutexUnlock(&m_lock);
    }

    void Widgets::AuroraDisconnect() {
        if (m_aSock >= 0) { close(m_aSock); m_aSock = -1; }
    }

    void Widgets::AuroraStep() {
        bool enabled, dirty;
        char user[64], pass[64];
        std::string sendmsg;
        mutexLock(&m_lock);
        enabled = m_auroraEnabled;
        dirty   = m_aDirty;
        if (dirty) m_aDirty = false;
        strncpy(user, m_auroraUser, sizeof(user)); user[sizeof(user) - 1] = '\0';
        strncpy(pass, m_auroraPass, sizeof(pass)); pass[sizeof(pass) - 1] = '\0';
        sendmsg = m_aSendQueue; m_aSendQueue.clear();
        mutexUnlock(&m_lock);

        auto status = [&](const char *s) {
            mutexLock(&m_lock); m_aStatus = s; mutexUnlock(&m_lock);
        };

        if (!enabled) { if (m_aSock >= 0) AuroraDisconnect(); return; }

        if (dirty) { AuroraDisconnect(); m_aToken.clear(); }

        // 1) Log in to get a token.
        if (m_aToken.empty()) {
            if (!user[0] || !pass[0]) { status("set login"); return; }
            status("connecting");
            char body[192];
            snprintf(body, sizeof(body), "%s|%s|", user, pass);
            std::string resp;
            if (!net::Post("http://104.236.25.60:6767/api/login", body, "text/plain", nullptr, resp)) {
                status("offline"); return;
            }
            if (resp.find("ERR") != std::string::npos) { status("login failed"); return; }
            size_t bar = resp.find('|');
            m_aToken = (bar == std::string::npos) ? resp : resp.substr(0, bar);
            if (m_aToken.empty()) { status("login failed"); return; }
        }

        // 2) Connect the live-message TCP socket.
        if (m_aSock < 0) {
            int s = socket(AF_INET, SOCK_STREAM, 0);
            if (s < 0) { status("offline"); return; }
            struct sockaddr_in srv;
            memset(&srv, 0, sizeof(srv));
            srv.sin_family = AF_INET;
            srv.sin_port   = htons(3033);
            srv.sin_addr.s_addr = inet_addr("104.236.25.60");
            if (connect(s, (struct sockaddr *)&srv, sizeof(srv)) != 0) { close(s); status("offline"); return; }
            int nb = 1; ioctl(s, FIONBIO, &nb);
            m_aSock = s;
            status("live");
        }

        // 3) Send a queued outgoing message.
        if (!sendmsg.empty()) {
            char body[512];
            snprintf(body, sizeof(body), "%s|general|", sendmsg.c_str());
            std::string resp;
            net::Post("http://104.236.25.60:6767/api/chat", body, "text/plain", m_aToken.c_str(), resp);
        }

        // 4) Drain any incoming message (username|message|room per packet).
        char buf[2048];
        ssize_t len = recv(m_aSock, buf, sizeof(buf) - 1, 0);
        if (len > 0) {
            buf[len] = '\0';
            char *u = strtok(buf, "|");
            char *m = strtok(nullptr, "|");
            (void)strtok(nullptr, "|"); // room, unused
            if (u && m) AddAuroraLine(u, m);
        } else if (len == 0) {
            AuroraDisconnect(); // server closed
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

    bool renderaurorafirst = true;

int Widgets::Render(gfx::Gfx *gfx, const ui::Theme &t, int x, int y, int w) {

    int cy = y;

    auto renderWeatherWidget = [&]() {
        if (!m_weatherEnabled) return;

        bool valid; float temp, wind; int code; char place[64];
        
        {
            mutexLock(&m_lock);
            valid = m_wValid; temp = m_wTemp; wind = m_wWind; code = m_wCode;
            strncpy(place, m_wPlace, sizeof(place)); 
            place[sizeof(place) - 1] = '\0';
            mutexUnlock(&m_lock);
        }

        const int h = 175;
        gfx->FillRect(x, cy, w, h, WithAlpha(t.bg_bottom, 105));
        gfx->FillRect(x, cy, w, 3, t.accent);
        gfx->Text(FontSize::Small, x + 18, cy + 14, t.dim, "WEATHER");

        if (!valid) {
            gfx->Text(FontSize::Normal, x + 18, cy + 60, t.dim, "Loading...");
        } else {
            char tb[16]; snprintf(tb, sizeof(tb), "%.0f\xC2\xB0""C", temp);
            char wb[32]; snprintf(wb, sizeof(wb), "Wind %.0f km/h", wind);
            gfx->Text(FontSize::Title,  x + 18, cy + 40,  t.fg,  tb);
            gfx->Text(FontSize::Normal, x + 18, cy + 96,  t.accent, WeatherText(code));
            gfx->Text(FontSize::Small,  x + 18, cy + 128, t.dim, place);
            gfx->Text(FontSize::Small,  x + 18, cy + 152, t.dim, wb);
        }
        
        cy += h + 12;
    };

    auto renderAuroraWidget = [&]() {
        if (!m_auroraEnabled) return;

        std::vector<std::string> rawLines;
        std::string status;
        
        {
            mutexLock(&m_lock);
            for (int i = 0; i < m_aCount; i++) {
                rawLines.push_back(m_aLines[i]);
            }
            status = m_aStatus;
            mutexUnlock(&m_lock);
        }

        // UI Tuning
        int newX = x - 200;      
        int newW = w + 200;      
        const int maxCharsPerLine = 35; 
        const int maxDisplayLines = 8;
        const std::string prefix = "auroracross: from "; 
        std::vector<std::string> displayLines;

        // Word-wrapping logic with manual '\n' splitting
        for (std::string& raw_s : rawLines) {
            if (raw_s.find(prefix) == 0) {
                raw_s.erase(0, prefix.length()); 
            }

            size_t currentPos = 0;
            
            // Loop through the string, chunking by '\n'
            while (currentPos <= raw_s.length()) {
                size_t newlinePos = raw_s.find('\n', currentPos);
                
                // If no more newlines are found, process until the end of the string
                if (newlinePos == std::string::npos) {
                    newlinePos = raw_s.length();
                }

                // Extract the segment between the current position and the newline
                std::string lineSegment = raw_s.substr(currentPos, newlinePos - currentPos);

                // Handle empty lines (e.g., "\n\n")
                if (lineSegment.empty()) {
                    // Only push a blank line if we aren't at the very end of the string
                    if (newlinePos < raw_s.length()) {
                        displayLines.push_back("");
                    }
                } else {
                    // Apply your maxCharsPerLine logic to this specific segment
                    size_t start = 0;
                    while (start < lineSegment.length()) {
                        if (lineSegment.length() - start <= maxCharsPerLine) {
                            displayLines.push_back(lineSegment.substr(start));
                            break;
                        }
                        
                        size_t spacePos = lineSegment.rfind(' ', start + maxCharsPerLine);
                        if (spacePos == std::string::npos || spacePos <= start) {
                            displayLines.push_back(lineSegment.substr(start, maxCharsPerLine));
                            start += maxCharsPerLine;
                        } else {
                            displayLines.push_back(lineSegment.substr(start, spacePos - start));
                            start = spacePos + 1;
                        }
                    }
                }

                // Advance past the newline character we just found
                currentPos = newlinePos + 1;
            }
        }

        // Determine how many lines we are actually allowed to draw
        int totalDrawLines = std::min((int)displayLines.size(), maxDisplayLines);
        
        // If we have more lines than the max, skip the oldest ones at the beginning
        int startIndex = 0;
        if (displayLines.size() > maxDisplayLines) {
            startIndex = displayLines.size() - maxDisplayLines;
        }

        const int h = 56 + (totalDrawLines * 26); 

        gfx->FillRect(newX, cy, newW, h, WithAlpha(t.bg_bottom, 105));
        gfx->FillRect(newX, cy, newW, 3, t.accent);
        
        gfx->Text(FontSize::Small, newX + 18, cy + 12, t.dim, "AURORACHAT");
        int sw = gfx->TextWidth(FontSize::Small, status.c_str());
        gfx->Text(FontSize::Small, newX + newW - sw - 16, cy + 12, t.accent, status.c_str());

        // Draw the text starting from our calculated startIndex
        for (int i = 0; i < totalDrawLines; i++) {
            gfx->Text(FontSize::Small, newX + 14, cy + 44 + i * 26, t.fg, displayLines[startIndex + i].c_str());
        }

        cy += h + 12;
    };

    if (renderaurorafirst) {
        renderAuroraWidget();
        renderWeatherWidget();
    } else {
        renderWeatherWidget();
        renderAuroraWidget();
    }

    return cy; 
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

    void Widgets::SetAuroraEnabled(bool v) {
        mutexLock(&m_lock);
        m_auroraEnabled = v; m_aDirty = true;
        if (!v) { m_aStatus = "off"; m_aCount = 0; }
        mutexUnlock(&m_lock);
        SaveConfig();
    }
    void Widgets::SetAuroraUser(const char *u) {
        mutexLock(&m_lock);
        strncpy(m_auroraUser, u ? u : "", sizeof(m_auroraUser));
        m_auroraUser[sizeof(m_auroraUser) - 1] = '\0';
        m_aDirty = true;
        mutexUnlock(&m_lock);
        SaveConfig();
    }
    void Widgets::SetAuroraPass(const char *p) {
        mutexLock(&m_lock);
        strncpy(m_auroraPass, p ? p : "", sizeof(m_auroraPass));
        m_auroraPass[sizeof(m_auroraPass) - 1] = '\0';
        m_aDirty = true;
        mutexUnlock(&m_lock);
        SaveConfig();
    }
    void Widgets::AuroraSend(const char *text) {
        if (!text || !text[0]) return;
        mutexLock(&m_lock); m_aSendQueue = text; mutexUnlock(&m_lock);
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
            else if (!strcmp(k, "aurora_enabled")) m_auroraEnabled = atoi(v) != 0;
            else if (!strcmp(k, "aurora_user")) {
                strncpy(m_auroraUser, v, sizeof(m_auroraUser));
                m_auroraUser[sizeof(m_auroraUser) - 1] = '\0';
            }
            else if (!strcmp(k, "aurora_pass")) {
                strncpy(m_auroraPass, v, sizeof(m_auroraPass));
                m_auroraPass[sizeof(m_auroraPass) - 1] = '\0';
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
        fprintf(fp, "aurora_enabled=%d\n", m_auroraEnabled ? 1 : 0);
        fprintf(fp, "aurora_user=%s\n", m_auroraUser);
        fprintf(fp, "aurora_pass=%s\n", m_auroraPass);
        fclose(fp);
    }

} // namespace sl::menu::widgets
