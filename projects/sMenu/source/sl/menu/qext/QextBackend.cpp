// The SMI commands, carried out in-process for the qlaunch-ext build. sSystem's
// RunPendingAction is the reference; what differs is that the loader tracks what
// is on screen, so nothing here relaunches a menu or waits for an applet to end.
#include <sl/menu/smi/QextBackend.hpp>
#include <sl/smi/Protocol.hpp>
#include <sl/sys/HbLaunchRequest.hpp>
#include <sl/sys/hb/LaunchQueue.hpp>
#include <sl/sys/pwr/Power.hpp>
#include <sl/menu/dbg/Crash.hpp>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>

// The loader's exports.
extern "C" {
    Result qext_launch_title(u64 tid);
    Result qext_launch_title_as(u64 tid, AccountUid uid);   // scripts/qext/loader.patch
    Result qext_resume_game(void);
    Result qext_terminate_game(void);
    Result qext_sleep(void);
    Result qext_launch_applet(int kind);                    // 0 album, 1 controllers, 2 mii
}
namespace la {
    Result Start(AppletId id, s32 version);
    Result Start(AppletId id, s32 version, const void *a, size_t na);
}
namespace nro {
    Result Launch(u64 donor, const char *nro_path, const char *argv_or_null);
}

// LaunchQueue.cpp logs through this; in sSystem it is daemon.log.
void DaemonLog(const char *fmt, ...) {
    FILE *fp = fopen("sdmc:/slaunch/boot.log", "a");
    if (!fp) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);
    fputc('\n', fp);
    fclose(fp);
}

namespace sl::menu::qext {
namespace {

    using sl::smi::SystemMessage;
    namespace pwr = sl::sys::pwr;
    namespace hb  = sl::sys::hb;

    constexpr u64 kAlbumProgramId = 0x010000000000100DULL;
    constexpr const char *kAlbumDir    = "sdmc:/atmosphere/contents/010000000000100D";
    constexpr const char *kAlbumExefs  = "sdmc:/atmosphere/contents/010000000000100D/exefs";
    constexpr const char *kAlbumMarker = "sdmc:/atmosphere/contents/010000000000100D/.slaunch-hb";
    constexpr const char *kHbTarget    = "sdmc:/slaunch/hbtarget.txt";
    constexpr const char *kAskUserPath = "sdmc:/slaunch/config/ask_user";
    constexpr const char *kDonorPath   = "sdmc:/slaunch/config/hb_donor.txt";

    AccountUid g_User = {};
    bool g_Restart = false;
    bool g_WebPending = false;

    bool Exists(const char *path) { struct stat st; return stat(path, &st) == 0; }

    void Fail(const char *msg) {
        if (FILE *fp = fopen(sl::smi::PowerErrorPath, "w")) { fputs(msg, fp); fclose(fp); }
    }

    bool CopyFile(const char *from, const char *to) {
        FILE *in = fopen(from, "rb");
        if (!in) return false;
        FILE *out = fopen(to, "wb");
        if (!out) { fclose(in); return false; }
        static char buf[0x8000];
        bool ok = true;
        size_t n;
        while (ok && (n = fread(buf, 1, sizeof(buf), in)) > 0)
            ok = fwrite(buf, 1, n, out) == n;
        fclose(in);
        if (fclose(out) != 0) ok = false;
        return ok;
    }

    // Applet-mode homebrew runs in the album slot. sSystem serves hbloader there
    // through ECS, which needs libstratosphere and has to outlive the launch, so
    // here it is an ordinary content override that exists only for the launch.
    // ponytail: a file override rather than ECS, so a crash mid-launch leaves it
    // on the SD until the next boot's CleanUpLeftovers. ECS if that ever bites.
    void DisarmAlbum() {
        if (!Exists(kAlbumMarker)) return;
        char p[128];
        snprintf(p, sizeof(p), "%s/main", kAlbumExefs);       remove(p);
        snprintf(p, sizeof(p), "%s/main.npdm", kAlbumExefs);  remove(p);
        rmdir(kAlbumExefs);
        remove(kAlbumMarker);
        rmdir(kAlbumDir);
        remove(kHbTarget);
    }

    Result StartHbloader() {
        // Somebody else's album override is not ours to overwrite or delete.
        if (Exists(kAlbumExefs) && !Exists(kAlbumMarker)) {
            Fail("The Album title already has an override on the SD card (atmosphere/contents/010000000000100D)");
            return MAKERESULT(Module_Libnx, LibnxError_BadInput);
        }
        mkdir("sdmc:/atmosphere/contents", 0777);
        mkdir(kAlbumDir, 0777);
        mkdir(kAlbumExefs, 0777);
        if (FILE *fp = fopen(kAlbumMarker, "w")) fclose(fp);
        char to[128];
        snprintf(to, sizeof(to), "%s/main", kAlbumExefs);
        bool ok = CopyFile("sdmc:/slaunch/bin/hbloader/main", to);
        snprintf(to, sizeof(to), "%s/main.npdm", kAlbumExefs);
        ok = ok && CopyFile("sdmc:/slaunch/bin/hbloader/main.npdm", to);
        Result rc = ok ? la::Start(AppletId_LibraryAppletPhotoViewer, 0)
                       : MAKERESULT(Module_Libnx, LibnxError_IoError);
        DaemonLog("qext: hbloader in the album slot rc=0x%x", rc);
        if (R_FAILED(rc)) { DisarmAlbum(); Fail("Could not start the homebrew loader"); }
        return rc;
    }

