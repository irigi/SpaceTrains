#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstddef>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace spacetrains::util {

// A fixed set of worker threads for data-parallel loops. parallel_for(n, fn) calls
// fn(0..n-1), each index exactly once, on the workers and the calling thread, and
// returns when all are done. fn must not touch shared mutable state; callers combine
// the results in index order, so the outcome does not depend on the thread count.
class ThreadPool {
public:
    // threads == 0: SPACETRAINS_THREADS, else the hardware threads less two (the UI and
    // the OS keep a core), at least one.
    explicit ThreadPool(std::size_t threads = 0);
    ~ThreadPool();
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    std::size_t size() const { return workers_.size() + 1; }
    void parallel_for(std::size_t count, const std::function<void(std::size_t)>& fn);

private:
    void worker_loop();
    void run_indices();

    std::vector<std::thread> workers_;
    std::mutex mutex_;
    std::condition_variable work_ready_;
    std::condition_variable work_done_;
    const std::function<void(std::size_t)>* job_ {nullptr};
    std::size_t job_count_ {0};
    std::atomic<std::size_t> next_index_ {0};
    std::size_t busy_workers_ {0};
    std::uint64_t generation_ {0};
    bool stopping_ {false};
};

}  // namespace spacetrains::util
