#pragma once
#include <switch.h>
#include <string>
#include <vector>
#include <mutex>
#include <unordered_map>
#include <tuple>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cerrno>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sl/menu/gfx/Gfx.hpp>
#include <sl/menu/ui/Theme.hpp>
#include <sl/menu/net/Http.hpp>

// Include sol2 for Lua bindings
#include <sol/sol.hpp>

namespace sl::menu::widgets {

    // The standard interface for any custom widget. Beyond update()/render() a
    // widget may expose named configuration variables (e.g. a weather widget's
    // city) that the Theming > Widgets submenu lists and lets the user edit.
    class IWidget {
    public:
        virtual ~IWidget() = default;

        virtual void Update() = 0;
        virtual int  Render(gfx::Gfx* gfx, const ui::Theme& t, int x, int y, int w) = 0;

        // Configuration surface (default: none). Types are "string" or "bool".
        virtual std::string Name() const { return "Widget"; }
        virtual int         OptionCount() const { return 0; }
        virtual std::string OptionLabel(int) const { return ""; }
        virtual std::string OptionValue(int) const { return ""; }
        virtual std::string OptionType(int)  const { return "string"; }
        virtual void        SetOption(int /*index*/, const std::string& /*value*/) {}
    };

    // A widget whose behaviour lives in a Lua script on the SD card.
    //
    //   update()  - runs on the widget fetch thread; may block on the network.
    //   render(x,y,w) -> height  - runs on the main thread; draw only.
    //
    // Configuration: a script may define a `widget` table describing itself, e.g.
    //   widget = {
    //     name = "Weather",
    //     options = {
    //       { id = "enabled", label = "Enabled", type = "bool",   default = true },
    //       { id = "city",    label = "City",    type = "string", default = "London" },
    //     },
    //   }
    // The engine loads saved values (config/widgets/<script>.cfg), exposes them
    // to the script as a global `config` table (config.city, config.enabled, ...),
    // and calls the optional on_config(id) hook whenever a value changes.
    //
    // Engine API bound for the script:
    //   Drawing (render only):
    //     gfx_fill_rect(x,y,w,h, r,g,b,a)
    //     gfx_text(x,y,text) / gfx_text_ex(size,x,y,text, r,g,b) / gfx_text_width(text)
    //     gfx_image(path,x,y,w,h) -> bool
    //     theme_color(name) -> r,g,b   ("bg" "fg" "dim" "accent" "title")
    //     screen_width() / screen_height()
    //   Networking (update only):
    //     net_get(url) -> string|nil
    //     net_post(url, body, content_type?, authorization?) -> string|nil
    //     net_download(url, path) -> bool
    //     net_tcp_connect(host, port) -> sock|nil
    //     net_tcp_send(sock, data) -> bool
    //     net_tcp_recv(sock) -> string   (data) | "" (no data yet) | nil (closed)
    //     net_tcp_close(sock)
    class LuaWidget : public IWidget {
    private:
        struct Option { std::string id, label, type, value; };

        sol::state m_lua;
        // protected, not plain sol::function - we build -fno-exceptions, so an
        // unsafe call that errors would abort the whole menu instead of failing.
        sol::protected_function m_luaUpdate;
        sol::protected_function m_luaRender;
        sol::protected_function m_luaOnConfig;
        std::mutex m_luaLock;

        // Valid only for the duration of Render().
        gfx::Gfx* m_currentGfx = nullptr;
        const ui::Theme* m_currentTheme = nullptr;
        bool m_inRender = false;          // gate net_* out of the render path

        std::string m_name;
        std::vector<Option> m_options;
        std::string m_configPath;         // sdmc:/slaunch/config/widgets/<base>.cfg

        std::unordered_map<std::string, SDL_Texture*> m_images; // gfx_image cache, keyed by path
        std::vector<int> m_socks;                               // open net_tcp sockets, closed in dtor

        // ---- theme + config helpers -------------------------------------------
        SDL_Color ThemeColor(const std::string& name) const {
            const ui::Theme* t = m_currentTheme;
            if (!t) return SDL_Color{255, 255, 255, 255};
            if (name == "bg" || name == "bg_bottom") return t->bg_bottom;
            if (name == "bg_top")                    return t->bg_top;
            if (name == "fg")                        return t->fg;
            if (name == "dim")                       return t->dim;
            if (name == "accent")                    return t->accent;
            if (name == "title")                     return t->title;
            return t->fg;
        }

