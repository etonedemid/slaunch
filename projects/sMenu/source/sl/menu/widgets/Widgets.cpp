#include <sl/menu/widgets/Widgets.hpp>
#include <sl/menu/net/Http.hpp>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <algorithm>
#include <dirent.h>
#include <sys/stat.h>

namespace sl::menu::widgets {

    namespace {
        constexpr int  kWidgetW = 340;
        constexpr const char *kPosPath = "sdmc:/slaunch/config/widget_pos.txt";
        constexpr const char *kEnPath  = "sdmc:/slaunch/config/widget_enabled.txt";
    }

    void Widgets::Init() {
        net::GlobalInit();
        LoadWidgets();          // constructs LuaWidgets (runs each script) on this thread
        LoadPositions();        // default layout, then any saved positions
        LoadEnabled();          // menu-owned on/off state (default on)
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
        m_widgets.clear();      // frees LuaWidget textures/sockets (renderer still alive)
        net::GlobalExit();
    }

    void Widgets::ThreadTrampoline(void *arg) { static_cast<Widgets *>(arg)->ThreadLoop(); }

    void Widgets::ThreadLoop() {
        while (m_run) {
            for (size_t i = 0; i < m_widgets.size(); i++)
                if (IsEnabled((int)i)) m_widgets[i]->Update();   // disabled = not ticked
            svcSleepThread(166'000'000ULL); // ~6 Hz
        }
    }

    void Widgets::LoadWidgets() {
        m_widgets.clear();
        DIR *d = opendir("sdmc:/slaunch/widgets");
        if (!d) return;
        struct dirent *e;
        while ((e = readdir(d)) != nullptr) {
            const char *name = e->d_name;
            size_t len = strlen(name);
            if (len > 4 && strcasecmp(name + len - 4, ".lua") == 0) {
                std::string path = std::string("sdmc:/slaunch/widgets/") + name;
                m_widgets.push_back(std::make_unique<LuaWidget>(path));
            }
        }
        closedir(d);
    }

    void Widgets::Render(gfx::Gfx *gfx, const ui::Theme &t) {
        for (size_t i = 0; i < m_widgets.size(); i++) {
            Box &b = m_box[i];
            if (!IsEnabled((int)i)) { b.h = 0; continue; }   // off = not drawn / not grabbable
            int ynew = m_widgets[i]->Render(gfx, t, b.x, b.y, b.w);
            b.h = std::max(0, ynew - b.y);   // measured for hit-testing
        }
    }

    // ---- positions ---------------------------------------------------------
    int Widgets::HitTest(int px, int py) const {
        // Topmost first: later widgets draw over earlier ones at the same spot.
        for (int i = (int)m_box.size() - 1; i >= 0; i--) {
            const Box &b = m_box[i];
            if (b.h > 0 && px >= b.x && px < b.x + b.w && py >= b.y && py < b.y + b.h)
                return i;
        }
        return -1;
    }

    bool Widgets::GetBox(int i, int &x, int &y, int &w, int &h) const {
        if (i < 0 || i >= (int)m_box.size()) return false;
        x = m_box[i].x; y = m_box[i].y; w = m_box[i].w; h = m_box[i].h;
        return true;
    }

    void Widgets::MoveBy(int i, int dx, int dy) {
        if (i < 0 || i >= (int)m_box.size()) return;
        Box &b = m_box[i];
        b.x = std::min(std::max(0, b.x + dx), gfx::Gfx::Width  - b.w);
        b.y = std::min(std::max(0, b.y + dy), gfx::Gfx::Height - 40);
    }

    void Widgets::LoadPositions() {
        // Default layout: a right-hand column starting near the top. Widgets are
        // draggable, so this is only a first-run starting point.
        m_box.assign(m_widgets.size(), Box{});
        for (size_t i = 0; i < m_widgets.size(); i++)
            m_box[i] = Box{ gfx::Gfx::Width - kWidgetW - 28, 96 + (int)i * 190, kWidgetW, 0 };

        FILE *fp = fopen(kPosPath, "r");
        if (!fp) return;
        char line[192];
        while (fgets(line, sizeof(line), fp)) {
            line[strcspn(line, "\r\n")] = '\0';
            char *eq = strchr(line, '=');
            if (!eq) continue;
            *eq = '\0';
            const char *name = line;
            int x = 0, y = 0;
            if (sscanf(eq + 1, "%d,%d", &x, &y) != 2) continue;
            // Match to a widget by its display name.
            for (size_t i = 0; i < m_widgets.size(); i++) {
                if (m_widgets[i]->Name() == name) {
                    m_box[i].x = std::min(std::max(0, x), gfx::Gfx::Width  - m_box[i].w);
                    m_box[i].y = std::min(std::max(0, y), gfx::Gfx::Height - 40);
                    break;
                }
            }
        }
        fclose(fp);
    }

    void Widgets::SavePositions() {
        mkdir("sdmc:/slaunch", 0777);
        mkdir("sdmc:/slaunch/config", 0777);
        FILE *fp = fopen(kPosPath, "w");
        if (!fp) return;
        for (size_t i = 0; i < m_widgets.size(); i++)
            fprintf(fp, "%s=%d,%d\n", m_widgets[i]->Name().c_str(), m_box[i].x, m_box[i].y);
        fclose(fp);
    }

    // ---- enable flags ------------------------------------------------------
    void Widgets::SetEnabled(int i, bool on) {
        if (i < 0 || i >= (int)m_enabled.size()) return;
        m_enabled[i] = on ? 1 : 0;
        SaveEnabled();
    }

    void Widgets::LoadEnabled() {
        m_enabled.assign(m_widgets.size(), 1);   // default: on
        FILE *fp = fopen(kEnPath, "r");
        if (!fp) return;
        char line[128];
        while (fgets(line, sizeof(line), fp)) {
            line[strcspn(line, "\r\n")] = '\0';
            char *eq = strchr(line, '=');
            if (!eq) continue;
            *eq = '\0';
            for (size_t i = 0; i < m_widgets.size(); i++)
                if (m_widgets[i]->Name() == line) { m_enabled[i] = atoi(eq + 1) ? 1 : 0; break; }
        }
        fclose(fp);
    }

    void Widgets::SaveEnabled() {
        mkdir("sdmc:/slaunch", 0777);
        mkdir("sdmc:/slaunch/config", 0777);
        FILE *fp = fopen(kEnPath, "w");
        if (!fp) return;
        for (size_t i = 0; i < m_widgets.size(); i++)
            fprintf(fp, "%s=%d\n", m_widgets[i]->Name().c_str(), m_enabled[i] ? 1 : 0);
        fclose(fp);
    }

} // namespace sl::menu::widgets
