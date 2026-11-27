#pragma once
// Master Clock synchronisation (PTP/NTP-style) for sample-accurate multichannel playout.
//
// The sender owns the master timeline: master_us = steady_now_us - epoch_us. The receiver runs the
// four-timestamp exchange over the existing PING/PONG packets:
//   t1 (receiver tx, local)  t2 (sender rx, master)  t3 (sender tx, master)  t4 (receiver rx, local)
//   offset theta = ((t2 - t1) + (t3 - t4)) / 2     (master - local)
//   delay  delta = (t4 - t1) - (t3 - t2)
// Samples are gated by minimum-RTT (asymmetric queueing inflates delta, not theta's truth) and fed
// to a 2-state Kalman filter (offset, skew) whose measurement noise scales with delta.

#include <atomic>
#include <cstdint>
#include <deque>

namespace lv {

class MasterClock {
public:
    MasterClock() = default;

    // Writer (receiver network thread). All values in microseconds, 64-bit.
    void on_exchange(uint64_t t1_local, uint64_t t2_master, uint64_t t3_master, uint64_t t4_local);

    // Forces a fixed relation (tests / loopback: master == local + offset, no drift).
    void set_fixed_offset(int64_t offset_us);
    void reset();

    bool synced() const { return synced_.load(std::memory_order_acquire); }

    // Readers (any thread, wait-free seqlock).
    int64_t master_us(uint64_t local_us) const;
    int64_t master_samples(uint64_t local_us, uint32_t sample_rate) const;
    // Inverse mapping: local time at which the master clock shows `master_us`.
    int64_t local_us_for_master(int64_t master_us) const;

    double skew_ppm() const;
    uint32_t min_rtt_us() const { return min_rtt_us_.load(std::memory_order_relaxed); }
    uint32_t last_rtt_us() const { return last_rtt_us_.load(std::memory_order_relaxed); }
    // 1-sigma offset uncertainty in microseconds.
    double offset_sigma_us() const;
    uint64_t accepted_samples() const { return accepted_.load(std::memory_order_relaxed); }

private:
    struct Snapshot {
        uint64_t ref_local_us = 0;
        double offset_us = 0.0;  // master - local at ref_local_us
        double skew = 0.0;       // d(offset)/d(local), dimensionless (1e-6 = 1 ppm)
        double sigma_us = 1e9;
    };
    struct AtomicSnapshot {
        std::atomic<uint64_t> ref_local_us{0};
        std::atomic<uint64_t> offset_bits{0};
        std::atomic<uint64_t> skew_bits{0};
        std::atomic<uint64_t> sigma_bits{0x41CDCD6500000000ULL};  // 1e9
    };
    Snapshot read() const;
    void publish(const Snapshot& s);

    // Kalman state (writer thread only).
    bool init_ = false;
    double x_off_ = 0.0, x_skew_ = 0.0;
    double p00_ = 0.0, p01_ = 0.0, p11_ = 0.0;
    uint64_t t_ref_ = 0;
    uint64_t t_first_ = 0;          // first accepted exchange (skew needs a long baseline)
    uint32_t anchor_rtt_ = 0;       // RTT of the exchange the offset was last (re)initialised from
    std::deque<uint32_t> rtt_window_;

    mutable std::atomic<uint32_t> seq_{0};
    AtomicSnapshot snap_{};
    std::atomic<bool> synced_{false};
    std::atomic<uint32_t> min_rtt_us_{0};
    std::atomic<uint32_t> last_rtt_us_{0};
    std::atomic<uint64_t> accepted_{0};
};

}  // namespace lv
