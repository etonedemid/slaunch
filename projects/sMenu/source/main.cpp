// sMenu — sLaunch text-based menu library applet
// Runs in the eShop slot via Atmosphere ECS redirect.
// Receives SystemStatus from sSystem at launch via input storage.
// Sends SMI commands back to sSystem via appletPushToGeneralChannel.

#include <switch.h>
#include <cstring>
#include <cstdio>
#include <vector>

#include <sl/smi/Protocol.hpp>
#include <sl/os/Applications.hpp>
#include <sl/menu/smi/Commands.hpp>
#include <sl/menu/smi/MessageHandler.hpp>
#include <sl/menu/ui/TextUI.hpp>

using namespace sl;

static menu::ui::TextUI *g_UI = nullptr;
static bool               g_NeedAppReload = true;
static bool               g_Running       = true;

// ---------------------------------------------------------------------------
// Load the installed application list and push it to the UI
static void ReloadApps() {
    std::vector<NsApplicationRecord> records;
    if (R_FAILED(os::ListApplicationRecords(records)) || records.empty()) {
        if (g_UI) {
            g_UI->SetApps({});
            g_UI->SetStatus("No installed games found.");
        }
        return;
    }

    // Collect IDs
    std::vector<u64> ids;
    ids.reserve(records.size());
    for (auto &r : records) ids.push_back(r.application_id);

    // Fetch live view flags
    std::vector<NsApplicationView> views;
    os::ListApplicationViews(ids, views);

    // Build UI entries, filter to launchable titles only
    std::vector<menu::ui::AppEntry> entries;
    entries.reserve(records.size());

    for (size_t i = 0; i < records.size(); i++) {
        u64 app_id = records[i].application_id;

        // Check view flags
        bool can_launch = true;
        bool gamecard   = false;
        bool needs_upd  = false;
        if (i < views.size()) {
            auto flags = views[i].unk_x0; // NsApplicationView flags word
            // CanLaunch = bit 8, IsGameCard = bit 6, NeedsUpdate = bit 9
            // (flags struct layout: flags at offset 0x10 in NsApplicationView)
            // Use the raw struct via reinterpret
            auto *vf = reinterpret_cast<const u32*>(&views[i]);
            u32 f = vf[4]; // offset 0x10 = 16 bytes = 4 u32s from start
            can_launch  = (f & BIT(8)) != 0;
            gamecard    = (f & BIT(6)) != 0;
            needs_upd   = (f & BIT(9)) != 0;
        }

        if (!can_launch) continue;

        // Fetch NACP for the name
        NsApplicationControlData ctrl = {};
        std::string name;
        if (R_SUCCEEDED(os::GetApplicationControl(app_id, ctrl))) {
            name = os::GetAppName(ctrl.nacp);
        } else {
            char tmp[32];
            snprintf(tmp, sizeof(tmp), "0x%016llX", (unsigned long long)app_id);
            name = tmp;
        }

        entries.push_back({ app_id, std::move(name), gamecard, needs_upd });
    }

    if (g_UI) {
        g_UI->SetApps(std::move(entries));
        g_UI->SetStatus("App list refreshed.");
    }
    g_NeedAppReload = false;
}

// ---------------------------------------------------------------------------
// sSystem → sMenu event callbacks

static void OnHomeRequested() {
    // HOME while a game is running — sSystem already handles the foreground
    // transition; we just need to ensure we're showing the right state.
    if (g_UI) g_UI->MarkDirty();
}

static void OnSdEjected() {
    if (g_UI) {
        g_UI->SetStatus("SD card ejected! Some games may be unavailable.");
        g_NeedAppReload = true;
    }
}

static void OnSleepFinished() {
    if (g_UI) {
        g_UI->MarkDirty();
    }
}

static void OnAppListChanged() {
    g_NeedAppReload = true;
}

