#pragma once
#include <switch.h>
#include <sl/os/Applications.hpp>
#include <vector>
#include <string>

// Text-based menu UI for sLaunch
// Uses libnx's software console (framebuffer) — zero GPU, blazing fast.
// Console dimensions at default 8x16 font: 160 cols x 45 rows.

namespace sl::menu::ui {

    constexpr int Cols = 160;
    constexpr int Rows = 45;

    // One entry shown in the list
    struct AppEntry {
        u64         app_id;
        std::string name;
        bool        is_gamecard;
        bool        needs_update;
    };

    class TextUI {
    public:
        TextUI();
        ~TextUI() = default;

        // Call once at startup with initial data
        void Init(AccountUid selected_user, u64 suspended_app_id);

        // Replace the app list (e.g., after ApplicationListChanged)
        void SetApps(std::vector<AppEntry> apps);

        // Process HID input. Returns the app_id to launch (0 = none).
        // Also sets out_action for non-launch actions.
        enum class Action {
            None,
            LaunchApp,      // launch g_LaunchId
            ResumeApp,
            TerminateApp,
            OpenAlbum,
            OpenUserPage,
            OpenNetConnect,
            OpenMiiEdit,
            OpenPower,
            Quit,
        };

        Action Update(u64 &out_app_id);

        // Redraw the full screen (called after Update when dirty)
        void Render();

        // Notify UI that an app is now running / stopped
        void SetSuspendedApp(u64 app_id);
        void ClearSuspendedApp();

        // Notify UI of a user change
        void SetUser(AccountUid uid, const char *nickname);

        // Mark dirty to force redraw next frame
        void MarkDirty() { m_dirty = true; }

        // Show a temporary status message on the bottom bar
        void SetStatus(const char *msg);

    private:
        void DrawHeader();
        void DrawAppList();
        void DrawFooter();
        void DrawStatusBar();

        // Move cursor to (col, row) and print
        static void MoveTo(int col, int row);
        static void ClearLine(int row);

        std::vector<AppEntry> m_apps;
        int      m_cursor      = 0;
        int      m_scroll      = 0;
        u64      m_suspended   = 0;      // app_id of suspended game (0 = none)
        AccountUid m_user      = {};
        char     m_nickname[33]= "Player";
        char     m_status[128] = "";
        u64      m_status_tick = 0;
        bool     m_dirty       = true;

        // HID state
        PadState m_pad         = {};

        // Visible list rows (Rows - header - footer - statusbar)
        static constexpr int ListRows = Rows - 6;
    };

} // namespace sl::menu::ui
