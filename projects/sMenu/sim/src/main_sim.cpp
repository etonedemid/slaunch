// Desktop host for the sLaunch menu.
//
// This is the simulator's equivalent of sMenu/source/main.cpp: it owns the
// event loop, feeds the menu its input and its app list, and does nothing else.
// The menu itself is the real one - sim/ compiles the shipping Menu.cpp against
// the libnx shim in sim/include/switch.h.
//
// What it deliberately does NOT do is act on the actions the menu returns.
// Launching a game, opening a system applet or rebooting are all requests to a
// daemon that does not exist here, so they are logged and the menu carries on.
// That is the honest simulation: you can see every screen and every transition,
// and nothing pretends to have launched.

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <switch.h>

#include <sl/menu/gfx/Gfx.hpp>
#include <sl/menu/ui/Menu.hpp>

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using sl::menu::ui::Menu;
using sl::menu::ui::Btn;
using sl::menu::ui::AppEntry;

namespace {

    constexpr const char *kAppListCache = "sdmc:/slaunch/cache/applist.txt";

    // The same file sMenu writes on the console, read back verbatim. This is
    // what makes the simulator show YOUR library: sync-sd.sh copies the cache
    // off the card and the menu then behaves as it does on hardware, down to
    // which titles are gamecards and which want updates.
    std::vector<AppEntry> LoadAppList() {
        std::vector<AppEntry> e;
        FILE *fp = fopen(kAppListCache, "r");
        if (!fp) return e;
        char line[320];
        while (fgets(line, sizeof(line), fp)) {
            line[strcspn(line, "\r\n")] = '\0';
            char *t1 = strchr(line,   '\t'); if (!t1) continue; *t1 = '\0';
            char *t2 = strchr(t1 + 1, '\t'); if (!t2) continue; *t2 = '\0';
            char *t3 = strchr(t2 + 1, '\t'); if (!t3) continue; *t3 = '\0';
            const u64 id = strtoull(line, nullptr, 16);
            if (id) e.push_back({ id, std::string(t3 + 1),
                                  atoi(t1 + 1) != 0, atoi(t2 + 1) != 0 });
        }
        fclose(fp);
        return e;
    }

    const char *ActionName(Menu::Action a) {
        switch (a) {
            case Menu::Action::LaunchApp:        return "LaunchApp";
            case Menu::Action::ResumeApp:        return "ResumeApp";
            case Menu::Action::TerminateApp:     return "TerminateApp";
            case Menu::Action::OpenAlbum:        return "OpenAlbum";
            case Menu::Action::OpenUserPage:     return "OpenUserPage";
            case Menu::Action::OpenNetConnect:   return "OpenNetConnect";
            case Menu::Action::OpenMiiEdit:      return "OpenMiiEdit";
            case Menu::Action::OpenWebBrowser:   return "OpenWebBrowser";
            case Menu::Action::OpenControllers:  return "OpenControllers";
            case Menu::Action::OpenHomebrewMenu: return "OpenHomebrewMenu";
            case Menu::Action::PowerSleep:       return "PowerSleep";
            case Menu::Action::PowerReboot:      return "PowerReboot";
            case Menu::Action::PowerShutdown:    return "PowerShutdown";
            case Menu::Action::PowerPayload:     return "PowerPayload";
            case Menu::Action::LaunchHomebrew:   return "LaunchHomebrew";
            case Menu::Action::LaunchHomebrewApp:return "LaunchHomebrewApp";
            case Menu::Action::FinishSetup:      return "FinishSetup";
            case Menu::Action::Quit:             return "Quit";
            default:                             return "None";
        }
    }

    // Keyboard mapping. Chosen so the whole menu is reachable one-handed:
    // arrows/WASD move, Z/Enter is A, X/Backspace is B.
    Btn KeyToBtn(SDL_Keycode k) {
        switch (k) {
            case SDLK_UP:    case SDLK_w: return Btn::Up;
            case SDLK_DOWN:  case SDLK_s: return Btn::Down;
            case SDLK_LEFT:  case SDLK_a: return Btn::Left;
            case SDLK_RIGHT: case SDLK_d: return Btn::Right;
            case SDLK_z: case SDLK_RETURN:    return Btn::A;
            case SDLK_x: case SDLK_BACKSPACE: return Btn::B;
            case SDLK_c: return Btn::X;
            case SDLK_v: return Btn::Y;
            case SDLK_q: return Btn::L;
            case SDLK_e: return Btn::R;
            case SDLK_1: return Btn::Minus;
            case SDLK_2: return Btn::Plus;
            default:     return Btn::None;
        }
    }

