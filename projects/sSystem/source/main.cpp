// sSystem - sLaunch backend system applet
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
#include <cstdarg>
#include <cstdlib>
#include <sys/stat.h>

#include <sl/smi/Protocol.hpp>
#include <sl/os/Applications.hpp>
#include <sl/sys/app/Application.hpp>
#include <sl/sys/la/LibraryApplet.hpp>
#include <sl/sys/ecs/ExternalContent.hpp>
#include <sl/sys/pwr/Power.hpp>

using namespace sl;
using namespace sl::smi;
using namespace sl::sys;   // la::, app::, ecs::

static constexpr const char *MenuNsoPath = "sdmc:/slaunch/bin/sMenu/main";

static AccountUid g_SelectedUser = {};
static bool       g_Running      = true;

// A request from the menu is carried out only AFTER the menu applet closes
// (its slot must be free). The menu sends the command then exits; the loop
// executes the pending action and relaunches the menu as appropriate.
enum class Pending {
    None, LaunchApp, ResumeApp, OpenAlbum, OpenUserPage, OpenNetConnect,
    OpenMiiEdit, OpenWebBrowser, OpenControllers, OpenHomebrewMenu,
    OpenHomebrew, LaunchHomebrewApp, OpenSystemSettings,
    Reboot, Shutdown, RebootPayload,
};
static Pending g_Pending      = Pending::None;
static u64     g_PendingAppId = 0;
static char    g_PendingHbPath[FS_MAX_PATH] = {};
static char    g_PendingHbArgv[512] = {};
static u64     g_PendingDonorId = 0;
static char    g_PendingPayload[FS_MAX_PATH] = {};
// While an NRO runs as an application in a donor's slot, this is the donor id so
// we can unregister the ECS override (restore the game) once it exits.
static u64     g_HbOverrideDonor = 0;

// Which applet slot the menu is served into. Firmware decides which program a
// slot launches (see ecs::MenuSlots), so the working slot is not the same on
// every console: we start with the preferred one and fall through the rest if
// the menu never actually comes up.
static size_t        g_MenuSlot       = 0;      // index into ecs::MenuSlots
static ecs::MenuSlot g_MenuSlotForced = {};     // from takeover.txt, if any
static bool          g_MenuSlotIsForced = false;
static bool          g_MenuSlotsExhausted = false;

// Set as soon as the running menu sends its first SMI command: proof that what
// the slot launched is really sMenu and not the firmware's own applet.
static bool g_MenuTalked     = false;
static u64  g_MenuLaunchTick = 0;
// A menu that exits this fast without ever talking to us did not come up.
static constexpr u64 MenuFastExitMs = 3000;
static int  g_MenuFastExits  = 0;

static void LaunchMenu();   // defined below

// Bring-up diagnostics: the daemon has full SD access via its NPDM.
static void DaemonLog(const char *fmt, ...) {
    mkdir("sdmc:/slaunch", 0777);
    FILE *fp = fopen("sdmc:/slaunch/daemon.log", "a");
    if (!fp) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);
    fputc('\n', fp);
    fclose(fp);
}

// ---------------------------------------------------------------------------

static void PushMenuEvent(MenuMessage evt) {
    AppletStorage st;
    if (R_FAILED(appletCreateStorage(&st, StorageSize))) return;
    CommandHeader hdr { Magic, static_cast<u32>(evt) };
    appletStorageWrite(&st, 0, &hdr, sizeof(hdr));
    la::PushStorage(st);
}

// A power request that could not be carried out (only reboot-to-payload can
// fail) leaves its reason here; the menu shows it once and deletes the file.
static void WritePowerError(const char *msg) {
    if (!msg || !msg[0]) return;
    mkdir("sdmc:/slaunch", 0777);
    mkdir("sdmc:/slaunch/config", 0777);
    FILE *fp = fopen(PowerErrorPath, "w");
    if (!fp) return;
    fprintf(fp, "%s\n", msg);
    fclose(fp);
}

// Drop the active homebrew-as-app ECS override (restore the donor game). Safe to
// call unconditionally; a no-op when no override is active.
static void CleanupHbOverride() {
    if (g_HbOverrideDonor == 0) return;
    ecs::UnregisterExternalContent(g_HbOverrideDonor);
    remove("sdmc:/slaunch/hbtarget.txt");
    DaemonLog("hbapp: donor 0x%016lx restored", g_HbOverrideDonor);
    g_HbOverrideDonor = 0;
}

