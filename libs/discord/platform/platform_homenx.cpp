// HomeNX side of SwitchCord's platform layer: only what its Discord and network
// code calls (net/, discord/). The UI parts of platform.h aren't used here.
#include "platform/platform.h"

#include <chrono>
#include <switch.h>

namespace Platform {

int64_t NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

void Random(void* buf, size_t len) { randomGet(buf, len); }

bool NetworkUp() {
    NifmInternetConnectionStatus status{};
    // nifm is opened by the Theme Shop network runtime before Discord starts; if
    // it isn't, let the request itself fail instead of guessing.
    if (R_FAILED(nifmGetInternetConnectionStatus(nullptr, nullptr, &status)))
        return true;
    return status == NifmInternetConnectionStatus_Connected;
}

void SetCpuBoost(bool) {}

}  // namespace Platform