// ---------------------------------------------------------------------------
extern "C" void __appInit() {
    smInitialize();
    fsInitialize();
    fsdevMountSdmc();

    appletInitialize();
    hidInitialize();
    accountInitialize(AccountServiceType_System);
    nsInitialize();
    setsysInitialize();
    setInitialize();
    timeInitialize();

    // Mount our romfs (optional — for sounds, fonts in the future)
    // romfsMountFromFsdev("romfs:", 0, nullptr);
}

extern "C" void __appExit() {
    // romfsExit();
    timeExit();
    setExit();
    setsysExit();
    nsExit();
    accountExit();
    hidExit();
    appletExit();
    fsdevUnmountAll();
    fsExit();
    smExit();
}

// ---------------------------------------------------------------------------
int main() {
    // Initialize libnx console (uses framebuffer, no GPU required)
    PrintConsole *con = consoleInit(nullptr);
    (void)con;

    // Read SystemStatus from our first input storage (pushed by sSystem)
    smi::SystemStatus status = {};
    {
        AppletStorage st;
        if (R_SUCCEEDED(appletPopInData(&st))) {
            appletStorageRead(&st, 0, &status, sizeof(status));
            appletStorageClose(&st);
        }
    }

    // Initialize UI
    menu::ui::TextUI ui;
    g_UI = &ui;
    ui.Init(status.selected_user, status.suspended_app_id);

    if (status.has_suspended_app)
        ui.SetSuspendedApp(status.suspended_app_id);

    // Register message callbacks
    menu::smi::MessageCallbacks cbs;
    cbs.on_home_request    = OnHomeRequested;
    cbs.on_sd_ejected      = OnSdEjected;
    cbs.on_sleep_finished  = OnSleepFinished;
    cbs.on_app_list_changed = OnAppListChanged;

    // Load the initial app list
    g_NeedAppReload = true;

    // Main loop
    while (g_Running && appletMainLoop()) {
        // 1. Reload app list if needed (lazy — only when flagged)
        if (g_NeedAppReload) ReloadApps();

        // 2. Poll async messages from sSystem
        menu::smi::PollMessages(cbs);

        // 3. Process input
        u64 launch_id = 0;
        auto action = ui.Update(launch_id);

        switch (action) {
            case menu::ui::TextUI::Action::LaunchApp:
                if (status.has_suspended_app && launch_id == status.suspended_app_id) {
                    // Resume instead of re-launch
                    menu::smi::ResumeApp();
                    ui.SetStatus("Resuming game...");
                } else {
                    if (status.has_suspended_app) {
                        // Terminate the old game first
                        menu::smi::TerminateApp();
                        status.has_suspended_app = false;
                        status.suspended_app_id  = 0;
                        ui.ClearSuspendedApp();
                    }
                    menu::smi::LaunchApp(launch_id);
                    status.suspended_app_id  = launch_id;
                    status.has_suspended_app = true;
                    ui.SetSuspendedApp(launch_id);
                    ui.SetStatus("Launching...");
                }
                break;

            case menu::ui::TextUI::Action::ResumeApp:
                menu::smi::ResumeApp();
                ui.SetStatus("Resuming...");
                break;

            case menu::ui::TextUI::Action::TerminateApp:
                menu::smi::TerminateApp();
                status.has_suspended_app = false;
                status.suspended_app_id  = 0;
                ui.ClearSuspendedApp();
                ui.SetStatus("Game closed.");
                break;

            case menu::ui::TextUI::Action::OpenAlbum:
                menu::smi::OpenAlbum();
                break;

            case menu::ui::TextUI::Action::OpenUserPage:
                menu::smi::OpenUserPage();
                break;

            case menu::ui::TextUI::Action::OpenNetConnect:
                menu::smi::OpenNetConnect();
                break;

            case menu::ui::TextUI::Action::OpenPower:
                menu::smi::OpenPowerMenu();
                break;

            case menu::ui::TextUI::Action::Quit:
                g_Running = false;
                break;

            default:
                break;
        }

        // 4. Render (only if dirty)
        ui.Render();

        // ~60fps
        svcSleepThread(16'666'666ULL);
    }

    // Restore cursor on exit
    printf(ESC "?25h");
    consoleUpdate(nullptr);

    return 0;
}
