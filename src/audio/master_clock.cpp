#include "audio/master_clock.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace lv {

namespace {
constexpr size_t kRttWindow = 64;
constexpr double kSkewLimit = 500e-6;  // crystals never differ by more than a few hundred ppm
// Skew is only estimated over a baseline long enough that RTT-asymmetry noise (tens of us) cannot pose
// as drift: 50 us over 5 s is 10 ppm, the same 50 us over 0.5 s would be 100 ppm of wrong playout speed.
constexpr uint64_t kSkewMinBaselineUs = 5'000'000;
constexpr uint64_t kSkewMinSamples = 8;
inline uint64_t double_to_bits(double v) {
    uint64_t b = 0;
    std::memcpy(&b, &v, sizeof(b));
    return b;
}

inline double bits_to_double(uint64_t b) {
    double v = 0.0;
    std::memcpy(&v, &b, sizeof(v));
    return v;
}
}  // namespace

MasterClock::Snapshot MasterClock::read() const {
    Snapshot out;
    for (;;) {
        const uint32_t s0 = seq_.load(std::memory_order_acquire);
        if (s0 & 1u) continue;
        out.ref_local_us = snap_.ref_local_us.load(std::memory_order_relaxed);
        out.offset_us = bits_to_double(snap_.offset_bits.load(std::memory_order_relaxed));
        out.skew = bits_to_double(snap_.skew_bits.load(std::memory_order_relaxed));
        out.sigma_us = bits_to_double(snap_.sigma_bits.load(std::memory_order_relaxed));
        std::atomic_thread_fence(std::memory_order_acquire);
        if (seq_.load(std::memory_order_relaxed) == s0) return out;
    }
}

