#include <sl/sys/pwr/Power.hpp>
#include <cstdio>
#include <cstring>
#include <memory>

namespace sl::sys::pwr {

    namespace {

        // Atmosphere adds command 65001 (SetRebootPayload) to bpc, under the
        // separate service name "bpc:ams". libnx has no wrapper for it, so the
        // session is opened and dispatched by hand. On a console without that
        // extension the service simply does not exist and the open fails, which
        // is how we detect "this CFW cannot chainload payloads".
        Result AmsBpcSetRebootPayload(const void *payload, size_t size) {
            Handle h = INVALID_HANDLE;
            Result rc = smGetServiceOriginal(&h, smEncodeName("bpc:ams"));
            if (R_FAILED(rc)) return rc;

            Service srv;
            serviceCreate(&srv, h);
            rc = serviceDispatch(&srv, 65001,
                .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_In },
                .buffers      = { { payload, size } },
            );
            serviceClose(&srv);
            return rc;
        }

        // A power-state request was accepted: the system tears itself down
        // asynchronously from here. Never return to the caller, or the daemon
        // would relaunch the menu into a console that is mid-shutdown (that is
        // what previously left the screen dark with the menu still responding,
        // until auto-sleep kicked in a minute later).
        [[noreturn]] void AwaitPowerTransition() {
            while (true)
                svcSleepThread(100'000'000ULL);
        }

        // Orderly reboot/shutdown, strongest mechanism first:
        //  1. am's Start{Reboot,Shutdown}Sequence -- the exact call stock
        //     qlaunch makes (we are AppletType_SystemApplet, so it is allowed).
        //     Runs the full state machine: applet/game teardown, fade-out,
        //     PSC notification, then the actual power transition.
        //  2. spsm directly -- same state machine, skipping am's frontend part.
        //  3. Raw bpc as a last resort: yanks the PMIC without notifying
        //     anything. This was the old primary path, and doing it from a
        //     fully-awake system is what broke reboot (the request got eaten
        //     and the console drifted into a half-asleep state instead).
        void RequestTransition(bool reboot) {
            Result rc = reboot ? appletStartRebootSequence()
                               : appletStartShutdownSequence();
            if (R_SUCCEEDED(rc))
                AwaitPowerTransition();

            if (R_SUCCEEDED(spsmInitialize())) {
                rc = spsmShutdown(reboot);
                spsmExit();
                if (R_SUCCEEDED(rc))
                    AwaitPowerTransition();
            }

            if (R_SUCCEEDED(bpcInitialize())) {
                rc = reboot ? bpcRebootSystem() : bpcShutdownSystem();
                bpcExit();
                if (R_SUCCEEDED(rc))
                    AwaitPowerTransition();
            }
            // Everything failed or was rejected; return so the caller can put
            // the menu back up.
        }

    }

    void Sleep() {
        appletStartSleepSequence(true);
    }

    void Reboot() {
        RequestTransition(true);
    }

    void Shutdown() {
        RequestTransition(false);
    }

    PayloadResult RebootToPayload(const char *path) {
        if (!path || !path[0]) return PayloadResult::NotFound;

        FILE *fp = fopen(path, "rb");
        if (!fp) return PayloadResult::NotFound;

        fseek(fp, 0, SEEK_END);
        const long len = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        if (len <= 0)                      { fclose(fp); return PayloadResult::NotFound; }
        if ((size_t)len > MaxPayloadSize)  { fclose(fp); return PayloadResult::TooBig;   }

        // Zero-padded to the full IRAM window: the payload is copied as one block,
        // so anything past the file must not be leftover garbage.
        auto buf = std::make_unique<u8[]>(MaxPayloadSize);
        if (!buf) { fclose(fp); return PayloadResult::TooBig; }
        std::memset(buf.get(), 0, MaxPayloadSize);
        const size_t got = fread(buf.get(), 1, (size_t)len, fp);
        fclose(fp);
        if (got != (size_t)len) return PayloadResult::NotFound;

        if (R_FAILED(AmsBpcSetRebootPayload(buf.get(), MaxPayloadSize)))
            return PayloadResult::Unsupported;

        // With the payload staged, any reboot chainloads it (Atmosphere hooks
        // the reboot itself), so the orderly sequence is used here too.
        Reboot();
        return PayloadResult::RebootFailed;   // only reached if the reboot did not take
    }

    const char *PayloadResultText(PayloadResult r) {
        switch (r) {
            case PayloadResult::NotFound:    return "Payload file could not be read";
            case PayloadResult::TooBig:      return "Payload is too large to chainload";
            case PayloadResult::Unsupported: return "This CFW cannot reboot to payload";
            case PayloadResult::RebootFailed:return "The console refused to reboot";
            default:                         return "";
        }
    }

} // namespace sl::sys::pwr