// ---------------------------------------------------------------------------
// Dispatch one SystemMessage from sMenu
static void DispatchCommand(SystemMessage msg, const void *payload) {
    auto *as_user = static_cast<const PayloadSetUser*>(payload);
    auto *as_app  = static_cast<const PayloadAppLaunch*>(payload);

    // Most commands are queued as a pending action, carried out by the main
    // loop after the menu applet exits (so its slot is free). Terminate and
    // sleep act immediately since they do not need the applet slot.
    switch (msg) {
        case SystemMessage::SetSelectedUser:
            g_SelectedUser = as_user->uid;
            break;

        case SystemMessage::LaunchApplication:
            g_Pending      = Pending::LaunchApp;
            g_PendingAppId = as_app->app_id;
            break;
        case SystemMessage::ResumeApplication:
            g_Pending = Pending::ResumeApp;
            break;
        case SystemMessage::OpenAlbum:        g_Pending = Pending::OpenAlbum;        break;
        case SystemMessage::OpenNetConnect:   g_Pending = Pending::OpenNetConnect;   break;
        case SystemMessage::OpenSystemSettings: g_Pending = Pending::OpenSystemSettings; break;
        case SystemMessage::OpenMiiEdit:      g_Pending = Pending::OpenMiiEdit;      break;
        case SystemMessage::OpenUserPage:     g_Pending = Pending::OpenUserPage;     break;
        case SystemMessage::OpenWebBrowser:   g_Pending = Pending::OpenWebBrowser;   break;
        case SystemMessage::OpenControllers:  g_Pending = Pending::OpenControllers;  break;
        case SystemMessage::OpenHomebrewMenu: g_Pending = Pending::OpenHomebrewMenu; break;
        case SystemMessage::OpenHomebrew: {
            auto *hb = static_cast<const PayloadHomebrew*>(payload);
            g_Pending = Pending::OpenHomebrew;
            strncpy(g_PendingHbPath, hb->nro_path, sizeof(g_PendingHbPath) - 1);
            g_PendingHbPath[sizeof(g_PendingHbPath) - 1] = '\0';
            strncpy(g_PendingHbArgv, hb->argv, sizeof(g_PendingHbArgv) - 1);
            g_PendingHbArgv[sizeof(g_PendingHbArgv) - 1] = '\0';
            break;
        }
        case SystemMessage::LaunchHomebrewApplication: {
            auto *hb = static_cast<const PayloadHomebrew*>(payload);
            g_Pending = Pending::LaunchHomebrewApp;
            strncpy(g_PendingHbPath, hb->nro_path, sizeof(g_PendingHbPath) - 1);
            g_PendingHbPath[sizeof(g_PendingHbPath) - 1] = '\0';
            g_PendingDonorId = hb->donor_id;
            break;
        }

        case SystemMessage::TerminateApplication:
            if (app::g_AppRunning) app::Terminate();
            CleanupHbOverride();   // if it was a homebrew-as-app, restore the donor
            break;
        case SystemMessage::OpenPowerMenu:   // legacy opcode: plain sleep
        case SystemMessage::PowerSleep:
            pwr::Sleep();
            break;

        // Reboot / shutdown / chainload tear the whole system down, so they wait
        // for the menu applet to close like every other pending action does.
        case SystemMessage::PowerReboot:   g_Pending = Pending::Reboot;   break;
        case SystemMessage::PowerShutdown: g_Pending = Pending::Shutdown; break;
        case SystemMessage::RebootToPayload: {
            auto *rb = static_cast<const PayloadReboot*>(payload);
            g_Pending = Pending::RebootPayload;
            strncpy(g_PendingPayload, rb->payload_path, sizeof(g_PendingPayload) - 1);
            g_PendingPayload[sizeof(g_PendingPayload) - 1] = '\0';
            break;
        }
        case SystemMessage::RestartMenu:
        case SystemMessage::TerminateMenu:
        default:
            break;
    }
}

static bool HandleKeyboardRequest() {
    //stub
    return true;
}

