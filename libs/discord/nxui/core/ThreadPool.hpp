#pragma once
// All the client wants from nxui's pool: its workers off the render thread's core.
#include <atomic>
#include <switch.h>
namespace nxui {
inline void moveToWorkerCores() {
    // Core 3 first: a game owns 0-2 and its busy threads never let a thread of
    // ours run there, so the presence only went out on the way back to the menu.
    // On the menu, the main thread outranks these and they spill onto idle 1-2.
    svcSetThreadCoreMask(CUR_THREAD_HANDLE, 3, (1u << 1) | (1u << 2) | (1u << 3));
}
}
