#pragma once
#include <sl/smi/Protocol.hpp>

namespace sl::sys::smi {

    // Receive one command from sMenu (blocking - pops one AppletStorage from the
    // general channel and dispatches it). Returns the message type.
    // The payload bytes (after the header) are written into `payload_out`
    // up to `payload_max` bytes.
    sl::smi::SystemMessage ReceiveCommand(void *payload_out, size_t payload_max);

} // namespace sl::sys::smi
