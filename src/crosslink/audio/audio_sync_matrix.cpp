#include "audio/audio_sync_matrix.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace cl {

namespace {
constexpr uint32_t kCrossfadeSamples = 96;
constexpr uint32_t kGapFadeSamples = 96;           // 2 ms fade-out / fade-in around a gap (no click)
constexpr double kGrowJumpSec = 0.010;              // adaptive growth beyond 10 ms: one crossfaded re-anchor
constexpr double kDeadbandSamples = 1.5;
// Adaptive sizing: headroom = 3 x RFC 3550 jitter envelope + 2 ms guard.
constexpr double kJitterMarginFactor = 3.0;
constexpr double kJitterGuardMs = 2.0;
constexpr double kJitterReleaseSec = 4.0;  // envelope release time constant
constexpr double kGrowHoldSec = 3.0;       // keep the grown buffer at least this long after a burst
constexpr double kShrinkMsPerSec = 0.4;    // < 0.5 ms/s = what +/-0.05% resampling can follow without lag

inline uint32_t pow2_at_least(uint64_t v) {
    uint32_t p = 1;
    while (p < v) p <<= 1;
    return p;
}

inline float hermite4(float xm1, float x0, float x1, float x2, float t) {
    const float c0 = x0;
    const float c1 = 0.5f * (x1 - xm1);
    const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
    const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
    return ((c3 * t + c2) * t + c1) * t + c0;
}

inline uint32_t make_ring_tag(uint32_t epoch, uint64_t pos, uint32_t shift) {
    const uint32_t lap = uint32_t((pos >> shift) & 0x00FFFFFFu);
    const uint32_t ep = ((epoch - 1u) % 255u) + 1u;  // 1..255, never 0
    return (ep << 24) | lap;
}
}  // namespace

AudioSyncMatrix::AudioSyncMatrix(uint32_t sample_rate)
    : rate_(sample_rate ? sample_rate : 48000) {
    // 2.5 s of history (2 s max latency + retransmit/jitter headroom), rounded up to a power of two.
    cap_ = pow2_at_least(uint64_t(rate_) * 5 / 2);
    mask_ = cap_ - 1;
    shift_ = 0;
    while ((1u << shift_) < cap_) ++shift_;
    meter_scratch_.assign(4096, 0.0f);
}

AudioSyncMatrix::~AudioSyncMatrix() {
    for (auto& c : ch_) delete c.ring.load(std::memory_order_acquire);
}

