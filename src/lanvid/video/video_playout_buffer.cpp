#include "video/video_playout_buffer.hpp"

#include <algorithm>
#include <cmath>

namespace lv {

VideoPlayoutBuffer::VideoPlayoutBuffer(const VideoPlayoutConfig& cfg) {
    set_config(cfg);
}

void VideoPlayoutBuffer::set_config(const VideoPlayoutConfig& cfg) {
    std::lock_guard<std::mutex> lk(mu_);
    cfg_ = cfg;
    cfg_.target_delay_ms = std::clamp<uint32_t>(cfg_.target_delay_ms, 5, 250);
    cfg_.adaptive_max_ms = std::clamp<uint32_t>(cfg_.adaptive_max_ms, cfg_.target_delay_ms, 250);
    cfg_.idr_rate_limit_ms = std::clamp<uint32_t>(cfg_.idr_rate_limit_ms, 100, 5000);
    cfg_.max_queue_frames = std::clamp<size_t>(cfg_.max_queue_frames, 2, 64);
    if (cfg_.fixed_delay_mode) {
        effective_delay_ms_ = double(cfg_.target_delay_ms);
    } else {
        effective_delay_ms_ = std::max(effective_delay_ms_, double(cfg_.target_delay_ms));
    }
}

void VideoPlayoutBuffer::set_delay(uint32_t delay_ms, bool fixed_mode) {
    std::lock_guard<std::mutex> lk(mu_);
    cfg_.target_delay_ms = std::clamp<uint32_t>(delay_ms, 5, 250);
    cfg_.fixed_delay_mode = fixed_mode;
    if (fixed_mode) {
        effective_delay_ms_ = double(cfg_.target_delay_ms);
    } else {
        effective_delay_ms_ = std::clamp(effective_delay_ms_, double(cfg_.target_delay_ms), double(cfg_.adaptive_max_ms));
    }
}

void VideoPlayoutBuffer::set_idr_request_callback(std::function<void()> cb) {
    std::lock_guard<std::mutex> lk(mu_);
    on_idr_req_ = std::move(cb);
}

void VideoPlayoutBuffer::reset() {
    std::lock_guard<std::mutex> lk(mu_);
    queue_.clear();
    clock_init_ = false;
    has_media_ts_ = false;
    last_raw_ts32_ = 0;
    unwrapped_media_us_ = 0;
    clock_offset_us_ = 0;
    last_arrival_us_ = 0;
    jitter_us_ = 0.0;
    effective_delay_ms_ = double(cfg_.target_delay_ms);
    last_spike_us_ = 0;
    last_decay_us_ = 0;
    awaiting_idr_ = false;
}

uint64_t VideoPlayoutBuffer::unwrap_media_ts_locked(uint32_t raw_ts_us32, uint64_t arrival_us, bool& reanchored) {
    reanchored = false;
    if (!clock_init_) {
        clock_init_ = true;
        has_media_ts_ = (raw_ts_us32 != 0);
        last_raw_ts32_ = raw_ts_us32;
        unwrapped_media_us_ = has_media_ts_ ? uint64_t(raw_ts_us32) : arrival_us;
        clock_offset_us_ = int64_t(arrival_us) - int64_t(unwrapped_media_us_);
        last_arrival_us_ = arrival_us;
        last_decay_us_ = arrival_us;
        reanchored = true;
        return unwrapped_media_us_;
    }

    if (!has_media_ts_) {
        if (raw_ts_us32 != 0) {
            // Sender started stamping media_ts_us after initial unstamped frames: re-anchor cleanly.
            has_media_ts_ = true;
            last_raw_ts32_ = raw_ts_us32;
            unwrapped_media_us_ = uint64_t(raw_ts_us32);
            clock_offset_us_ = int64_t(arrival_us) - int64_t(unwrapped_media_us_);
            last_arrival_us_ = arrival_us;
            reanchored = true;
            return unwrapped_media_us_;
        }
        unwrapped_media_us_ = std::max(unwrapped_media_us_ + 1, arrival_us);
        return unwrapped_media_us_;
    }

    const int32_t delta = int32_t(raw_ts_us32 - last_raw_ts32_);
    // Large discontinuity (> 2 s backwards or forwards) -> sender restarted, re-anchor clock
    if (delta < -2'000'000 || delta > 2'000'000) {
        last_raw_ts32_ = raw_ts_us32;
        unwrapped_media_us_ += 16'667;
        clock_offset_us_ = int64_t(arrival_us) - int64_t(unwrapped_media_us_);
        jitter_us_ = 0.0;
        last_arrival_us_ = arrival_us;
        reanchored = true;
        return unwrapped_media_us_;
    }

    if (delta >= 0) {
        last_raw_ts32_ = raw_ts_us32;
        unwrapped_media_us_ += uint64_t(delta);
        return unwrapped_media_us_;
    }
    // Slight out-of-order timestamp: do NOT move `last_raw_ts32_` backwards so the next in-order frame
    // does not double-count `|delta|` and permanently drift the playout clock forward.
    const uint64_t back = uint64_t(-int64_t(delta));
    return unwrapped_media_us_ >= back ? (unwrapped_media_us_ - back) : 0;
}

void VideoPlayoutBuffer::request_idr_if_due_locked(uint64_t now_us, std::function<void()>& out_cb) {
    const uint64_t limit_us = uint64_t(cfg_.idr_rate_limit_ms) * 1000ull;
    if (last_idr_req_us_ == 0 || now_us - last_idr_req_us_ >= limit_us) {
        last_idr_req_us_ = now_us;
        ++idr_requests_;
        out_cb = on_idr_req_;
    }
}

void VideoPlayoutBuffer::push_frame(Image img, uint32_t frame_id, uint32_t media_ts_us32, uint64_t arrival_us,
                                    bool is_keyframe, bool corrupt_or_incomplete) {
    if (img.width <= 0 || img.height <= 0 || img.data.empty()) return;

    std::function<void()> idr_cb;
    {
        std::lock_guard<std::mutex> lk(mu_);
        ++frames_pushed_;

        if (corrupt_or_incomplete) {
            ++frames_dropped_corrupt_;
            awaiting_idr_ = true;
            request_idr_if_due_locked(arrival_us, idr_cb);
        } else if (awaiting_idr_) {
            if (is_keyframe) {
                awaiting_idr_ = false;
            } else {
                ++frames_dropped_corrupt_;
                request_idr_if_due_locked(arrival_us, idr_cb);
                goto done;
            }
        }

        if (!corrupt_or_incomplete) {
            bool reanchored = false;
            const uint64_t media_ts = unwrap_media_ts_locked(media_ts_us32, arrival_us, reanchored);
            if (!reanchored) {
                const int64_t sample_offset = int64_t(arrival_us) - int64_t(media_ts);
                const int64_t excess = sample_offset - clock_offset_us_;
                if (excess < 0) {
                    // Faster network path observed: tighten baseline immediately
                    clock_offset_us_ = sample_offset;
                } else {
                    // Very gentle upward drift tracking (<= 500 ppm) so clock skew never accumulates,
                    // while Wi-Fi arrival spikes do not pull the playout baseline forward.
                    const uint64_t dt_us = arrival_us > last_arrival_us_ ? (arrival_us - last_arrival_us_) : 0;
                    const int64_t max_drift = std::max<int64_t>(1, int64_t(dt_us / 2000));
                    clock_offset_us_ += std::min<int64_t>(excess, max_drift);

                    // Inter-arrival jitter EMA
                    jitter_us_ = jitter_us_ * 0.875 + double(excess) * 0.125;
                }
            }
            last_arrival_us_ = arrival_us;

            if (cfg_.fixed_delay_mode) {
                effective_delay_ms_ = double(cfg_.target_delay_ms);
            } else {
                const double jitter_ms = jitter_us_ / 1000.0;
                const double desired = std::clamp(std::max(double(cfg_.target_delay_ms), jitter_ms * 3.0 + 8.0),
                                                  double(cfg_.target_delay_ms), double(cfg_.adaptive_max_ms));
                if (desired > effective_delay_ms_) {
                    effective_delay_ms_ = desired;
                    last_spike_us_ = arrival_us;
                } else if (arrival_us > last_spike_us_ + 2'000'000ull && arrival_us > last_decay_us_) {
                    const double dt_s = double(arrival_us - last_decay_us_) / 1e6;
                    effective_delay_ms_ = std::max(double(cfg_.target_delay_ms), effective_delay_ms_ - dt_s * 4.0);
                }
                last_decay_us_ = arrival_us;
            }

            const int64_t scheduled_base = int64_t(media_ts) + clock_offset_us_;
            const int64_t delay_us = int64_t(std::llround(effective_delay_ms_ * 1000.0));
            const uint64_t due_us = uint64_t(std::max<int64_t>(0, scheduled_base + delay_us));

            QueuedFrame qf;
            qf.image = std::move(img);
            qf.frame_id = frame_id;
            qf.media_ts_us = media_ts;
            qf.due_us = due_us;
            qf.is_keyframe = is_keyframe;

            // Insert keeping monotonic media_ts order
            auto it = queue_.end();
            while (it != queue_.begin()) {
                auto prev = std::prev(it);
                if (prev->media_ts_us <= qf.media_ts_us) break;
                it = prev;
            }
            queue_.insert(it, std::move(qf));

            while (queue_.size() > cfg_.max_queue_frames) {
                queue_.pop_front();
                ++frames_dropped_late_;
            }
        }
    }

done:
    if (idr_cb) idr_cb();
}

bool VideoPlayoutBuffer::advance(uint64_t now_us) {
    std::lock_guard<std::mutex> lk(mu_);
    if (queue_.empty()) {
        if (has_held_ && last_advance_us_ != 0 && now_us > last_advance_us_ + 25'000ull) {
            ++frames_held_;
            last_advance_us_ = now_us;
        }
        return false;
    }

    int released = 0;
    QueuedFrame latest_due;
    while (!queue_.empty() && queue_.front().due_us <= now_us) {
        if (released > 0) ++frames_dropped_late_;
        latest_due = std::move(queue_.front());
        queue_.pop_front();
        ++released;
    }

    if (released > 0) {
        held_frame_ = std::move(latest_due.image);
        has_held_ = true;
        ++held_seq_;
        ++frames_released_;
        last_advance_us_ = now_us;
        return true;
    }

    if (has_held_ && last_advance_us_ != 0 && now_us > last_advance_us_ + 25'000ull) {
        ++frames_held_;
        last_advance_us_ = now_us;
    }
    return false;
}

bool VideoPlayoutBuffer::copy_held_frame(Image& out, uint64_t& inout_seq) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (!has_held_ || held_seq_ == inout_seq || held_frame_.width <= 0) return false;
    out = held_frame_;
    inout_seq = held_seq_;
    return true;
}