    // Read the full render target back and write a PNG. The target is
    // 1280*ss x 720*ss, so this is a genuinely high-resolution capture rather
    // than an upscaled 720p one.
    void Screenshot(SDL_Renderer *r, int ss) {
        int w = 0, h = 0;
        SDL_GetRendererOutputSize(r, &w, &h);
        SDL_Surface *shot = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32,
                                                           SDL_PIXELFORMAT_ARGB8888);
        if (!shot) { fprintf(stderr, "[sim] screenshot alloc failed\n"); return; }
        if (SDL_RenderReadPixels(r, nullptr, SDL_PIXELFORMAT_ARGB8888,
                                 shot->pixels, shot->pitch) != 0) {
            fprintf(stderr, "[sim] SDL_RenderReadPixels: %s\n", SDL_GetError());
            SDL_FreeSurface(shot);
            return;
        }
        mkdir("shots", 0777);
        char path[128];
        for (int i = 1; i < 10000; i++) {
            snprintf(path, sizeof(path), "shots/slaunch-%04d.png", i);
            struct stat st;
            if (stat(path, &st) != 0) break;
        }
        if (IMG_SavePNG(shot, path) == 0)
            printf("[sim] wrote %s (%dx%d, %dx supersample)\n", path, w, h, ss);
        else
            fprintf(stderr, "[sim] IMG_SavePNG: %s\n", SDL_GetError());
        SDL_FreeSurface(shot);
    }

    void Usage(const char *argv0) {
        printf("usage: %s [options]\n"
               "  --scale N        supersample factor 1-4 (default 2)\n"
               "  --font PATH      TTF to use as the console shared font\n"
               "  --user NAME      nickname shown in the header\n"
               "  --battery N      battery percent\n"
               "  --charging       show the charging indicator\n"
               "  --offline        report no network connection\n"
               "  --oobe           start in the setup wizard\n"
               "  --suspended ID   mark a title as the running game (hex app id)\n"
               "  --help\n", argv0);
    }

}   // namespace

int main(int argc, char **argv) {
    int  scale      = 2;
    bool oobe       = false;
    bool charging   = false;
    bool offline    = false;
    int  battery    = 78;
    u64  suspended  = 0;
    bool shot_mode  = false;
    int  settle     = -1;          // -1 = derive it, see below
    const char *nav = nullptr;
    const char *font = nullptr;
    const char *user = "Simulator";
    const char *sd   = nullptr;
    bool frametimes  = false;

    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        auto next = [&](const char *def) { return (i + 1 < argc) ? argv[++i] : def; };
        if      (a == "--scale")     scale     = atoi(next("2"));
        else if (a == "--font")      font      = next(nullptr);
        else if (a == "--user")      user      = next("Simulator");
        else if (a == "--battery")   battery   = atoi(next("78"));
        else if (a == "--suspended") suspended = strtoull(next("0"), nullptr, 16);
        else if (a == "--charging")  charging  = true;
        else if (a == "--offline")   offline   = true;
        else if (a == "--oobe")      oobe      = true;
        else if (a == "--shot")      shot_mode = true;
        else if (a == "--settle")    settle     = atoi(next("0"));
        else if (a == "--nav")       nav        = next("");
        else if (a == "--sd")        sd         = next(nullptr);
        else if (a == "--frametimes") frametimes = true;
        else if (a == "--help")      { Usage(argv[0]); return 0; }
        else { fprintf(stderr, "[sim] unknown option: %s\n", a.c_str()); Usage(argv[0]); return 1; }
    }

    // The welcome screen holds the first few seconds and swallows input, so
    // scripted presses start after it and the capture waits for them to finish.
    // Deriving this rather than making the caller guess is the difference
    // between --nav working first time and silently capturing the wrong screen.
    // All of this is in milliseconds, not frames.
    //
    // The welcome screen dismisses itself on a timer and swallows input until
    // it does, so scripted presses have to start after it. Counting frames
    // instead looked right under WSLg and then desynced on Windows, where the
    // frame rate differs - the presses were eaten by a welcome screen that was
    // still up, and the capture landed on the wrong screen entirely.
    constexpr u64 kNavStartMs = 5000;   // welcome screen, plus margin
    constexpr u64 kNavEveryMs = 220;    // long enough for a slide to settle
    const int nav_len = nav ? (int)strlen(nav) : 0;
    if (settle < 0)
        settle = (int)(kNavStartMs + (u64)nav_len * kNavEveryMs + 800);

    // WSL usually has no ALSA device, and SDL_mixer prints a wall of errors
    // before failing. The menu already copes with the mixer not opening, so the
    // only thing lost is the noise. An explicit SDL_AUDIODRIVER still wins.
    // Windows has working audio, so there the menu's music and sound effects
    // play as they do on the console.