void AudioSyncMatrix::reset() {
    started_ = false;
    err_filt_ = 0.0;
    jitter_env_us_ = 0.0;
    lag_env_samples_ = 0.0;
    lag_peak_samples_.store(0, std::memory_order_relaxed);
    hold_samples_ = 0;
    eff_ms_ = double(latency_ms_.load(std::memory_order_relaxed));
    eff_ms_pub_.store(eff_ms_, std::memory_order_relaxed);
    effective_latency_ms_.store(latency_ms_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    actual_latency_ms_.store(0.0, std::memory_order_relaxed);
    jitter_peak_us_.store(0, std::memory_order_relaxed);
    const uint32_t next_ep = epoch_gen_.fetch_add(1, std::memory_order_acq_rel) + 1u;
    active_epoch_ = next_ep;
    for (auto& c : ch_) {
        Ring* r = c.ring.load(std::memory_order_acquire);
        if (r) {
            for (uint32_t i = 0; i < r->size; ++i) r->tag[i].store(0u, std::memory_order_relaxed);
        }
        c.head.store(0, std::memory_order_relaxed);
        c.first_ts.store(UINT64_MAX, std::memory_order_relaxed);
        c.seen_write_count = c.write_count.load(std::memory_order_relaxed);
        c.active_left = 0;
        c.in_gap = false;
        c.gap_pending = false;
        c.active.store(false, std::memory_order_relaxed);
    }
    latency_dirty_.store(true, std::memory_order_release);
}

void AudioSyncMatrix::request_epoch_reset() {
    epoch_gen_.fetch_add(1, std::memory_order_acq_rel);
    for (auto& c : ch_) {
        c.head.store(0, std::memory_order_relaxed);
        c.first_ts.store(UINT64_MAX, std::memory_order_relaxed);
    }
    epoch_resets_.fetch_add(1, std::memory_order_relaxed);
    epoch_reset_req_.store(true, std::memory_order_release);
}

void AudioSyncMatrix::set_target_latency_ms(uint32_t ms) {
    ms = std::clamp(ms, kMinLatencyMs, kMaxLatencyMs);
    if (latency_ms_.exchange(ms, std::memory_order_acq_rel) != ms) {
        // A new target is an explicit user action: re-anchor once (crossfaded) at the new delay. In
        // Adaptive mode the extra jitter headroom is kept on top (consumer recomputes it next block).
        if (!adaptive_jitter_.load(std::memory_order_acquire)) {
            effective_latency_ms_.store(ms, std::memory_order_release);
            eff_ms_pub_.store(double(ms), std::memory_order_release);
        }
        latency_dirty_.store(true, std::memory_order_release);
    }
}

void AudioSyncMatrix::set_adaptive_jitter(bool enabled, uint32_t min_ms, uint32_t max_ms) {
    min_ms = std::clamp(min_ms, kMinLatencyMs, kMaxLatencyMs);
    max_ms = std::clamp(max_ms, min_ms, kMaxLatencyMs);
    jitter_min_ms_.store(min_ms, std::memory_order_relaxed);
    jitter_max_ms_.store(max_ms, std::memory_order_relaxed);
    if (enabled) deterministic_playout_.store(false, std::memory_order_release);
    adaptive_jitter_.store(enabled, std::memory_order_release);
    // Switching to Fixed is applied by the consumer on its next block: if the buffer had grown, it
    // re-anchors (crossfaded) to exactly target_latency_ms instead of drifting back for minutes.
}

void AudioSyncMatrix::set_deterministic_playout(bool enabled, double max_trim_ppm) {
    deterministic_playout_.store(enabled, std::memory_order_release);
    if (enabled) {
        adaptive_jitter_.store(false, std::memory_order_release);
        ratio_limit_.store(std::clamp(max_trim_ppm, 50.0, 5000.0) * 1e-6, std::memory_order_relaxed);
        const uint32_t base = latency_ms_.load(std::memory_order_relaxed);
        cushion_pub_ms_.store(0.0, std::memory_order_relaxed);
        eff_ms_pub_.store(double(base), std::memory_order_release);
        effective_latency_ms_.store(base, std::memory_order_release);
    }
}

void AudioSyncMatrix::observe_network_jitter_us(uint32_t jitter_us) {
    uint32_t prev = jitter_peak_us_.load(std::memory_order_relaxed);
    while (jitter_us > prev &&
           !jitter_peak_us_.compare_exchange_weak(prev, jitter_us, std::memory_order_relaxed)) {
    }
}

void AudioSyncMatrix::observe_arrival_lag_samples(int64_t lag) {
    if (lag <= 0) return;
    const uint32_t v = uint32_t(std::min<int64_t>(lag, int64_t(rate_) * 2));
    uint32_t prev = lag_peak_samples_.load(std::memory_order_relaxed);
    while (v > prev && !lag_peak_samples_.compare_exchange_weak(prev, v, std::memory_order_relaxed)) {
    }
}

void AudioSyncMatrix::update_effective_latency(uint32_t frames, bool adaptive) {
    const double base = double(latency_ms_.load(std::memory_order_relaxed));
    const double dt = double(frames) / double(rate_);

    // Jitter envelope: instant attack, exponential release (time based, independent of packet rate).
    const double release = std::exp(-dt / kJitterReleaseSec);
    const double peak = double(jitter_peak_us_.exchange(0, std::memory_order_relaxed));
    jitter_env_us_ = std::max(peak, jitter_env_us_ * release);
    jitter_env_pub_us_.store(jitter_env_us_, std::memory_order_relaxed);
    const double lag_peak = double(lag_peak_samples_.exchange(0, std::memory_order_relaxed));
    lag_env_samples_ = std::max(lag_peak, lag_env_samples_ * release);
    lag_env_pub_ms_.store(lag_env_samples_ * 1000.0 / double(rate_), std::memory_order_relaxed);

    const bool det = deterministic_playout_.load(std::memory_order_acquire);
    if (det) {
        // Deterministic timestamp-driven playout: fixed latency horizon (5..10 ms), never expands cushion.
        cushion_ms_ = 0.0;
        cushion_hold_s_ = 0.0;
        cushion_pub_ms_.store(0.0, std::memory_order_relaxed);
        eff_ms_ = base;
        hold_samples_ = 0;
    } else if (!adaptive) {
        if (!prefill_enabled_.load(std::memory_order_relaxed)) {
            cushion_ms_ = 0.0;
        } else if (cushion_hold_s_ > 0.0) {
            cushion_hold_s_ = std::max(0.0, cushion_hold_s_ - dt);
        } else {
            cushion_ms_ = std::max(0.0, cushion_ms_ - kShrinkMsPerSec * dt);
        }
        cushion_pub_ms_.store(cushion_ms_, std::memory_order_relaxed);
        eff_ms_ = base + cushion_ms_;
        hold_samples_ = 0;
    } else {
        const double lo = std::max(base, double(jitter_min_ms_.load(std::memory_order_relaxed)));
        const double hi = std::max(base, double(jitter_max_ms_.load(std::memory_order_relaxed)));
        // Headroom from whichever estimate is larger: measured arrival lag (needs a synced MasterClock;
        // covers one-way delay + packetization + jitter) or the RFC 3550 jitter rule of thumb.
        const double block_ms = dt * 1000.0;
        const double by_lag = lag_env_samples_ > 0.0
                                  ? lag_env_samples_ * 1000.0 / double(rate_) + block_ms + kJitterGuardMs
                                  : 0.0;
        const double by_jitter = kJitterMarginFactor * jitter_env_us_ / 1000.0 + kJitterGuardMs;
        const double want = std::clamp(std::ceil(std::max(by_lag, by_jitter)), lo, hi);
        if (eff_ms_ < lo) eff_ms_ = lo;
        if (eff_ms_ > hi) eff_ms_ = hi;
        if (want > eff_ms_ + 0.25) {  // burst: grow at once (the cursor follows via +/-0.5% stretch)
            eff_ms_ = want;
            hold_samples_ = uint64_t(kGrowHoldSec * double(rate_));
        } else if (hold_samples_ > 0) {
            hold_samples_ = hold_samples_ > frames ? hold_samples_ - frames : 0;
        } else if (want < eff_ms_) {  // calm again: glide back to the target below the resampler's speed
            eff_ms_ = std::max(want, eff_ms_ - kShrinkMsPerSec * dt);
        }
    }
    eff_ms_pub_.store(eff_ms_, std::memory_order_relaxed);
    effective_latency_ms_.store(uint32_t(std::lround(eff_ms_)), std::memory_order_release);
}

void AudioSyncMatrix::write_frame_atomic(uint64_t ts, const float* const* planes,
                                         uint32_t num_channels, uint32_t frames) {
    if (!planes || frames == 0) return;
    const uint32_t nch = std::min(num_channels, kChannels);
    const uint32_t ep = epoch_gen_.load(std::memory_order_acquire);
    // Ensure rings exist for all channels first
    for (uint32_t chi = 0; chi < nch; ++chi) {
        Chan& c = ch_[chi];
        Ring* r = c.ring.load(std::memory_order_relaxed);
        if (!r) {
            r = new Ring(cap_);
            c.ring.store(r, std::memory_order_release);
        }
        const float* src = planes[chi];
        for (uint32_t i = 0; i < frames; ++i) {
            const uint32_t idx = uint32_t(ts + i) & mask_;
            const float v = (src && std::isfinite(src[i])) ? src[i] : 0.0f;
            r->data[idx].store(v, std::memory_order_relaxed);
        }
    }
    // Publish all N channels for [ts, ts + frames) with release semantics
    const uint64_t end = ts + frames;
    for (uint32_t chi = 0; chi < nch; ++chi) {
        Chan& c = ch_[chi];
        Ring* r = c.ring.load(std::memory_order_relaxed);
        for (uint32_t i = 0; i < frames; ++i) {
            const uint64_t p = ts + i;
            r->tag[uint32_t(p) & mask_].store(make_ring_tag(ep, p, shift_), std::memory_order_release);
        }
        const uint64_t first = c.first_ts.load(std::memory_order_relaxed);
        if (ts < first) c.first_ts.store(ts, std::memory_order_relaxed);
        if (end > c.head.load(std::memory_order_relaxed)) c.head.store(end, std::memory_order_release);
        c.write_count.fetch_add(1, std::memory_order_release);
    }
}

void AudioSyncMatrix::write(uint16_t channel, uint64_t ts, const float* s, uint32_t n) {
    if (channel >= kChannels || !s || n == 0) return;
    const uint32_t ep = epoch_gen_.load(std::memory_order_acquire);
    Chan& c = ch_[channel];
    Ring* r = c.ring.load(std::memory_order_relaxed);
    if (!r) {
        r = new Ring(cap_);
        c.ring.store(r, std::memory_order_release);
    }
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t idx = uint32_t(ts + i) & mask_;
        const float v = std::isfinite(s[i]) ? s[i] : 0.0f;
        r->data[idx].store(v, std::memory_order_relaxed);
    }
    for (uint32_t i = 0; i < n; ++i) {
        const uint64_t p = ts + i;
        r->tag[uint32_t(p) & mask_].store(make_ring_tag(ep, p, shift_), std::memory_order_release);
    }
    uint64_t first = c.first_ts.load(std::memory_order_relaxed);
    if (ts < first) c.first_ts.store(ts, std::memory_order_relaxed);
    const uint64_t end = ts + n;
    if (end > c.head.load(std::memory_order_relaxed)) c.head.store(end, std::memory_order_release);
    c.write_count.fetch_add(1, std::memory_order_release);
}

