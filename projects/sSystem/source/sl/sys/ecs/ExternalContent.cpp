#include <sl/sys/ecs/ExternalContent.hpp>
#include <stratosphere.hpp>
#include <stratosphere/fssrv/interface_adapters/fssrv_filesystem_interface_adapter.hpp>
#include <stratosphere/fssystem/fssystem_subdirectory_filesystem.hpp>
#include <stratosphere/fs/fs_remote_filesystem.hpp>
#include <cstring>

// ECS implementation adapted from uLaunch's ul::system::ecs / sf::IpcManager
// (https://github.com/Xortroll/uLaunch, GPLv2, (C) Xortroll & contributors).

namespace sl::sys::ecs {

    namespace {

        // ---- sf server manager (serves ECS filesystem sessions) --------------
        struct ServerOptions {
            static constexpr size_t PointerBufferSize   = 0x800;
            static constexpr size_t MaxDomains          = 0x40;
            static constexpr size_t MaxDomainObjects    = 0x100;
            static constexpr bool   CanDeferInvokeRequest = false;
            static constexpr bool   CanManageMitmServers  = false;
        };
        constexpr size_t MaxEcsSessions = 6;

        // No named ports: sessions are added manually via RegisterSession.
        class EcsServerManager : public ams::sf::hipc::ServerManager<0, ServerOptions, MaxEcsSessions> {
            private:
                ams::Result OnNeedsToAccept(int, Server *) override {
                    return ams::ResultSuccess();
                }
        };

        EcsServerManager    g_Manager;
        ams::os::ThreadType g_ManagerThread;
        alignas(ams::os::ThreadStackAlignment) u8 g_ManagerThreadStack[0x8000];

        using Allocator     = ams::sf::ExpHeapAllocator;
        using ObjectFactory = ams::sf::ObjectFactory<ams::sf::ExpHeapAllocator::Policy>;

        ams::os::SdkMutex        g_AllocatorLock;
        alignas(0x40) constinit u8 g_AllocatorHeap[0x8000];
        ams::lmem::HeapHandle    g_AllocatorHeapHandle;
        Allocator                g_Allocator;
        bool                     g_AllocatorReady = false;
        bool                     g_ServerUp = false;

        // Separate file: this runs on the serve thread, so writing to daemon.log
        // would race with the main thread's writes.
        void EcsLog(const char *msg) {
            FILE *fp = fopen("sdmc:/slaunch/ecs.log", "a");
            if (!fp) return;
            fprintf(fp, "%s\n", msg);
            fclose(fp);
        }

        void ManagerThreadFunc(void *) {
            EcsLog("server thread: entering LoopProcess");
            g_Manager.LoopProcess();
            EcsLog("server thread: LoopProcess RETURNED (server died!)");
        }

        template<typename Impl, typename Iface, typename ...Args>
        auto MakeSharedObject(Args &&...args) {
            std::scoped_lock lk(g_AllocatorLock);
            return ObjectFactory::CreateSharedEmplaced<Iface, Impl>(
                std::addressof(g_Allocator), std::forward<Args>(args)...);
        }

        // ldr:shel AtmosphereRegisterExternalCode (cmd 65000): returns a session
        // handle we serve the replacement exefs on.
        inline Result RegisterExternalCodeCmd(u64 program_id, Handle *out_h) {
            return serviceDispatchIn(ldrShellGetServiceSession(), 65000, program_id,
                .out_handle_attrs = { SfOutHandleAttr_HipcMove },
                .out_handles      = out_h,
            );
        }

        // ldr:shel AtmosphereUnregisterExternalCode (cmd 65001).
        inline Result UnregisterExternalCodeCmd(u64 program_id) {
            return serviceDispatchIn(ldrShellGetServiceSession(), 65001, program_id);
        }

    } // namespace

    // Allocator must be ready before we build served objects; the serve thread
    // is started only AFTER the first session is registered, so LoopProcess
    // never begins with an empty wait set (which would make it exit at once).
    Result InitializeServer() {
        if (g_AllocatorReady) return 0;
        g_AllocatorHeapHandle = ams::lmem::CreateExpHeap(
            g_AllocatorHeap, sizeof(g_AllocatorHeap), ams::lmem::CreateOption_None);
        g_Allocator.Attach(g_AllocatorHeapHandle);
        g_AllocatorReady = true;
        return 0;
    }

    static void StartServerThreadOnce() {
        if (g_ServerUp) return;
        R_ABORT_UNLESS(ams::os::CreateThread(
            &g_ManagerThread, &ManagerThreadFunc, nullptr,
            g_ManagerThreadStack, sizeof(g_ManagerThreadStack), 10));
        ams::os::StartThread(&g_ManagerThread);
        g_ServerUp = true;
    }

    Result RegisterExternalContent(u64 program_id, const char *exefs_path) {
        InitializeServer();

        // Run the ams-result logic in a lambda; convert to libnx Result at the
        // boundary so callers (which use libnx Result) stay simple.
        const ams::Result r = [&]() -> ams::Result {
            Handle move_h = INVALID_HANDLE;
            R_TRY(RegisterExternalCodeCmd(program_id, &move_h));

            FsFileSystem sd_fs;
            R_TRY(fsOpenSdCardFileSystem(&sd_fs));

            std::shared_ptr<ams::fs::fsa::IFileSystem> remote_sd =
                std::make_shared<ams::fs::RemoteFileSystem>(sd_fs);
            auto subdir_fs =
                std::make_shared<ams::fssystem::SubDirectoryFileSystem>(std::move(remote_sd));

            ams::fs::Path exefs_fs_path;
            R_TRY(exefs_fs_path.Initialize(exefs_path, std::strlen(exefs_path)));
            R_TRY(exefs_fs_path.Normalize(ams::fs::PathFlags{}));
            R_TRY(subdir_fs->Initialize(exefs_fs_path));

            auto sd_ifs = MakeSharedObject<ams::fssrv::impl::FileSystemInterfaceAdapter,
                                           ams::fssrv::sf::IFileSystem>(std::move(subdir_fs), false);
            R_TRY(g_Manager.RegisterSession(
                move_h, ams::sf::cmif::ServiceObjectHolder(std::move(sd_ifs))));
            R_SUCCEED();
        }();

        // Only now (a session is registered) start the serve loop, so its wait
        // set is non-empty from the first iteration.
        if (R_SUCCEEDED(r))
            StartServerThreadOnce();

        return r.GetValue();
    }

    Result UnregisterExternalContent(u64 program_id) {
        // Tell Atmosphere's loader to stop using our external code. The fs
        // session we registered for it becomes orphaned and is cleaned up by
        // the server manager when the handle is dropped.
        return UnregisterExternalCodeCmd(program_id);
    }

} // namespace sl::sys::ecs
