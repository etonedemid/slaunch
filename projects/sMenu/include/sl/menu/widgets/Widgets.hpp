#pragma once
#include <switch.h>
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

        bool AnyEnabled() const { return m_weatherEnabled; }

        // Config (persisted to slaunch/config/widgets.txt).
        void LoadConfig();
        void SaveConfig();
        bool WeatherEnabled() const { return m_weatherEnabled; }
        void SetWeatherEnabled(bool v);
        const char *WeatherCity() const { return m_weatherCity; }
        void SetWeatherCity(const char *city);

        // Draw the enabled widget cards down a column [x .. x+w]. Returns the
        // total height used.
        int Render(gfx::Gfx *gfx, const ui::Theme &t, int x, int y, int w);

    private:
        // ---- config ----
        bool m_weatherEnabled = false;
        char m_weatherCity[64] = "";

        // ---- live weather data (guarded by m_lock) ----
        mutable Mutex m_lock;
        bool  m_wValid = false;
        float m_wTemp  = 0;
        float m_wWind  = 0;
        int   m_wCode  = 0;
        char  m_wPlace[64] = "";
        bool  m_wDirty = true;   // config changed -> refetch soon

        // ---- fetch thread ----
        Thread m_thread = {};
        bool   m_run = false;
        bool   m_started = false;

        static void ThreadTrampoline(void *arg);
        void ThreadLoop();
        bool FetchWeather();
    };

} // namespace sl::menu::widgets
