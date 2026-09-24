#include <sl/menu/usb/Mtp.hpp>
#include <haze.hpp>
#include <haze/transfer_status.hpp>
#include <atomic>

namespace sl::menu::usb {

    namespace {

        // Wakes the server's wait and tells it to stop. The event is left
        // signalled until the server waits on it, so a stop that lands before
        // the server has started waiting is not lost.
        class StopConsumer : public haze::EventConsumer {
            public:
                haze::EventReactor *reactor = nullptr;
                void ProcessEvent() override { reactor->SetResult(haze::ResultStopRequested()); }
        };

        haze::PtpObjectHeap g_heap;
        haze::EventReactor  g_reactor;
        haze::PtpResponder  g_responder;
        StopConsumer        g_stop;
        UEvent              g_stop_event;
        Thread              g_thread;
        bool                g_started = false;
        std::atomic<bool>   g_serving { false };   // USB is ours: safe to ask its state
        std::atomic<bool>   g_connected { false }; // last answer from MtpConnected

        void Run(void *) {
            if (R_FAILED(haze::LoadDeviceProperties())) return;
            // Fails when something else still holds USB; the menu simply runs
            // without file transfer until it is next started.
            if (R_FAILED(g_responder.Initialize(&g_reactor, &g_heap))) {
                g_responder.Finalize();
                return;
            }
            g_stop.reactor = &g_reactor;
            if (g_reactor.AddConsumer(&g_stop, waiterForUEvent(&g_stop_event))) {
                g_serving = true;
                (void)g_responder.LoopProcess();   // returns once stopped
                g_serving = false;
                g_reactor.RemoveConsumer(&g_stop);
            }
            g_responder.Finalize();
        }

    }

    void MtpStart() {
        if (g_started) return;
        ueventCreate(&g_stop_event, false);
        g_reactor.SetResult(haze::ResultSuccess());
        // Core 2: the menu draws on 0 and decodes art on 1. A transfer is
        // bounded by USB and the card, not by this thread.
        if (R_FAILED(threadCreate(&g_thread, Run, nullptr, nullptr, 0x10000, 0x2E, 2)) &&
            R_FAILED(threadCreate(&g_thread, Run, nullptr, nullptr, 0x10000, 0x2E, -2)))
            return;
        threadStart(&g_thread);
        g_started = true;
    }

    void MtpStop() {
        if (!g_started) return;
        ueventSignal(&g_stop_event);
        threadWaitForExit(&g_thread);
        threadClose(&g_thread);
        g_started = false;
        g_connected = false;
    }

    bool MtpConnected() {
        UsbState st = UsbState_Detached;
        const bool c = g_serving && R_SUCCEEDED(usbDsGetState(&st)) && st == UsbState_Configured;
        g_connected = c;
        return c;
    }

    void MtpGetStatus(MtpStatus &out) {
        const auto &s = haze::GetTransferStatus();
        out.connected = g_connected;
        out.op        = (MtpOp)s.op.load(std::memory_order_acquire);
        out.done      = s.done.load(std::memory_order_relaxed);
        out.total     = s.total.load(std::memory_order_relaxed);
        out.bytes     = s.bytes.load(std::memory_order_relaxed);
        out.received  = s.received.load(std::memory_order_relaxed);
        out.sent      = s.sent.load(std::memory_order_relaxed);
        out.deleted   = s.deleted.load(std::memory_order_relaxed);
        // Keeps the last name it read cleanly if this one was mid-write.
        char name[sizeof(out.name)];
        if (haze::TransferName(name, sizeof(name))) memcpy(out.name, name, sizeof(name));
    }

}
