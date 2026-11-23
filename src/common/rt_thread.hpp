#pragma once
// Real-time scheduling for audio / MIDI / network threads.
//
//   macOS   : Mach THREAD_TIME_CONSTRAINT_POLICY (period / computation / constraint in host ticks)
//   Windows : MMCSS "Pro Audio" task (AvSetMmThreadCharacteristicsW, avrt) + timeBeginPeriod(1)
//   Linux   : SCHED_FIFO when permitted (RLIMIT_RTPRIO / CAP_SYS_NICE), otherwise unchanged
//
// Use RealtimeThreadScope at the top of a thread function; it reverts on scope exit (MMCSS handle,
// timer resolution). All calls are best effort and never throw.

#include <cstdint>

namespace lv {

// period_us: how often the thread wakes (e.g. 128 frames @ 48 kHz = 2667 us);
// computation_us: CPU time it needs per period; constraint_us: deadline within the period.
bool make_current_thread_realtime(uint32_t period_us, uint32_t computation_us, uint32_t constraint_us);

// True if the calling thread currently runs with the real-time policy set above (for tests / UI).
bool current_thread_is_realtime();

class RealtimeThreadScope {
public:
    // Sensible defaults: computation = 25 % and constraint = 75 % of the period.
    explicit RealtimeThreadScope(uint32_t period_us);
    RealtimeThreadScope(uint32_t period_us, uint32_t computation_us, uint32_t constraint_us);
    ~RealtimeThreadScope();
    RealtimeThreadScope(const RealtimeThreadScope&) = delete;
    RealtimeThreadScope& operator=(const RealtimeThreadScope&) = delete;
    bool active() const { return active_; }

private:
    bool active_ = false;
    void* mmcss_ = nullptr;  // Windows MMCSS task handle
    bool timer_ = false;     // Windows timeBeginPeriod(1) taken
};

}  // namespace lv