uint64_t AudioSyncMatrix::newest_media_ts() const {
    uint64_t m = 0;
    for (const auto& c : ch_) m = std::max(m, c.head.load(std::memory_order_acquire));
    return m;
}

bool AudioSyncMatrix::sample_at(const Ring& r, int64_t pos, uint32_t epoch, float& v) const {
    if (pos < 0) return false;
    const uint32_t idx = uint32_t(pos) & mask_;
    const uint32_t want = make_ring_tag(epoch, uint64_t(pos), shift_);
    if (r.tag[idx].load(std::memory_order_acquire) != want) return false;
    const float s = r.data[idx].load(std::memory_order_relaxed);
    if (r.tag[idx].load(std::memory_order_acquire) != want) return false;
    v = s;
    return true;
}

int64_t AudioSyncMatrix::desired_cursor(const MasterClock& clk, uint64_t local_now_us) const {
    const double eff_ms = deterministic_playout_.load(std::memory_order_acquire)
                              ? double(latency_ms_.load(std::memory_order_relaxed))
                              : (adaptive_jitter_.load(std::memory_order_acquire)
                                     ? eff_ms_pub_.load(std::memory_order_acquire)
                                     : double(latency_ms_.load(std::memory_order_relaxed)) +
                                           cushion_pub_ms_.load(std::memory_order_relaxed));
    const int64_t lat = int64_t(std::llround(eff_ms * double(rate_) / 1000.0));
    int64_t master;
    if (clk.synced()) {
        master = clk.master_samples(local_now_us, rate_);
    } else {
        master = int64_t(newest_media_ts());  // unsynced fallback: follow the newest arrival
    }
    return master - lat;
}

