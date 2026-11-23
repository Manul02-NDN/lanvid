#pragma once
// MidiDejitterQueue — Sample-Accurate 48 kHz MIDI De-Jitter Playout Queue.
//
// Absorbs Wi-Fi UDP arrival jitter (0..12+ ms) so outgoing MIDI events are emitted with a static,
// deterministic playout delay locked to the audio buffer configuration (`target_buffer_ms` and
// `Fixed / Adaptive` latency mode):
//
//   target_playout_ts = mapped_local_rx_ts + target_buffer_samples
//   where target_buffer_samples = llround(target_buffer_ms * (sample_rate / 1000.0))
//
// - When MasterClock / AudioSyncMatrix is active, MIDI is locked directly to the same 48 kHz
//   timeline as audio (0.0 ms A/V-MIDI skew).
// - When running MIDI-only (or before MasterClock converges), a minimum-transit-delay baseline
//   maps sender media_ts into local 48 kHz samples so Fixed delay holds steady across Wi-Fi jitter.
// - Late events (arrival > target_playout_ts during extreme Wi-Fi spikes) are NEVER dropped:
//   they are counted in `late_events` and emitted immediately on the current tick (zero stuck notes).

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>
#include <vector>

#include "midi/midi_sync_engine.hpp"

namespace lv {

struct MidiDejitterStats {
    double target_buffer_ms = 50.0;
    double playout_delay_ms = 50.0;  // active delay (== target_buffer_ms in Fixed mode, ~1.5 ms in MIDI Only)
    double absorbed_jitter_ms = 0.0; // smoothed Wi-Fi arrival jitter absorbed by the queue
    bool fixed_mode = true;
    bool synced_with_audio = false;
    bool midi_only_mode = false;
    uint32_t queued_events = 0;
    uint64_t total_enqueued = 0;
    uint64_t total_dispatched = 0;
    uint64_t late_events = 0;
};

class MidiDejitterQueue {
public:
    using DispatchFn = std::function<void(const TimedMidiEvent& ev, bool was_late)>;

    explicit MidiDejitterQueue(uint32_t sample_rate = 48000);

    // Live configuration update (safe to call from UI / kControlSync / audio thread).
    void set_latency(double target_buffer_ms, bool adaptive);

    // Updates the active effective buffer (used in Adaptive mode to follow AudioSyncMatrix)
    // and whether the queue is currently locked to an active AudioSyncMatrix stream.
    void set_effective_buffer_ms(double effective_ms, bool synced_with_audio);

    // Switches into/out of "MIDI Only" ultra-low-latency mode (0..2 ms micro-buffer, default 1.5 ms),
    // detaching from the 15-20+ ms audio buffer while still absorbing micro Wi-Fi jitter.
    void set_midi_only_mode(bool enabled, double ultra_low_ms = 1.5);
    bool is_midi_only_mode() const;

    int64_t target_buffer_samples() const;
    double target_buffer_ms() const;
    double active_playout_delay_ms() const;
    bool is_fixed_mode() const;

    // Enqueues a deduplicated MIDI event with its sender media_ts_us and local arrival time.
    // If `master_synced` is true, `master_offset_us` = (master_us - local_us) maps sender time
    // directly into local 48 kHz samples. Otherwise uses minimum-transit baseline tracking.
    void push(const TimedMidiEvent& ev, uint64_t arrival_local_us,
              bool master_synced = false, int64_t master_offset_us = 0);

    // Releases all queued events whose `target_playout_samples <= local_now_samples` in order.
    uint32_t pop_due_local(uint64_t local_now_us, const DispatchFn& on_event);

    // Releases all queued events whose `ev.media_ts_us <= playout_media_ts_us` in order
    // (used when AudioSyncMatrix is actively rendering audio for 0.0 ms A/V-MIDI lock).
    uint32_t pop_due_media(uint64_t playout_media_ts_us, const DispatchFn& on_event);

    void reset();
    MidiDejitterStats stats() const;

private:
    struct ScheduledEvent {
        int64_t target_playout_samples = 0;
        TimedMidiEvent ev{};
        bool was_late = false;

        bool operator>(const ScheduledEvent& o) const {
            if (target_playout_samples != o.target_playout_samples) {
                return target_playout_samples > o.target_playout_samples;
            }
            if (ev.media_ts_us != o.ev.media_ts_us) {
                return ev.media_ts_us > o.ev.media_ts_us;
            }
            return ev.event_seq > o.ev.event_seq;
        }
    };

    int64_t us_to_samples(int64_t us) const;
    double active_delay_ms_locked() const;

    uint32_t sample_rate_ = 48000;
    mutable std::mutex mu_;

    double target_buffer_ms_ = 50.0;
    double effective_buffer_ms_ = 50.0;
    bool adaptive_ = false;
    bool synced_with_audio_ = false;
    bool midi_only_mode_ = false;
    double midi_only_delay_ms_ = 1.5;

    // Minimum transit offset baseline (arrival_local_samples - media_ts_samples) for MIDI-only mode.
    bool has_baseline_ = false;
    int64_t min_offset_samples_ = 0;
    double absorbed_jitter_ms_ = 0.0;

    std::priority_queue<ScheduledEvent, std::vector<ScheduledEvent>, std::greater<ScheduledEvent>> queue_;

    std::atomic<uint64_t> total_enqueued_{0};
    std::atomic<uint64_t> total_dispatched_{0};
    std::atomic<uint64_t> late_events_{0};
};

}  // namespace lv
