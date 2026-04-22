// sSystem — sLaunch backend system applet
// Replaces qlaunch (title ID 0100000000001000) via Atmosphere content override.
// Responsibilities:
//   - Launch/suspend/terminate applications
//   - Manage sMenu as a library applet via ECS
//   - Handle HOME button, sleep, gamecard events
//   - Process SMI commands from sMenu

#include <switch.h>
#include <stratosphere.hpp>
#include <cstring>
#include <cstdio>

#include <sl/smi/Protocol.hpp>
#include <sl/os/Applications.hpp>
#include <sl/sys/app/Application.hpp>
#include <sl/sys/la/LibraryApplet.hpp>
#include <sl/sys/ecs/ExternalContent.hpp>

using namespace sl;
using namespace sl::smi;

static constexpr const char *MenuNsoPath = "sdmc:/slaunch/bin/sMenu/main";

static AccountUid g_SelectedUser = {};
static bool       g_Running      = true;

// ---------------------------------------------------------------------------

static void PushMenuEvent(MenuMessage evt) {
    AppletStorage st;
    if (R_FAILED(appletCreateStorage(&st, StorageSize))) return;
    CommandHeader hdr { Magic, static_cast<u32>(evt) };
    appletStorageWrite(&st, 0, &hdr, sizeof(hdr));
    la::PushStorage(st);
}

// ---------------------------------------------------------------------------
// Dispatch one SystemMessage from sMenu
static void DispatchCommand(SystemMessage msg, const void *payload) {
    auto *as_user = static_cast<const PayloadSetUser*>(payload);
    auto *as_app  = static_cast<const PayloadAppLaunch*>(payload);

    switch (msg) {
        case SystemMessage::SetSelectedUser:
            g_SelectedUser = as_user->uid;
            break;

        case SystemMessage::LaunchApplication:
            if (!app::g_AppRunning)
                app::Launch(as_app->app_id, g_SelectedUser);
            break;

        case SystemMessage::ResumeApplication:
            if (app::g_AppRunning)
                app::Resume();
            break;

        case SystemMessage::TerminateApplication:
            if (app::g_AppRunning)
                app::Terminate();
            break;

        case SystemMessage::OpenAlbum:
            la::OpenSystemApplet(AppletId_LibraryAppletPhotoViewer, nullptr, 0, nullptr, 0);
            break;

        case SystemMessage::OpenNetConnect: {
            const u8 net_args[4] = {};
            la::OpenSystemApplet(AppletId_LibraryAppletNetConnect,
                                 net_args, sizeof(net_args), nullptr, 0);
            break;
        }

        case SystemMessage::OpenMiiEdit:
            la::OpenSystemApplet(AppletId_LibraryAppletMiiEdit, nullptr, 0, nullptr, 0);
            break;

        case SystemMessage::OpenUserPage:
            la::OpenSystemApplet(AppletId_LibraryAppletMyPage, nullptr, 0, nullptr, 0);
            break;

        case SystemMessage::OpenPowerMenu:
            appletStartSleepSequence(true);
            break;

        case SystemMessage::RestartMenu:
            la::StopMenu();
            break;

        case SystemMessage::TerminateMenu:
            g_Running = false;
            break;

        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// Drain all pending SMI storages from sMenu's output queue
static void DrainSMIQueue() {
    union {
        PayloadSetUser   set_user;
        PayloadAppLaunch app_launch;
        PayloadHomebrew  homebrew;
        u8               raw[sizeof(PayloadHomebrew)];
    } payload = {};

    AppletStorage st;
    while (R_SUCCEEDED(appletHolderPopOutData(&la::g_MenuHolder, &st))) {
        CommandHeader hdr = {};
        appletStorageRead(&st, 0, &hdr, sizeof(hdr));
        memset(&payload, 0, sizeof(payload));
        appletStorageRead(&st, sizeof(hdr), &payload, sizeof(payload));
        appletStorageClose(&st);

        if (hdr.magic == Magic) {
            DispatchCommand(static_cast<SystemMessage>(hdr.val), &payload);
        }
    }
}

// ---------------------------------------------------------------------------
// Process system-level applet messages (HOME, sleep, etc.)
static void HandleGeneralChannel() {
    AppletMessage msg;
    while (R_SUCCEEDED(appletGetMessage(&msg))) {
        switch (msg) {
            case AppletMessage_HomeButtonChanged:
                PushMenuEvent(MenuMessage::HomeRequested);
                break;
            case AppletMessage_SdCardOut:
                PushMenuEvent(MenuMessage::SdCardEjected);
                break;
            case AppletMessage_ResumeOperationForSleep:
                PushMenuEvent(MenuMessage::SleepFinished);
                break;
            default:
                break;
        }
    }
}

// ---------------------------------------------------------------------------
static void LaunchMenu() {
    SystemStatus status = {};
    status.selected_user     = g_SelectedUser;
    status.suspended_app_id  = app::g_AppId;
    status.has_suspended_app = app::g_AppRunning;

    ecs::RegisterMenu(MenuNsoPath);

    if (R_FAILED(la::LaunchMenu(ecs::MenuAppletId)))
        fatalThrow(MAKERESULT(Module_Libnx, LibnxError_NotFound));

    la::PushToMenu(&status, sizeof(status));
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
    nssuInitialize();
    psmInitialize();
    nifmInitialize(NifmServiceType_User);
    setsysInitialize();
    setInitialize();
    timeInitialize();
    ldrShellInitialize();
    pmshellInitialize();
}

extern "C" void __appExit() {
    ecs::UnregisterMenu();
    pmshellFinalize();
    ldrShellFinalize();
    timeExit();
    setExit();
    setsysExit();
    nifmExit();
    psmExit();
    nssuExit();
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
    // Select default user
    accountGetPreselectedUser(&g_SelectedUser);

    if (!accountUidIsValid(&g_SelectedUser)) {
        // Invoke user-select applet
        AppletHolder sel;
        if (R_SUCCEEDED(appletCreateLibraryApplet(
                &sel, AppletId_LibraryAppletPlayerSelect,
                LibAppletMode_AllForeground)))
        {
            LibAppletArgs args;
            libappletArgsCreate(&args, 0);
            libappletArgsPush(&args, &sel);
            appletHolderStart(&sel);
            while (!appletHolderCheckFinished(&sel))
                svcSleepThread(16'666'666ULL);

            struct { u64 result; AccountUid uid; } out = {};
            libappletPopOutData(&sel, &out, sizeof(out), nullptr);
            if (accountUidIsValid(&out.uid))
                g_SelectedUser = out.uid;
            appletHolderClose(&sel);
        }
    }

    LaunchMenu();

    while (g_Running && appletMainLoop()) {
        // Drain SMI commands sent by sMenu
        if (la::g_MenuRunning)
            DrainSMIQueue();

        // System events (HOME, sleep, gamecard)
        HandleGeneralChannel();

        // Check if the running application has naturally exited
        if (app::g_AppRunning && app::Update())
            app::FocusSystem();

        // Check if sMenu has exited (crash or restart request)
        if (!la::IsMenuAlive() && g_Running) {
            la::StopMenu();
            ecs::UnregisterMenu();
            svcSleepThread(50'000'000ULL);
            LaunchMenu();
        }

        svcSleepThread(16'666'666ULL);
    }

    la::StopMenu();
    if (app::g_AppRunning)
        app::Terminate();

    return 0;
}