AudioSyncMatrix::Plan AudioSyncMatrix::plan(const MasterClock& clk, uint64_t local_now_us,
                                            uint32_t frames) {
    Plan p;
    if (epoch_reset_req_.exchange(false, std::memory_order_acq_rel)) {
        // New sender epoch: the old timeline is gone. Fade the old position out (crossfade into a
        // position with no data = silence) and re-anchor once the new stream has data / clock sync.
        p.old_epoch = active_epoch_;
        active_epoch_ = epoch_gen_.load(std::memory_order_acquire);
        p.epoch = active_epoch_;
        p.seek = started_;
        p.old_cursor = cursor_;
        p.cursor = -4.0 * double(frames);  // negative positions never hold data -> silence
        p.ratio = 1.0;
        started_ = false;
        err_filt_ = 0.0;
        cursor_ = p.cursor;
        actual_latency_ms_.store(0.0, std::memory_order_relaxed);
        return p;
    }
    if (!started_) {
        active_epoch_ = epoch_gen_.load(std::memory_order_acquire);
    }
    p.epoch = active_epoch_;
    p.old_epoch = active_epoch_;
    const bool det = deterministic_playout_.load(std::memory_order_acquire);
    const bool adaptive = !det && adaptive_jitter_.load(std::memory_order_acquire);
    const double prev_eff = eff_ms_;
    update_effective_latency(frames, adaptive);
    // Adaptive -> Fixed while the buffer was grown: snap (crossfaded) to exactly the fixed target.
    const bool to_fixed = was_adaptive_ && !adaptive && std::fabs(prev_eff - eff_ms_) > 0.5;
    was_adaptive_ = adaptive;
    const int64_t desired = desired_cursor(clk, local_now_us);
    const bool dirty = latency_dirty_.exchange(false, std::memory_order_acq_rel) || to_fixed;
    const double master_now = clk.synced() ? double(clk.master_samples(local_now_us, rate_))
                                           : double(newest_media_ts());
    auto publish_actual = [&](double cursor) {
        actual_latency_ms_.store(started_ ? std::max(0.0, (master_now - cursor) * 1000.0 / double(rate_)) : 0.0,
                                 std::memory_order_relaxed);
    };
    if (!started_) {
        if (!clk.synced() && newest_media_ts() == 0) {  // nothing to align to yet
            p.cursor = cursor_ = double(std::max<int64_t>(desired, 0));
            p.ratio = 1.0;
            return p;
        }
        started_ = true;
        cursor_ = double(desired);
        err_filt_ = 0.0;
        prefill_ = prefill_enabled_.load(std::memory_order_relaxed);
        p.cursor = cursor_;
        p.ratio = 1.0;
        publish_actual(p.cursor);
        return p;
    }

    // Deterministic (Dante-style) playout keeps its deadline: a dry spell is faded and concealed, never
    // paused, so the prefill hysteresis only applies to the Fixed / Adaptive buffers.
    if (det) {
        underrun_flag_.store(false, std::memory_order_relaxed);
        prefill_ = false;
        realize_cushion_ = false;
    } else if (prefill_enabled_.load(std::memory_order_acquire)) {
        const double base = double(latency_ms_.load(std::memory_order_relaxed));
        if (underrun_flag_.exchange(false, std::memory_order_acq_rel)) {
            // Only a DRY underrun (the cursor ran past the newest data) pauses playout. A hole from a lost
            // packet with newer data already queued is concealed and playback continues: holding there
            // would add latency for nothing (with 5 ms packets, "ahead" legitimately dips below 75 %).
            const double deficit = cursor_ - double(newest_media_ts());
            if (deficit > 0.0) {
                if (!prefill_) prefill_events_.fetch_add(1, std::memory_order_relaxed);
                prefill_ = true;
            }
            // Dry underrun: the delay was too short for this network. Learn the missing amount + one render
            // block + 2 ms guard as a cushion (Fixed mode) and realise it right now, inside the dropout.
            if (deficit > 0.0 && !adaptive && !det && clk.synced()) {
                const double now_ms = (master_now - cursor_) * 1000.0 / double(rate_);
                const double want = now_ms - base + (deficit + double(frames)) * 1000.0 / double(rate_) +
                                    kJitterGuardMs;
                cushion_ms_ = std::clamp(want, cushion_ms_, 80.0);
                cushion_hold_s_ = 30.0;
                cushion_pub_ms_.store(cushion_ms_, std::memory_order_relaxed);
                eff_ms_ = base + cushion_ms_;
                eff_ms_pub_.store(eff_ms_, std::memory_order_relaxed);
                effective_latency_ms_.store(uint32_t(std::lround(eff_ms_)), std::memory_order_release);
                realize_cushion_ = true;
            }
        }
        if (prefill_) {
            const double need = double(prefill_pct_.load(std::memory_order_relaxed)) / 100.0 * base *
                                double(rate_) / 1000.0;
            const double ahead = double(newest_media_ts()) - cursor_;
            const double lat_now = (master_now - cursor_) * 1000.0 / double(rate_);
            const bool short_of_cushion = realize_cushion_ && lat_now < eff_ms_ - 0.25;
            // Never let prefill hold freeze cursor_ into a 200-300 ms latency blowup: if packets
            // arrived and cursor_ fell > 30 ms behind eff_ms_, re-anchor cleanly to desired now.
            if (lat_now > eff_ms_ + 30.0 && ahead >= need) {
                prefill_ = false;
                realize_cushion_ = false;
                p.seek = true;
                p.old_cursor = cursor_;
                cursor_ = double(desired);
                err_filt_ = 0.0;
                p.cursor = cursor_;
                p.ratio = 1.0;
                resyncs_.fetch_add(1, std::memory_order_relaxed);
                publish_actual(p.cursor);
                return p;
            }
            if (ahead < need || short_of_cushion) {  // refill: hold the position (gap fade), nothing replayed
                p.cursor = cursor_;
                p.ratio = 0.0;
                p.hold = true;
                publish_actual(cursor_);
                return p;
            }
            prefill_ = false;
            if (realize_cushion_) err_filt_ = 0.0;
            realize_cushion_ = false;
        }
    }

    const double err = double(desired) - cursor_;
    // The micro-resampler only ever corrects up to +/-500 ppm by default (+/-0.2 % for monitor phones): it can never lock into
    // an audible slow-down. Anything it cannot absorb is re-anchored once with a 96-sample crossfade:
    //  * |error| > 40 ms in either mode (consumer stall, clock jump, latency change),
    //  * Adaptive growth > 10 ms (a Wi-Fi burst needs the deeper buffer now, not after a minute of stretch).
    const double hard = double(rate_) * 0.040;
    const bool grow_jump = adaptive && err < -double(rate_) * kGrowJumpSec;
    if (dirty || grow_jump || std::fabs(err) > hard) {
        p.seek = true;
        p.old_cursor = cursor_;
        cursor_ = double(desired);
        err_filt_ = 0.0;
        p.cursor = cursor_;
        p.ratio = 1.0;
        resyncs_.fetch_add(1, std::memory_order_relaxed);
        publish_actual(p.cursor);
        return p;
    }

    // Low-pass the phase error (audio callbacks are bursty).
    // Deterministic timestamp-driven mode uses a tighter 0.25 s filter and +/-0.1% (+/-1000 ppm) micro-rate trim
    // so the playout cursor locks strictly onto T_master_now - target_latency_samples without expanding buffer.
    const double filt_tau = det ? 0.25 : 0.5;
    const double alpha = std::min(1.0, double(frames) / (double(rate_) * filt_tau));
    err_filt_ += (err - err_filt_) * alpha;

    // Track consumer clock vs MasterClock smoothly via micro-resampling:
    //  - Deterministic mode: tight 1.0 s gain, deadband 0.5 sample, clamped to +/-ratio_limit_ (default +/-0.1%).
    //  - Adaptive mode: 1.5 s gain; Fixed mode: 4.0 s gain.
    const double deadband = det ? 0.5 : kDeadbandSamples;
    const double gain_sec = det ? 1.0 : (adaptive ? 1.5 : 4.0);
    double off = 0.0;
    if (std::fabs(err_filt_) > deadband) {
        const double lim = ratio_limit_.load(std::memory_order_relaxed);
        off = std::clamp(err_filt_ / (double(rate_) * gain_sec), -lim, lim);
    }
    p.cursor = cursor_;
    p.ratio = 1.0 + off;
    ratio_ppm_.store(off * 1e6, std::memory_order_relaxed);
    publish_actual(p.cursor);
    return p;
}