    Result OpenHomebrewApplet(const char *nro, const char *argv) {
        if (nro) {
            char buf[FS_MAX_PATH + 600];
            const int n = snprintf(buf, sizeof(buf), (argv && argv[0]) ? "%s\n%s\n" : "%s\n", nro, argv);
            FILE *fp = n > 0 && n < (int)sizeof(buf) ? fopen(kHbTarget, "w") : nullptr;
            if (!fp) return MAKERESULT(Module_Libnx, LibnxError_IoError);
            fwrite(buf, 1, n, fp);
            fclose(fp);
        } else {
            remove(kHbTarget);   // no target: hbloader boots hbmenu.nro
        }
        return StartHbloader();
    }

    // The loader's message pump is not running while this waits, so the sleep
    // requests it would have handled are answered here (see sSystem's PickUser).
    bool PickUser(AccountUid &out) {
        AppletHolder sel;
        if (R_FAILED(appletCreateLibraryApplet(&sel, AppletId_LibraryAppletPlayerSelect,
                                               LibAppletMode_AllForeground)))
            return false;
        LibAppletArgs args;
        libappletArgsCreate(&args, 0);
        libappletArgsPush(&args, &sel);
        appletHolderStart(&sel);
        while (!appletHolderCheckFinished(&sel)) {
            u32 msg = 0;
            while (R_SUCCEEDED(appletGetMessage(&msg)))
                if (msg == 22 /*short power press*/ || msg == 29 /*auto power down*/)
                    qext_sleep();
            svcSleepThread(16'666'666ULL);
        }
        struct { u64 result; AccountUid uid; } res = {};
        libappletPopOutData(&sel, &res, sizeof(res), nullptr);
        appletHolderClose(&sel);
        if (res.result != 0 || !accountUidIsValid(&res.uid)) return false;
        out = res.uid;
        return true;
    }

    bool ShouldAskUser(u64 app_id) {
        if (!Exists(kAskUserPath)) return false;
        s32 n = 0;
        AccountUid uids[ACC_USER_LIST_SIZE];
        if (R_FAILED(accountListAllUsers(uids, ACC_USER_LIST_SIZE, &n)) || n < 2) return false;
        auto ctl = std::make_unique<NsApplicationControlData>();
        u64 got = 0;
        if (R_FAILED(nsGetApplicationControlData(NsApplicationControlSource_Storage, app_id,
                                                 ctl.get(), sizeof(*ctl), &got)))
            return true;
        return ctl->nacp.startup_user_account != 0;
    }

    Result LaunchApp(u64 id) {
        // A plugin's virtual title: the loader knows how it boots, and whose it is.
        if (qext_title_is_virtual(id)) {
            const Result rc = qext_launch_title(id);
            DaemonLog("qext: launch virtual 0x%016lx rc=0x%x", id, rc);
            if (R_FAILED(rc)) Fail("That could not be started");
            return rc;
        }
        AccountUid player = g_User;
        if (!accountUidIsValid(&player)) accountGetLastOpenedUser(&player);
        if (ShouldAskUser(id) && !PickUser(player)) return 0;   // cancelled
        Result rc = qext_launch_title_as(id, player);
        DaemonLog("qext: launch 0x%016lx rc=0x%x", id, rc);
        if (R_FAILED(rc)) Fail("The game could not be started");
        return rc;
    }

    Result LaunchHomebrewApp(u64 donor, const char *nro, const char *argv) {
        Result rc = nro::Launch(donor, nro, argv);
        DaemonLog("qext: nro %s under 0x%016lx rc=0x%x", nro, donor, rc);
        if (R_FAILED(rc)) Fail("The homebrew could not be started");
        return rc;
    }

