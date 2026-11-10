#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

// Fire-and-forget work on its own thread. libnx has no pthread_detach, so
// std::thread::detach() throws, and that took HOME down (the abort unmounted the
// SD card under the music thread: the "2168-0002" crash on News and Social).
// Finished threads are joined on the next call; one still running at exit is
// left to the process exit (never destroyed, so it never terminates the menu).
inline void runInBackground(std::function<void()> fn) {
    struct Job {
        std::thread thread;
        std::atomic<bool> done{false};
    };
    static std::mutex mu;
    static auto& jobs = *new std::vector<std::unique_ptr<Job>>; // never destroyed
    std::lock_guard<std::mutex> lk(mu);
    for (auto it = jobs.begin(); it != jobs.end();) {
        if ((*it)->done) {
            (*it)->thread.join();
            it = jobs.erase(it);
        } else {
            ++it;
        }
    }
    auto job = std::make_unique<Job>();
    Job* raw = job.get();
    job->thread = std::thread([raw, fn = std::move(fn)] {
        fn();
        raw->done = true;
    });
    jobs.push_back(std::move(job));
}

// Lets go of a thread without detach() (see above): it keeps running and its
// std::thread is simply never destroyed.
inline void abandonThread(std::thread& t) {
    if (t.joinable()) (void)new std::thread(std::move(t));
}