void AudioSyncMatrix::render_channel(uint16_t chi, const Plan& p, uint32_t frames, float* dst,
                                     size_t stride) {
    Chan& c = ch_[chi];
    const Ring* r = c.ring.load(std::memory_order_acquire);

    // Activity tracking (time-free): any write since the previous block re-arms a 1 s window.
    const uint64_t wc = c.write_count.load(std::memory_order_acquire);
    if (wc != c.seen_write_count) {
        c.seen_write_count = wc;
        c.active_left = int64_t(rate_);
    } else {
        c.active_left = std::max<int64_t>(0, c.active_left - frames);
    }
    const bool active = c.active_left > 0;
    c.active.store(active, std::memory_order_relaxed);

    if (!r) {
        for (uint32_t i = 0; i < frames; ++i) dst[size_t(i) * stride] = 0.0f;
        return;
    }
    const uint64_t first_ts = c.first_ts.load(std::memory_order_relaxed);

    float peak = 0.0f;
    auto read_at = [&](double base, double ratio, uint32_t i, uint32_t epoch, bool& ok) -> float {
        const double pos = base + double(i) * ratio;
        const int64_t i0 = int64_t(std::floor(pos));
        const double frac = pos - double(i0);
        float x0 = 0.0f;
        ok = sample_at(*r, i0, epoch, x0);
        if (!ok) return 0.0f;
        if (frac < 1e-9) return x0;
        float xm1 = x0, x1 = x0, x2 = x0;
        sample_at(*r, i0 - 1, epoch, xm1);
        // The look-ahead samples must exist: substituting a missing one would give THIS channel a
        // slightly different value than the others (a silent inter-channel phase error). Treat it as
        // late data instead (gap -> concealment + underrun counter), like a missing x0.
        if (!sample_at(*r, i0 + 1, epoch, x1) || !sample_at(*r, i0 + 2, epoch, x2)) {
            ok = false;
            return 0.0f;
        }
        return hermite4(xm1, x0, x1, x2, float(frac));
    };

    bool gap_event = false;
    bool resumed_after_gap = false;
    for (uint32_t i = 0; i < frames; ++i) {
        bool ok = false;
        float v = p.hold ? 0.0f : read_at(p.cursor, p.ratio, i, p.epoch, ok);
        if (p.seek && i < kCrossfadeSamples) {
            bool ok_old = false;
            const float vo = read_at(p.old_cursor, 1.0, i, p.old_epoch, ok_old);
            const float w = float(i + 1) / float(kCrossfadeSamples + 1);
            v = v * w + vo * (1.0f - w);
            ok = ok || ok_old;
        }
        if (!ok) {
            const int64_t pos = int64_t(std::floor(p.cursor + double(i) * p.ratio));
            // Not an underrun while the stream has not started yet at this position.
            if (!p.hold && active && first_ts != UINT64_MAX && uint64_t(std::max<int64_t>(pos, 0)) >= first_ts) {
                if (!c.in_gap) gap_event = true;
                c.in_gap = true;
                c.gap_pending = true;
            }
            // Gap: decay the last played value over 2 ms instead of a hard step to zero.
            c.gap_hold *= c.gap_decay;
            v = c.gap_hold;
            c.gap_fadein = 0;
        } else {
            c.in_gap = false;
            if (c.gap_pending) {  // audio resumed after a real gap: that was an underrun (not a stream end)
                c.gap_pending = false;
                resumed_after_gap = true;
            }
            if (c.gap_fadein < kGapFadeSamples) {
                // Resume after a gap: ramp in from the decayed tail (no step, even after a long gap).
                if (c.gap_fadein == 0) c.gap_from = c.gap_hold;
                const float w = float(++c.gap_fadein) / float(kGapFadeSamples);
                v = c.gap_from * (1.0f - w) + v * w;
            }
            c.gap_hold = v;
        }
        dst[size_t(i) * stride] = v;
        peak = std::max(peak, std::fabs(v));
    }
    // An underrun is counted when audio RESUMES after a gap, so a sender that simply stops (end of stream,
    // disconnect) does not show up as dropouts. The prefill hysteresis below still reacts at gap start.
    if (resumed_after_gap) c.underruns.fetch_add(1, std::memory_order_relaxed);
    if (gap_event) {
        // Only trigger a prefill halt when the buffer genuinely ran dry (cursor reached/passed
        // newest_media_ts). An isolated lost packet inside a populated buffer is smoothly
        // concealed by Opus PLC / gap-fade without halting playback!
        const double block_end = p.cursor + double(frames) * p.ratio;
        if (block_end >= double(newest_media_ts())) {
            underrun_flag_.store(true, std::memory_order_release);
        }
    }
    c.played.fetch_add(frames, std::memory_order_relaxed);

    const float db = peak > 1e-6f ? 20.0f * std::log10(peak) : -120.0f;
    const float release = 20.0f * float(frames) / (0.3f * float(rate_));
    const float prev = c.level_db.load(std::memory_order_relaxed);
    c.level_db.store(std::max(db, prev - release), std::memory_order_relaxed);
}

