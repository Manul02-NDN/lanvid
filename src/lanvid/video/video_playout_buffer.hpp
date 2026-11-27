#pragma once
// VideoPlayoutBuffer — Per-camera studio playout clock & jitter buffer for LanVid Video & OBS.
//
// Eliminates Wi-Fi frame jitter & micro-freezes via:
//   * Fixed Delay mode (default, 5..250 ms, presets 16 / 33 / 50 / 80 ms): releases frames strictly on
//     a smooth studio playout clock anchored to `media_ts_us`.
//   * Adaptive Wi-Fi mode: expands window up to 200 ms on arrival jitter bursts and decays smoothly.
//   * Non-Blocking Frame Hold: when a frame misses its window, the previous valid frame is held
//     without blocking UI/render threads.
//   * Smart IDR Request: drops corrupt/incomplete inter-frames and triggers a rate-limited (<= 1/500ms)
//     keyframe request until a clean IDR arrives.

#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>

#include "common/image.hpp"

namespace lv {

struct VideoPlayoutConfig {
    bool fixed_delay_mode = true;
    uint32_t target_delay_ms = 33;
    uint32_t adaptive_max_ms = 200;
    uint32_t idr_rate_limit_ms = 500;
    size_t max_queue_frames = 16;
};

struct VideoPlayoutStats {
    uint32_t target_delay_ms = 33;
    double effective_delay_ms = 33.0;
    bool fixed_mode = true;
    double jitter_ms = 0.0;
    uint32_t queued_frames = 0;
    uint64_t frames_pushed = 0;
    uint64_t frames_released = 0;
    uint64_t frames_held = 0;
    uint64_t frames_dropped_late = 0;
    uint64_t frames_dropped_corrupt = 0;
    uint64_t idr_requests = 0;
};

class VideoPlayoutBuffer {
public:
    VideoPlayoutBuffer() = default;
    explicit VideoPlayoutBuffer(const VideoPlayoutConfig& cfg);

    void set_config(const VideoPlayoutConfig& cfg);
    void set_delay(uint32_t delay_ms, bool fixed_mode);
    void set_idr_request_callback(std::function<void()> cb);
    void reset();

    // Enqueues a decoded frame stamped with sender `media_ts_us32` and local `arrival_us`.
    // Corrupt/incomplete P-frames are discarded and trigger a rate-limited IDR request.
    void push_frame(Image img, uint32_t frame_id, uint32_t media_ts_us32, uint64_t arrival_us,
                    bool is_keyframe, bool corrupt_or_incomplete = false);

    // Advances the playout clock to `now_us`. Returns true if a new frame became current.
    bool advance(uint64_t now_us);

    // Copies the currently held frame if its sequence differs from `inout_seq`.
    bool copy_held_frame(Image& out, uint64_t& inout_seq) const;

    // Convenience: advance(now_us) + copy_held_frame(out, inout_seq). Never blocks.
    bool poll_frame(uint64_t now_us, Image& out, uint64_t& inout_seq);

    VideoPlayoutStats stats() const;

private:
    struct QueuedFrame {
        Image image;
        uint32_t frame_id = 0;
        uint64_t media_ts_us = 0;
        uint64_t due_us = 0;
        bool is_keyframe = false;
    };

    uint64_t unwrap_media_ts_locked(uint32_t raw_ts_us32, uint64_t arrival_us, bool& reanchored);
    void request_idr_if_due_locked(uint64_t now_us, std::function<void()>& out_cb);

    mutable std::mutex mu_;
    VideoPlayoutConfig cfg_{};
    std::function<void()> on_idr_req_;

    std::deque<QueuedFrame> queue_;
    Image held_frame_;
    uint64_t held_seq_ = 0;
    bool has_held_ = false;

    // Playout clock state
    bool clock_init_ = false;
    bool has_media_ts_ = false;
    uint32_t last_raw_ts32_ = 0;
    uint64_t unwrapped_media_us_ = 0;
    int64_t clock_offset_us_ = 0;
    uint64_t last_arrival_us_ = 0;
    double jitter_us_ = 0.0;
    double effective_delay_ms_ = 33.0;
    uint64_t last_spike_us_ = 0;
    uint64_t last_decay_us_ = 0;

    // Smart IDR state
    bool awaiting_idr_ = false;
    uint64_t last_idr_req_us_ = 0;
    uint64_t last_advance_us_ = 0;

    // Telemetry
    uint64_t frames_pushed_ = 0;
    uint64_t frames_released_ = 0;
    uint64_t frames_held_ = 0;
    uint64_t frames_dropped_late_ = 0;
    uint64_t frames_dropped_corrupt_ = 0;
    uint64_t idr_requests_ = 0;
};

}  // namespace lv
