// sMenu - sLaunch SDL2 menu, a library applet served into an applet slot by
// sSystem via ECS. It renders the UI and, since a library applet cannot launch
// titles or system applets itself, asks the sSystem daemon (which runs as
// qlaunch) to do so over the SMI protocol.

#include <switch.h>
#include <SDL2/SDL.h>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <vector>
#include <string>
#include <memory>
#include <unordered_map>
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

// Background app loader (so HOME shows the menu immediately, then fills in).
static Thread                       g_LoadThread = {};
static std::vector<menu::ui::AppEntry> g_LoadResult;
static std::vector<u64>             g_CachedIds;    // ids of the currently-shown list
static volatile bool                g_LoadDone = false;
static volatile bool                g_LoadNoChange = false; // list unchanged -> keep cache
static bool                         g_LoadRunning = false;

static constexpr const char *SetupMarker = "sdmc:/slaunch/config/setup_done";

static u64 g_BootTick0 = 0;   // set at the top of __appInit; boot.log is ms-stamped

static u64 BootMs() {
    if (!g_BootTick0) return 0;
    return (armGetSystemTick() - g_BootTick0) * 1000 / armGetSystemTickFreq();
}

static void BootLog(const char *fmt, ...) {
    mkdir("sdmc:/slaunch", 0777);
    FILE *fp = fopen("sdmc:/slaunch/boot.log", "a");
    if (!fp) return;
    fprintf(fp, "[+%4llums] ", (unsigned long long)BootMs());
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
// Icon cache (uLaunch-style): the JPEG embedded in a title's control data is
// written to slaunch/cache/icons/<id>.jpg so the menu's icon UI modes can load
// it as a texture without re-querying NS every frame. "Fetch and compare": we
// only rewrite when the file is missing or its size differs from the icon NS
// just returned, so an unchanged title costs one stat() and no write.
static constexpr const char *IconCacheDir = "sdmc:/slaunch/cache/icons";

static void CacheAppIcon(u64 app_id, const NsApplicationControlData &ctrl, u64 ctrl_size) {
    const size_t icon_len = os::IconSize(ctrl_size);
    if (icon_len == 0) return; // icon-less title

    char path[64];
    snprintf(path, sizeof(path), "%s/%016llX.jpg", IconCacheDir,
             (unsigned long long)app_id);

    struct stat st;
    if (stat(path, &st) == 0 && (size_t)st.st_size == icon_len)
        return; // already cached and unchanged

    FILE *fp = fopen(path, "wb");
    if (!fp) return;
    fwrite(ctrl.icon, 1, icon_len, fp);
    fclose(fp);
}

// Title-name cache (app_id -> display name). nsGetApplicationControlData is a
// synchronous per-title read that dominates menu start-up when a big library is
// installed; caching the resolved names lets a repeat launch skip it for every
// title we have already seen. New/uncached titles are the only ones that hit NS.
static constexpr const char *NameCachePath = "sdmc:/slaunch/cache/names.txt";

static std::unordered_map<u64, std::string> LoadNameCache() {
    std::unordered_map<u64, std::string> m;
    FILE *fp = fopen(NameCachePath, "r");
    if (!fp) return m;
    char line[320];
    while (fgets(line, sizeof(line), fp)) {
        line[strcspn(line, "\r\n")] = '\0';
        char *eq = strchr(line, '=');       // split on the FIRST '=' (names may contain '=')
        if (!eq) continue;
        *eq = '\0';
        u64 id = strtoull(line, nullptr, 16);
        if (id != 0) m[id] = eq + 1;
    }
    fclose(fp);
    return m;
}

static void SaveNameCache(const std::vector<std::pair<u64, std::string>> &entries) {
    FILE *fp = fopen(NameCachePath, "w");
    if (!fp) return;
    for (auto &e : entries)
        fprintf(fp, "%016llX=%s\n", (unsigned long long)e.first, e.second.c_str());
    fclose(fp);
}

// ---------------------------------------------------------------------------
// The heavy part of a reload: enumerate installed titles and resolve their
// names/icons. No UI or global state is touched, so this is safe to run on a
// worker thread while the menu keeps rendering a "Loading" screen (see main()).
static std::vector<menu::ui::AppEntry> LoadAppEntries() {
    std::vector<NsApplicationRecord> records;
    if (R_FAILED(os::ListApplicationRecords(records)) || records.empty())
        return {};

    std::vector<u64> ids;
    ids.reserve(records.size());
    for (auto &r : records) ids.push_back(r.application_id);
    std::vector<NsApplicationView> views;
    os::ListApplicationViews(ids, views);

    mkdir("sdmc:/slaunch", 0777);
    mkdir("sdmc:/slaunch/cache", 0777);
    mkdir(IconCacheDir, 0777);

    // Prior run's resolved names; a title with a cached name AND a cached icon
    // needs no NS control fetch this launch.
    std::unordered_map<u64, std::string> nameCache = LoadNameCache();
    std::vector<std::pair<u64, std::string>> freshCache;
    freshCache.reserve(records.size());
    bool hitNs = false;

    // Heap, not stack: NsApplicationControlData is ~144KB, too big for a worker
    // thread's stack. Reused across titles (GetApplicationControl memsets it).
    auto ctrl = std::make_unique<NsApplicationControlData>();

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

        // Fast path: reuse the cached name when its icon is already on disk. Only
        // fall back to the (expensive) NS control fetch for new/uncached titles.
        std::string name;
        char iconPath[64];
        snprintf(iconPath, sizeof(iconPath), "%s/%016llX.jpg", IconCacheDir,
                 (unsigned long long)app_id);
        struct stat ist;
        const bool iconCached = (stat(iconPath, &ist) == 0 && ist.st_size > 0);
        auto cached = nameCache.find(app_id);

        if (iconCached && cached != nameCache.end()) {
            name = cached->second;
        } else {
            u64 ctrl_size = 0;
            if (R_SUCCEEDED(os::GetApplicationControl(app_id, *ctrl, ctrl_size))) {
                name = os::GetAppName(ctrl->nacp);
                CacheAppIcon(app_id, *ctrl, ctrl_size); // extract JPEG for the icon UI
            } else {
                char tmp[32]; snprintf(tmp, sizeof(tmp), "0x%016llX", (unsigned long long)app_id);
                name = tmp;
            }
            hitNs = true;
        }

        freshCache.emplace_back(app_id, name);
        entries.push_back({ app_id, std::move(name), gamecard, needs_upd });
    }

    // Rewrite the name cache when we resolved anything new or the library set
    // changed size (prunes uninstalled titles).
    if (hitNs || freshCache.size() != nameCache.size())
        SaveNameCache(freshCache);

    return entries;
}