uint64_t AudioSyncMatrix::total_underruns() const {
    uint64_t n = 0;
    for (const auto& c : ch_) n += c.underruns.load(std::memory_order_relaxed);
    return n;
}

void AudioSyncMatrix::pop_planar(const MasterClock& clk, uint64_t local_now_us, float* const* out,
                                 uint32_t num_channels, uint32_t frames) {
    if (frames == 0) return;
    num_channels = std::min(num_channels, kChannels);
    const Plan p = plan(clk, local_now_us, frames);
    for (uint32_t c = 0; c < kChannels; ++c) {
        if (c < num_channels && out && out[c]) {
            render_channel(uint16_t(c), p, frames, out[c], 1);
        } else if (ch_[c].ring.load(std::memory_order_acquire)) {
            // Meter-only channels still need to advance their activity/level state.
            if (meter_scratch_.size() < frames) meter_scratch_.resize(frames);
            render_channel(uint16_t(c), p, frames, meter_scratch_.data(), 1);
        }
    }
    if (atomic_frame_lock_.load(std::memory_order_acquire) && out) {
        // Strict atomic frame lock: if any active channel missed sample `pos` (unrecovered by FEC),
        // zero all rendered channels for that exact sample index simultaneously -> 0-sample skew.
        for (uint32_t i = 0; i < frames; ++i) {
            const int64_t i0 = int64_t(std::floor(p.cursor + double(i) * p.ratio));
            bool frame_incomplete = false;
            for (uint32_t c = 0; c < num_channels; ++c) {
                if (!ch_[c].active.load(std::memory_order_relaxed)) continue;
                const Ring* r = ch_[c].ring.load(std::memory_order_acquire);
                float dummy = 0.0f;
                if (!r || !sample_at(*r, i0, p.epoch, dummy)) {
                    frame_incomplete = true;
                    break;
                }
            }
            if (frame_incomplete) {
                for (uint32_t c = 0; c < num_channels; ++c) {
                    if (out[c]) out[c][i] = 0.0f;
                }
            }
        }
    }
    cursor_ = p.cursor + double(frames) * p.ratio;
    cursor_pub_.store(cursor_, std::memory_order_relaxed);
}

