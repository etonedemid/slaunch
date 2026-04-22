#include <sl/sys/smi/SystemProtocol.hpp>
#include <sl/sys/la/LibraryApplet.hpp>

namespace sl::sys::smi {

    using namespace sl::smi;

    SystemMessage ReceiveCommand(void *payload_out, size_t payload_max) {
        // Pop one storage from sMenu's output queue
        AppletStorage st;
        if (R_FAILED(la::PopStorage(st)))
            return SystemMessage::Invalid;

        StorageReader r(st);

        CommandHeader hdr = {};
        if (R_FAILED(r.Read(hdr))) return SystemMessage::Invalid;
        if (hdr.magic != Magic)   return SystemMessage::Invalid;

        auto msg = static_cast<SystemMessage>(hdr.val);

        // Read payload if caller wants it
        if (payload_out && payload_max > 0)
            r.ReadRaw(payload_out, payload_max);

        return msg;
    }

} // namespace sl::sys::smi