#ifndef _WIN32
    SDL_setenv("SDL_AUDIODRIVER", "dummy", 0);
#endif

#ifdef _WIN32
    // Winsock needs starting before any socket call, which libnx does not.
    // Without it the chat widget's socket() fails outright instead of failing
    // to connect, which is a different bug to be looking at.
    { WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa); }
#endif

    // Simulated console state, before anything reads it.
    if (font) simsw::SetFontPath(font);
    simsw::SetNickname(user);
    simsw::SetBattery(battery, charging);
    if (offline) simsw::SetNetwork(false, false, 0, "", "");

    // Work from the simulated card root rather than from the source tree.
    //
    // Every path in the menu begins with "sdmc:/", and Linux is perfectly happy
    // to have a directory actually called that - so changing into a root which
    // contains one makes every one of those paths resolve, with no path
    // rewriting anywhere in the menu.
    //
    // Keeping that root outside the repo matters twice over: a colon is not a
    // legal character in a Windows filename, so an in-tree copy is a hazard for
    // anything on the Windows side that walks the checkout, and a card's worth
    // of icons and box art is far quicker to read from the Linux filesystem
    // than across the Windows interop mount.
    {
#ifdef _WIN32
        // On Windows the card sits next to the executable as a plain "sdmc"
        // folder - no colon, because Windows will not allow one in a name, so
        // the paths are rewritten on the way into the C library instead (see
        // include/sim_win_compat.h).
        const std::string root = sd ? std::string(sd) : std::string(".");
        const char *card = "sdmc";
#else
        // On Linux the directory really is called "sdmc:" and every path in the
        // menu resolves against it with no rewriting at all.
        const char *home = getenv("HOME");
        const std::string root = sd ? std::string(sd)
                                    : std::string(home ? home : ".") + "/.slaunch-sim";
        const char *card = "sdmc:";
#endif
        if (chdir(root.c_str()) != 0) {
            fprintf(stderr, "[sim] cannot enter %s\n"
                            "      Run sim/sync-sd.sh to build it.\n", root.c_str());
            return 1;
        }
        struct stat st;
        if (stat(card, &st) != 0) {
            fprintf(stderr, "[sim] %s has no '%s' directory.\n"
                            "      Run sim/sync-sd.sh to populate it.\n",
                    root.c_str(), card);
            return 1;
        }
#ifdef _WIN32
        SimWinSetRoot(card);
#endif
        printf("[sim] card: %s/%s\n", root.c_str(), card);
    }

    static const char *kAaMarker = "sdmc:/slaunch/config/aa_pending";
    bool aa_confirmed = false;
    sl::menu::gfx::Gfx gfx;
    gfx.SetSupersample(scale);

    // Same start-up the console does, so the marker guard is exercised here
    // rather than only on hardware. At --scale 1 the anti-aliasing setting is
    // read from the card and armed exactly as main.cpp arms it; the marker is
    // cleared further down once a frame has been presented.
    if (scale == 1) {
        bool want_aa = false;
        if (FILE *fp = fopen("sdmc:/slaunch/config/settings.txt", "r")) {
            char line[128];
            while (fgets(line, sizeof(line), fp)) {
                int v = 0;
                if (sscanf(line, "antialias=%d", &v) == 1) { want_aa = (v != 0); break; }
            }
            fclose(fp);
        }
        if (want_aa) {
            struct stat st;
            if (stat(kAaMarker, &st) == 0) {
                printf("[sim] anti-aliasing disarmed (last run drew no frame)\n");
                want_aa = false;
                remove(kAaMarker);
                // Same as the console: make the disarm stick.
                if (FILE *cf = fopen("sdmc:/slaunch/config/settings.txt", "r")) {
                    std::string all; char ln[192];
                    while (fgets(ln, sizeof(ln), cf))
                        all += (strncmp(ln, "antialias=", 10) == 0) ? "antialias=0\n" : ln;
                    fclose(cf);
                    if ((cf = fopen("sdmc:/slaunch/config/settings.txt", "w"))) {
                        fwrite(all.data(), 1, all.size(), cf); fclose(cf);
                    }
                }
            } else if (FILE *m = fopen(kAaMarker, "w")) {
                fputs("1\n", m);
                fclose(m);
            }
        }
        gfx.SetAntialias(want_aa);
        printf("[sim] anti-aliasing %s\n", want_aa ? "on" : "off");
    }
    gfx.SetWindowTitle("sLaunch simulator");
    if (!gfx.Init()) { fprintf(stderr, "[sim] gfx.Init failed\n"); return 1; }
    printf("[sim] font: %s\n", simsw::FontPath());
    printf("[sim] %dx%d output (%dx supersample). F12 = screenshot, F11 = OOBE, Esc = quit.\n",
           1280 * gfx.Supersample(), 720 * gfx.Supersample(), gfx.Supersample());

    // The menu is scoped so its destructor frees textures while the renderer is
    // still alive, exactly as main.cpp does on the console.
    {
        Menu ui;
        sl::menu::ui::g_sd_ok = true;

        AccountUid uid{};
        accountListAllUsers(&uid, 1, nullptr);
        ui.Init(&gfx, uid, suspended, oobe);
        if (suspended) ui.SetSuspendedApp(suspended);

        auto apps = LoadAppList();
        if (apps.empty()) {
            printf("[sim] %s is empty or missing - the menu will show no games.\n",
                   kAppListCache);
            ui.SetLoading(false);
        } else {
            printf("[sim] %zu titles from the transplanted card cache\n", apps.size());
            ui.SetApps(std::move(apps));
        }

        SDL_Joystick *joy = SDL_JoystickOpen(0);

        // Auto-repeat, matching the console host: one initial delay then an
        // accelerating repeat, so held-direction behaviour is the same thing
        // being tested here as ships.
        const u64 freq        = armGetSystemTickFreq();
        const u64 RepeatDelay = (360 * freq) / 1000;
        auto ms = [&](u64 m) { return (m * freq) / 1000; };
        int held_v = 0, held_h = 0;
        u64 next_v = 0, next_h = 0, start_v = 0, start_h = 0;

        auto run = [&](Menu::Action a, u64 id) {
            if (a == Menu::Action::None) return;
            // Nothing here can launch anything; see the file header.
            printf("[sim] action: %s%s", ActionName(a), id ? " id=" : "");
            if (id) printf("%016llX", (unsigned long long)id);
            printf("\n");
        };

        // Frame timing. The mean says almost nothing about how a menu feels;
        // the worst frame is the one you notice, so that is what is reported.
        u64 ft_prev = armGetSystemTick();
        u64 ft_max = 0, ft_total = 0, ft_count = 0, ft_over100 = 0;

        bool running = true;
        // Scripted navigation, paced in wall-clock time so it behaves the same
        // whatever frame rate the host runs at. One press every kNavEveryMs so
        // the menu's own slide animations run between them, exactly as they
        // would under a human; firing them all at once would capture a menu
        // that never settled anywhere.
        const u64  t0    = armGetSystemTick();
        const u64  freq_ms = armGetSystemTickFreq() / 1000;
        size_t     nav_i = 0;
        auto elapsed_ms = [&]() { return (armGetSystemTick() - t0) / freq_ms; };
        bool mouse_down = false;
        int  mouse_x = 0, mouse_y = 0;
        bool was_touching = false;
        int  last_tx = 0, last_ty = 0;

        while (running) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_QUIT) { running = false; continue; }
                if (ev.type == SDL_KEYDOWN && !ev.key.repeat) {
                    const SDL_Keycode k = ev.key.keysym.sym;
                    if (k == SDLK_ESCAPE)  { running = false; continue; }
                    if (k == SDLK_F12)     { Screenshot(gfx.Renderer(), gfx.Supersample()); continue; }
                    if (k == SDLK_F10)     { ui.ToggleDebugOverlay(); continue; }
                    // Directions are polled below so they auto-repeat; taking
                    // them here as well would double every press.
                    const Btn b = KeyToBtn(k);
                    if (b == Btn::None || b == Btn::Up || b == Btn::Down ||
                        b == Btn::Left || b == Btn::Right) continue;
                    u64 id = 0;
                    run(ui.OnButton(b, id), id);
                } else if (ev.type == SDL_JOYBUTTONDOWN) {
                    static const Btn kJoy[] = {
                        Btn::A, Btn::B, Btn::X, Btn::Y, Btn::None, Btn::None,
                        Btn::L, Btn::R, Btn::None, Btn::None, Btn::Plus, Btn::Minus,
                    };
                    const int n = ev.jbutton.button;
                    const Btn b = (n >= 0 && n < (int)(sizeof(kJoy) / sizeof(kJoy[0])))
                                      ? kJoy[n] : Btn::None;
                    if (b == Btn::None) continue;
                    u64 id = 0;
                    run(ui.OnButton(b, id), id);
                } else if (ev.type == SDL_MOUSEBUTTONDOWN) {
                    mouse_down = true; mouse_x = ev.button.x; mouse_y = ev.button.y;
                } else if (ev.type == SDL_MOUSEBUTTONUP) {
                    mouse_down = false;
                } else if (ev.type == SDL_MOUSEMOTION) {
                    mouse_x = ev.motion.x; mouse_y = ev.motion.y;
                }
            }

            // The mouse stands in for the touchscreen. SDL reports window
            // pixels; the menu works in 1280x720, so divide by the supersample
            // factor or every tap lands at the wrong place at scale > 1.
            {
                const int ss = gfx.Supersample();
                u64 id = 0;
                if (mouse_down) {
                    last_tx = mouse_x / ss; last_ty = mouse_y / ss;
                    run(ui.OnTouch(was_touching ? 1 : 0, last_tx, last_ty, id), id);
                    was_touching = true;
                } else if (was_touching) {
                    run(ui.OnTouch(2, last_tx, last_ty, id), id);
                    was_touching = false;
                }
            }

            // Directions: keyboard state plus a real pad if one is plugged in.
            int dir_v = 0, dir_h = 0;
            {
                const Uint8 *ks = SDL_GetKeyboardState(nullptr);
                bool up    = ks[SDL_SCANCODE_UP]    || ks[SDL_SCANCODE_W];
                bool down  = ks[SDL_SCANCODE_DOWN]  || ks[SDL_SCANCODE_S];
                bool left  = ks[SDL_SCANCODE_LEFT]  || ks[SDL_SCANCODE_A];
                bool right = ks[SDL_SCANCODE_RIGHT] || ks[SDL_SCANCODE_D];
                if (joy) {
                    constexpr int Dead = 20000;
                    const Uint8  hat = SDL_JoystickGetHat(joy, 0);
                    const Sint16 ay  = SDL_JoystickGetAxis(joy, 1);
                    const Sint16 ax  = SDL_JoystickGetAxis(joy, 0);
                    up    |= (hat & SDL_HAT_UP)    || ay < -Dead;
                    down  |= (hat & SDL_HAT_DOWN)  || ay >  Dead;
                    left  |= (hat & SDL_HAT_LEFT)  || ax < -Dead;
                    right |= (hat & SDL_HAT_RIGHT) || ax >  Dead;

                    auto norm = [](Sint16 v) {
                        if (v > -Dead && v < Dead) return 0.0f;
                        const float f = (float)v / 32767.0f;
                        return f < -1.0f ? -1.0f : (f > 1.0f ? 1.0f : f);
                    };
                    ui.SetRightStick(norm(SDL_JoystickGetAxis(joy, 2)),
                                     norm(SDL_JoystickGetAxis(joy, 3)));
                }
                dir_v = up ? -1 : down ? 1 : 0;
                dir_h = left ? -1 : right ? 1 : 0;
            }

            const u64 now = armGetSystemTick();
            auto step = [&](int dir, int &held, u64 &next, u64 &start, Btn neg, Btn pos) {
                if (dir == 0) { held = 0; return; }
                const bool fresh = (dir != held);
                bool fire = false;
                if (fresh) { fire = true; start = now; next = now + RepeatDelay; }
                else if (now >= next) {
                    fire = true;
                    const u64 held_ms = ((now - start) * 1000) / freq;
                    const u64 iv = held_ms < 700 ? 90 : held_ms < 1500 ? 55 : 32;
                    next = now + ms(iv);
                }
                held = dir;
                if (fire) {
                    u64 id = 0;
                    ui.SetNavFresh(fresh);
                    run(ui.OnButton(dir < 0 ? neg : pos, id), id);
                }
            };
            step(dir_v, held_v, next_v, start_v, Btn::Up,   Btn::Down);
            step(dir_h, held_h, next_h, start_h, Btn::Left, Btn::Right);

            if (nav && nav[nav_i] &&
                elapsed_ms() >= kNavStartMs + (u64)nav_i * kNavEveryMs) {
                Btn b = Btn::None;
                switch (nav[nav_i]) {
                    case 'l': b = Btn::Left;  break;
                    case 'r': b = Btn::Right; break;
                    case 'u': b = Btn::Up;    break;
                    case 'd': b = Btn::Down;  break;
                    case 'a': b = Btn::A;     break;
                    case 'b': b = Btn::B;     break;
                    case 'x': b = Btn::X;     break;
                    case 'y': b = Btn::Y;     break;
                    case 'p': b = Btn::Plus;  break;
                    case 'm': b = Btn::Minus; break;
                    case '[': b = Btn::L;     break;
                    case ']': b = Btn::R;     break;
                    default:  break;
                }
                nav_i++;
                if (b != Btn::None) {
                    u64 id = 0;
                    ui.SetNavFresh(true);
                    run(ui.OnButton(b, id), id);
                }
            }

            {
                const u64 now_t = armGetSystemTick();
                const u64 dt = (now_t - ft_prev) * 1000 / armGetSystemTickFreq();
                ft_prev = now_t;
                if (ft_count > 0) {          // skip the first, which includes init
                    if (dt > ft_max) ft_max = dt;
                    if (dt > 100) ft_over100++;
                    ft_total += dt;
                }
                ft_count++;
            }

            ui.Render();
            // A frame has reached the screen, so the renderer came up fine.
            if (!aa_confirmed) { aa_confirmed = true; remove(kAaMarker); }
            // Actions the menu deferred in order to animate first; see
            // Menu::TakePendingAction.
            {
                u64 pending_id = 0;
                run(ui.TakePendingAction(pending_id), pending_id);
            }
            ui.InitDeferred();

            // Scripted capture: let the animations settle first, or the
            // screenshot catches the menu mid-slide.
            if (shot_mode && elapsed_ms() >= (u64)settle) {
                Screenshot(gfx.Renderer(), gfx.Supersample());
                running = false;
            }
        }

        if (frametimes && ft_count > 1) {
            printf("[sim] frame max %llums  mean %llums  over-100ms %llu of %llu\n",
                   (unsigned long long)ft_max,
                   (unsigned long long)(ft_total / (ft_count - 1)),
                   (unsigned long long)ft_over100,
                   (unsigned long long)(ft_count - 1));
        }

        if (joy) SDL_JoystickClose(joy);
    }

    gfx.Exit();
    return 0;
}