// ---------------------------------------------------------------------------
// Carry out the queued action once the menu applet has closed. Games/resume
// leave the menu closed (the game runs); opening a system applet blocks until
// it exits and then the menu comes back.
static void RunPendingAction() {
    const Pending p = g_Pending;
    g_Pending = Pending::None;

    switch (p) {
        case Pending::LaunchApp:
            if (app::g_AppRunning) app::Terminate();
            CleanupHbOverride();   // dropping an override before a normal launch
            if (R_FAILED(app::Launch(g_PendingAppId, g_SelectedUser)))
                LaunchMenu(); // launch failed -> back to the menu
            break;
        case Pending::ResumeApp:
            if (app::g_AppRunning) app::Resume();
            else                   LaunchMenu();
            break;
        case Pending::OpenAlbum: {
            // Photo viewer: version 0x10000, 1-byte "show all album files for
            // home menu" arg.
            const u8 album_arg = AlbumLaArg_ShowAllAlbumFilesForHomeMenu;
            la::OpenSystemApplet(AppletId_LibraryAppletPhotoViewer, 0x10000,
                                 &album_arg, sizeof(album_arg));
            LaunchMenu();
            break;
        }
        case Pending::OpenUserPage: {
            // MyPage: show the selected user's profile (version 0x10000).
            FriendsLaArg arg = {};
            arg.hdr.type = FriendsLaArgType_ShowMyProfile;
            arg.hdr.uid  = g_SelectedUser;
            la::OpenSystemApplet(AppletId_LibraryAppletMyPage, 0x10000, &arg, sizeof(arg));
            LaunchMenu();
            break;
        }
        case Pending::OpenNetConnect: {
            // NetConnect: version 1, 4-byte type = HomeMenu(1).
            const u32 type = 1;
            la::OpenSystemApplet(AppletId_LibraryAppletNetConnect, 1, &type, sizeof(type));
            LaunchMenu();
            break;
        }
        case Pending::OpenSystemSettings: {
            // Full System Settings via the "set" applet (id 0x16). On retail this
            // applet (010000000000100E) is NOT installed -- settings are built
            // into stock qlaunch, which we replace -- so the launch just errors
            // out and we fall back to the menu. Works on dev/emulator units.
            Result rc = la::OpenSystemApplet(AppletId_LibraryAppletSet, 1);
            DaemonLog("settings: OpenSystemApplet(set) rc=0x%x", rc);
            LaunchMenu();
            break;
        }
        case Pending::OpenMiiEdit: {
            // MiiEdit takes NO common args (version -1) plus a MiiLaAppletInput.
            // Passing common args / a null input here is what crashed it.
            MiiLaAppletInput in = {};
            in.version          = hosversionAtLeast(10, 2, 0) ? 4 : 3;
            in.mode             = MiiLaAppletMode_ShowMiiEdit;
            in.special_key_code = MiiSpecialKeyCode_Normal;
            la::OpenSystemApplet(AppletId_LibraryAppletMiiEdit, -1, &in, sizeof(in));
            LaunchMenu();
            break;
        }
        case Pending::OpenWebBrowser: {
            // Build a normal web-page config and start the web applet with it.
            WebCommonConfig cfg;
            if (R_SUCCEEDED(webPageCreate(&cfg, "https://etonedemid.github.io/slaunch/"))) {
                webConfigSetWhitelist(&cfg, "^http*");
                la::OpenSystemApplet(AppletId_LibraryAppletWeb, cfg.version,
                                     &cfg.arg, sizeof(cfg.arg));
            }
            LaunchMenu();
            break;
        }
        case Pending::OpenControllers:
            // "Change grip/order" -- libnx's system helper builds the correct
            // private+public args and runs the applet itself.
            {
                HidLaControllerSupportArg arg;
                hidLaCreateControllerSupportArg(&arg);
                HidLaControllerSupportResultInfo info;
                hidLaShowControllerSupportForSystem(&info, &arg, false);
            }
            LaunchMenu();
            break;
        case Pending::OpenHomebrewMenu: {
            // Serve nx-hbloader into the album applet slot and launch it; with
            // no target it loads sdmc:/hbmenu.nro. Then unregister so the real
            // Album shortcut is restored. The menu (sMenu) has already exited,
            // so this doesn't touch its own ECS slot.
            remove("sdmc:/slaunch/hbtarget.txt"); // ensure no stale target -> hbmenu.nro
            Result rc = ecs::RegisterExternalContent(ecs::HbloaderProgramId, ecs::HbloaderExefsDir);
            DaemonLog("hbmenu: register hbloader rc=0x%x", rc);
            if (R_SUCCEEDED(rc)) {
                DaemonLog("hbmenu: launching hbloader applet (blocking)");
                la::OpenSystemApplet(ecs::HbloaderAppletId, 0); // blocks until hbmenu closes
                DaemonLog("hbmenu: hbloader applet exited");
                ecs::UnregisterExternalContent(ecs::HbloaderProgramId);
            }
            DaemonLog("hbmenu: relaunching sMenu");
            LaunchMenu();
            break;
        }
        case Pending::OpenHomebrew: {
            // Launch the chosen .nro via our target-aware hbloader fork: it reads
            // this one-shot file at startup and loads that .nro instead of hbmenu.
            if (FILE *tf = fopen("sdmc:/slaunch/hbtarget.txt", "w")) {
                fprintf(tf, "%s\n", g_PendingHbPath);
                fclose(tf);
            }
            Result rc = ecs::RegisterExternalContent(ecs::HbloaderProgramId, ecs::HbloaderExefsDir);
            DaemonLog("hb: register rc=0x%x nro=%s", rc, g_PendingHbPath);
            if (R_SUCCEEDED(rc)) {
                DaemonLog("hb: launching hbloader applet for %s (blocking)", g_PendingHbPath);
                la::OpenSystemApplet(ecs::HbloaderAppletId, 0); // blocks until it exits
                DaemonLog("hb: hbloader applet exited");
                ecs::UnregisterExternalContent(ecs::HbloaderProgramId);
            }
            remove("sdmc:/slaunch/hbtarget.txt"); // one-shot: don't retarget hbmenu
            DaemonLog("hb: relaunching sMenu");
            LaunchMenu();
            break;
        }
        case Pending::LaunchHomebrewApp: {
            // Run the .nro as an APPLICATION (full RAM/perms): serve hbloader into
            // a donor game's slot via ECS, then launch that title. hbloader reads
            // hbtarget.txt and boots the .nro. The ECS override is unregistered
            // when the app exits (see the main loop), restoring the game. ECS is
            // runtime-only, so a reboot also clears any stuck override.
            if (app::g_AppRunning) app::Terminate();
            CleanupHbOverride();   // drop any previous override first
            if (FILE *tf = fopen("sdmc:/slaunch/hbtarget.txt", "w")) {
                fprintf(tf, "%s\n", g_PendingHbPath);
                fclose(tf);
            }
            Result rc = ecs::RegisterExternalContent(g_PendingDonorId, ecs::HbloaderAppExefsDir);
            DaemonLog("hbapp: donor=0x%016lx register rc=0x%x nro=%s",
                      g_PendingDonorId, rc, g_PendingHbPath);
            if (R_SUCCEEDED(rc)) {
                Result lr = app::Launch(g_PendingDonorId, g_SelectedUser);
                DaemonLog("hbapp: Launch(0x%016lx) rc=0x%x", g_PendingDonorId, lr);
                if (R_SUCCEEDED(lr)) {
                    g_HbOverrideDonor = g_PendingDonorId;   // app runs; cleaned up on exit
                    DaemonLog("hbapp: running as app, will clean up on exit");
                } else {
                    ecs::UnregisterExternalContent(g_PendingDonorId);
                    remove("sdmc:/slaunch/hbtarget.txt");
                    LaunchMenu();
                }
            } else {
                ecs::UnregisterExternalContent(g_PendingDonorId);
                remove("sdmc:/slaunch/hbtarget.txt");
                LaunchMenu();
            }
            break;
        }
        // ---- power ----------------------------------------------------------
        // The game is closed first so it shuts down cleanly rather than being cut
        // off mid-frame by the reboot. Each of these only comes back if it failed,
        // in which case the menu is put back up (with the reason, for payloads).
        case Pending::Reboot:
            if (app::g_AppRunning) app::Terminate();
            CleanupHbOverride();
            // Stop the menu holder completely before reboot to avoid leaving
            // stale state that can block bpcRebootSystem.
            la::StopMenu();
            DaemonLog("power: calling Reboot()");
            pwr::Reboot();
            DaemonLog("power: reboot did not take (returned)");
            LaunchMenu();
            break;
        case Pending::Shutdown:
            if (app::g_AppRunning) app::Terminate();
            CleanupHbOverride();
            la::StopMenu();
            DaemonLog("power: calling Shutdown()");
            pwr::Shutdown();
            DaemonLog("power: shutdown did not take (returned)");
            LaunchMenu();
            break;
        case Pending::RebootPayload: {
            if (app::g_AppRunning) app::Terminate();
            CleanupHbOverride();
            const auto r = pwr::RebootToPayload(g_PendingPayload);
            DaemonLog("power: payload=%s failed (%s)", g_PendingPayload,
                      pwr::PayloadResultText(r));
            WritePowerError(pwr::PayloadResultText(r));
            LaunchMenu();
            break;
        }

        case Pending::None:
        default:
            LaunchMenu();
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
        PayloadReboot    reboot;
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
            g_MenuTalked = true;   // whatever is in the slot really is sMenu
            DispatchCommand(static_cast<SystemMessage>(hdr.val), &payload);
        }
    }
}