void MasterClock::publish(const Snapshot& s) {
    const uint32_t s0 = seq_.load(std::memory_order_relaxed);
    seq_.store(s0 + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    snap_.ref_local_us.store(s.ref_local_us, std::memory_order_relaxed);
    snap_.offset_bits.store(double_to_bits(s.offset_us), std::memory_order_relaxed);
    snap_.skew_bits.store(double_to_bits(s.skew), std::memory_order_relaxed);
    snap_.sigma_bits.store(double_to_bits(s.sigma_us), std::memory_order_relaxed);
    seq_.store(s0 + 2, std::memory_order_release);
}

void MasterClock::reset() {
    init_ = false;
    t_first_ = 0;
    anchor_rtt_ = 0;
    rtt_window_.clear();
    synced_.store(false, std::memory_order_release);
    min_rtt_us_.store(0, std::memory_order_relaxed);
    accepted_.store(0, std::memory_order_relaxed);
    publish(Snapshot{});
}

void MasterClock::set_fixed_offset(int64_t offset_us) {
    Snapshot s;
    s.ref_local_us = 0;
    s.offset_us = double(offset_us);
    s.skew = 0.0;
    s.sigma_us = 0.0;
    publish(s);
    synced_.store(true, std::memory_order_release);
}

void MasterClock::on_exchange(uint64_t t1, uint64_t t2, uint64_t t3, uint64_t t4) {
    if (t4 <= t1) return;
    const int64_t total = int64_t(t4 - t1);
    const int64_t srv = int64_t(t3) - int64_t(t2);
    if (srv < 0) return;
    const int64_t delta = total - srv;
    if (delta < 0 || delta > 2'000'000) return;
    last_rtt_us_.store(uint32_t(delta), std::memory_order_relaxed);

    rtt_window_.push_back(uint32_t(delta));
    if (rtt_window_.size() > kRttWindow) rtt_window_.pop_front();
    const uint32_t min_rtt = *std::min_element(rtt_window_.begin(), rtt_window_.end());
    min_rtt_us_.store(min_rtt, std::memory_order_relaxed);

    // Minimum-RTT gate: queueing delay is asymmetric, so slow exchanges carry a biased theta.
    // Only near-minimum exchanges are trusted: anything slower carries queueing / scheduling delay
    // (e.g. a PING waiting in the sender's socket until its next service_control() pass).
    const double excess = double(delta) - double(min_rtt);
    const double gate = std::max(150.0, double(min_rtt) * 0.25);
    if (init_ && excess > gate) return;

    const double theta = (double(int64_t(t2) - int64_t(t1)) + double(int64_t(t3) - int64_t(t4))) * 0.5;
    const uint64_t t_meas = t1 + uint64_t(total / 2);

    // Measurement noise: half of the delay uncertainty plus a timestamp-quantisation floor.
    const double sigma_m = 20.0 + 0.5 * excess + 0.05 * double(min_rtt);
    const double R = sigma_m * sigma_m;

    // A new minimum RTT far below the one the offset was anchored on means every earlier sample carried
    // (asymmetric) queueing bias - typically a sender that answered PINGs late during start-up. Re-anchor
    // the offset on the better sample instead of letting the filter "learn" the bias as clock drift.
    const bool better_anchor = init_ && double(delta) + gate < double(anchor_rtt_);

    if (!init_ || better_anchor) {
        init_ = true;
        x_off_ = theta;
        x_skew_ = 0.0;
        p00_ = R;
        p01_ = 0.0;
        p11_ = (30e-6) * (30e-6);
        t_ref_ = t_meas;
        t_first_ = t_meas;
        anchor_rtt_ = uint32_t(delta);
        accepted_.store(0, std::memory_order_relaxed);
    } else {
        if (t_meas <= t_ref_) return;
        const double dt = double(t_meas - t_ref_);
        // Predict.
        x_off_ += x_skew_ * dt;
        const double q_off = 0.05 * 0.05 * (dt * 1e-6);   // offset random walk (us^2)
        const double q_skew = (0.02e-6) * (0.02e-6) * (dt * 1e-6);
        const double p00 = p00_ + dt * (2.0 * p01_ + dt * p11_) + q_off;
        const double p01 = p01_ + dt * p11_;
        const double p11 = p11_ + q_skew;
        // Update (H = [1 0]).
        const double innov = theta - x_off_;
        const double S = p00 + R;
        const double k0 = p00 / S;
        // Offset-only tracking until the baseline is long enough for a trustworthy skew.
        const bool skew_ok = t_meas >= t_first_ && (t_meas - t_first_) >= kSkewMinBaselineUs &&
                             accepted_.load(std::memory_order_relaxed) >= kSkewMinSamples;
        const double k1 = skew_ok ? p01 / S : 0.0;
        x_off_ += k0 * innov;
        x_skew_ += k1 * innov;
        x_skew_ = std::clamp(x_skew_, -kSkewLimit, kSkewLimit);
        p00_ = (1.0 - k0) * p00;
        p01_ = skew_ok ? (1.0 - k0) * p01 : 0.0;
        p11_ = skew_ok ? p11 - k1 * p01 : p11_;
        t_ref_ = t_meas;
        anchor_rtt_ = std::min<uint32_t>(anchor_rtt_, uint32_t(delta));
    }

    Snapshot s;
    s.ref_local_us = t_ref_;
    s.offset_us = x_off_;
    s.skew = x_skew_;
    s.sigma_us = std::sqrt(std::max(0.0, p00_));
    publish(s);
    accepted_.fetch_add(1, std::memory_order_relaxed);
    synced_.store(true, std::memory_order_release);
}

int64_t MasterClock::master_us(uint64_t local_us) const {
    const Snapshot s = read();
    const double dt = double(int64_t(local_us) - int64_t(s.ref_local_us));
    return int64_t(local_us) + int64_t(std::llround(s.offset_us + s.skew * dt));
}

int64_t MasterClock::master_samples(uint64_t local_us, uint32_t sample_rate) const {
    const int64_t m = master_us(local_us);
    // Split to avoid 64-bit overflow for multi-day uptimes.
    const int64_t whole = m / 1'000'000;
    const int64_t frac = m % 1'000'000;
    return whole * int64_t(sample_rate) + (frac * int64_t(sample_rate)) / 1'000'000;
}

int64_t MasterClock::local_us_for_master(int64_t master_us_v) const {
    const Snapshot s = read();
    // local + offset + skew*(local - ref) = master  =>  solve for local.
    const double num = double(master_us_v) - s.offset_us + s.skew * double(int64_t(s.ref_local_us));
    return int64_t(std::llround(num / (1.0 + s.skew)));
}

double MasterClock::skew_ppm() const { return read().skew * 1e6; }
double MasterClock::offset_sigma_us() const { return read().sigma_us; }

}  // namespace lv
