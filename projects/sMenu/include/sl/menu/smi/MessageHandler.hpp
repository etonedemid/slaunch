#pragma once
#include <sl/smi/Protocol.hpp>

// Handles async MenuMessage events sent from sSystem → sMenu
// Call PollMessages() each frame; register callbacks before the loop.

namespace sl::menu::smi {

    struct MessageCallbacks {
        void (*on_home_request)()         = nullptr;
        void (*on_sd_ejected)()           = nullptr;
        void (*on_sleep_finished)()       = nullptr;
        void (*on_app_list_changed)()     = nullptr;
        void (*on_gamecard_inserted)()    = nullptr;
        void (*on_gamecard_removed)()     = nullptr;
    };

    // Check the general channel for pending MenuMessage storages and dispatch.
    // Non-blocking; call each frame.
    void PollMessages(const MessageCallbacks &cbs);

} // namespace sl::menu::smi
