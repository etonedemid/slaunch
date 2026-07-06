// sMenu - sLaunch SDL2 menu, a library applet served into an applet slot by
// sSystem via ECS. It renders the UI and, since a library applet cannot launch
// titles or system applets itself, asks the sSystem daemon (which runs as
// qlaunch) to do so over the SMI protocol.

#include <switch.h>
#include <SDL2/SDL.h>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <vector>
#include <sys/stat.h>

#include <sl/smi/Protocol.hpp>
#include <sl/os/Applications.hpp>
#include <sl/menu/smi/Commands.hpp>
#include <sl/menu/smi/MessageHandler.hpp>
#include <sl/menu/gfx/Gfx.hpp>
#include <sl/menu/ui/Menu.hpp>

using namespace sl;
using menu::ui::Menu;
using menu::ui::Btn;

extern "C" {
    // Explicitly declare we are a library applet. A raw NSO launched into an
    // applet slot by qlaunch (not via hbloader's homebrew ABI) has no env for
    // libnx to auto-detect this from, so without it libnx never runs the applet
    // focus/foreground handshake -- appletGetFocusState() stays 0 and the
    // compositor shows our layer black no matter what we render. uMenu sets the
    // same thing for the same reason.
    AppletType __nx_applet_type = AppletType_LibraryApplet;

    // 0 = let libnx auto-size the heap to the applet slot's available memory.
    // A fixed 128MB request cannot be satisfied in a library-applet slot, so
    // svcSetHeapSize fails and the process never starts (confirmed on HW: a
    // tiny NSO with auto-sizing loaded fine where the 5.4MB one with a forced
    // 128MB heap did not).
    u64 __nx_heap_size = 0;
}

static Menu *g_UI            = nullptr;
static bool  g_NeedAppReload = true;
static bool  g_Running       = true;

static constexpr const char *SetupMarker = "sdmc:/slaunch/config/setup_done";

static void BootLog(const char *fmt, ...) {
    mkdir("sdmc:/slaunch", 0777);
    FILE *fp = fopen("sdmc:/slaunch/boot.log", "a");
    if (!fp) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);
    fputc('\n', fp);
    fclose(fp);
}

static bool IsFirstRun() {
    struct stat st;
    return stat(SetupMarker, &st) != 0;
}
static void MarkSetupDone() {
    mkdir("sdmc:/slaunch", 0777);
    mkdir("sdmc:/slaunch/config", 0777);
    FILE *fp = fopen(SetupMarker, "w");
    if (fp) { fputs("1\n", fp); fclose(fp); }
}

// ---------------------------------------------------------------------------
enum SwitchJoyButton {
    JOY_A = 0, JOY_B = 1, JOY_X = 2, JOY_Y = 3,
    JOY_L = 6, JOY_R = 7, JOY_PLUS = 10, JOY_MINUS = 11,
    JOY_DLEFT = 12, JOY_DUP = 13, JOY_DRIGHT = 14, JOY_DDOWN = 15,
};
static Btn TranslateButton(int b) {
    switch (b) {
        case JOY_A: return Btn::A;   case JOY_B: return Btn::B;
        case JOY_X: return Btn::X;   case JOY_Y: return Btn::Y;
        case JOY_L: return Btn::L;   case JOY_R: return Btn::R;
        case JOY_PLUS: return Btn::Plus;  case JOY_MINUS: return Btn::Minus;
        case JOY_DUP: return Btn::Up;     case JOY_DDOWN: return Btn::Down;
        case JOY_DLEFT: return Btn::Left; case JOY_DRIGHT: return Btn::Right;
        default: return Btn::None;
    }
}

// ---------------------------------------------------------------------------
static void ReloadApps() {
    std::vector<NsApplicationRecord> records;
    if (R_FAILED(os::ListApplicationRecords(records)) || records.empty()) {
        if (g_UI) g_UI->SetApps({});
        g_NeedAppReload = false;
        return;
    }
    std::vector<u64> ids;
    ids.reserve(records.size());
    for (auto &r : records) ids.push_back(r.application_id);
    std::vector<NsApplicationView> views;
    os::ListApplicationViews(ids, views);

    std::vector<menu::ui::AppEntry> entries;
    entries.reserve(records.size());
    for (size_t i = 0; i < records.size(); i++) {
        u64 app_id = records[i].application_id;

        // uLaunch shows *every* application record -- we never drop one. The
        // view flags are only for the cosmetic gamecard/update badge. Note:
        // NsApplicationView.flags is a u32 at offset 0xC (index 3); the old
        // code read index 4 (garbage), so its can_launch filter hid every game.
        bool gamecard = false, needs_upd = false;
        if (i < views.size()) {
            u32 f = *reinterpret_cast<const u32*>(
                        reinterpret_cast<const u8*>(&views[i]) + 0xC);
            gamecard  = (f & BIT(2)) != 0;
            needs_upd = (f & BIT(4)) != 0;
        }

        NsApplicationControlData ctrl = {};
        std::string name;
        if (R_SUCCEEDED(os::GetApplicationControl(app_id, ctrl)))
            name = os::GetAppName(ctrl.nacp);
        else {
            char tmp[32]; snprintf(tmp, sizeof(tmp), "0x%016llX", (unsigned long long)app_id);
            name = tmp;
        }
        entries.push_back({ app_id, std::move(name), gamecard, needs_upd });
    }
    if (g_UI) g_UI->SetApps(std::move(entries));
    g_NeedAppReload = false;
}

