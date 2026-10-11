#include "util/ThreadPool.hpp"

#include <algorithm>
#include <cstdlib>
#include <string>

namespace spacetrains::util {

namespace {

std::size_t default_thread_count() {
    if (const char* env = std::getenv("SPACETRAINS_THREADS")) {
        try {
            return std::max<std::size_t>(1, static_cast<std::size_t>(std::stoul(env)));
        } catch (...) {
        }
    }
    const std::size_t hardware = std::thread::hardware_concurrency();
    return hardware > 2 ? hardware - 2 : 1;
}

}  // namespace

ThreadPool::ThreadPool(std::size_t threads) {
    const std::size_t total = threads == 0 ? default_thread_count() : threads;
    for (std::size_t i = 1; i < total; ++i) {
        workers_.emplace_back([this] { worker_loop(); });
    }
}

ThreadPool::~ThreadPool() {
    {
        const std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    work_ready_.notify_all();
    for (auto& worker : workers_) {
        worker.join();
    }
}

void ThreadPool::run_indices() {
    for (std::size_t i = next_index_.fetch_add(1); i < job_count_; i = next_index_.fetch_add(1)) {
        (*job_)(i);
    }
}

void ThreadPool::worker_loop() {
    std::uint64_t seen_generation = 0;
    for (;;) {
        {
            std::unique_lock lock(mutex_);
            work_ready_.wait(lock, [&] { return stopping_ || generation_ != seen_generation; });
            if (stopping_) {
                return;
            }
            seen_generation = generation_;
            ++busy_workers_;
        }
        run_indices();
        {
            const std::lock_guard lock(mutex_);
            --busy_workers_;
        }
        work_done_.notify_one();
    }
}

void ThreadPool::parallel_for(std::size_t count, const std::function<void(std::size_t)>& fn) {
    if (count == 0) {
        return;
    }
    if (workers_.empty() || count == 1) {
        for (std::size_t i = 0; i < count; ++i) {
            fn(i);
        }
        return;
    }
    {
        const std::lock_guard lock(mutex_);
        job_ = &fn;
        job_count_ = count;
        next_index_.store(0);
        ++generation_;
    }
    work_ready_.notify_all();
    run_indices();
    std::unique_lock lock(mutex_);
    // Workers that woke late find no index left and leave at once; wait for the busy ones.
    work_done_.wait(lock, [&] { return busy_workers_ == 0 && next_index_.load() >= job_count_; });
    job_ = nullptr;
}

}  // namespace spacetrains::util
