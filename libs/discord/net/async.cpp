#include "net/async.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "net/http.h"

#include <nxui/core/ThreadPool.hpp>

#include "core/BackgroundThread.hpp"

namespace Async {
namespace {

struct WorkerPool {
    std::mutex mu;
    std::condition_variable cv;
    std::deque<std::function<void()>> jobs;
    std::vector<std::thread> threads;
    std::atomic<int> live{0};
    bool stop = false;

    void Start(int n) {
        stop = false;
        live += n;
        for (int i = 0; i < n; i++) {
            threads.emplace_back([this] {
                nxui::moveToWorkerCores(); // HomeNX: keep core 0 for the menu's frames
                while (true) {
                    std::function<void()> job;
                    {
                        std::unique_lock<std::mutex> lk(mu);
                        cv.wait(lk, [this] { return stop || !jobs.empty(); });
                        if (stop) break;
                        job = std::move(jobs.front());
                        jobs.pop_front();
                    }
                    job();
                }
                Http::ThreadCleanup();
                --live;
            });
        }
    }

    void RequestStop() {
        {
            std::lock_guard<std::mutex> lk(mu);
            stop = true;
            jobs.clear();
        }
        cv.notify_all();
    }

    bool StopFor(int timeoutMs) {
        RequestStop();
        for (int t = 0; t < timeoutMs && live > 0; t += 5)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        const bool done = live <= 0;
        for (auto& t : threads) {
            if (done) t.join();
            else abandonThread(t); // finishes its request on its own (libnx can't detach)
        }
        threads.clear();
        return done;
    }

    void Stop() {
        {
            std::lock_guard<std::mutex> lk(mu);
            stop = true;
            jobs.clear();
        }
        cv.notify_all();
        for (auto& t : threads) t.join();
        threads.clear();
    }
};

// Never destroyed: a worker let go by StopFor() may still use them at exit.
WorkerPool& g_api = *new WorkerPool;
WorkerPool& g_images = *new WorkerPool;
std::mutex& g_mainMu = *new std::mutex;
std::vector<std::function<void()>>& g_main = *new std::vector<std::function<void()>>;

WorkerPool& Get(Pool p) { return p == Pool::Api ? g_api : g_images; }

}  // namespace

void Start() {
    g_api.Start(2);
    g_images.Start(3);
}

void Stop() {
    g_api.Stop();
    g_images.Stop();
    std::lock_guard<std::mutex> lk(g_mainMu);
    g_main.clear();
}

bool StopFor(int timeoutMs) {
    const bool api = g_api.StopFor(timeoutMs);
    const bool images = g_images.StopFor(timeoutMs / 2);
    std::lock_guard<std::mutex> lk(g_mainMu);
    g_main.clear();
    return api && images;
}

void RequestStop() {
    g_api.RequestStop();
    g_images.RequestStop();
}

void Run(Pool pool, std::function<void()> work) {
    WorkerPool& p = Get(pool);
    {
        std::lock_guard<std::mutex> lk(p.mu);
        p.jobs.push_back(std::move(work));
    }
    p.cv.notify_one();
}

void Post(std::function<void()> fn) {
    std::lock_guard<std::mutex> lk(g_mainMu);
    g_main.push_back(std::move(fn));
}

int Pump() {
    std::vector<std::function<void()>> todo;
    {
        std::lock_guard<std::mutex> lk(g_mainMu);
        todo.swap(g_main);
    }
    for (auto& fn : todo) fn();
    return (int)todo.size();
}

void ClearPending(Pool pool) {
    WorkerPool& p = Get(pool);
    std::lock_guard<std::mutex> lk(p.mu);
    p.jobs.clear();
}

int PendingCount(Pool pool) {
    WorkerPool& p = Get(pool);
    std::lock_guard<std::mutex> lk(p.mu);
    return (int)p.jobs.size();
}

}  // namespace Async
