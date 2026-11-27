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
// shared Hermite micro-resampler, hard-limited to +/-500 ppm (ratio 0.9995 .. 1.0005), tracks the audio
// device clock drift versus the master clock; because the ratio is common to all channels, inter-channel
// phase is never disturbed. Errors it cannot absorb are re-anchored once with a crossfade.
//
// Exactly ONE consumer may render from a matrix: every pop advances the shared cursor, so two pullers
// (e.g. a meter pump and a device mixer) would play it at double speed and fight the re-anchor.

#include <algorithm>
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
    //
    //   Fixed mode    (adaptive off, wired Ethernet / live monitoring): the playout delay is held at exactly
    //                 target_latency_ms; only the +/-100 ppm device-clock drift is tracked.
    //   Adaptive mode (Wi-Fi 6): the delay grows within [target_latency_ms .. max_ms] (default 200 ms) as
    //                 soon as the measured network jitter needs more headroom (small steps via +/-0.05%
    //                 micro-resampling, steps > 10 ms via one crossfaded re-anchor), and after the burst it
    //                 returns to the target slowly through +/-0.05% micro-resampling - no click.
    void set_target_latency_ms(uint32_t ms);
    uint32_t target_latency_ms() const { return latency_ms_.load(std::memory_order_relaxed); }
    // Delay the buffer is currently steering towards (== target in Fixed mode, >= target in Adaptive mode).
    uint32_t effective_latency_ms() const { return effective_latency_ms_.load(std::memory_order_relaxed); }
    // Measured playout delay right now: master clock position minus the playout cursor, in ms
    // (0 until playout has started). This is the "actual buffer" shown in the UI.
    double current_effective_latency_ms() const { return actual_latency_ms_.load(std::memory_order_relaxed); }
    void set_adaptive_jitter_enabled(bool enabled) { set_adaptive_jitter(enabled, kMinLatencyMs, kAdaptiveMaxMs); }
    bool adaptive_jitter_enabled() const { return adaptive_jitter_.load(std::memory_order_acquire); }
    // Adaptive range: [max(target, min_ms) .. max(target, max_ms)].
    void set_adaptive_jitter(bool enabled, uint32_t min_ms = kMinLatencyMs, uint32_t max_ms = kAdaptiveMaxMs);
    // Network thread: RFC 3550 inter-arrival jitter of any stream (us). Cheap, lock-free.
    void observe_network_jitter_us(uint32_t jitter_us);
    // Network thread: how late a directly received block arrived = MasterClock position at arrival minus
    // the end of its samples (one-way delay + packetization + jitter). Adaptive mode sizes the buffer so
    // the worst recent arrival still lands before its playout: delay >= lag envelope + render block + 2 ms.
    void observe_arrival_lag_samples(int64_t lag_samples);
    double arrival_lag_envelope_ms() const { return lag_env_pub_ms_.load(std::memory_order_relaxed); }
    // Jitter envelope (fast attack / ~4 s release) the adaptive mode sizes the buffer from.
    uint32_t jitter_envelope_us() const { return uint32_t(jitter_env_pub_us_.load(std::memory_order_relaxed)); }
    static constexpr uint32_t kAdaptiveMaxMs = 200;
    // Strict inter-channel atomic frame lock: if any active channel is missing at sample `pos`,
    // all active channels output 0 together so zero inter-track skew is mathematically guaranteed.
    void set_atomic_frame_lock(bool enabled) { atomic_frame_lock_.store(enabled, std::memory_order_release); }
    bool atomic_frame_lock() const { return atomic_frame_lock_.load(std::memory_order_acquire); }

    // Underrun hysteresis (personal monitoring): after an underrun (and at start) playout pauses with a
    // 2 ms fade until at least `percent` % of the target buffer is queued again, instead of trickling out
    // one packet at a time on the edge of empty. In Fixed mode the extra delay that recovery needed is then
    // kept as a cushion (held 30 s after the last underrun, then glides back at 0.4 ms/s), max 40 ms.
    void set_underrun_prefill(bool enabled, uint32_t percent = 75) {
        prefill_pct_.store(std::clamp<uint32_t>(percent, 10, 100), std::memory_order_relaxed);
        prefill_enabled_.store(enabled, std::memory_order_release);
    }
    // Drift-correction range of the micro-resampler (default 500 ppm; monitor phones use 2000 = +/-0.2 %).
    void set_max_resample_ppm(double ppm) { ratio_limit_.store(std::clamp(ppm, 50.0, 5000.0) * 1e-6, std::memory_order_relaxed); }
    // Dante-inspired Deterministic Timestamp-Driven Playout: locks the playout horizon strictly to
    // target_latency_ms (e.g. 5..10 ms) without expanding buffer/cushion on small network jitter;
    // absorbs clock & arrival phase drift smoothly via +/-0.1% (+/-1000 ppm) micro-rate resampling.
    void set_deterministic_playout(bool enabled, double max_trim_ppm = 1000.0);
    bool deterministic_playout() const { return deterministic_playout_.load(std::memory_order_acquire); }
    uint64_t prefill_events() const { return prefill_events_.load(std::memory_order_relaxed); }
    double fixed_cushion_ms() const { return cushion_pub_ms_.load(std::memory_order_relaxed); }
    // Total underrun events (start of a gap) over all channels.
    uint64_t total_underruns() const;

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
        std::unique_ptr<std::atomic<float>[]> data;
        std::unique_ptr<std::atomic<uint32_t>[]> tag;  // (epoch << 24) | lap; 0 = never written
        uint32_t size = 0;

        explicit Ring(uint32_t cap)
            : data(std::make_unique<std::atomic<float>[]>(cap)),
              tag(std::make_unique<std::atomic<uint32_t>[]>(cap)),
              size(cap) {
            for (uint32_t i = 0; i < cap; ++i) {
                data[i].store(0.0f, std::memory_order_relaxed);
                tag[i].store(0u, std::memory_order_relaxed);
            }
        }
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
        bool gap_pending = false;        // gap seen, not yet followed by audio (counted on resume)
        float gap_hold = 0.0f;           // last played value, decays through a gap
        float gap_from = 0.0f;
        float gap_decay = 0.93f;         // per-sample (~2 ms to -60 dB at 48 kHz)
        uint32_t gap_fadein = 1u << 30;  // samples since the gap ended
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
        bool hold = false;        // prefill: render the gap fade, do not advance, do not count underruns
        uint32_t epoch = 1;
        uint32_t old_epoch = 1;
    };

    Plan plan(const MasterClock& clk, uint64_t local_now_us, uint32_t frames);
    void render_channel(uint16_t ch, const Plan& p, uint32_t frames, float* dst, size_t stride);
    bool sample_at(const Ring& r, int64_t pos, uint32_t epoch, float& v) const;

    uint32_t rate_;
    uint32_t cap_;
    uint32_t shift_;
    uint32_t mask_;
    std::atomic<uint32_t> latency_ms_{50};
    std::atomic<uint32_t> effective_latency_ms_{50};
    std::atomic<bool> latency_dirty_{true};
    std::atomic<bool> adaptive_jitter_{false};
    std::atomic<uint32_t> jitter_min_ms_{5};
    std::atomic<uint32_t> jitter_max_ms_{kAdaptiveMaxMs};
    std::atomic<uint32_t> jitter_peak_us_{0};       // max jitter reported since the last rendered block
    std::atomic<double> jitter_env_pub_us_{0.0};
    std::atomic<uint32_t> lag_peak_samples_{0};    // max arrival lag since the last rendered block
    std::atomic<double> lag_env_pub_ms_{0.0};
    std::atomic<double> eff_ms_pub_{50.0};          // consumer's steering target (fractional ms)
    std::atomic<double> actual_latency_ms_{0.0};
    std::atomic<bool> atomic_frame_lock_{false};
    std::atomic<bool> deterministic_playout_{false};
    std::atomic<bool> epoch_reset_req_{false};
    std::atomic<uint32_t> epoch_gen_{1};
    std::atomic<uint64_t> epoch_resets_{0};

    Chan ch_[kChannels];

    // consumer state
    bool started_ = false;
    uint32_t active_epoch_ = 1;
    std::vector<float> meter_scratch_;
    std::atomic<double> ratio_limit_{500e-6};
    std::atomic<bool> prefill_enabled_{false};
    std::atomic<uint32_t> prefill_pct_{75};
    std::atomic<bool> underrun_flag_{false};
    std::atomic<uint64_t> prefill_events_{0};
    std::atomic<double> cushion_pub_ms_{0.0};
    bool prefill_ = false;
    bool realize_cushion_ = false;
    double cushion_ms_ = 0.0;       // Fixed mode: learned extra delay after an underrun
    double cushion_hold_s_ = 0.0;
    double cursor_ = 0.0;
    double err_filt_ = 0.0;
    double jitter_env_us_ = 0.0;     // consumer-side envelope
    double lag_env_samples_ = 0.0;   // consumer-side arrival-lag envelope
    double eff_ms_ = 50.0;           // consumer-side adaptive steering target
    uint64_t hold_samples_ = 0;      // no shrinking while > 0 (after a growth step)
    bool was_adaptive_ = false;
    void update_effective_latency(uint32_t frames, bool adaptive);
    std::atomic<double> cursor_pub_{0.0};
    std::atomic<double> ratio_ppm_{0.0};
    std::atomic<uint64_t> resyncs_{0};
};

}  // namespace lv