// Persisted copy of the fully-resolved list, so a relaunch shows games on the
// first frame instead of re-enumerating. Format per line: hexid<TAB>gc<TAB>upd<TAB>name.
static constexpr const char *AppListCache = "sdmc:/slaunch/cache/applist.txt";

static void SaveAppList(const std::vector<menu::ui::AppEntry> &e) {
    mkdir("sdmc:/slaunch", 0777);
    mkdir("sdmc:/slaunch/cache", 0777);
    FILE *fp = fopen(AppListCache, "w");
    if (!fp) return;
    for (auto &a : e)
        fprintf(fp, "%016llX\t%d\t%d\t%s\n", (unsigned long long)a.app_id,
                a.is_gamecard ? 1 : 0, a.needs_update ? 1 : 0, a.name.c_str());
    fclose(fp);
}

static std::vector<menu::ui::AppEntry> LoadAppList() {
    std::vector<menu::ui::AppEntry> e;
    FILE *fp = fopen(AppListCache, "r");
    if (!fp) return e;
    char line[320];
    while (fgets(line, sizeof(line), fp)) {
        line[strcspn(line, "\r\n")] = '\0';
        char *t1 = strchr(line,   '\t'); if (!t1) continue; *t1 = '\0';
        char *t2 = strchr(t1 + 1, '\t'); if (!t2) continue; *t2 = '\0';
        char *t3 = strchr(t2 + 1, '\t'); if (!t3) continue; *t3 = '\0';
        u64 id = strtoull(line, nullptr, 16);
        if (id) e.push_back({ id, std::string(t3 + 1), atoi(t1 + 1) != 0, atoi(t2 + 1) != 0 });
    }
    fclose(fp);
    return e;
}