static void OnSdEjected()      { g_NeedAppReload = true; }
static void OnAppListChanged() { g_NeedAppReload = true; }

// ---------------------------------------------------------------------------
// libnx internals: normally called by libnx's default __appInit, which we
// override -- so we must call them ourselves. __nx_win_init() creates the
// default nwindow (the display layer) that SDL's switch backend needs.
extern "C" {
    void __libnx_init_time(void);
    void __nx_win_init(void);
    void __nx_win_exit(void);
}

extern "C" void __appInit() {
    smInitialize();
    fsInitialize();
    fsdevMountSdmc();             // sdmc: available -> BootLog works from here
    BootLog("appinit: sd mounted");
    timeInitialize();
    __libnx_init_time();
    setsysInitialize();
    setInitialize();
    // Set the real firmware version, or version-gated service init (ns, ...)
    // misbehaves/hangs thinking it is running on 0.0.0.
    SetSysFirmwareVersion fw = {};
    if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw)))
        hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro) | BIT(31));
    BootLog("appinit: setsys/hosver");
    appletInitialize();                          BootLog("appinit: applet");
    hidInitialize();
    hidInitializeTouchScreen();                  BootLog("appinit: hid");
    accountInitialize(AccountServiceType_System);
    nsInitialize();                              BootLog("appinit: account/ns");
    psmInitialize();
    plInitialize(PlServiceType_User);            BootLog("appinit: pl");
    // Networking for the widgets (weather/chat). Best-effort: if this fails the
    // widgets just show nothing, the menu is unaffected.
    nifmInitialize(NifmServiceType_User);
    socketInitializeDefault();                   BootLog("appinit: net");
    __nx_win_init();                             BootLog("appinit: win (done)");
}
extern "C" void __appExit() {
    __nx_win_exit();
    socketExit(); nifmExit();
    plExit(); psmExit(); nsExit(); accountExit(); hidExit(); appletExit();
    setExit(); setsysExit(); timeExit();
    fsdevUnmountAll(); fsExit(); smExit();
}

// ---------------------------------------------------------------------------
// Menu actions -> SMI requests to the sSystem daemon.
// Only one library applet can be active at a time, and an application takes
// over the whole screen. So to launch a game or open a system applet, the menu
// sends the request and then EXITS; the daemon carries out the action once our
// slot is free, and relaunches the menu afterwards (uLaunch's model).
static void DispatchAction(Menu::Action action, u64 launch_id,
                           bool &has_suspended, u64 &suspended_id) {
    switch (action) {
        case Menu::Action::LaunchApp:
            if (has_suspended && launch_id == suspended_id)
                menu::smi::ResumeApp();
            else
                menu::smi::LaunchApp(launch_id);
            g_Running = false; // hand off to the daemon
            break;
        case Menu::Action::ResumeApp:
            menu::smi::ResumeApp();
            g_Running = false;
            break;
        case Menu::Action::TerminateApp:
            // Just closes the running game; the menu stays up.
            menu::smi::TerminateApp();
            has_suspended = false; suspended_id = 0;
            g_UI->ClearSuspendedApp();
            break;
        case Menu::Action::OpenAlbum:        menu::smi::OpenAlbum();        g_Running = false; break;
        case Menu::Action::OpenUserPage:     menu::smi::OpenUserPage();     g_Running = false; break;
        case Menu::Action::OpenNetConnect:   menu::smi::OpenNetConnect();   g_Running = false; break;
        case Menu::Action::OpenMiiEdit:      menu::smi::OpenMiiEdit();      g_Running = false; break;
        case Menu::Action::OpenWebBrowser:   menu::smi::OpenWebBrowser();   g_Running = false; break;
        case Menu::Action::OpenControllers:  menu::smi::OpenControllers();  g_Running = false; break;
        case Menu::Action::OpenHomebrewMenu: menu::smi::OpenHomebrewMenu(); g_Running = false; break;
        case Menu::Action::OpenPower:        menu::smi::OpenPowerMenu();    break; // sleep; menu stays
        case Menu::Action::FinishSetup:
            MarkSetupDone();
            g_UI->SetStatus("Setup complete. Welcome!");
            break;
        case Menu::Action::Quit: g_Running = false; break;
        default: break;
    }
}

