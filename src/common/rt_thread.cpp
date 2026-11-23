#include "common/rt_thread.hpp"

#include <algorithm>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <avrt.h>
#include <mmsystem.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#else
#include <pthread.h>
#include <sched.h>
#endif

namespace lv {

#if defined(_WIN32)
namespace {
thread_local bool t_rt = false;
}
#endif

bool make_current_thread_realtime(uint32_t period_us, uint32_t computation_us, uint32_t constraint_us) {
    period_us = std::max<uint32_t>(period_us, 500);
    constraint_us = std::clamp<uint32_t>(constraint_us, 100, period_us);
    computation_us = std::clamp<uint32_t>(computation_us, 50, constraint_us);
#if defined(__APPLE__)
    mach_timebase_info_data_t tb{};
    mach_timebase_info(&tb);
    auto us_to_ticks = [&](uint32_t us) -> uint32_t {
        return uint32_t((uint64_t(us) * 1000ull * tb.denom) / tb.numer);
    };
    thread_time_constraint_policy_data_t pol{};
    pol.period = us_to_ticks(period_us);
    pol.computation = us_to_ticks(computation_us);
    pol.constraint = us_to_ticks(constraint_us);
    pol.preemptible = TRUE;
    const kern_return_t kr = thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY,
                                               reinterpret_cast<thread_policy_t>(&pol),
                                               THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    return kr == KERN_SUCCESS;
#elif defined(_WIN32)
    (void)computation_us;
    (void)constraint_us;
    const bool ok = SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL) != 0;
    t_rt = ok;
    return ok;
#else
    (void)period_us;
    (void)computation_us;
    (void)constraint_us;
    sched_param sp{};
    sp.sched_priority = std::min(70, sched_get_priority_max(SCHED_FIFO));
    return pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) == 0;
#endif
}

bool current_thread_is_realtime() {
#if defined(__APPLE__)
    thread_time_constraint_policy_data_t pol{};
    mach_msg_type_number_t count = THREAD_TIME_CONSTRAINT_POLICY_COUNT;
    boolean_t get_default = FALSE;
    const kern_return_t kr = thread_policy_get(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY,
                                               reinterpret_cast<thread_policy_t>(&pol), &count, &get_default);
    return kr == KERN_SUCCESS && !get_default && pol.period != 0;
#elif defined(_WIN32)
    return t_rt;
#else
    int policy = 0;
    sched_param sp{};
    return pthread_getschedparam(pthread_self(), &policy, &sp) == 0 && policy == SCHED_FIFO;
#endif
}

RealtimeThreadScope::RealtimeThreadScope(uint32_t period_us)
    : RealtimeThreadScope(period_us, period_us / 4, period_us * 3 / 4) {}

RealtimeThreadScope::RealtimeThreadScope(uint32_t period_us, uint32_t computation_us, uint32_t constraint_us) {
#if defined(_WIN32)
    timer_ = timeBeginPeriod(1) == TIMERR_NOERROR;
    DWORD task_index = 0;
    HANDLE h = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);
    if (h) {
        AvSetMmThreadPriority(h, AVRT_PRIORITY_HIGH);
        mmcss_ = h;
    }
    active_ = make_current_thread_realtime(period_us, computation_us, constraint_us) || h != nullptr;
#else
    active_ = make_current_thread_realtime(period_us, computation_us, constraint_us);
#endif
}

RealtimeThreadScope::~RealtimeThreadScope() {
#if defined(_WIN32)
    if (mmcss_) AvRevertMmThreadCharacteristics(static_cast<HANDLE>(mmcss_));
    if (timer_) timeEndPeriod(1);
    t_rt = false;
#endif
}

}  // namespace lv
