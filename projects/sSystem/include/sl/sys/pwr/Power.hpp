#pragma once
#include <switch.h>

// Power actions for the menu's power screen. Sleep/reboot/shutdown are plain
// libnx (bpc); rebooting into a payload is an Atmosphere extension, so it is the
// only one that can be unavailable at runtime.

namespace sl::sys::pwr {

    // A reboot payload is chainloaded out of IRAM, which is what caps its size.
    constexpr size_t MaxPayloadSize = 0x2F000;

    enum class PayloadResult {
        Ok,             // never actually returned: the console reboots first
        NotFound,       // no such file on the SD card
        TooBig,         // larger than MaxPayloadSize
        Unsupported,    // no bpc:ams -> this CFW cannot chainload payloads
        RebootFailed,   // the payload was accepted but bpc would not reboot
    };

    void Sleep();

    // Both of these only return if the request failed.
    void Reboot();
    void Shutdown();

    // Hand the payload at `path` to Atmosphere and reboot into it.
    PayloadResult RebootToPayload(const char *path);

    // One-line explanation of a failed RebootToPayload, for the menu's status bar.
    const char *PayloadResultText(PayloadResult r);

} // namespace sl::sys::pwr