        static std::string BaseName(const std::string& path) {
            size_t slash = path.find_last_of("/\\");
            std::string b = (slash == std::string::npos) ? path : path.substr(slash + 1);
            size_t dot = b.find_last_of('.');
            if (dot != std::string::npos) b = b.substr(0, dot);
            return b;
        }

        // Write one option's value into the Lua `config` table with its type.
        void PushOption(sol::table& cfg, const Option& o) {
            if (o.type == "bool")      cfg[o.id] = (o.value == "1");
            else if (o.type == "int")  cfg[o.id] = atoi(o.value.c_str());
            else                       cfg[o.id] = o.value;
        }

        void LoadConfigFile() {
            FILE* fp = fopen(m_configPath.c_str(), "r");
            if (!fp) return;
            char line[256];
            while (fgets(line, sizeof(line), fp)) {
                line[strcspn(line, "\r\n")] = '\0';
                char* eq = strchr(line, '=');
                if (!eq) continue;
                *eq = '\0';
                const char* id = line;
                const char* val = eq + 1;
                for (auto& o : m_options)
                    if (o.id == id) { o.value = val; break; }
            }
            fclose(fp);
        }

        void SaveConfigFile() {
            mkdir("sdmc:/slaunch", 0777);
            mkdir("sdmc:/slaunch/config", 0777);
            mkdir("sdmc:/slaunch/config/widgets", 0777);
            FILE* fp = fopen(m_configPath.c_str(), "w");
            if (!fp) return;
            for (auto& o : m_options)
                fprintf(fp, "%s=%s\n", o.id.c_str(), o.value.c_str());
            fclose(fp);
        }

        // Read the `widget` descriptor table (name + options) after the script ran.
        void LoadDescriptor(const std::string& scriptPath) {
            m_name = BaseName(scriptPath);
            sol::optional<sol::table> w = m_lua["widget"];
            if (!w) return;
            m_name = w->get_or("name", m_name);

            sol::optional<sol::table> opts = (*w)["options"];
            if (!opts) return;
            size_t n = opts->size();
            for (size_t i = 1; i <= n; i++) {
                sol::optional<sol::table> oo = (*opts)[i];
                if (!oo) continue;
                Option o;
                o.id    = oo->get_or("id", std::string(""));
                if (o.id.empty()) continue;
                o.label = oo->get_or("label", o.id);
                o.type  = oo->get_or("type", std::string("string"));
                if (o.type == "bool") {
                    bool d = oo->get_or("default", false);
                    o.value = d ? "1" : "0";
                } else if (o.type == "int") {
                    int d = oo->get_or("default", 0);
                    o.value = std::to_string(d);
                } else {
                    o.value = oo->get_or("default", std::string(""));
                }
                m_options.push_back(std::move(o));
            }
        }

