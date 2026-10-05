#include <sl/sys/app/Application.hpp>
#include <cstring>
#include <memory>

void DaemonLog(const char *fmt, ...);   // main.cpp

namespace sl::sys::app {

    AppletApplication g_AppHolder   = {};
    u64               g_AppId       = 0;
    bool              g_AppRunning  = false;
    bool              g_AppHasFocus = false;

    // Tick when the current app was launched, for the suspend-stabilization guard.
    static u64        g_LaunchTick  = 0;

    // Build the 0x88-byte preselected-user launch parameter expected by games.
    // Layout (from qlaunch / uLaunch): magic(4) + is_user_selected(1) + pad(3) +
    // uid(16) + padding. The is_user_selected byte MUST be 1 -- leaving it 0
    // makes the game think no user was preselected and it exits right after the
    // "Licensed by Nintendo" splash.
    static Result PushUserParam(AccountUid user) {
        struct alignas(4) UserParam {
            u32        magic;             // 0xC79497CA
            u8         is_user_selected;  // 1
            u8         pad[3];
            AccountUid uid;
            u8         unused[0x70];
        } param = {};
        static_assert(sizeof(param) == 0x88);
        param.magic            = 0xC79497CA;
        param.is_user_selected = 1;
        param.uid              = user;

        AppletStorage st;
        Result rc = appletCreateStorage(&st, sizeof(param));
        if (rc != 0) return rc;
        rc = appletStorageWrite(&st, 0, &param, sizeof(param));
        if (rc != 0) { appletStorageClose(&st); return rc; }
        rc = appletApplicationPushLaunchParameter(
            &g_AppHolder, AppletLaunchParameterKind_PreselectedUser, &st);
        appletStorageClose(&st);
        return rc;
    }

    // Stock qlaunch creates a game's save data before launching it, and games
    // rely on that: they mount their save without creating it, and abort with
    // 2002-1002 (target not found) when it is missing - which is every game
    // never played on that account before (issue #6). Same as qlaunch / uLaunch:
    // open it to see if it exists, create it from the NACP sizes if not.
    static Result EnsureSave(u64 app_id, u64 owner, AccountUid uid, FsSaveDataType type,
                           FsSaveDataSpaceId space, u64 size, u64 journal) {
        if (size == 0) return 0;
        const FsSaveDataAttribute attr = {
            .application_id = app_id, .uid = uid, .system_save_data_id = 0,
            .save_data_type = (u8)type, .save_data_rank = FsSaveDataRank_Primary,
            .save_data_index = 0,
        };
        FsFileSystem fs;
        const Result orc = fsOpenSaveDataFileSystem(&fs, space, &attr);
        if (R_SUCCEEDED(orc)) { fsFsClose(&fs); return 0; }
        const FsSaveDataCreationInfo info = {
            .save_data_size = (s64)size, .journal_size = (s64)journal,
            .available_size = 0x4000, .owner_id = owner, .flags = 0,
            .save_data_space_id = (u8)space,
        };
        const FsSaveDataMetaInfo meta = {
            .size = (type == FsSaveDataType_Bcat) ? 0u : 0x40060u,
            .type = (u8)((type == FsSaveDataType_Bcat) ? FsSaveDataMetaType_None
                                                       : FsSaveDataMetaType_Thumbnail),
        };
        const Result crc = fsCreateSaveDataFileSystem(&attr, &info, &meta);
        DaemonLog("save: app=0x%016lx type=%d size=0x%lx open rc=0x%x -> create rc=0x%x",
                  app_id, (int)type, size, orc, crc);
        return crc;
    }

    // fs 2002-0030..0045: one of the "not enough free space" results.
    bool IsNoSpace(Result rc) {
        return R_MODULE(rc) == 2 /* fs */ && R_DESCRIPTION(rc) >= 30 && R_DESCRIPTION(rc) <= 45;
    }

