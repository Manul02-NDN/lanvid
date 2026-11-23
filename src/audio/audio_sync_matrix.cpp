#include "audio/audio_sync_matrix.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace lv {

namespace {
constexpr uint32_t kCrossfadeSamples = 96;
constexpr double kMaxRatioOffset = 100e-6;          // +/-100 ppm (standard LAN)
constexpr double kAdaptiveMaxRatioOffset = 500e-6;  // +/-0.05% (+/-500 ppm for Wi-Fi jitter absorption)
constexpr double kDeadbandSamples = 1.5;

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
}  // namespace

AudioSyncMatrix::AudioSyncMatrix(uint32_t sample_rate)
    : rate_(sample_rate ? sample_rate : 48000) {
    // 2.5 s of history (2 s max latency + retransmit/jitter headroom), rounded up to a power of two.
    cap_ = pow2_at_least(uint64_t(rate_) * 5 / 2);
    mask_ = cap_ - 1;
    shift_ = 0;
    while ((1u << shift_) < cap_) ++shift_;
}

AudioSyncMatrix::~AudioSyncMatrix() {
    for (auto& c : ch_) delete c.ring.load(std::memory_order_acquire);
}

void AudioSyncMatrix::reset() {
    started_ = false;
    err_filt_ = 0.0;
    for (auto& c : ch_) {
        Ring* r = c.ring.load(std::memory_order_acquire);
        if (r) std::fill(r->tag.begin(), r->tag.end(), uint16_t(0));
        c.head.store(0, std::memory_order_relaxed);
        c.first_ts.store(UINT64_MAX, std::memory_order_relaxed);
        c.seen_write_count = c.write_count.load(std::memory_order_relaxed);
        c.active_left = 0;
        c.in_gap = false;
        c.active.store(false, std::memory_order_relaxed);
    }
    latency_dirty_.store(true, std::memory_order_release);
}

void AudioSyncMatrix::set_target_latency_ms(uint32_t ms) {
    ms = std::clamp(ms, kMinLatencyMs, kMaxLatencyMs);
    if (latency_ms_.exchange(ms, std::memory_order_acq_rel) != ms) {
        effective_latency_ms_.store(ms, std::memory_order_release);
        latency_dirty_.store(true, std::memory_order_release);
    }
}

void AudioSyncMatrix::set_adaptive_jitter(bool enabled, uint32_t min_ms, uint32_t max_ms) {
    min_ms = std::clamp(min_ms, kMinLatencyMs, kMaxLatencyMs);
    max_ms = std::clamp(max_ms, min_ms, kMaxLatencyMs);
    jitter_min_ms_.store(min_ms, std::memory_order_relaxed);
    jitter_max_ms_.store(max_ms, std::memory_order_relaxed);
    adaptive_jitter_.store(enabled, std::memory_order_release);
    if (!enabled) {
        effective_latency_ms_.store(latency_ms_.load(std::memory_order_relaxed), std::memory_order_release);
    }
}

void AudioSyncMatrix::observe_network_jitter_us(uint32_t jitter_us) {
    // Fast attack, slow decay envelope on network jitter
    uint32_t prev = observed_jitter_us_.load(std::memory_order_relaxed);
    uint32_t next = (jitter_us > prev) ? jitter_us : uint32_t(double(prev) * 0.995 + double(jitter_us) * 0.005);
    observed_jitter_us_.store(next, std::memory_order_relaxed);

    if (adaptive_jitter_.load(std::memory_order_acquire)) {
        const uint32_t base = latency_ms_.load(std::memory_order_relaxed);
        const uint32_t lo = jitter_min_ms_.load(std::memory_order_relaxed);
        const uint32_t hi = jitter_max_ms_.load(std::memory_order_relaxed);
        // Reserve ~2.5x measured jitter margin inside [min_ms, max_ms] without triggering hard reanchor.
        const uint32_t margin_ms = uint32_t((uint64_t(next) * 5 + 1999) / 2000);
        const uint32_t want = std::clamp(std::max(base, lo + margin_ms), lo, hi);
        const uint32_t cur = effective_latency_ms_.load(std::memory_order_relaxed);
        if (want > cur) {
            effective_latency_ms_.store(want, std::memory_order_release);
        } else if (want < cur && cur > base) {
            effective_latency_ms_.store(cur - 1, std::memory_order_release);
        }
    }
}

