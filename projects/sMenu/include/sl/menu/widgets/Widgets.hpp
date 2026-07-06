#pragma once
#include <switch.h>
#include <string>
#include <sl/menu/gfx/Gfx.hpp>
#include <sl/menu/ui/Theme.hpp>

// Small home-screen widgets that pull live data over the network on a
// background thread. Weather (open-meteo) is implemented first; the structure
// leaves room for AuroraChat and others. Everything is best-effort: with no
// network the cards just show a placeholder and the menu is unaffected.

namespace sl::menu::widgets {

    class Widgets {
    public:
        void Init();   // load config, init curl, start the fetch thread
        void Exit();   // stop the thread + cleanup (call before socketExit)

        bool AnyEnabled() const { return m_weatherEnabled || m_auroraEnabled; }

        // Config (persisted to slaunch/config/widgets.txt).
        void LoadConfig();
        void SaveConfig();
        bool WeatherEnabled() const { return m_weatherEnabled; }
        void SetWeatherEnabled(bool v);
        const char *WeatherCity() const { return m_weatherCity; }
        void SetWeatherCity(const char *city);

        bool AuroraEnabled() const { return m_auroraEnabled; }
        void SetAuroraEnabled(bool v);
        const char *AuroraUser() const { return m_auroraUser; }
        void SetAuroraUser(const char *u);
        void SetAuroraPass(const char *p);
        bool AuroraHasPass() const { return m_auroraPass[0] != '\0'; }
        // Queue a chat message to send (from the on-screen keyboard).
        void AuroraSend(const char *text);

        // Draw the enabled widget cards down a column [x .. x+w]. Returns the
        // total height used.
        int Render(gfx::Gfx *gfx, const ui::Theme &t, int x, int y, int w);

    private:
        // ---- config ----
        bool m_weatherEnabled = false;
        char m_weatherCity[64] = "";
        bool m_auroraEnabled = false;
        char m_auroraUser[64] = "";
        char m_auroraPass[64] = "";

        // ---- live weather data (guarded by m_lock) ----
        mutable Mutex m_lock;
        bool  m_wValid = false;
        float m_wTemp  = 0;
        float m_wWind  = 0;
        int   m_wCode  = 0;
        char  m_wPlace[64] = "";
        bool  m_wDirty = true;   // config changed -> refetch soon

        // ---- AuroraChat state (guarded by m_lock) ----
        static constexpr int AuroraMaxLines = 6;
        std::string m_aLines[AuroraMaxLines];
        int   m_aCount = 0;
        std::string m_aStatus = "off";  // off / connecting / live / error
        std::string m_aToken;           // login token (thread-only after set)
        std::string m_aSendQueue;       // pending outgoing message
        int   m_aSock = -1;             // live message TCP socket (thread-only)
        bool  m_aDirty = true;          // config changed -> re-login/reconnect

        void AddAuroraLine(const char *user, const char *msg);

        // ---- fetch thread ----
        Thread m_thread = {};
        bool   m_run = false;
        bool   m_started = false;

        static void ThreadTrampoline(void *arg);
        void ThreadLoop();
        bool FetchWeather();
        void AuroraStep();       // login / connect / drain socket / send
        void AuroraDisconnect();
    };

} // namespace sl::menu::widgets