bool VideoPlayoutBuffer::poll_frame(uint64_t now_us, Image& out, uint64_t& inout_seq) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!queue_.empty()) {
        int released = 0;
        QueuedFrame latest_due;
        while (!queue_.empty() && queue_.front().due_us <= now_us) {
            if (released > 0) ++frames_dropped_late_;
            latest_due = std::move(queue_.front());
            queue_.pop_front();
            ++released;
        }
        if (released > 0) {
            held_frame_ = std::move(latest_due.image);
            has_held_ = true;
            ++held_seq_;
            ++frames_released_;
            last_advance_us_ = now_us;
        } else if (has_held_ && last_advance_us_ != 0 && now_us > last_advance_us_ + 25'000ull) {
            ++frames_held_;
            last_advance_us_ = now_us;
        }
    } else if (has_held_ && last_advance_us_ != 0 && now_us > last_advance_us_ + 25'000ull) {
        ++frames_held_;
        last_advance_us_ = now_us;
    }

    if (!has_held_ || held_seq_ == inout_seq || held_frame_.width <= 0) return false;
    out = held_frame_;
    inout_seq = held_seq_;
    return true;
}

VideoPlayoutStats VideoPlayoutBuffer::stats() const {
    std::lock_guard<std::mutex> lk(mu_);
    VideoPlayoutStats s{};
    s.target_delay_ms = cfg_.target_delay_ms;
    s.effective_delay_ms = effective_delay_ms_;
    s.fixed_mode = cfg_.fixed_delay_mode;
    s.jitter_ms = jitter_us_ / 1000.0;
    s.queued_frames = uint32_t(queue_.size());
    s.frames_pushed = frames_pushed_;
    s.frames_released = frames_released_;
    s.frames_held = frames_held_;
    s.frames_dropped_late = frames_dropped_late_;
    s.frames_dropped_corrupt = frames_dropped_corrupt_;
    s.idr_requests = idr_requests_;
    return s;
}

}  // namespace lv
