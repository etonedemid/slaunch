#pragma once
#include <switch.h>

namespace sl::sys::la {

    // The AppletHolder for the currently-running library applet (sMenu)
    extern AppletHolder g_MenuHolder;
    extern bool         g_MenuRunning;

    // Launch sMenu as a library applet via ECS redirect, in whichever slot the
    // daemon picked for this firmware (ecs::MenuSlots).
    // The status blob is pushed as the applet's input data BEFORE it starts,
    // so sMenu can read it with appletPopInData at launch.
    Result LaunchMenu(AppletId menu_applet_id, const void *status, size_t status_size);

    // Stop sMenu (graceful exit with 15s timeout, then force)
    void   StopMenu();

    bool   IsMenuAlive();

    // Push an SMI event storage into sMenu's input queue
    Result PushStorage(AppletStorage &st);

    // Requests the system sends qlaunch over the general channel rather than as
    // applet messages - the HOME-hold quick menu's Sleep/Power off/Restart among
    // them. Values from SwitchU and uLaunch, which agree.
    enum GeneralChannelMsg : u32 {
        Gc_Home     = 2,
        Gc_Sleep    = 3,
        Gc_Shutdown = 5,
        Gc_Reboot   = 6,
    };

    // Pops one general-channel storage. Returns false when the channel is empty;
    // `msg` is 0 for anything that is not a well-formed request.
    bool   PopGeneralChannel(u32 &msg);

    // Applet messages that mean "go to sleep": power button, idle timeout,
    // overheating, low battery, and the TV entering standby over HDMI-CEC.
    // RequestToPrepareSleep (25) is deliberately absent: Horizon sends it inside
    // a sleep already under way, so answering it starts a second one.
    inline bool IsSleepMessage(u32 msg) {
        return msg == 22 || msg == 27 || msg == 28 || msg == 29 || msg == 32;
    }

    // Open a system library applet (album, web, mii editor, ...) and block
    // until it closes. Each applet needs a specific common-args `la_version`
    // and its own input struct(s); a negative version means "push no common
    // args at all" (required by e.g. MiiEdit). Passing the wrong version or a
    // null/zero input where the applet expects one makes it fault -- which is
    // why MiiEdit was crashing. Modelled on uLaunch's la::Start.
    Result OpenSystemApplet(AppletId id, s32 la_version,
                            const void *in1 = nullptr, size_t in1_size = 0,
                            const void *in2 = nullptr, size_t in2_size = 0);

} // namespace sl::sys::la