    // The first creation that failed, or 0.
    // `wants_user`: the NACP's StartupUserAccount is set. A game that has it
    // at None picks its players itself; handing it a preselected user anyway
    // opens that account for it, and picking the same one in the game's own
    // selector then loops. Unknown (no NACP) keeps the old behaviour.
    static Result EnsureSaves(u64 app_id, AccountUid user, bool &wants_user) {
        wants_user = true;
        auto ctl = std::make_unique<NsApplicationControlData>();
        u64 got = 0;
        const Result rc = nsGetApplicationControlData(NsApplicationControlSource_Storage, app_id,
                                                      ctl.get(), sizeof(*ctl), &got);
        DaemonLog("save: app=0x%016lx user=%016lx%016lx control rc=0x%x", app_id,
                  user.uid[0], user.uid[1], rc);
        if (R_FAILED(rc)) return 0;   // no NACP: launch as before
        const NacpStruct &n = ctl->nacp;
        wants_user = n.startup_user_account != 0;
        DaemonLog("save: nacp user=%d account=0x%lx device=0x%lx temp=0x%lx cache=0x%lx bcat=0x%lx",
                  n.startup_user_account, n.user_account_save_data_size, n.device_save_data_size,
                  n.temporary_storage_size, n.cache_storage_size,
                  n.bcat_delivery_cache_storage_size);
        const u64 owner = n.save_data_owner_id;
        // Every save is still tried after one fails; the first failure decides.
        Result first = 0;
        auto keep = [&](Result r) { if (R_FAILED(r) && !first) first = r; };
        if (wants_user && accountUidIsValid(&user))
            keep(EnsureSave(app_id, owner, user, FsSaveDataType_Account, FsSaveDataSpaceId_User,
                       n.user_account_save_data_size, n.user_account_save_data_journal_size));
        keep(EnsureSave(app_id, owner, {}, FsSaveDataType_Device, FsSaveDataSpaceId_User,
                   n.device_save_data_size, n.device_save_data_journal_size));
        keep(EnsureSave(app_id, owner, {}, FsSaveDataType_Temporary, FsSaveDataSpaceId_Temporary,
                   n.temporary_storage_size, 0));
        keep(EnsureSave(app_id, owner, {}, FsSaveDataType_Cache, FsSaveDataSpaceId_User,
                   n.cache_storage_size, n.cache_storage_journal_size));
        keep(EnsureSave(app_id, 0x010000000000000CULL, {}, FsSaveDataType_Bcat, FsSaveDataSpaceId_User,
                   n.bcat_delivery_cache_storage_size, 0x200000));
        return first;
    }

    Result Launch(u64 app_id, AccountUid user) {
        if (g_AppRunning) return MAKERESULT(Module_Libnx, LibnxError_BadInput);

        // Touch the app (marks as recently used in NS)
        nsTouchApplication(app_id);
        // Launching anyway would only start a game that aborts on its save
        // mount; the caller tells the user why instead.
        bool wants_user = true;
        if (const Result src = EnsureSaves(app_id, user, wants_user); IsNoSpace(src))
            return src;

        Result rc = appletCreateApplication(&g_AppHolder, app_id);
        if (rc != 0) return rc;

        rc = wants_user ? PushUserParam(user) : 0;
        if (rc == 0) {
            // Release foreground so the app can acquire it
            appletUnlockForeground();
            rc = appletApplicationStart(&g_AppHolder);
        }
        // Otherwise the failed app's holder stays open: am keeps it as the
        // application, and the next launch is refused.
        if (rc != 0) {
            appletApplicationClose(&g_AppHolder);
            memset(&g_AppHolder, 0, sizeof(g_AppHolder));
            appletRequestToGetForeground();
            return rc;
        }

        appletApplicationRequestForApplicationToGetForeground(&g_AppHolder);

        g_AppId       = app_id;
        g_AppRunning  = true;
        g_AppHasFocus = true;
        g_LaunchTick  = armGetSystemTick();
        return 0;
    }

    bool CanSuspend() {
        if (!g_AppRunning) return false;
        // Give the app ~1.5s to finish launching and take the foreground before a
        // HOME press is allowed to suspend it (otherwise am faults).
        const u64 settle = armGetSystemTickFreq() * 3 / 2;
        return (armGetSystemTick() - g_LaunchTick) >= settle;
    }

    Result Resume() {
        if (!g_AppRunning) return MAKERESULT(Module_Libnx, LibnxError_NotFound);
        return FocusApplication();
    }

    void Terminate() {
        if (!g_AppRunning) return;

        appletApplicationTerminateAllLibraryApplets(&g_AppHolder);
        appletApplicationRequestExit(&g_AppHolder);

        // Wait up to 15 seconds for graceful exit
        if (R_FAILED(eventWait(&g_AppHolder.StateChangedEvent, 15'000'000'000ULL)))
            appletApplicationTerminate(&g_AppHolder);

        appletApplicationClose(&g_AppHolder);
        memset(&g_AppHolder, 0, sizeof(g_AppHolder));
        g_AppId       = 0;
        g_AppRunning  = false;
        g_AppHasFocus = false;

        appletRequestToGetForeground();
    }

    bool Update() {
        if (!g_AppRunning) return false;
        if (!appletApplicationCheckFinished(&g_AppHolder)) return false;

        // App has exited naturally
        appletApplicationClose(&g_AppHolder);
        memset(&g_AppHolder, 0, sizeof(g_AppHolder));
        g_AppId       = 0;
        g_AppRunning  = false;
        g_AppHasFocus = false;

        appletRequestToGetForeground();
        return true;
    }

    Result FocusApplication() {
        if (!g_AppRunning) return MAKERESULT(Module_Libnx, LibnxError_NotFound);
        g_AppHasFocus = true;
        return appletApplicationRequestForApplicationToGetForeground(&g_AppHolder);
    }

    Result FocusSystem() {
        g_AppHasFocus = false;
        return appletRequestToGetForeground();
    }

} // namespace sl::sys::app
