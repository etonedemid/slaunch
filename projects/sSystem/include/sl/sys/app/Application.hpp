#pragma once
#include <switch.h>

namespace sl::sys::app {

    // Tracks the currently running application (if any).
    // Applications use the dedicated AppletApplication type (distinct from the
    // AppletHolder used for library applets) in the current libnx applet API.
    extern AppletApplication g_AppHolder;
    extern u64          g_AppId;
    extern bool         g_AppRunning;
    extern bool         g_AppHasFocus;

    // Launch a game. Sends the preselected-user parameter.
    // Caller must have already called SetSelectedUser.
    Result Launch(u64 app_id, AccountUid user);

    // Give foreground back to the currently suspended application
    Result Resume();

    // Request clean exit, fall back to force-terminate after timeout
    void   Terminate();

    // Called each frame from the main loop; returns true when the app exited
    bool   Update();

    // Give foreground to the application (take it from ourselves)
    Result FocusApplication();

    // Take foreground back from the application (bring menu up)
    Result FocusSystem();

} // namespace sl::sys::app
