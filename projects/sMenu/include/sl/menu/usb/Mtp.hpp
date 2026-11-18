#pragma once
#include <switch.h>

// USB file transfer: an MTP server (Atmosphere's haze, libs/haze) running for
// as long as the menu is up, so plugging the console into a computer shows the
// SD card as a drive. The menu process exits whenever it hands off to a game,
// homebrew or a system applet, which is also exactly when the server has to
// let go of USB for whatever runs next.
namespace sl::menu::usb {

    void MtpStart();       // returns at once; the server runs on its own thread
    void MtpStop();        // waits for the server to let go of USB
    bool MtpConnected();   // asks USB; about once a second is plenty

    // Switch Frame Tap streaming to its PC viewer over USB needs the same port,
    // and the menu used to take it back on every return from a game. While it
    // starts with the console set to connection = usb, MtpStart leaves USB to
    // it; MtpTakeUsb takes the port anyway, until the menu restarts.
    bool MtpYieldsToFrameTap();
    void MtpTakeUsb();

    // What the server is doing, cheap enough to read every frame.
    enum class MtpOp { None, Receive, Send, Delete };   // Receive: computer -> console
    struct MtpStatus {
        bool  connected = false;     // as of the last MtpConnected()
        MtpOp op = MtpOp::None;
        u64   done = 0, total = 0;   // current file; total 0 = size not known
        u64   bytes = 0;             // everything moved this session, for a rate
        u32   received = 0, sent = 0, deleted = 0;
        char  name[128] = {};        // path of the file, as the server has it
    };
    void MtpGetStatus(MtpStatus &out);

}