void AudioSyncMatrix::write_frame_atomic(uint64_t ts, const float* const* planes,
                                         uint32_t num_channels, uint32_t frames) {
    if (!planes || frames == 0) return;
    const uint32_t nch = std::min(num_channels, kChannels);
    // Ensure rings exist for all channels first
    for (uint32_t chi = 0; chi < nch; ++chi) {
        Chan& c = ch_[chi];
        Ring* r = c.ring.load(std::memory_order_relaxed);
        if (!r) {
            r = new Ring();
            r->data.assign(cap_, 0.0f);
            r->tag.assign(cap_, 0);
            c.ring.store(r, std::memory_order_release);
        }
        const float* src = planes[chi];
        for (uint32_t i = 0; i < frames; ++i) {
            const uint32_t idx = uint32_t(ts + i) & mask_;
            r->data[idx] = src ? src[i] : 0.0f;
        }
    }
    // Single release fence publishes all N channels for [ts, ts + frames) atomically
    std::atomic_thread_fence(std::memory_order_release);
    const uint64_t end = ts + frames;
    for (uint32_t chi = 0; chi < nch; ++chi) {
        Chan& c = ch_[chi];
        Ring* r = c.ring.load(std::memory_order_relaxed);
        for (uint32_t i = 0; i < frames; ++i) {
            const uint64_t p = ts + i;
            r->tag[uint32_t(p) & mask_] = uint16_t((p >> shift_) + 1);
        }
        const uint64_t first = c.first_ts.load(std::memory_order_relaxed);
        if (ts < first) c.first_ts.store(ts, std::memory_order_relaxed);
        if (end > c.head.load(std::memory_order_relaxed)) c.head.store(end, std::memory_order_release);
        c.write_count.fetch_add(1, std::memory_order_release);
    }
}

