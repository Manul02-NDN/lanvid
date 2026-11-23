#pragma once
// Sample-Accurate MIDI over IP (MIDI2) Sync Engine.
//
// - MidiTxEngine: assigns monotonic `event_seq`, keeps a 16-event redundancy ring so every event
//   (especially Note Off) is repeated in the next 2 packets or heartbeat packets -> zero stuck notes
//   even under 5%+ Wi-Fi packet loss.
// - MidiRxEngine: deduplicates by `event_seq` and schedules each event against the AudioSyncMatrix
//   playout cursor (`media_ts_us`), dispatching to local MIDI outputs in lockstep with audio (< 1 ms).

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>
#include <vector>

#include "net/packet.hpp"

namespace lv {

struct TimedMidiEvent {
    uint32_t event_seq = 0;
    uint64_t media_ts_us = 0;
    uint8_t status = 0;
    uint8_t data1 = 0;
    uint8_t data2 = 0;
    uint8_t port_index = 0;  // 0..15

    bool operator>(const TimedMidiEvent& o) const {
        if (media_ts_us != o.media_ts_us) return media_ts_us > o.media_ts_us;
        return event_seq > o.event_seq;
    }
};

struct MidiTxStats {
    uint64_t packets_sent = 0;
    uint64_t events_pushed = 0;
    uint64_t redundant_events_sent = 0;
};

struct MidiRxStats {
    uint64_t packets_rx = 0;
    uint64_t events_unique = 0;
    uint64_t events_deduped = 0;
    uint64_t events_dispatched = 0;
};

class MidiTxEngine {
public:
    static constexpr size_t kRedundancyRingSize = 16;
    static constexpr uint8_t kRedundantRepeats = 2;  // repeat each event in the next 2 packets

    using SendFn = std::function<bool(const uint8_t* dgram, size_t len)>;

    explicit MidiTxEngine(SendFn send, uint32_t stream_id = 0);

    // Enqueues one MIDI short message tagged with MasterClock `media_ts_us`.
    uint32_t push_event(uint8_t status, uint8_t data1, uint8_t data2, uint8_t port_index, uint64_t media_ts_us);

    // Packs pending + recent redundancy events into a kMidiFrame (MIDI2) datagram and transmits it.
    // Returns true if a datagram was sent.
    bool flush(uint64_t base_media_ts_us, uint32_t wallclock_us, bool heartbeat = false);

    MidiTxStats stats() const;

private:
    struct HistEntry {
        TimedMidiEvent ev{};
        uint8_t repeats_left = 0;
        bool valid = false;
    };

    SendFn send_;
    uint32_t stream_id_ = 0;
    mutable std::mutex mu_;
    uint32_t next_event_seq_ = 1;
    uint32_t next_batch_seq_ = 1;
    std::vector<TimedMidiEvent> pending_;
    HistEntry history_[kRedundancyRingSize]{};
    size_t hist_head_ = 0;
    std::atomic<uint64_t> packets_sent_{0};
    std::atomic<uint64_t> events_pushed_{0};
    std::atomic<uint64_t> redundant_sent_{0};
};

class MidiRxEngine {
public:
    static constexpr uint32_t kDedupWindow = 1024;  // power of two

    using DispatchFn = std::function<void(const TimedMidiEvent& ev)>;

    MidiRxEngine();

    // Processes an incoming kMidiFrame payload (after the 28-byte LVSP PacketHeader).
    bool on_packet(const PacketHeader& h, const uint8_t* payload, size_t len);

    // Releases all queued events whose `media_ts_us <= playout_media_ts_us` in chronological order.
    uint32_t pop_due_events(uint64_t playout_media_ts_us, const DispatchFn& on_event);

    void reset();
    MidiRxStats stats() const;

private:
    bool is_duplicate_and_mark(uint32_t seq);

    mutable std::mutex mu_;
    bool has_seq_ = false;
    uint32_t highest_seq_ = 0;
    uint32_t seen_seq_[kDedupWindow]{};
    bool seen_valid_[kDedupWindow]{};
    std::priority_queue<TimedMidiEvent, std::vector<TimedMidiEvent>, std::greater<TimedMidiEvent>> queue_;

    std::atomic<uint64_t> packets_rx_{0};
    std::atomic<uint64_t> events_unique_{0};
    std::atomic<uint64_t> events_deduped_{0};
    std::atomic<uint64_t> events_dispatched_{0};
};

}  // namespace lv