// ---------------------------------------------------------------------------
// qlaunch-only applet messages that libnx's AppletMessage_ enum omits (only the
// system applet receives them). Values from Horizon `am` / uLaunch.
enum SystemAppletMessage : u32 {
    Msg_DetectShortPressingHomeButton  = 20,
    Msg_DetectShortPressingPowerButton = 22,
    Msg_FinishedSleepSequence          = 26,
    Msg_AutoPowerDown                  = 29,
    Msg_SdCardRemoved                  = 33,
};

static void HandleSleep() {
    pwr::Sleep();
}

// The SD card was physically removed while the console is on. Everything sMenu
// and the daemon serve lives on that card (ECS overrides read straight from it),
// so we must not keep running. Show the warning on the menu if it is up, then
// reboot after ~3s. When a game is in the foreground the menu isn't resident and
// can't be relaunched (its files are on the pulled card), so we can only reboot.
static void HandleSdCardRemoved() {
    DaemonLog("sd: card removed while on -> warn + reboot in 3s");
    if (la::IsMenuAlive())
        PushMenuEvent(MenuMessage::SdCardEjected);
    svcSleepThread(3'000'000'000ULL);   // leave the warning up long enough to read
    pwr::Reboot();
}

// HOME button toggles between a game and the menu, like the real HOME Menu:
//  - in a game        -> suspend it (keep it running) and show the menu
//  - in the menu with
//    a suspended game -> close the menu and resume the game
static void HandleHomeButton() {
    if (app::g_AppRunning && app::g_AppHasFocus) {
        // Ignore HOME while the app is still coming up: suspending mid-launch
        // races the foreground handshake and faults am (crash). The user can
        // press HOME again a moment later.
        if (!app::CanSuspend()) {
            DaemonLog("home: ignored (app still launching)");
            return;
        }
        // In a game: suspend (do NOT terminate) and bring the menu up over it.
        const u64 t0 = armGetSystemTick();
        app::FocusSystem();
        const u64 t1 = armGetSystemTick();
        if (!la::IsMenuAlive())
            LaunchMenu();
        const u64 t2 = armGetSystemTick();
        const u64 hz = armGetSystemTickFreq();
        DaemonLog("home: FocusSystem=%llums LaunchMenu=%llums",
                  (unsigned long long)((t1 - t0) * 1000 / hz),
                  (unsigned long long)((t2 - t1) * 1000 / hz));
    } else if (la::IsMenuAlive() && app::g_AppRunning) {
        // In the menu with a game suspended: resume the game.
        la::StopMenu();
        app::Resume();
    }
}

// Process system-level applet messages (HOME, power/sleep, ...). Called from the
// main loop whenever the menu is up or a game is running.
static void PumpAppletMessages() {
    u32 msg = 0;
    while (R_SUCCEEDED(appletGetMessage(&msg))) {
        switch (msg) {
            case Msg_DetectShortPressingHomeButton:  HandleHomeButton(); break;
            case Msg_DetectShortPressingPowerButton:
            case Msg_AutoPowerDown:                  HandleSleep();      break;
            case Msg_SdCardRemoved:                  HandleSdCardRemoved(); break;
            default: break;
        }
    }
}

// ---------------------------------------------------------------------------
// Menu slot: where sMenu is served, and what to do when it does not come up.

// sdmc:/slaunch/config/takeover.txt -- one program id, e.g. "0x010000000000100B".
// Read once at boot: that slot is tried before the built-in list.
static void LoadMenuSlotOverride() {
    FILE *fp = fopen(ecs::MenuSlotOverridePath, "r");
    if (!fp) return;
    char line[64] = {};
    const bool got = fgets(line, sizeof(line), fp) != nullptr;
    fclose(fp);
    if (!got) return;

    const u64 program_id = strtoull(line, nullptr, 16);   // "0x..." or bare hex
    const AppletId applet_id = ecs::AppletIdForProgramId(program_id);
    if (applet_id == AppletId_None) {
        DaemonLog("menu: takeover.txt names 0x%016lx, which is not a known applet"
                  " program -- ignoring", program_id);
        return;
    }
    g_MenuSlotForced   = { applet_id, "takeover.txt", { program_id, 0 }, 1 };
    g_MenuSlotIsForced = true;
    DaemonLog("menu: takeover.txt forces program 0x%016lx (applet 0x%x)",
              program_id, (int)applet_id);
}

static const ecs::MenuSlot &CurrentMenuSlot() {
    if (g_MenuSlotIsForced) return g_MenuSlotForced;
    return ecs::MenuSlots[g_MenuSlot];
}

// Move to the next candidate slot. Returns false when there is nothing left to
// try, in which case the caller keeps using the current slot -- retrying
// forever beats a console that is stuck on black.
static bool AdvanceMenuSlot(const char *why) {
    // A slot pinned by hand gets the first go, but a typo in takeover.txt must
    // not cost the user their console: drop back to the built-in list.
    if (g_MenuSlotIsForced) {
        g_MenuSlotIsForced = false;
        g_MenuSlot         = 0;
        g_MenuFastExits    = 0;
        DaemonLog("menu: %s -- ignoring takeover.txt and going back to the"
                  " built-in slot list ('%s')", why, ecs::MenuSlots[0].name);
        return true;
    }
    if (g_MenuSlot + 1 >= ecs::MenuSlotCount) {
        if (!g_MenuSlotsExhausted) {
            g_MenuSlotsExhausted = true;
            DaemonLog("menu: %s, and no applet slot is left to try -- the menu"
                      " will keep retrying on '%s'. Put a program id in %s to"
                      " point it at a different slot.",
                      why, CurrentMenuSlot().name, ecs::MenuSlotOverridePath);
        }
        return false;
    }
    g_MenuSlot++;
    g_MenuFastExits = 0;
    DaemonLog("menu: %s -> falling back to the '%s' slot",
              why, ecs::MenuSlots[g_MenuSlot].name);
    return true;
}

// The menu applet closed. If it never sent a single command and was only up for
// an instant, the slot launched the firmware's own applet instead of sMenu (our
// ECS override was registered for a program id this firmware does not map that
// slot to) -- that applet opens and closes immediately. Two in a row and we try
// the next slot instead of relaunching the same nothing forever.
static void NoteMenuExited() {
    if (g_MenuTalked) { g_MenuFastExits = 0; return; }

    const u64 alive_ms = (armGetSystemTick() - g_MenuLaunchTick) * 1000
                       / armGetSystemTickFreq();
    if (alive_ms >= MenuFastExitMs) { g_MenuFastExits = 0; return; }

    g_MenuFastExits++;
    DaemonLog("menu: slot '%s' exited after %llums without a word (%d in a row)"
              " -- either this firmware launches a different program from that"
              " slot, or sMenu crashed on startup",
              CurrentMenuSlot().name, (unsigned long long)alive_ms, g_MenuFastExits);
    if (g_MenuFastExits >= 2 && !AdvanceMenuSlot("menu never came up")) {
        // Nothing left to switch to, so the relaunch below is a retry of what
        // just failed. Pace it: without this the daemon spins the applet (and
        // the log) as fast as the firmware can open and close it.
        svcSleepThread(1'000'000'000ULL);
    }
}

static void LaunchMenu() {
    SystemStatus status = {};
    status.selected_user     = g_SelectedUser;
    status.suspended_app_id  = app::g_AppId;
    status.has_suspended_app = app::g_AppRunning;

    const ecs::MenuSlot &slot = CurrentMenuSlot();

    // Serve sMenu's SD exefs folder into every program id this slot is known to
    // resolve to (the mapping differs between firmwares), then launch the slot
    // as a library applet with the status blob as input data.
    for (u8 i = 0; i < slot.program_id_count; i++) {
        Result ecs_rc = ecs::RegisterExternalContent(slot.program_ids[i], ecs::MenuExefsDir);
        DaemonLog("RegisterExternalContent(0x%016lx, %s) rc=0x%x",
                  slot.program_ids[i], ecs::MenuExefsDir, ecs_rc);
    }

    Result la_rc = la::LaunchMenu(slot.applet_id, &status, sizeof(status));
    DaemonLog("LaunchMenu(slot=%s applet=0x%x) rc=0x%x",
              slot.name, (int)slot.applet_id, la_rc);

    g_MenuTalked     = false;
    g_MenuLaunchTick = armGetSystemTick();

    if (R_FAILED(la_rc)) {
        // The slot would not start at all (its program is not installed on this
        // firmware, am refused it, ...). Try the next candidate before giving
        // up: a fatal at least names an error, but only the log explains it.
        if (AdvanceMenuSlot("slot would not start")) {
            LaunchMenu();
            return;
        }
        fatalThrow(MAKERESULT(Module_Libnx, LibnxError_NotFound));
    }
}

// ---------------------------------------------------------------------------
// Stratosphere sysmodule entry. libstratosphere's crt0 provides __appInit/main
// and calls these hooks; we must set up both heaps ourselves and never return
// (qlaunch exiting would crash am). Model adapted from uLaunch's uSystem.
extern "C" {
    extern u8 *fake_heap_start;
    extern u8 *fake_heap_end;
    // Provided by libstratosphere's libnx shim; we set them for a system applet.
    extern AppletType __nx_applet_type;
    extern u32        __nx_fs_num_sessions;
}

namespace {
    constexpr size_t StratHeapSize = 16 * 1024 * 1024;
    alignas(ams::os::MemoryPageSize) constinit u8 g_StratHeap[StratHeapSize];
    constexpr size_t LibnxHeapSize = 4 * 1024 * 1024;
    alignas(ams::os::MemoryPageSize) constinit u8 g_LibnxHeap[LibnxHeapSize];
}

// Service init results, kept until the SD card is mounted: DaemonLog needs
// sdmc:, and everything below it runs before that exists. Without this a
// service the firmware refuses is a fatal on a black screen with nothing
// written down anywhere; with it the log names the service and its rc.
namespace {

    struct InitStep {
        const char *name;
        Result      rc;
        bool        critical;   // the daemon cannot be the HOME Menu without it
    };
    InitStep g_InitSteps[16] = {};
    size_t   g_InitStepCount = 0;

    Result RecordInit(const char *name, Result rc, bool critical) {
        if (g_InitStepCount < sizeof(g_InitSteps) / sizeof(g_InitSteps[0]))
            g_InitSteps[g_InitStepCount++] = { name, rc, critical };
        return rc;
    }

}

namespace ams {

    namespace init {

        void InitializeSystemModule() {
            __nx_applet_type     = AppletType_SystemApplet;
            __nx_fs_num_sessions = 3;

            // sm and fs come first and are not survivable: without them there
            // is no SD card, so not even a log to say what went wrong.
            R_ABORT_UNLESS(sm::Initialize());
            R_ABORT_UNLESS(fsInitialize());

            // Firmware version FIRST. Every libnx service init below picks its
            // command ids and structure layouts from it, and with the version
            // still unset they all take the 1.0.0 path -- which is not what any
            // console we support actually speaks. libstratosphere may already
            // have set it during startup, so only fill it in when it is empty.
            RecordInit("setsys", setsysInitialize(), true);
            if (hosversionGet() == 0) {
                SetSysFirmwareVersion fw = {};
                if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw)))
                    hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro) | BIT(31));
            }

            RecordInit("applet",   appletInitialize(), true);
            RecordInit("hid",      hidInitialize(), true);
            RecordInit("account",  accountInitialize(AccountServiceType_System), true);
            RecordInit("ns",       nsInitialize(), true);
            RecordInit("set",      setInitialize(), true);
            RecordInit("ldr:shel", ldrShellInitialize(), true);  // no ECS -> no menu

            // Nothing the menu cannot live without: the daemon only reads a
            // battery level / network state / clock through these, so a
            // firmware that will not hand one over gets a working menu and a
            // line in the log rather than a fatal on a black screen.
            RecordInit("psm",     psmInitialize(), false);
            RecordInit("nifm",    nifmInitialize(NifmServiceType_User), false);
            RecordInit("time",    timeInitialize(), false);
            RecordInit("pmshell", pmshellInitialize(), false);

            // SD last: it is not always ready before the other services init.
            R_ABORT_UNLESS(fsdevMountSdmc());

            // The log exists from here. Write down what happened above before
            // acting on it, so a console that cannot boot the menu still leaves
            // a readable reason on the card.
            remove("sdmc:/slaunch/daemon.log");
            const u32 ver = hosversionGet();
            DaemonLog("daemon: firmware %u.%u.%u", HOSVER_MAJOR(ver),
                      HOSVER_MINOR(ver), HOSVER_MICRO(ver));
            if (ver == 0)
                DaemonLog("daemon: WARNING firmware version unknown -- libnx will"
                          " speak 1.0.0 to every service");
            else if (hosversionBefore(9, 0, 0))
                DaemonLog("daemon: WARNING firmware is below 9.0.0, which is untested");

            for (size_t i = 0; i < g_InitStepCount; i++) {
                if (R_SUCCEEDED(g_InitSteps[i].rc)) continue;
                DaemonLog("daemon: %s init failed rc=0x%x%s", g_InitSteps[i].name,
                          g_InitSteps[i].rc,
                          g_InitSteps[i].critical ? " (fatal)" : " -- continuing without it");
            }
            for (size_t i = 0; i < g_InitStepCount; i++)
                if (g_InitSteps[i].critical)
                    R_ABORT_UNLESS(g_InitSteps[i].rc);
        }

        void FinalizeSystemModule() {
            pmshellExit();  ldrShellExit(); timeExit();   setExit();
            setsysExit();   nifmExit();     psmExit();     nsExit();
            accountExit();  hidExit();      appletExit();
            fsdevUnmountAll(); fsExit();
        }

        void Startup() {
            // libstratosphere heap (new/delete/malloc via ams) + libnx heap
            // (internal malloc_r used by the stdlib).
            init::InitializeAllocator(g_StratHeap, StratHeapSize);
            fake_heap_start = g_LibnxHeap;
            fake_heap_end   = g_LibnxHeap + LibnxHeapSize;
        }

    }

    void Main() {
        DaemonLog("daemon: Main enter (stratosphere sysmodule, qlaunch)");

        LoadMenuSlotOverride();

        // Required system-applet startup: apply the idle/power policy. Without
        // this qlaunch is not fully "the home menu", and the library applets it
        // launches never acquire the foreground (they render but stay hidden).
        Result idle_rc = appletLoadAndApplyIdlePolicySettings();
        DaemonLog("daemon: idle policy rc=0x%x", idle_rc);

        // Select default user.
        accountGetPreselectedUser(&g_SelectedUser);
        if (!accountUidIsValid(&g_SelectedUser)) {
            AppletHolder sel;
            if (R_SUCCEEDED(appletCreateLibraryApplet(
                    &sel, AppletId_LibraryAppletPlayerSelect,
                    LibAppletMode_AllForeground))) {
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

        ::LaunchMenu();

        // qlaunch must never terminate.
        while (true) {
            // Drain commands the menu queued (readable even just after it
            // exits, so the final launch/open request is not missed).
            if (la::g_MenuRunning)
                DrainSMIQueue();

            // HOME / power-button / sleep messages from Horizon.
            PumpAppletMessages();

            // A running game exited on its own -> back to the menu. Only check
            // when the menu is NOT up: while the menu is open over a suspended
            // game the game is intentionally backgrounded, and its suspend
            // state-change must not be misread as an exit (which would close it).
            if (!la::IsMenuAlive() && app::g_AppRunning && app::Update()) {
                DaemonLog("app: exited naturally (hb_override=0x%016lx)", g_HbOverrideDonor);
                CleanupHbOverride();  // homebrew-as-app exited -> restore the donor
                DaemonLog("app: relaunching sMenu after app exit");
                ::LaunchMenu();
            }

            // The menu applet closed -> carry out whatever it asked for.
            if (la::g_MenuRunning && !la::IsMenuAlive()) {
                la::StopMenu();
                NoteMenuExited();   // may switch slots if it never came up
                RunPendingAction();
            }

            svcSleepThread(16'666'666ULL);
        }
    }

    NORETURN void Exit(int) {
        AMS_ABORT("qlaunch (sSystem) must not exit");
    }

}
