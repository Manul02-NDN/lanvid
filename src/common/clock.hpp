#pragma once
// Monotonic steady clock in microseconds + accurate frame-pacing sleep helper.

#include <chrono>
#include <cstdint>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <mach/mach_time.h>
#endif

namespace lv {

inline uint64_t now_us() {
    using namespace std::chrono;
    return uint64_t(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

inline uint32_t now_us32() { return uint32_t(now_us()); }

// Signed difference (a - b) for wrapping 32-bit microsecond timestamps (valid up to ~35 min).
inline int32_t diff_us32(uint32_t a, uint32_t b) { return int32_t(a - b); }

#if defined(_WIN32)
namespace detail {
struct ThreadWaitableTimer {
    HANDLE handle = nullptr;
    ThreadWaitableTimer()
        : handle(CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                        TIMER_ALL_ACCESS)) {}
    ~ThreadWaitableTimer() {
        if (handle) CloseHandle(handle);
    }
    ThreadWaitableTimer(const ThreadWaitableTimer&) = delete;
    ThreadWaitableTimer& operator=(const ThreadWaitableTimer&) = delete;
};
inline HANDLE thread_waitable_timer() {
    static thread_local ThreadWaitableTimer t;
    return t.handle;
}
}  // namespace detail
#endif

// Sleeps accurately until `target_us`, periodically calling `idle_cb()` if provided.
template <class Fn>
inline void sleep_until_us(uint64_t target_us, Fn&& idle_cb) {
#if defined(_WIN32)
    HANDLE timer = detail::thread_waitable_timer();
#endif
    for (;;) {
        const uint64_t now = now_us();
        if (now >= target_us) return;
        idle_cb();
        const uint64_t rem = target_us - now;
#if defined(__APPLE__)
        if (rem > 300) {
            const uint64_t chunk_us = rem > 4000 ? 2000 : (rem - 150);
            // Absolute Mach deadline: microsecond-accurate wakeup prevents 100% busy-spin
            // (and macOS RT thread computation-budget throttling) on 1 ms pacer ticks.
            static const mach_timebase_info_data_t tb = [] {
                mach_timebase_info_data_t t{};
                mach_timebase_info(&t);
                return t;
            }();
            mach_wait_until(mach_absolute_time() + chunk_us * 1000ull * tb.denom / tb.numer);
            continue;
        }
#else
        if (rem > 1200) {
            const uint64_t chunk_us = rem > 4000 ? 2000 : (rem - 600);
#if defined(_WIN32)
            if (timer) {
                LARGE_INTEGER due;
                due.QuadPart = -LONGLONG(chunk_us * 10);  // 100-ns units, relative
                if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
                    WaitForSingleObject(timer, INFINITE);
                    continue;
                }
            }
#endif
            std::this_thread::sleep_for(std::chrono::microseconds(chunk_us));
            continue;
        }
#endif
        if (rem > 150) {
            std::this_thread::yield();
        }
    }
}

}  // namespace lv
