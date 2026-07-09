#include <sl/sys/app/Application.hpp>
#include <cstring>

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

    Result Launch(u64 app_id, AccountUid user) {
        if (g_AppRunning) return MAKERESULT(Module_Libnx, LibnxError_BadInput);

        // Touch the app (marks as recently used in NS)
        nsTouchApplication(app_id);

        Result rc = appletCreateApplication(&g_AppHolder, app_id);
        if (rc != 0) return rc;

        rc = PushUserParam(user);
        if (rc != 0) return rc;

        // Release foreground so the app can acquire it
        appletUnlockForeground();

        rc = appletApplicationStart(&g_AppHolder);
        if (rc != 0) return rc;

        // Hand focus to the app
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
