#include <sl/menu/smi/MessageHandler.hpp>

namespace sl::menu::smi {

    void PollMessages(const MessageCallbacks &cbs) {
        using namespace sl::smi;

        AppletStorage st;
        // Daemon -> applet: events arrive on our InData queue (the daemon pushes
        // them with appletHolderPushInData). Non-blocking; drain until empty.
        // The initial SystemStatus is popped once in main() before this loop.
        while (R_SUCCEEDED(appletPopInData(&st))) {
            CommandHeader hdr = {};
            if (R_FAILED(appletStorageRead(&st, 0, &hdr, sizeof(hdr)))) {
                appletStorageClose(&st);
                continue;
            }
            appletStorageClose(&st);

            if (hdr.magic != Magic) continue;

            switch (static_cast<MenuMessage>(hdr.val)) {
                case MenuMessage::HomeRequested:
                    if (cbs.on_home_request) cbs.on_home_request();
                    break;
                case MenuMessage::SdCardEjected:
                    if (cbs.on_sd_ejected) cbs.on_sd_ejected();
                    break;
                case MenuMessage::SleepFinished:
                    if (cbs.on_sleep_finished) cbs.on_sleep_finished();
                    break;
                case MenuMessage::ApplicationListChanged:
                    if (cbs.on_app_list_changed) cbs.on_app_list_changed();
                    break;
                case MenuMessage::GameCardInserted:
                    if (cbs.on_gamecard_inserted) cbs.on_gamecard_inserted();
                    break;
                case MenuMessage::GameCardRemoved:
                    if (cbs.on_gamecard_removed) cbs.on_gamecard_removed();
                    break;
                default:
                    break;
            }
        }
    }

} // namespace sl::menu::smi
