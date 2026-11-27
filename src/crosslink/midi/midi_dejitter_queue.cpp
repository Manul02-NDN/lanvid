#include "midi/midi_dejitter_queue.hpp"

#include <algorithm>
#include <cmath>

namespace cl {

MidiDejitterQueue::MidiDejitterQueue(uint32_t sample_rate)
    : sample_rate_(sample_rate ? sample_rate : 48000) {}

int64_t MidiDejitterQueue::us_to_samples(int64_t us) const {
    return static_cast<int64_t>(std::llround(double(us) * double(sample_rate_) / 1'000'000.0));
}

double MidiDejitterQueue::active_delay_ms_locked() const {
    if (midi_only_mode_) return midi_only_delay_ms_;
    if (!adaptive_) return target_buffer_ms_;
    return std::max(target_buffer_ms_, effective_buffer_ms_);
}

void MidiDejitterQueue::set_midi_only_mode(bool enabled, double ultra_low_ms) {
    std::lock_guard<std::mutex> lk(mu_);
    midi_only_mode_ = enabled;
    midi_only_delay_ms_ = std::clamp(ultra_low_ms, 0.0, 2.0);
}

bool MidiDejitterQueue::is_midi_only_mode() const {
    std::lock_guard<std::mutex> lk(mu_);
    return midi_only_mode_;
}

void MidiDejitterQueue::set_latency(double target_buffer_ms, bool adaptive) {
    std::lock_guard<std::mutex> lk(mu_);
    target_buffer_ms_ = std::clamp(target_buffer_ms, 2.0, 2000.0);
    adaptive_ = adaptive;
    if (!adaptive_) {
        effective_buffer_ms_ = target_buffer_ms_;
    } else if (effective_buffer_ms_ < target_buffer_ms_) {
        effective_buffer_ms_ = target_buffer_ms_;
    }
}

void MidiDejitterQueue::set_effective_buffer_ms(double effective_ms, bool synced_with_audio) {
    std::lock_guard<std::mutex> lk(mu_);
    synced_with_audio_ = synced_with_audio;
    if (effective_ms >= 2.0 && effective_ms <= 2000.0) {
        effective_buffer_ms_ = effective_ms;
    }
}

int64_t MidiDejitterQueue::target_buffer_samples() const {
    std::lock_guard<std::mutex> lk(mu_);
    return static_cast<int64_t>(std::llround(active_delay_ms_locked() * double(sample_rate_) / 1000.0));
}

double MidiDejitterQueue::target_buffer_ms() const {
    std::lock_guard<std::mutex> lk(mu_);
    return target_buffer_ms_;
}

double MidiDejitterQueue::active_playout_delay_ms() const {
    std::lock_guard<std::mutex> lk(mu_);
    return active_delay_ms_locked();
}

bool MidiDejitterQueue::is_fixed_mode() const {
    std::lock_guard<std::mutex> lk(mu_);
    return !adaptive_;
}

void MidiDejitterQueue::push(const TimedMidiEvent& ev, uint64_t arrival_local_us,
                             bool master_synced, int64_t master_offset_us) {
    std::lock_guard<std::mutex> lk(mu_);
    const int64_t media_samples = us_to_samples(int64_t(ev.media_ts_us));
    const int64_t arrival_samples = us_to_samples(int64_t(arrival_local_us));

    int64_t mapped_local_rx_samples = arrival_samples;
    if (master_synced) {
        // Exact mapping via MasterClock (master_us = local_us + master_offset_us)
        mapped_local_rx_samples = us_to_samples(int64_t(ev.media_ts_us) - master_offset_us);
    } else {
        // Standalone / MIDI-only mode: track the minimum-transit-delay baseline floor
        // (arrival_samples - media_samples) so zero-jitter packets define the reference line.
        const int64_t obs_offset = arrival_samples - media_samples;
        if (!has_baseline_) {
            min_offset_samples_ = obs_offset;
            has_baseline_ = true;
        } else if (obs_offset < min_offset_samples_) {
            // Faster packet arrived -> tighten baseline immediately (true minimum transit floor)
            min_offset_samples_ = obs_offset;
            drift_relax_ticks_ = 0;
        } else {
            // Very slow upward relaxation (+1 sample per 128 events) handles gentle quartz drift
            // without chasing Wi-Fi jitter spikes.
            const int64_t excess = obs_offset - min_offset_samples_;
            if (excess > int64_t(sample_rate_) / 2) {
                // Step-change (> 500 ms clock jump / sender restart) -> re-anchor immediately
                min_offset_samples_ = obs_offset;
                drift_relax_ticks_ = 0;
            } else if (excess > 0 && ++drift_relax_ticks_ >= 128u) {
                ++min_offset_samples_;
                drift_relax_ticks_ = 0;
            }
        }
        mapped_local_rx_samples = media_samples + min_offset_samples_;
    }

    const int64_t buf_samples =
        static_cast<int64_t>(std::llround(active_delay_ms_locked() * double(sample_rate_) / 1000.0));
    const int64_t target_playout = mapped_local_rx_samples + buf_samples;

    // Measure arrival jitter above the baseline floor (in ms) for live UI telemetry.
    const int64_t jitter_samples = std::max<int64_t>(0, arrival_samples - mapped_local_rx_samples);
    const double jitter_ms = double(jitter_samples) * 1000.0 / double(sample_rate_);
    absorbed_jitter_ms_ = (jitter_ms > absorbed_jitter_ms_)
                              ? (absorbed_jitter_ms_ * 0.5 + jitter_ms * 0.5)
                              : (absorbed_jitter_ms_ * 0.92 + jitter_ms * 0.08);

    const bool late = (arrival_samples > target_playout);
    if (late) {
        late_events_.fetch_add(1, std::memory_order_relaxed);
    }

    ScheduledEvent se{};
    // Late events get scheduled at `arrival_samples` so they release immediately on the current tick
    // (never dropped -> zero stuck Note Off events).
    se.target_playout_samples = late ? arrival_samples : target_playout;
    se.ev = ev;
    se.was_late = late;
    if (queue_.size() >= kMaxQueuedEvents) {
        auto oldest = std::min_element(queue_.begin(), queue_.end(), [](const ScheduledEvent& a, const ScheduledEvent& b) {
            return b > a;
        });
        if (oldest != queue_.end()) queue_.erase(oldest);
    }
    queue_.push_back(se);
    total_enqueued_.fetch_add(1, std::memory_order_relaxed);
}

uint32_t MidiDejitterQueue::pop_due_local(uint64_t local_now_us, const DispatchFn& on_event) {
    const int64_t now_samples = us_to_samples(int64_t(local_now_us));
    std::vector<ScheduledEvent> due;
    {
        std::lock_guard<std::mutex> lk(mu_);
        size_t keep = 0;
        for (size_t i = 0; i < queue_.size(); ++i) {
            if (queue_[i].target_playout_samples <= now_samples) {
                due.push_back(queue_[i]);
            } else {
                if (keep != i) queue_[keep] = queue_[i];
                ++keep;
            }
        }
        queue_.resize(keep);
    }
    std::sort(due.begin(), due.end(), [](const ScheduledEvent& a, const ScheduledEvent& b) { return b > a; });
    for (const auto& se : due) {
        if (on_event) on_event(se.ev, se.was_late);
        total_dispatched_.fetch_add(1, std::memory_order_relaxed);
    }
    return uint32_t(due.size());
}

uint32_t MidiDejitterQueue::pop_due_media(uint64_t playout_media_ts_us, const DispatchFn& on_event) {
    std::vector<ScheduledEvent> due;
    {
        std::lock_guard<std::mutex> lk(mu_);
        // Release every event whose sender `media_ts_us <= playout_media_ts_us` (or late-flagged),
        // immune to head-of-line blocking when buffer latency decreases mid-stream.
        size_t keep = 0;
        for (size_t i = 0; i < queue_.size(); ++i) {
            if (queue_[i].ev.media_ts_us <= playout_media_ts_us || queue_[i].was_late) {
                due.push_back(queue_[i]);
            } else {
                if (keep != i) queue_[keep] = queue_[i];
                ++keep;
            }
        }
        queue_.resize(keep);
    }
    std::sort(due.begin(), due.end(), [](const ScheduledEvent& a, const ScheduledEvent& b) {
        if (a.ev.media_ts_us != b.ev.media_ts_us) return a.ev.media_ts_us < b.ev.media_ts_us;
        return a.ev.event_seq < b.ev.event_seq;
    });
    for (const auto& se : due) {
        if (on_event) on_event(se.ev, se.was_late);
        total_dispatched_.fetch_add(1, std::memory_order_relaxed);
    }
    return uint32_t(due.size());
}

void MidiDejitterQueue::reset() {
    std::lock_guard<std::mutex> lk(mu_);
    has_baseline_ = false;
    min_offset_samples_ = 0;
    drift_relax_ticks_ = 0;
    absorbed_jitter_ms_ = 0.0;
    queue_.clear();
}

MidiDejitterStats MidiDejitterQueue::stats() const {
    std::lock_guard<std::mutex> lk(mu_);
    MidiDejitterStats s{};
    s.target_buffer_ms = target_buffer_ms_;
    s.playout_delay_ms = active_delay_ms_locked();
    s.absorbed_jitter_ms = absorbed_jitter_ms_;
    s.fixed_mode = midi_only_mode_ ? true : !adaptive_;
    s.synced_with_audio = midi_only_mode_ ? false : synced_with_audio_;
    s.midi_only_mode = midi_only_mode_;
    s.queued_events = uint32_t(queue_.size());
    s.total_enqueued = total_enqueued_.load(std::memory_order_relaxed);
    s.total_dispatched = total_dispatched_.load(std::memory_order_relaxed);
    s.late_events = late_events_.load(std::memory_order_relaxed);
    return s;
}

}  // namespace cl
