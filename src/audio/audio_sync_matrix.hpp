#pragma once
// 64-channel phase-coherent playout matrix.
//
// Every channel owns a lock-free sample-addressed ring (float32, indexed by media timestamp, so
// out-of-order / retransmitted / FEC-recovered packets simply land at their sample position).
// The consumer renders all channels from ONE shared cursor:
//
//     T_playout = T_master_now - TargetLatencySamples
//
// so streams arriving with different network delays leave the matrix aligned to the sample. A single
// shared +/-100 ppm Hermite micro-resampler tracks the audio device clock drift versus the master
// clock; because the ratio is common to all channels, inter-channel phase is never disturbed.

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "audio/master_clock.hpp"

namespace lv {

struct MatrixChannelMeter {
    float level_dbfs = -120.0f;  // peak with ~300 ms/20 dB release
    bool active = false;         // data arrived within the last second
    uint64_t underruns = 0;      // gaps in an active channel at the playout cursor
    uint64_t samples_played = 0;
};

class AudioSyncMatrix {
public:
    static constexpr uint32_t kChannels = 64;
    static constexpr uint32_t kMinLatencyMs = 2;
    static constexpr uint32_t kMaxLatencyMs = 2000;

    explicit AudioSyncMatrix(uint32_t sample_rate = 48000);
    ~AudioSyncMatrix();
    AudioSyncMatrix(const AudioSyncMatrix&) = delete;
    AudioSyncMatrix& operator=(const AudioSyncMatrix&) = delete;

    uint32_t sample_rate() const { return rate_; }
    uint32_t ring_capacity() const { return cap_; }  // samples per channel (power of two)

    // ---- Producer (single network thread) ----
    void write(uint16_t channel, uint64_t media_ts, const float* samples, uint32_t n);
    // Atomic multi-channel frame block write: binds all `num_channels` to one monotonic `media_ts`.
    void write_frame_atomic(uint64_t media_ts, const float* const* planes, uint32_t num_channels, uint32_t frames);

    // ---- Latency & Adaptive Wi-Fi Jitter Buffer control ----
    void set_target_latency_ms(uint32_t ms);
    uint32_t target_latency_ms() const { return latency_ms_.load(std::memory_order_relaxed); }
    uint32_t effective_latency_ms() const { return effective_latency_ms_.load(std::memory_order_relaxed); }
    // Enables adaptive Wi-Fi jitter buffer (min_ms..max_ms, default 5..200 ms) with smooth +/-0.05% micro-resampling.
    void set_adaptive_jitter(bool enabled, uint32_t min_ms = 5, uint32_t max_ms = 200);
    void observe_network_jitter_us(uint32_t jitter_us);
    // Strict inter-channel atomic frame lock: if any active channel is missing at sample `pos`,
    // all active channels output 0 together so zero inter-track skew is mathematically guaranteed.
    void set_atomic_frame_lock(bool enabled) { atomic_frame_lock_.store(enabled, std::memory_order_release); }
    bool atomic_frame_lock() const { return atomic_frame_lock_.load(std::memory_order_acquire); }

    // ---- Consumer (audio thread) ----
    // Planar render: out[c] receives `frames` samples for c < num_channels (null entries skipped).
    void pop_planar(const MasterClock& clk, uint64_t local_now_us, float* const* out,
                    uint32_t num_channels, uint32_t frames);
    // Interleaved render: out[frame * num_channels + c].
    void pop_interleaved(const MasterClock& clk, uint64_t local_now_us, float* out,
                         uint32_t num_channels, uint32_t frames);

    MatrixChannelMeter meter(uint16_t channel) const;
    double playout_cursor() const { return cursor_pub_.load(std::memory_order_relaxed); }
    int64_t desired_cursor(const MasterClock& clk, uint64_t local_now_us) const;
    uint64_t resync_count() const { return resyncs_.load(std::memory_order_relaxed); }
    double resample_ppm() const { return ratio_ppm_.load(std::memory_order_relaxed); }
    // Highest media timestamp written to any channel (end of newest sample).
    uint64_t newest_media_ts() const;
    void reset();

    // ---- Stream epoch reset (sender restarted: new MasterClock timeline, sequence numbers reset) ----
    // Producer side (network thread): forget heads / first timestamps and ask the consumer for a
    // click-free re-anchor: the next rendered block crossfades the old position into silence and
    // playout restarts on the new timeline as soon as it has data.
    void request_epoch_reset();
    uint64_t epoch_resets() const { return epoch_resets_.load(std::memory_order_relaxed); }

    // MasterClock time (us) of the sample at the playout cursor (end of the last rendered block).
    uint64_t playout_media_ts_us() const {
        const double c = cursor_pub_.load(std::memory_order_relaxed);
        return c > 0.0 ? uint64_t(c * 1e6 / double(rate_)) : 0;
    }

private:
    struct Ring {
        std::vector<float> data;
        std::vector<uint16_t> tag;  // lap tag + 1 per sample; 0 = never written
    };
    struct Chan {
        std::atomic<Ring*> ring{nullptr};
        std::atomic<uint64_t> head{0};          // end of newest written sample
        std::atomic<uint64_t> first_ts{UINT64_MAX};
        std::atomic<uint64_t> write_count{0};
        // consumer-side state
        uint64_t seen_write_count = 0;
        int64_t active_left = 0;
        bool in_gap = false;
        std::atomic<float> level_db{-120.0f};
        std::atomic<bool> active{false};
        std::atomic<uint64_t> underruns{0};
        std::atomic<uint64_t> played{0};
    };

    struct Plan {
        double cursor = 0.0;
        double ratio = 1.0;
        bool seek = false;
        double old_cursor = 0.0;  // where the previous path would have continued
    };

    Plan plan(const MasterClock& clk, uint64_t local_now_us, uint32_t frames);
    void render_channel(uint16_t ch, const Plan& p, uint32_t frames, float* dst, size_t stride);
    bool sample_at(const Ring& r, int64_t pos, float& v) const;

    uint32_t rate_;
    uint32_t cap_;
    uint32_t shift_;
    uint32_t mask_;
    std::atomic<uint32_t> latency_ms_{50};
    std::atomic<uint32_t> effective_latency_ms_{50};
    std::atomic<bool> latency_dirty_{true};
    std::atomic<bool> adaptive_jitter_{false};
    std::atomic<uint32_t> jitter_min_ms_{5};
    std::atomic<uint32_t> jitter_max_ms_{200};
    std::atomic<uint32_t> observed_jitter_us_{0};
    std::atomic<bool> atomic_frame_lock_{false};
    std::atomic<bool> epoch_reset_req_{false};
    std::atomic<uint64_t> epoch_resets_{0};

    Chan ch_[kChannels];

    // consumer state
    bool started_ = false;
    double cursor_ = 0.0;
    double err_filt_ = 0.0;
    std::atomic<double> cursor_pub_{0.0};
    std::atomic<double> ratio_ppm_{0.0};
    std::atomic<uint64_t> resyncs_{0};
};

}  // namespace lv
