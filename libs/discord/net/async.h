#pragma once

#include <functional>

// Background work and main-thread hand-back. API calls and image downloads run on
// separate pools so a slow image never delays sending a message.
namespace Async {

enum class Pool { Api, Images };

void Start();
void Stop();
// Tells the workers to finish (drops queued work) without waiting for them;
// Stop() then only joins. HomeNX starts this when a game launches.
void RequestStop();
// Stop() that waits at most timeoutMs; workers still busy then are let go.
bool StopFor(int timeoutMs);
void Run(Pool pool, std::function<void()> work);
// Queues fn to run on the main thread during Pump().
void Post(std::function<void()> fn);
// Runs queued main-thread callbacks. Returns how many ran.
int Pump();
// Drops queued (not yet started) work, e.g. image loads for a channel we left.
void ClearPending(Pool pool);
int PendingCount(Pool pool);

}  // namespace Async
