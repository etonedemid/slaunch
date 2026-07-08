#pragma once
#include <switch.h>
#include <string>
#include <mutex>
#include <sl/menu/gfx/Gfx.hpp>
#include <sl/menu/ui/Theme.hpp>

// Include sol2 for Lua bindings
#include <sol/sol.hpp>

namespace sl::menu::widgets {

    // The standard interface for any custom widget
    class IWidget {
    public:
        virtual ~IWidget() = default;
        
        virtual void Update() = 0; 
        
        virtual int Render(gfx::Gfx* gfx, const ui::Theme& t, int x, int y, int w) = 0; 
    };

    // The Lua 
    class LuaWidget : public IWidget {
    private:
        sol::state m_lua;
        sol::function m_luaUpdate;
        sol::function m_luaRender;
        std::mutex m_luaLock; 

        gfx::Gfx* m_currentGfx = nullptr;
        const ui::Theme* m_currentTheme = nullptr;

        void BindAPI() {
            m_lua.set_function("gfx_fill_rect", [this](int x, int y, int w, int h, int r, int g, int b, int a) {
                if (m_currentGfx) {
                    // m_currentGfx->FillRect(x, y, w, h, {r, g, b, a});
                    // hmm...
                }
            });

            m_lua.set_function("gfx_text", [this](int x, int y, const std::string& text) {
                if (m_currentGfx) {
                    // m_currentGfx->Text(gfx::FontSize::Small, x, y, SDL_Color{255,255,255,255}, text.c_str());
                }
            });

            // Consider warning users not to call this inside render()!
            m_lua.set_function("net_get", [](const std::string& url) {
                std::string resp;
                // net::Get(url.c_str(), resp);
                return resp;
            });
        }

    public:
        explicit LuaWidget(const std::string& scriptPath) {
            // Open standard Lua libraries (math, string, etc.)
            m_lua.open_libraries(sol::lib::base, sol::lib::string, sol::lib::math);
            
            // Bind C++ engine functions to Lua
            BindAPI();
            
            // Load and run the script, capturing errors safely at init
            auto result = m_lua.script_file(scriptPath, sol::script_default_on_error);
            if (result.valid()) {
                m_luaUpdate = m_lua["update"];
                m_luaRender = m_lua["render"];
            } else {
                sol::error err = result;
                // BootLog("Lua Init Error for %s: %s", scriptPath.c_str(), err.what());
            }
        }

        void Update() override {
            std::lock_guard<std::mutex> lock(m_luaLock);
            if (m_luaUpdate.valid()) {
                // Catch Lua errors so bad scripts don't crash everything
                auto result = m_luaUpdate();
                if (!result.valid()) {
                    sol::error err = result;

                }
            }
        }

        int Render(gfx::Gfx* gfx, const ui::Theme& t, int x, int y, int w) override {
            int newY = y;
            
            std::lock_guard<std::mutex> lock(m_luaLock);
            
            // 1. Inject context so bound APIs know WHERE to draw
            m_currentGfx = gfx;
            m_currentTheme = &t;

            // 2. Execute Lua Render
            if (m_luaRender.valid()) {
                auto result = m_luaRender(x, y, w);
                if (result.valid()) {
                    newY = result; // Script returns the height it consumed
                } else {
                    sol::error err = result;
                    // BootLog("Lua Render Error: %s", err.what());
                }
            }

            // 3. Clear context to prevent dangling pointer access outside Render
            m_currentGfx = nullptr;
            m_currentTheme = nullptr;

            return newY;
        }
    };

} // namespace sl::menu::widgets