void AudioSyncMatrix::pop_interleaved(const MasterClock& clk, uint64_t local_now_us, float* out,
                                      uint32_t num_channels, uint32_t frames) {
    if (frames == 0) return;
    num_channels = std::min(num_channels, kChannels);
    const Plan p = plan(clk, local_now_us, frames);
    for (uint32_t c = 0; c < kChannels; ++c) {
        if (c < num_channels && out) {
            render_channel(uint16_t(c), p, frames, out + c, num_channels);
        } else if (ch_[c].ring.load(std::memory_order_acquire)) {
            if (meter_scratch_.size() < frames) meter_scratch_.resize(frames);
            render_channel(uint16_t(c), p, frames, meter_scratch_.data(), 1);
        }
    }
    if (atomic_frame_lock_.load(std::memory_order_acquire) && out) {
        for (uint32_t i = 0; i < frames; ++i) {
            const int64_t i0 = int64_t(std::floor(p.cursor + double(i) * p.ratio));
            bool frame_incomplete = false;
            for (uint32_t c = 0; c < num_channels; ++c) {
                if (!ch_[c].active.load(std::memory_order_relaxed)) continue;
                const Ring* r = ch_[c].ring.load(std::memory_order_acquire);
                float dummy = 0.0f;
                if (!r || !sample_at(*r, i0, p.epoch, dummy)) {
                    frame_incomplete = true;
                    break;
                }
            }
            if (frame_incomplete) {
                std::memset(out + size_t(i) * num_channels, 0, size_t(num_channels) * sizeof(float));
            }
        }
    }
    cursor_ = p.cursor + double(frames) * p.ratio;
    cursor_pub_.store(cursor_, std::memory_order_relaxed);
}

MatrixChannelMeter AudioSyncMatrix::meter(uint16_t channel) const {
    MatrixChannelMeter m;
    if (channel >= kChannels) return m;
    const Chan& c = ch_[channel];
    m.level_dbfs = c.level_db.load(std::memory_order_relaxed);
    m.active = c.active.load(std::memory_order_relaxed);
    m.underruns = c.underruns.load(std::memory_order_relaxed);
    m.samples_played = c.played.load(std::memory_order_relaxed);
    return m;
}

}  // namespace cl