    u64 ConfiguredDonor() {
        FILE *fp = fopen(kDonorPath, "r");
        if (!fp) return 0;
        char line[32] = {};
        u64 id = 0;
        if (fgets(line, sizeof(line), fp)) id = strtoull(line, nullptr, 16);
        fclose(fp);
        return id;
    }

} // namespace

void CleanUpLeftovers() {
    DisarmAlbum();
    // Also what an upgrade from sSystem can leave: a queued homebrew it never got to,
    // which would otherwise launch the first time this returns to the menu, and a
    // crash count that would start the new menu in safe mode.
    hb::LaunchQueue::Reset();
    remove(sl::menu::dbg::kCrashCountPath);
}

void OnBackInMenu() {
    DisarmAlbum();
    hb::Request req;
    if (!hb::LaunchQueue::Take(req)) return;
    const u64 donor = req.mode == sl::sys::hb::LaunchMode::App
                          ? (req.donor ? req.donor : ConfiguredDonor()) : 0;
    if (donor) LaunchHomebrewApp(donor, req.nro, req.argv);
    else       OpenHomebrewApplet(req.nro, req.argv);
}

bool TakeWebRequest() {
    const bool r = g_WebPending;
    g_WebPending = false;
    return r;
}

void StartWeb(unsigned heap_mb_left) {
    // An applet that runs out of memory takes the console with it. On hardware:
    // out of memory at 192 and 100 MB of heap, hung at 114. 64 is a guess at what
    // is low enough, so anything above it is refused instead of risking that.
    if (heap_mb_left > 64) {
        Fail("The browser needs more memory than the menu could free");
        return;
    }
    static WebCommonConfig cfg;   // ~0x2000 bytes of TLVs
    Result rc = webPageCreate(&cfg, "https://etonedemid.github.io/slaunch/");
    if (R_SUCCEEDED(rc)) {
        webConfigSetWhitelist(&cfg, "^http*");
        rc = la::Start(AppletId_LibraryAppletWeb, cfg.version, &cfg.arg, sizeof(cfg.arg));
    }
    DaemonLog("qext: browser rc=0x%x", rc);
    if (R_FAILED(rc)) Fail("Could not open the browser");
}

void RenameTitle(u64 tid, const char *name) {
    if (name && name[0]) { qext_title_rename(tid, name); return; }
    const int n = qext_title_count();
    for (int i = 0; i < n; i++) {
        char original[0x201];
        if (qext_title_id(i) == tid && qext_title_original_name(i, original, sizeof(original)) > 0) {
            qext_title_rename(tid, original);
            return;
        }
    }
}

bool TakeRestart() {
    const bool r = g_Restart;
    g_Restart = false;
    return r;
}

} // namespace sl::menu::qext

namespace sl::smi {

Result QextCommand(SystemMessage msg, const void *payload, size_t) {
    using namespace sl::menu::qext;
    namespace pwr = sl::sys::pwr;
    switch (msg) {
        case SystemMessage::SetSelectedUser:
            g_User = static_cast<const PayloadSetUser *>(payload)->uid;
            return 0;
        case SystemMessage::LaunchApplication:
            return LaunchApp(static_cast<const PayloadAppLaunch *>(payload)->app_id);
        case SystemMessage::ResumeApplication:    return qext_resume_game();
        case SystemMessage::TerminateApplication: return qext_terminate_game();
        case SystemMessage::OpenAlbum:            return qext_launch_applet(0);
        case SystemMessage::OpenControllers:      return qext_launch_applet(1);
        case SystemMessage::OpenMiiEdit:          return qext_launch_applet(2);
        case SystemMessage::OpenUserPage: {
            FriendsLaArg arg = {};
            arg.hdr.type = FriendsLaArgType_ShowMyProfile;
            arg.hdr.uid  = g_User;
            return la::Start(AppletId_LibraryAppletMyPage, 0x10000, &arg, sizeof(arg));
        }
        case SystemMessage::OpenNetConnect: {
            const u32 type = 1;   // HomeMenu
            return la::Start(AppletId_LibraryAppletNetConnect, 1, &type, sizeof(type));
        }
        case SystemMessage::OpenWebBrowser:
            g_WebPending = true;
            return 0;
        case SystemMessage::OpenSystemSettings:
            return la::Start(AppletId_LibraryAppletSet, 1);
        case SystemMessage::OpenHomebrewMenu:
            return OpenHomebrewApplet(nullptr, nullptr);
        case SystemMessage::OpenHomebrew: {
            auto *p = static_cast<const PayloadHomebrew *>(payload);
            return OpenHomebrewApplet(p->nro_path, p->argv);
        }
        case SystemMessage::LaunchHomebrewApplication: {
            auto *p = static_cast<const PayloadHomebrew *>(payload);
            return LaunchHomebrewApp(p->donor_id, p->nro_path, p->argv);
        }
        case SystemMessage::OpenPowerMenu:
        case SystemMessage::PowerSleep:    return qext_sleep();
        case SystemMessage::PowerReboot:   pwr::Reboot();   return 0;
        case SystemMessage::PowerShutdown: pwr::Shutdown(); return 0;
        case SystemMessage::RebootToPayload: {
            auto *p = static_cast<const PayloadReboot *>(payload);
            Fail(pwr::PayloadResultText(pwr::RebootToPayload(p->payload_path)));
            return 0;
        }
        case SystemMessage::RestartMenu:
        case SystemMessage::TerminateMenu:
            g_Restart = true;
            return 0;
        default:   // MenuReady, ReloadAppList: nothing is listening for them here
            return 0;
    }
}

} // namespace sl::smi
