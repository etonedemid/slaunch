#pragma once
#include <switch.h>
#include <sl/Result.hpp>
#include <cstring>

// sLaunch SMI (System-Menu Interface) protocol
// Communication between sSystem (backend) and sMenu (UI) via AppletStorage blobs.
// Layout per transaction: [CommandHeader][payload...]
// Storage size: 0x4000 bytes (16 KB - sufficient for all commands)

namespace sl::smi {

    constexpr u32 Magic = 0x214C4D53; // "SML!"
    constexpr size_t StorageSize = 0x4000;

    // Commands: sMenu → sSystem
    enum class SystemMessage : u32 {
        Invalid = 0,
        SetSelectedUser,
        LaunchApplication,
        ResumeApplication,
        TerminateApplication,
        OpenHomebrew,          // launch an NRO as an applet
        LaunchHomebrewApplication, // launch an NRO as an application (donor slot)
        OpenAlbum,
        OpenNetConnect,
        OpenUserPage,
        OpenMiiEdit,
        OpenWebBrowser,        // offline web applet
        OpenControllers,       // controller-management applet
        OpenHomebrewMenu,      // launch the installed hbmenu NRO
        OpenPowerMenu,
        RestartMenu,
        ReloadAppList,
        TerminateMenu,
        OpenSystemSettings,    // full System Settings ("set" applet, id 0x16)
    };

    // Events: sSystem → sMenu (async)
    enum class MenuMessage : u32 {
        Invalid = 0,
        HomeRequested,
        SdCardEjected,
        GameCardInserted,
        GameCardRemoved,
        ApplicationListChanged,
        SleepFinished,
    };

    struct CommandHeader {
        u32 magic;
        u32 val;
    };
    static_assert(sizeof(CommandHeader) == 8);

    // Payload for SetSelectedUser
    struct PayloadSetUser {
        AccountUid uid;
    };

    // Payload for LaunchApplication / TerminateApplication / OpenHomebrew
    struct PayloadAppLaunch {
        u64 app_id;
    };

    struct PayloadHomebrew {
        char nro_path[FS_MAX_PATH];
        char argv[512];
        u64  donor_id;   // LaunchHomebrewApplication: game slot to run the NRO in
    };

    // Passed from sSystem → sMenu at startup via input storage
    struct SystemStatus {
        AccountUid selected_user;
        u64        suspended_app_id;      // 0 if none
        bool       has_suspended_app;
        u8         _pad[3];
        u32        added_apps_count;
        u32        deleted_apps_count;
    };
    static_assert(sizeof(SystemStatus) <= StorageSize);

    // -------------------------------------------------------------------------
    // Software-keyboard bridge. A library applet cannot launch swkbd, so the
    // menu writes a request file and exits; the daemon (qlaunch) shows swkbd and
    // writes the result back; the menu applies it on relaunch. These purpose ids
    // are shared by both sides.
    enum KbPurpose : u32 {
        Kb_RenameGame = 0,
        Kb_WeatherCity,   // legacy (hardcoded weather widget removed; kept for enum stability)
        Kb_AuroraUser,    // legacy
        Kb_AuroraPass,    // legacy
        Kb_ThemeName,
        Kb_AuroraSend,    // legacy
        Kb_WidgetOption,  // editing a Lua widget's exposed string option
    };
    constexpr const char *KbRequestPath = "sdmc:/slaunch/config/kb_req.txt";
    constexpr const char *KbResultPath  = "sdmc:/slaunch/config/kb_result.txt";

    // -------------------------------------------------------------------------
    // RAII storage writer/reader helpers

    class StorageWriter {
        AppletStorage st;
        size_t        offset = 0;
        bool          valid  = false;
    public:
        StorageWriter() {
            if (R_SUCCEEDED(appletCreateStorage(&st, StorageSize)))
                valid = true;
        }
        ~StorageWriter() {
            if (valid) appletStorageClose(&st);
        }
        StorageWriter(const StorageWriter&) = delete;

        bool IsValid() const { return valid; }

        template<typename T>
        Result Write(const T &val) {
            auto rc = appletStorageWrite(&st, offset, &val, sizeof(T));
            if (R_SUCCEEDED(rc)) offset += sizeof(T);
            return rc;
        }

        Result WriteRaw(const void *data, size_t size) {
            auto rc = appletStorageWrite(&st, offset, data, size);
            if (R_SUCCEEDED(rc)) offset += size;
            return rc;
        }

        AppletStorage& GetStorage() { return st; }
    };

    class StorageReader {
        AppletStorage st;
        size_t        offset = 0;
        bool          valid  = false;
    public:
        explicit StorageReader(AppletStorage &src) : st(src), valid(true) {}
        ~StorageReader() {
            if (valid) appletStorageClose(&st);
        }
        StorageReader(const StorageReader&) = delete;

        template<typename T>
        Result Read(T &out) {
            auto rc = appletStorageRead(&st, offset, &out, sizeof(T));
            if (R_SUCCEEDED(rc)) offset += sizeof(T);
            return rc;
        }

        Result ReadRaw(void *out, size_t size) {
            auto rc = appletStorageRead(&st, offset, out, size);
            if (R_SUCCEEDED(rc)) offset += size;
            return rc;
        }
    };

    // Send a command from sMenu to sSystem (via the library applet holder)
    // Push into the outgoing data queue; sSystem reads it.
    template<typename TPayload>
    inline Result SendMenuCommand(SystemMessage msg, const TPayload &payload) {
        StorageWriter w;
        if (!w.IsValid()) return MAKERESULT(Module_Libnx, LibnxError_BadInput);

        // Use raw libnx Result checks (not R_TRY): this header is shared with
        // sSystem, where R_TRY is Atmosphere's macro returning ams::Result and
        // would clash with the libnx Result (u32) return type here.
        CommandHeader hdr { Magic, static_cast<u32>(msg) };
        Result rc = w.Write(hdr);
        if (rc != 0) return rc;
        rc = w.Write(payload);
        if (rc != 0) return rc;

        // Applet -> daemon: push onto our OutData queue; the daemon (which holds
        // us) drains it with appletHolderPopOutData. (Events come the other way
        // via appletPopInData.) This matches the library-applet IPC uLaunch uses.
        return appletPushOutData(&w.GetStorage());
    }

    inline Result SendMenuCommand(SystemMessage msg) {
        StorageWriter w;
        if (!w.IsValid()) return MAKERESULT(Module_Libnx, LibnxError_BadInput);

        CommandHeader hdr { Magic, static_cast<u32>(msg) };
        Result rc = w.Write(hdr);
        if (rc != 0) return rc;
        return appletPushOutData(&w.GetStorage());
    }

} // namespace sl::smi
