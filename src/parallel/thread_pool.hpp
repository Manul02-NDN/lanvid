#pragma once
// Fixed-size worker ThreadPool supporting:
//  - low-latency parallel_for with lock-free atomic work stealing (caller participates);
//  - async task enqueue with per-worker slot IDs (for receiver-side streaming slice decode).

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace lv {

class ThreadPool {
public:
    // num_threads == 0 selects std::thread::hardware_concurrency() (minimum 1).
    explicit ThreadPool(uint32_t num_threads = 0);
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // Logical concurrency target (at most num_threads() threads execute parallel_for at once).
    uint32_t num_threads() const { return num_threads_; }

    // Number of distinct slot IDs passed to callbacks: [0 .. slot_count() - 1].
    // Workers use [0 .. num_threads-1]; caller participating in parallel_for uses `num_threads`.
    uint32_t slot_count() const { return num_threads_ + 1; }

    // Runs fn(index, slot_id) for every index in [0, count).
    // Blocks until all indices have completed.
    void parallel_for(uint32_t count, const std::function<void(uint32_t index, uint32_t slot_id)>& fn);

    // Enqueues an asynchronous task to run on one of the background workers (slot_id in [0, num_threads)).
    void enqueue(std::function<void(uint32_t slot_id)> fn);

    // Waits until all enqueued asynchronous tasks have completed.
    void wait_idle();

private:
    struct Batch {
        std::atomic<uint32_t> next{0};
        uint32_t count = 0;
        const std::function<void(uint32_t, uint32_t)>* fn = nullptr;
        uint32_t helpers_remaining = 0;
        uint32_t active_helpers = 0;
        bool closed = false;
    };

    void worker_loop(uint32_t worker_id);

    uint32_t num_threads_;
    std::vector<std::thread> workers_;

    std::mutex mu_;
    std::condition_variable cv_;
    std::condition_variable done_cv_;
    bool stop_ = false;

    // Active parallel_for batch (serialized if called from multiple threads simultaneously).
    Batch* active_batch_ = nullptr;
    uint64_t batch_gen_ = 0;

    // Async FIFO queue.
    std::deque<std::function<void(uint32_t)>> tasks_;
    uint32_t active_async_ = 0;
};

}  // namespace lv
