#include <sl/sys/la/LibraryApplet.hpp>
#include <cstring>

namespace sl::sys::la {

    AppletHolder g_MenuHolder = {};
    bool         g_MenuRunning = false;

    Result LaunchMenu(AppletId menu_applet_id) {
        if (g_MenuRunning) return MAKERESULT(Module_Libnx, LibnxError_AlreadyExists);

        R_TRY(appletCreateLibraryApplet(
            &g_MenuHolder, menu_applet_id, LibAppletMode_AllForeground));

        LibAppletArgs la_args;
        libappletArgsCreate(&la_args, 0);
        libappletArgsSetPlayStartupSound(&la_args, false);
        R_TRY(libappletArgsPush(&la_args, &g_MenuHolder));

        R_TRY(appletHolderStart(&g_MenuHolder));

        g_MenuRunning = true;
        return ResultSuccess();
    }

    void StopMenu() {
        if (!g_MenuRunning) return;
        appletHolderRequestExitOrTerminate(&g_MenuHolder, 15'000'000'000ULL);
        appletHolderClose(&g_MenuHolder);
        memset(&g_MenuHolder, 0, sizeof(g_MenuHolder));
        g_MenuRunning = false;
    }

    bool IsMenuAlive() {
        if (!g_MenuRunning) return false;
        return !appletHolderCheckFinished(&g_MenuHolder);
    }

    Result PushToMenu(const void *data, size_t size) {
        return libappletPushInData(&g_MenuHolder, data, size);
    }

    Result PopFromMenu(void *out, size_t size) {
        return libappletPopOutData(&g_MenuHolder, out, size, nullptr);
    }

    Result PushStorage(AppletStorage &st) {
        return appletHolderPushInData(&g_MenuHolder, &st);
    }

    Result PopStorage(AppletStorage &st) {
        return appletHolderPopOutData(&g_MenuHolder, &st);
    }

    Result OpenSystemApplet(AppletId id, const void *args, size_t args_size,
                             void *out, size_t out_size) {
        AppletHolder holder;
        R_TRY(appletCreateLibraryApplet(&holder, id, LibAppletMode_AllForeground));

        LibAppletArgs la;
        libappletArgsCreate(&la, 0);
        R_TRY(libappletArgsPush(&la, &holder));

        if (args && args_size > 0)
            libappletPushInData(&holder, args, args_size);

        R_TRY(appletHolderStart(&holder));

        // Wait for it to finish
        while (!appletHolderCheckFinished(&holder))
            svcSleepThread(16'666'666ULL); // ~60fps polling

        if (out && out_size > 0)
            libappletPopOutData(&holder, out, out_size, nullptr);

        appletHolderClose(&holder);
        return ResultSuccess();
    }

} // namespace sl::sys::la
