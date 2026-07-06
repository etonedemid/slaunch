#include <sl/sys/la/LibraryApplet.hpp>
#include <cstring>

namespace sl::sys::la {

    AppletHolder g_MenuHolder = {};
    bool         g_MenuRunning = false;

    Result LaunchMenu(AppletId menu_applet_id, const void *status, size_t status_size) {
        if (g_MenuRunning) return MAKERESULT(Module_Libnx, LibnxError_BadInput);

        Result rc = appletCreateLibraryApplet(
            &g_MenuHolder, menu_applet_id, LibAppletMode_AllForeground);
        if (rc != 0) return rc;

        LibAppletArgs la_args;
        libappletArgsCreate(&la_args, 0);
        libappletArgsSetPlayStartupSound(&la_args, false);
        rc = libappletArgsPush(&la_args, &g_MenuHolder);
        if (rc != 0) return rc;

        // Status must be pushed as input data before the applet starts.
        if (status && status_size > 0) {
            rc = libappletPushInData(&g_MenuHolder, status, status_size);
            if (rc != 0) return rc;
        }

        rc = appletHolderStart(&g_MenuHolder);
        if (rc != 0) return rc;

        g_MenuRunning = true;
        return 0;
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

    Result OpenSystemApplet(AppletId id, s32 la_version,
                            const void *in1, size_t in1_size,
                            const void *in2, size_t in2_size) {
        AppletHolder holder;
        Result rc = appletCreateLibraryApplet(&holder, id, LibAppletMode_AllForeground);
        if (rc != 0) return rc;

        // A negative version means the applet is started without common args.
        if (la_version >= 0) {
            LibAppletArgs la;
            libappletArgsCreate(&la, (u32)la_version);
            libappletArgsSetPlayStartupSound(&la, true);
            rc = libappletArgsPush(&la, &holder);
            if (rc != 0) { appletHolderClose(&holder); return rc; }
        }

        if (in1 && in1_size > 0) {
            rc = libappletPushInData(&holder, in1, in1_size);
            if (rc != 0) { appletHolderClose(&holder); return rc; }
        }
        if (in2 && in2_size > 0) {
            rc = libappletPushInData(&holder, in2, in2_size);
            if (rc != 0) { appletHolderClose(&holder); return rc; }
        }

        rc = appletHolderStart(&holder);
        if (rc != 0) { appletHolderClose(&holder); return rc; }

        // While a system applet (album/mii/hbmenu/...) is foreground the daemon
        // is the one that receives HOME/power messages, so we must act on them
        // here or they are lost. HOME closes the applet (this is how you exit
        // hbmenu, which otherwise just reloads itself); power sleeps.
        while (!appletHolderCheckFinished(&holder)) {
            u32 msg = 0;
            while (R_SUCCEEDED(appletGetMessage(&msg))) {
                if (msg == 20 /*DetectShortPressingHomeButton*/) {
                    appletHolderRequestExitOrTerminate(&holder, 5'000'000'000ULL);
                } else if (msg == 22 /*power*/ || msg == 29 /*auto power down*/) {
                    appletStartSleepSequence(true);
                }
            }
            svcSleepThread(16'666'666ULL); // ~60fps polling
        }

        appletHolderClose(&holder);
        return 0;
    }

} // namespace sl::sys::la
