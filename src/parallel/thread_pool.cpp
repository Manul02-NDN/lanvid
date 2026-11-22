#include "parallel/thread_pool.hpp"

#include <algorithm>

namespace lv {

ThreadPool::ThreadPool(uint32_t num_threads) {
    if (num_threads == 0) {
        num_threads = std::thread::hardware_concurrency();
    }
    num_threads_ = std::clamp(num_threads, 1u, 128u);

    workers_.reserve(num_threads_);
    for (uint32_t id = 0; id < num_threads_; ++id) {
        workers_.emplace_back([this, id] { worker_loop(id); });
    }
}

ThreadPool::~ThreadPool() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    for (auto& w : workers_) {
        if (w.joinable()) w.join();
    }
}

void ThreadPool::worker_loop(uint32_t worker_id) {
    uint64_t seen_gen = 0;
    std::unique_lock<std::mutex> lk(mu_);
    for (;;) {
        cv_.wait(lk, [&] {
            return stop_ || (active_batch_ != nullptr && !active_batch_->closed && seen_gen != batch_gen_) ||
                   !tasks_.empty();
        });

        // Priority 1: low-latency parallel_for batch.
        if (active_batch_ != nullptr && !active_batch_->closed && seen_gen != batch_gen_) {
            seen_gen = batch_gen_;
            if (active_batch_->helpers_remaining > 0) {
                --active_batch_->helpers_remaining;
                ++active_batch_->active_helpers;
                Batch* b = active_batch_;
                lk.unlock();

                for (;;) {
                    const uint32_t idx = b->next.fetch_add(1, std::memory_order_relaxed);
                    if (idx >= b->count) break;
                    (*b->fn)(idx, worker_id);
                }

                lk.lock();
                --b->active_helpers;
                if (b->active_helpers == 0 && b->closed) {
                    done_cv_.notify_all();
                }
            }
            continue;
        }

        // Priority 2: async queued task.
        if (!tasks_.empty()) {
            auto task = std::move(tasks_.front());
            tasks_.pop_front();
            ++active_async_;
            lk.unlock();

            task(worker_id);

            lk.lock();
            --active_async_;
            if (active_async_ == 0 && tasks_.empty()) {
                done_cv_.notify_all();
            }
            continue;
        }

        if (stop_) break;
    }
}

void ThreadPool::parallel_for(uint32_t count, const std::function<void(uint32_t, uint32_t)>& fn) {
    if (count == 0) return;
    if (num_threads_ <= 1 || count == 1) {
        for (uint32_t i = 0; i < count; ++i) fn(i, 0);
        return;
    }

    Batch b;
    b.count = count;
    b.fn = &fn;
    // Caller participates as one thread, so wake at most (min(num_threads_, count) - 1) helpers.
    b.helpers_remaining = std::min(num_threads_, count) - 1;

    {
        std::unique_lock<std::mutex> lk(mu_);
        // Serialize if two callers invoke parallel_for concurrently.
        done_cv_.wait(lk, [&] { return active_batch_ == nullptr; });
        active_batch_ = &b;
        ++batch_gen_;
    }
    cv_.notify_all();

    // Calling thread immediately starts pulling work using slot `num_threads_`.
    const uint32_t caller_slot = num_threads_;
    for (;;) {
        const uint32_t idx = b.next.fetch_add(1, std::memory_order_relaxed);
        if (idx >= b.count) break;
        fn(idx, caller_slot);
    }

    {
        std::unique_lock<std::mutex> lk(mu_);
        b.closed = true;
        done_cv_.wait(lk, [&] { return b.active_helpers == 0; });
        active_batch_ = nullptr;
        done_cv_.notify_all();  // wake any queued concurrent parallel_for caller
    }
}

void ThreadPool::enqueue(std::function<void(uint32_t)> fn) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        tasks_.push_back(std::move(fn));
    }
    cv_.notify_one();
}

void ThreadPool::wait_idle() {
    std::unique_lock<std::mutex> lk(mu_);
    done_cv_.wait(lk, [&] { return tasks_.empty() && active_async_ == 0; });
}

}  // namespace lv