        void BindAPI() {
            m_lua.set_function("screen_width",  []() { return gfx::Gfx::Width;  });
            m_lua.set_function("screen_height", []() { return gfx::Gfx::Height; });

            m_lua.set_function("gfx_fill_rect", [this](int x, int y, int w, int h,
                                                       int r, int g, int b, int a) {
                if (!m_currentGfx) return;
                m_currentGfx->FillRect(x, y, w, h,
                    SDL_Color{ (Uint8)r, (Uint8)g, (Uint8)b, (Uint8)a });
            });

            m_lua.set_function("gfx_text", [this](int x, int y, const std::string& text) {
                if (!m_currentGfx) return;
                m_currentGfx->Text(gfx::FontSize::Small, x, y,
                                   SDL_Color{255, 255, 255, 255}, text.c_str());
            });

            m_lua.set_function("gfx_text_ex", [this](int size, int x, int y,
                                                     const std::string& text,
                                                     int r, int g, int b) {
                if (!m_currentGfx) return;
                if (size < 0) size = 0;
                if (size > (int)gfx::FontSize::Title) size = (int)gfx::FontSize::Title;
                m_currentGfx->Text((gfx::FontSize)size, x, y,
                    SDL_Color{ (Uint8)r, (Uint8)g, (Uint8)b, 255 }, text.c_str());
            });

            m_lua.set_function("gfx_text_width", [this](const std::string& text) {
                if (!m_currentGfx) return 0;
                return m_currentGfx->TextWidth(gfx::FontSize::Small, text.c_str());
            });

            m_lua.set_function("theme_color", [this](const std::string& name) {
                SDL_Color c = ThemeColor(name);
                return std::make_tuple((int)c.r, (int)c.g, (int)c.b);
            });

            m_lua.set_function("gfx_image", [this](const std::string& path,
                                                   int x, int y, int w, int h) {
                if (!m_currentGfx) return false;
                SDL_Texture* tex = nullptr;
                auto it = m_images.find(path);
                if (it != m_images.end()) {
                    tex = it->second;
                } else {
                    tex = m_currentGfx->LoadImage(path.c_str());
                    m_images[path] = tex; // cache even nullptr so we don't retry every frame
                }
                if (!tex) return false;
                m_currentGfx->DrawImage(tex, x, y, w, h);
                return true;
            });

            // ---- networking (update() only) ----
            m_lua.set_function("net_get", [this](const std::string& url)
                                              -> sol::optional<std::string> {
                if (m_inRender) return sol::nullopt;
                std::string resp;
                if (!net::Get(url.c_str(), resp)) return sol::nullopt;
                return resp;
            });

            m_lua.set_function("net_download", [this](const std::string& url,
                                                      const std::string& path) {
                if (m_inRender) return false;
                std::string data;
                if (!net::Get(url.c_str(), data) || data.empty()) return false;
                FILE* fp = fopen(path.c_str(), "wb");
                if (!fp) return false;
                size_t wrote = fwrite(data.data(), 1, data.size(), fp);
                fclose(fp);
                return wrote == data.size();
            });

            // HTTP POST. content_type and authorization are optional (pass nil).
            m_lua.set_function("net_post", [this](const std::string& url,
                                                  const std::string& body,
                                                  sol::optional<std::string> content_type,
                                                  sol::optional<std::string> authorization)
                                                     -> sol::optional<std::string> {
                if (m_inRender) return sol::nullopt;
                std::string resp;
                const char* ct = content_type ? content_type->c_str() : nullptr;
                const char* au = authorization ? authorization->c_str() : nullptr;
                if (!net::Post(url.c_str(), body.c_str(), ct, au, resp)) return sol::nullopt;
                return resp;
            });

            // Raw TCP for widgets that need a live socket (e.g. chat). The socket
            // is left non-blocking so net_tcp_recv never stalls a frame.
            m_lua.set_function("net_tcp_connect", [this](const std::string& host, int port)
                                                      -> sol::optional<int> {
                if (m_inRender) return sol::nullopt;
                struct sockaddr_in srv;
                memset(&srv, 0, sizeof(srv));
                srv.sin_family = AF_INET;
                srv.sin_port   = htons((u16)port);
                in_addr_t a = inet_addr(host.c_str());
                if (a == INADDR_NONE) {
                    struct hostent* he = gethostbyname(host.c_str());
                    if (!he || !he->h_addr_list[0]) return sol::nullopt;
                    memcpy(&srv.sin_addr, he->h_addr_list[0], he->h_length);
                } else {
                    srv.sin_addr.s_addr = a;
                }
                int s = socket(AF_INET, SOCK_STREAM, 0);
                if (s < 0) return sol::nullopt;
                if (connect(s, (struct sockaddr*)&srv, sizeof(srv)) != 0) {
                    close(s);
                    return sol::nullopt;
                }
                int nb = 1; ioctl(s, FIONBIO, &nb);
                m_socks.push_back(s);
                return s;
            });

            m_lua.set_function("net_tcp_send", [this](int sock, const std::string& data) {
                if (m_inRender || sock < 0) return false;
                size_t sent = 0;
                while (sent < data.size()) {
                    ssize_t k = send(sock, data.data() + sent, data.size() - sent, 0);
                    if (k <= 0) return false;
                    sent += (size_t)k;
                }
                return true;
            });

            // string with bytes on data, "" when nothing is waiting, nil on close.
            m_lua.set_function("net_tcp_recv", [this](int sock)
                                                   -> sol::optional<std::string> {
                if (m_inRender || sock < 0) return std::string();
                char buf[2048];
                ssize_t n = recv(sock, buf, sizeof(buf) - 1, 0);
                if (n > 0) return std::string(buf, (size_t)n);
                if (n == 0) return sol::nullopt;                 // peer closed
                if (errno == EWOULDBLOCK || errno == EAGAIN) return std::string();
                return sol::nullopt;                              // error
            });

            m_lua.set_function("net_tcp_close", [this](int sock) {
                if (sock < 0) return;
                close(sock);
                for (auto it = m_socks.begin(); it != m_socks.end(); ++it)
                    if (*it == sock) { m_socks.erase(it); break; }
            });
        }

