#include <sl/sys/app/Application.hpp>
#include <cstring>

namespace sl::sys::app {

    AppletHolder g_AppHolder  = {};
    u64          g_AppId      = 0;
    bool         g_AppRunning = false;
    bool         g_AppHasFocus = false;

    // Build the 0x88-byte preselected-user launch parameter expected by games
    static Result PushUserParam(AccountUid user) {
        // Layout: magic(4) + uid(16) + padding to 0x88
        struct alignas(4) UserParam {
            u32        magic;   // 0xC79497CA
            u8         _pad0[4];
            AccountUid uid;
            u8         _rest[0x88 - 4 - 4 - sizeof(AccountUid)];
        } param = {};
        static_assert(sizeof(param) == 0x88);
        param.magic = 0xC79497CA;
        param.uid   = user;

        AppletStorage st;
        R_TRY(appletCreateStorage(&st, sizeof(param)));
        R_TRY(appletStorageWrite(&st, 0, &param, sizeof(param)));
        R_TRY(appletApplicationPushLaunchParameter(
            &g_AppHolder,
            AppletLaunchParameterKind_PreselectedUser, &st));
        appletStorageClose(&st);
        return ResultSuccess();
    }

    Result Launch(u64 app_id, AccountUid user) {
        if (g_AppRunning) return MAKERESULT(Module_Libnx, LibnxError_AlreadyExists);

        // Touch the app (marks as recently used in NS)
        nsTouchApplication(app_id);

        // Create the application holder
        R_TRY(appletCreateApplication(&g_AppHolder, app_id));

        // Push the selected user
        R_TRY(PushUserParam(user));

        // Release foreground so the app can acquire it
        appletUnlockForeground();

        // Start it
        R_TRY(appletApplicationStart(&g_AppHolder));

        // Hand focus to the app
        appletApplicationRequestForApplicationToGetForeground(&g_AppHolder);

        g_AppId      = app_id;
        g_AppRunning = true;
        g_AppHasFocus = true;
        return ResultSuccess();
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