// ---------------------------------------------------------------------------
int main() {
    // sSystem hands us the selected user + suspended-app state at launch.
    smi::SystemStatus status = {};
    {
        AppletStorage st;
        if (R_SUCCEEDED(appletPopInData(&st))) {
            appletStorageRead(&st, 0, &status, sizeof(status));
            appletStorageClose(&st);
        }
    }

    BootLog("applet: main enter (status user_valid=%d suspended=%d)",
            (int)accountUidIsValid(&status.selected_user), (int)status.has_suspended_app);

    menu::ui::g_sd_ok = true; // applet NPDM grants SD access

    menu::gfx::Gfx gfx;
    if (!gfx.Init()) {
        BootLog("applet: gfx.Init FAILED: %s", SDL_GetError());
        return 0; // sSystem will relaunch us
    }
    BootLog("applet: gfx.Init OK, entering loop");

    SDL_Joystick *joy = SDL_JoystickOpen(0);

    Menu ui;
    g_UI = &ui;
    ui.Init(&gfx, status.selected_user, status.suspended_app_id, IsFirstRun());

    bool has_suspended = status.has_suspended_app;
    u64  suspended_id  = status.suspended_app_id;
    if (has_suspended) ui.SetSuspendedApp(suspended_id);

    menu::smi::MessageCallbacks cbs;
    cbs.on_sd_ejected       = OnSdEjected;
    cbs.on_app_list_changed = OnAppListChanged;

    g_NeedAppReload = true;
    constexpr int Deadzone = 20000;

    auto run = [&](Menu::Action act, u64 launch_id) {
        if (act != Menu::Action::None)
            DispatchAction(act, launch_id, has_suspended, suspended_id);
    };

    // Directional auto-repeat: a held dpad/stick keeps scrolling after a short
    // initial delay, and accelerates the longer it is held. Action buttons stay
    // discrete (event-driven).
    const u64 freq        = armGetSystemTickFreq();
    const u64 RepeatDelay = (360 * freq) / 1000; // before the first repeat
    auto ms = [&](u64 m) { return (m * freq) / 1000; };
    int held_v = 0, held_h = 0;                  // current held direction (-1/0/+1)
    u64 next_v = 0, next_h = 0;                   // tick of the next repeat
    u64 start_v = 0, start_h = 0;                 // tick the hold began (for accel)

    while (g_Running && appletMainLoop()) {
        if (g_NeedAppReload) ReloadApps();
        menu::smi::PollMessages(cbs);

        // Action buttons via events (no repeat); directions are polled below.
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) { g_Running = false; continue; }
            if (ev.type == SDL_JOYBUTTONDOWN) {
                Btn b = TranslateButton(ev.jbutton.button);
                if (b == Btn::None || b == Btn::Up || b == Btn::Down ||
                    b == Btn::Left || b == Btn::Right)
                    continue;
                u64 launch_id = 0;
                run(ui.OnButton(b, launch_id), launch_id);
            }
        }

        // Poll the current directional state from dpad + hat + left stick.
        int dir_v = 0, dir_h = 0;
        if (joy) {
            Uint8  hat = SDL_JoystickGetHat(joy, 0);
            Sint16 ay  = SDL_JoystickGetAxis(joy, 1);
            Sint16 ax  = SDL_JoystickGetAxis(joy, 0);
            bool up    = (hat & SDL_HAT_UP)    || ay < -Deadzone || SDL_JoystickGetButton(joy, JOY_DUP);
            bool down  = (hat & SDL_HAT_DOWN)  || ay >  Deadzone || SDL_JoystickGetButton(joy, JOY_DDOWN);
            bool left  = (hat & SDL_HAT_LEFT)  || ax < -Deadzone || SDL_JoystickGetButton(joy, JOY_DLEFT);
            bool right = (hat & SDL_HAT_RIGHT) || ax >  Deadzone || SDL_JoystickGetButton(joy, JOY_DRIGHT);
            dir_v = up ? -1 : down ? 1 : 0;
            dir_h = left ? -1 : right ? 1 : 0;
        }
        const u64 now = armGetSystemTick();
        auto step = [&](int dir, int &held, u64 &next, u64 &start, Btn neg, Btn pos) {
            if (dir == 0) { held = 0; return; }
            const bool fresh = (dir != held);
            bool fire = false;
            if (fresh) {
                fire = true; start = now; next = now + RepeatDelay;
            } else if (now >= next) {
                fire = true;
                // Accelerate: the repeat interval shrinks the longer it is held.
                const u64 held_ms = ((now - start) * 1000) / freq;
                const u64 iv = held_ms < 700 ? 90 : held_ms < 1500 ? 55 : 32;
                next = now + ms(iv);
            }
            held = dir;
            if (fire) {
                u64 launch_id = 0;
                ui.SetNavFresh(fresh);
                run(ui.OnButton(dir < 0 ? neg : pos, launch_id), launch_id);
            }
        };
        step(dir_v, held_v, next_v, start_v, Btn::Up,   Btn::Down);
        step(dir_h, held_h, next_h, start_h, Btn::Left, Btn::Right);

        ui.Render();
    }

    if (joy) SDL_JoystickClose(joy);
    gfx.Exit();
    return 0;
}