    public:
        explicit LuaWidget(const std::string& scriptPath) {
            m_configPath = "sdmc:/slaunch/config/widgets/" + BaseName(scriptPath) + ".cfg";

            m_lua.open_libraries(sol::lib::base, sol::lib::string, sol::lib::math,
                                 sol::lib::table, sol::lib::os);
            BindAPI();

            auto result = m_lua.script_file(scriptPath, sol::script_pass_on_error);
            if (!result.valid()) return;

            m_luaUpdate   = m_lua["update"];
            m_luaRender   = m_lua["render"];
            m_luaOnConfig = m_lua["on_config"];

            // Read the descriptor, load saved values over the defaults, and expose
            // the resolved config to the script.
            LoadDescriptor(scriptPath);
            LoadConfigFile();
            sol::table cfg = m_lua.create_named_table("config");
            for (auto& o : m_options) PushOption(cfg, o);
        }

        ~LuaWidget() override {
            for (auto& kv : m_images)
                if (kv.second) SDL_DestroyTexture(kv.second);
            for (int s : m_socks)
                if (s >= 0) close(s);
        }

        // ---- IWidget config surface (main-thread only) ------------------------
        std::string Name()        const override { return m_name; }
        int         OptionCount() const override { return (int)m_options.size(); }
        std::string OptionLabel(int i) const override {
            return (i >= 0 && i < (int)m_options.size()) ? m_options[i].label : "";
        }
        std::string OptionValue(int i) const override {
            return (i >= 0 && i < (int)m_options.size()) ? m_options[i].value : "";
        }
        std::string OptionType(int i) const override {
            return (i >= 0 && i < (int)m_options.size()) ? m_options[i].type : "string";
        }
        void SetOption(int i, const std::string& value) override {
            if (i < 0 || i >= (int)m_options.size()) return;
            Option& o = m_options[i];
            o.value = value;
            {
                std::lock_guard<std::mutex> lock(m_luaLock);
                sol::table cfg = m_lua["config"];
                if (cfg.valid()) PushOption(cfg, o);
                if (m_luaOnConfig.valid()) { auto r = m_luaOnConfig(o.id); (void)r; }
            }
            SaveConfigFile();
        }

        // ---- run loop ---------------------------------------------------------
        void Update() override {
            std::lock_guard<std::mutex> lock(m_luaLock);
            if (!m_luaUpdate.valid()) return;
            auto result = m_luaUpdate();
            (void)result; // errors land in result (valid()==false); ignore a bad tick
        }

        int Render(gfx::Gfx* gfx, const ui::Theme& t, int x, int y, int w) override {
            int newY = y;
            // Never block the render thread: if update() holds the lock (it's mid
            // network fetch, which can take seconds), skip drawing this frame. The
            // widget pops in once its fetch returns instead of freezing the menu.
            std::unique_lock<std::mutex> lock(m_luaLock, std::try_to_lock);
            if (!lock.owns_lock()) return y;

            m_currentGfx   = gfx;
            m_currentTheme = &t;
            m_inRender = true;

            if (m_luaRender.valid()) {
                auto result = m_luaRender(x, y, w);
                if (result.valid()) {
                    // render() returns the HEIGHT it used; the caller wants the
                    // new bottom Y, so add it to the start y. (Without the "+ y"
                    // the measured box height came out negative -> 0, which broke
                    // widget hit-testing / dragging.)
                    sol::optional<int> h = result;
                    if (h) newY = y + *h;
                }
            }

            m_inRender = false;
            m_currentGfx = nullptr;
            m_currentTheme = nullptr;
            return newY;
        }
    };

} // namespace sl::menu::widgets