// One cheap NS call: just the installed title ids, for change detection.
static std::vector<u64> CurrentAppIds() {
    std::vector<NsApplicationRecord> records;
    std::vector<u64> ids;
    if (R_SUCCEEDED(os::ListApplicationRecords(records)))
        for (auto &r : records) ids.push_back(r.application_id);
    return ids;
}

static void LoadAppsTrampoline(void *) {
    // Cheap check first: if the installed title set matches what we're already
    // showing, do nothing (no per-app work). Only re-resolve when it changed.
    std::vector<u64> ids = CurrentAppIds();
    if (!ids.empty() && ids == g_CachedIds) {
        g_LoadNoChange = true;
    } else {
        g_LoadResult = LoadAppEntries();
        SaveAppList(g_LoadResult);
    }
    g_LoadDone = true;
}

// The daemon sends this when the SD card is physically pulled while powered on.
// Show the full-screen warning; the daemon reboots the console shortly after.
static void OnSdEjected()      { if (g_UI) g_UI->ShowSdRemoved(); }
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
    g_BootTick0 = armGetSystemTick();   // base for boot.log ms stamps
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
        case Menu::Action::LaunchHomebrew:
            menu::smi::OpenHomebrew(g_UI->HomebrewPath().c_str());
            g_Running = false;
            break;
        case Menu::Action::LaunchHomebrewApp:
            menu::smi::LaunchHomebrewApp(g_UI->HomebrewDonor(), g_UI->HomebrewPath().c_str());
            g_Running = false;
            break;
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
        // The daemon pushes a LibAppletArgs common-args storage (0x20 bytes)
        // FIRST, then our SystemStatus. Pop a couple of InData storages and read
        // whichever one matches SystemStatus by size (the common-args blob is a
        // different size), so we don't mistake the args header for the status
        // (that left the whole status zeroed -> no user, no suspended highlight).
        for (int i = 0; i < 3; i++) {
            AppletStorage st;
            if (R_FAILED(appletPopInData(&st))) break;
            s64 sz = 0;
            appletStorageGetSize(&st, &sz);
            if (sz == (s64)sizeof(status))
                appletStorageRead(&st, 0, &status, sizeof(status));
            appletStorageClose(&st);
        }
    }

    BootLog("applet: main enter (status user_valid=%d suspended=%d id=%016llX)",
            (int)accountUidIsValid(&status.selected_user), (int)status.has_suspended_app,
            (unsigned long long)status.suspended_app_id);

    menu::ui::g_sd_ok = true; // applet NPDM grants SD access

    menu::gfx::Gfx gfx;
    if (!gfx.Init()) {
        BootLog("applet: gfx.Init FAILED: %s", SDL_GetError());
        return 0; // sSystem will relaunch us
    }
    BootLog("applet: gfx.Init OK, entering loop");

    SDL_Joystick *joy = SDL_JoystickOpen(0);

    // Re-arm the touchscreen after SDL_Init (which reconfigures HID for its own
    // pad handling and can leave the touch screen unconfigured for our reads).
    hidInitializeTouchScreen();

    // Scope the Menu so its destructor (which frees icon/widget textures) runs
    // while the SDL renderer is still alive - gfx.Exit() is called after this
    // block closes.
    {
    Menu ui;
    g_UI = &ui;
    ui.Init(&gfx, status.selected_user, status.suspended_app_id, IsFirstRun());

    // Instant: show last run's cached game list on the very first frame. The
    // background loader then just verifies the ids and only re-resolves if the
    // installed set actually changed. g_NeedAppReload (true) drives that check.
    {
        auto cached = LoadAppList();
        if (!cached.empty()) {
            g_CachedIds.clear();
            for (auto &a : cached) g_CachedIds.push_back(a.app_id);
            ui.SetApps(std::move(cached));
        } else {
            ui.SetLoading(true);
        }
    }

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

    u64 g_NextVerify = armGetSystemTick();   // periodic app-list re-verify

    while (g_Running && appletMainLoop()) {
        // Re-verify the installed set every ~2s: the daemon doesn't push gamecard
        // events, so poll instead. The check is cheap (one NS list + id compare)
        // and only does real work when something actually changed (card in/out,
        // install, uninstall).
        if (!g_LoadRunning && armGetSystemTick() >= g_NextVerify) {
            g_NeedAppReload = true;
            g_NextVerify = armGetSystemTick() + armGetSystemTickFreq() * 2;
        }
        // Load the app list on a worker thread so the menu draws immediately
        // (with a "Loading" screen) instead of hanging until it's done.
        if (g_NeedAppReload && !g_LoadRunning) {
            g_NeedAppReload = false;
            g_LoadDone = false;
            g_LoadNoChange = false;
            g_LoadResult.clear();
            if (R_SUCCEEDED(threadCreate(&g_LoadThread, LoadAppsTrampoline, nullptr,
                                         nullptr, 0x10000, 0x3B, -2))) {
                threadStart(&g_LoadThread);
                g_LoadRunning = true;
            } else {
                ui.SetApps(LoadAppEntries());   // fallback: synchronous
            }
        }
        if (g_LoadRunning && g_LoadDone) {
            threadWaitForExit(&g_LoadThread);
            threadClose(&g_LoadThread);
            g_LoadRunning = false;
            if (!g_LoadNoChange) {              // list changed -> apply the fresh one
                g_CachedIds.clear();
                for (auto &a : g_LoadResult) g_CachedIds.push_back(a.app_id);
                ui.SetApps(std::move(g_LoadResult));
            }
            ui.SetLoading(false);
        }
        menu::smi::PollMessages(cbs);

        // Action buttons via events (no repeat); directions are polled below.
        // SDL maps touch to mouse events; keep the button state across frames --
        // a still finger sends no motion, so otherwise a held touch reads as a
        // release each frame and a long-press never accumulates.
        static bool mouse_down = false; static int mouse_x = 0, mouse_y = 0;
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
            } else if (ev.type == SDL_MOUSEBUTTONDOWN) {
                mouse_down = true;  mouse_x = ev.button.x; mouse_y = ev.button.y;
            } else if (ev.type == SDL_MOUSEBUTTONUP) {
                mouse_down = false;
            } else if (ev.type == SDL_MOUSEMOTION) {
                mouse_x = ev.motion.x; mouse_y = ev.motion.y;
            }
        }

        // Touch: prefer libnx HID, fall back to the mouse-mapped touch. Coords
        // are already in 1280x720 render space.
        {
            static bool was_touching = false;
            static int  last_tx = 0, last_ty = 0;
            HidTouchScreenState ts = {0};
            bool now_touch = false; int nx = 0, ny = 0;
            if (hidGetTouchScreenStates(&ts, 1) && ts.count > 0) {
                now_touch = true; nx = (int)ts.touches[0].x; ny = (int)ts.touches[0].y;
            } else if (mouse_down) {
                now_touch = true; nx = mouse_x; ny = mouse_y;
            }

            u64 tlaunch = 0;
            if (now_touch) {
                last_tx = nx; last_ty = ny;
                run(ui.OnTouch(was_touching ? 1 : 0, nx, ny, tlaunch), tlaunch);
                was_touching = true;
            } else if (was_touching) {
                run(ui.OnTouch(2, last_tx, last_ty, tlaunch), tlaunch);
                was_touching = false;
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
        // First frame is up; now do the heavier init (widgets) off the hot path.
        ui.InitDeferred();
    }
    } // ui destroyed here, before gfx.Exit(), so its textures free cleanly

    if (joy) SDL_JoystickClose(joy);
    gfx.Exit();
    return 0;
}