void AudioSyncMatrix::write(uint16_t channel, uint64_t ts, const float* s, uint32_t n) {
    if (channel >= kChannels || n == 0) return;
    Chan& c = ch_[channel];
    Ring* r = c.ring.load(std::memory_order_relaxed);
    if (!r) {
        r = new Ring();
        r->data.assign(cap_, 0.0f);
        r->tag.assign(cap_, 0);
        c.ring.store(r, std::memory_order_release);
    }
    for (uint32_t i = 0; i < n; ++i) {
        const uint64_t p = ts + i;
        const uint32_t idx = uint32_t(p) & mask_;
        r->data[idx] = s[i];
        std::atomic_thread_fence(std::memory_order_release);
        r->tag[idx] = uint16_t((p >> shift_) + 1);
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

bool AudioSyncMatrix::sample_at(const Ring& r, int64_t pos, float& v) const {
    if (pos < 0) return false;
    const uint32_t idx = uint32_t(pos) & mask_;
    const uint16_t want = uint16_t((uint64_t(pos) >> shift_) + 1);
    if (r.tag[idx] != want) return false;
    v = r.data[idx];
    return true;
}

int64_t AudioSyncMatrix::desired_cursor(const MasterClock& clk, uint64_t local_now_us) const {
    const uint32_t eff_ms = adaptive_jitter_.load(std::memory_order_acquire)
                                ? effective_latency_ms_.load(std::memory_order_acquire)
                                : latency_ms_.load(std::memory_order_relaxed);
    const int64_t lat = int64_t(eff_ms) * rate_ / 1000;
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
    const int64_t desired = desired_cursor(clk, local_now_us);
    const bool dirty = latency_dirty_.exchange(false, std::memory_order_acq_rel);
    const bool adaptive = adaptive_jitter_.load(std::memory_order_acquire);
    if (!started_) {
        if (!clk.synced() && newest_media_ts() == 0) {  // nothing to align to yet
            p.cursor = cursor_ = double(std::max<int64_t>(desired, 0));
            p.ratio = 1.0;
            return p;
        }
        started_ = true;
        cursor_ = double(desired);
        err_filt_ = 0.0;
        p.cursor = cursor_;
        p.ratio = 1.0;
        return p;
    }

    const double err = double(desired) - cursor_;
    // In adaptive Wi-Fi jitter mode, absorb spikes up to 250 ms via smooth +/-0.05% micro-resampling
    // instead of triggering audible hard reanchors.
    const double hard = double(rate_) * (adaptive ? 0.250 : 0.040);
    if (dirty || std::fabs(err) > hard) {
        p.seek = true;
        p.old_cursor = cursor_;
        cursor_ = double(desired);
        err_filt_ = 0.0;
        p.cursor = cursor_;
        p.ratio = 1.0;
        resyncs_.fetch_add(1, std::memory_order_relaxed);
        return p;
    }

    // Low-pass the phase error (audio callbacks are bursty) with a ~0.5 s time constant.
    const double alpha = std::min(1.0, double(frames) / (double(rate_) * 0.5));
    err_filt_ += (err - err_filt_) * alpha;

    const double max_off = adaptive ? kAdaptiveMaxRatioOffset : kMaxRatioOffset;
    double off = 0.0;
    if (std::fabs(err_filt_) > kDeadbandSamples) {
        off = std::clamp(err_filt_ / (double(rate_) * (adaptive ? 1.5 : 4.0)), -max_off, max_off);
    }
    p.cursor = cursor_;
    p.ratio = 1.0 + off;
    ratio_ppm_.store(off * 1e6, std::memory_order_relaxed);
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
    auto read_at = [&](double base, double ratio, uint32_t i, bool& ok) -> float {
        const double pos = base + double(i) * ratio;
        const int64_t i0 = int64_t(std::floor(pos));
        const double frac = pos - double(i0);
        float x0 = 0.0f;
        ok = sample_at(*r, i0, x0);
        if (!ok) return 0.0f;
        if (frac < 1e-9) return x0;
        float xm1 = x0, x1 = x0, x2 = x0;
        sample_at(*r, i0 - 1, xm1);
        // The look-ahead samples must exist: substituting a missing one would give THIS channel a
        // slightly different value than the others (a silent inter-channel phase error). Treat it as
        // late data instead (gap -> concealment + underrun counter), like a missing x0.
        if (!sample_at(*r, i0 + 1, x1) || !sample_at(*r, i0 + 2, x2)) {
            ok = false;
            return 0.0f;
        }
        return hermite4(xm1, x0, x1, x2, float(frac));
    };

    bool gap_event = false;
    for (uint32_t i = 0; i < frames; ++i) {
        bool ok = false;
        float v = read_at(p.cursor, p.ratio, i, ok);
        if (p.seek && i < kCrossfadeSamples) {
            bool ok_old = false;
            const float vo = read_at(p.old_cursor, 1.0, i, ok_old);
            const float w = float(i + 1) / float(kCrossfadeSamples + 1);
            v = v * w + vo * (1.0f - w);
            ok = ok || ok_old;
        }
        if (!ok) {
            const int64_t pos = int64_t(std::floor(p.cursor + double(i) * p.ratio));
            // Not an underrun while the stream has not started yet at this position.
            if (active && first_ts != UINT64_MAX && uint64_t(std::max<int64_t>(pos, 0)) >= first_ts) {
                if (!c.in_gap) gap_event = true;
                c.in_gap = true;
            }
            v = 0.0f;
        } else {
            c.in_gap = false;
        }
        dst[size_t(i) * stride] = v;
        peak = std::max(peak, std::fabs(v));
    }
    if (gap_event) c.underruns.fetch_add(1, std::memory_order_relaxed);
    c.played.fetch_add(frames, std::memory_order_relaxed);

    const float db = peak > 1e-6f ? 20.0f * std::log10(peak) : -120.0f;
    const float release = 20.0f * float(frames) / (0.3f * float(rate_));
    const float prev = c.level_db.load(std::memory_order_relaxed);
    c.level_db.store(std::max(db, prev - release), std::memory_order_relaxed);
}

void AudioSyncMatrix::pop_planar(const MasterClock& clk, uint64_t local_now_us, float* const* out,
                                 uint32_t num_channels, uint32_t frames) {
    if (frames == 0) return;
    num_channels = std::min(num_channels, kChannels);
    const Plan p = plan(clk, local_now_us, frames);
    for (uint32_t c = 0; c < kChannels; ++c) {
        if (c < num_channels && out && out[c]) {
            render_channel(uint16_t(c), p, frames, out[c], 1);
        } else if (ch_[c].ring.load(std::memory_order_relaxed)) {
            // Meter-only channels still need to advance their activity/level state.
            static thread_local std::vector<float> scratch;
            if (scratch.size() < frames) scratch.resize(frames);
            render_channel(uint16_t(c), p, frames, scratch.data(), 1);
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
                if (!r || !sample_at(*r, i0, dummy)) {
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
        } else if (ch_[c].ring.load(std::memory_order_relaxed)) {
            static thread_local std::vector<float> scratch;
            if (scratch.size() < frames) scratch.resize(frames);
            render_channel(uint16_t(c), p, frames, scratch.data(), 1);
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
                if (!r || !sample_at(*r, i0, dummy)) {
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

}  // namespace lv
