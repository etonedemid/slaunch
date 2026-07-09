#pragma once
#include <switch.h>
#include <vector>
#include <memory>
#include <sl/menu/gfx/Gfx.hpp>
#include <sl/menu/ui/Theme.hpp>
#include "IWidget.hpp"

// Home-screen widgets. Every widget is a Lua script under
// sdmc:/slaunch/widgets/*.lua (see LuaWidget). A background thread ticks each
// widget's update() (which may hit the network); Render() draws them stacked on
// the home screen. The Theming > Widgets submenu edits each widget's exposed
// options through the IWidget config surface.

namespace sl::menu::widgets {

    class Widgets {
    public:
        void Init();   // init curl, load widgets, start the fetch thread
        void Exit();   // stop the thread + cleanup (call before socketExit)

        bool AnyEnabled() const { return !m_widgets.empty(); }

        // Access for the Theming > Widgets submenu.
        int      Count() const { return (int)m_widgets.size(); }
        IWidget *At(int i) {
            return (i >= 0 && i < (int)m_widgets.size()) ? m_widgets[i].get() : nullptr;
        }

        // Whether a widget is loaded/shown. This is owned by the menu (persisted
        // to config/widget_enabled.txt), NOT by the widget script - a disabled
        // widget is neither ticked nor drawn.
        bool IsEnabled(int i) const {
            return (i >= 0 && i < (int)m_enabled.size()) ? (m_enabled[i] != 0) : false;
        }
        void SetEnabled(int i, bool on);

        // Draw every widget at its own (draggable) position. The box height is
        // measured from each widget's returned height so hit-testing works.
        void Render(gfx::Gfx *gfx, const ui::Theme &t);

        // Touch-drag support (positions are per-widget and persisted).
        int  HitTest(int px, int py) const;                 // widget under point, or -1
        bool GetBox(int i, int &x, int &y, int &w, int &h) const;
        void MoveBy(int i, int dx, int dy);                 // clamped to the screen
        void SavePositions();

    private:
        // Loaded once at Init and never resized afterwards, so the fetch thread
        // (update) and main thread (render / submenu edits) can read it without a
        // manager-level lock - each LuaWidget guards its own Lua state.
        std::vector<std::unique_ptr<IWidget>> m_widgets;
        void LoadWidgets();

        // Per-widget placement. w is fixed; h is remeasured each frame.
        struct Box { int x, y, w, h; };
        std::vector<Box> m_box;
        void LoadPositions();   // apply saved positions over the default layout

        // Menu-owned enable flags (char, since std::vector<bool> isn't addressable).
        std::vector<char> m_enabled;
        void LoadEnabled();
        void SaveEnabled();

        Thread m_thread = {};
        bool   m_run = false;
        bool   m_started = false;
        static void ThreadTrampoline(void *arg);
        void ThreadLoop();
    };

} // namespace sl::menu::widgets
