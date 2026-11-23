#include "midi/midi_sync_engine.hpp"

#include <algorithm>
#include <climits>

namespace lv {

MidiTxEngine::MidiTxEngine(SendFn send, uint32_t stream_id)
    : send_(std::move(send)), stream_id_(stream_id) {}

uint32_t MidiTxEngine::push_event(uint8_t status, uint8_t data1, uint8_t data2, uint8_t port_index,
                                  uint64_t media_ts_us) {
    std::lock_guard<std::mutex> lk(mu_);
    TimedMidiEvent ev{};
    ev.event_seq = next_event_seq_++;
    ev.media_ts_us = media_ts_us;
    ev.status = status;
    ev.data1 = data1;
    ev.data2 = data2;
    ev.port_index = port_index < kMaxMidiPorts ? port_index : 0;
    pending_.push_back(ev);
    events_pushed_.fetch_add(1, std::memory_order_relaxed);
    return ev.event_seq;
}

bool MidiTxEngine::flush(uint64_t base_media_ts_us, uint32_t wallclock_us, bool heartbeat) {
    MidiEventPacket out_evs[kMaxMidiEventsPerPacket];
    uint16_t count = 0;
    uint16_t flags = heartbeat ? kMidiFlagHeartbeat : 0;
    uint32_t batch_seq = 0;

    {
        std::lock_guard<std::mutex> lk(mu_);
        const bool have_new = !pending_.empty();

        // 1. Add recent history events that still have redundancy repeats left (or on heartbeat)
        bool added_redundant = false;
        for (size_t i = 0; i < kRedundancyRingSize && count < kMaxMidiEventsPerPacket; ++i) {
            auto& he = history_[i];
            if (!he.valid) continue;
            if (!heartbeat && he.repeats_left == 0) continue;
            const int64_t delta = int64_t(he.ev.media_ts_us) - int64_t(base_media_ts_us);
            if (delta < -2000000 || delta > 2000000) continue;  // skip stale (>2s old)
            MidiEventPacket p{};
            p.event_seq = he.ev.event_seq;
            p.offset_us = int32_t(std::clamp<int64_t>(delta, INT32_MIN, INT32_MAX));
            p.status = he.ev.status;
            p.data1 = he.ev.data1;
            p.data2 = he.ev.data2;
            p.port_index = he.ev.port_index;
            out_evs[count++] = p;
            if (he.repeats_left > 0) --he.repeats_left;
            added_redundant = true;
            redundant_sent_.fetch_add(1, std::memory_order_relaxed);
        }
        if (added_redundant) flags |= kMidiFlagRedundant;

        // 2. Append newly queued events and record them into the 16-event redundancy ring
        for (const auto& ev : pending_) {
            if (count >= kMaxMidiEventsPerPacket) break;
            const int64_t delta = int64_t(ev.media_ts_us) - int64_t(base_media_ts_us);
            MidiEventPacket p{};
            p.event_seq = ev.event_seq;
            p.offset_us = int32_t(std::clamp<int64_t>(delta, INT32_MIN, INT32_MAX));
            p.status = ev.status;
            p.data1 = ev.data1;
            p.data2 = ev.data2;
            p.port_index = ev.port_index;
            out_evs[count++] = p;

            history_[hist_head_] = {ev, kRedundantRepeats, true};
            hist_head_ = (hist_head_ + 1) % kRedundancyRingSize;
        }
        pending_.clear();

        if (count == 0 && !have_new && !heartbeat) return false;
        batch_seq = next_batch_seq_++;
    }

    MidiFrameHeader mh{};
    mh.stream_id = stream_id_;
    mh.batch_seq = batch_seq;
    mh.base_media_ts_us = base_media_ts_us;
    mh.event_count = count;
    mh.flags = flags;

    uint8_t payload[kMidiFrameHeaderSize + kMaxMidiEventsPerPacket * kMidiEventPacketSize];
    const size_t pl_len = write_midi_frame(payload, sizeof(payload), mh, out_evs, count);
    if (pl_len == 0) return false;

    PacketHeader ph{};
    ph.type = PacketType::kMidiFrame;
    ph.flags = kFlagHasCrc;
    ph.frame_id = batch_seq;
    ph.timestamp_us = wallclock_us;
    ph.seq = uint16_t(batch_seq & 0xFFFFu);

    uint8_t dgram[kPacketHeaderSize + sizeof(payload)];
    const size_t dlen = write_packet(dgram, sizeof(dgram), ph, payload, pl_len);
    if (dlen > 0 && send_ && send_(dgram, dlen)) {
        packets_sent_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    return false;
}

MidiTxStats MidiTxEngine::stats() const {
    return {packets_sent_.load(std::memory_order_relaxed),
            events_pushed_.load(std::memory_order_relaxed),
            redundant_sent_.load(std::memory_order_relaxed)};
}

// ---- MidiRxEngine ----

MidiRxEngine::MidiRxEngine() { reset(); }

void MidiRxEngine::reset() {
    std::lock_guard<std::mutex> lk(mu_);
    has_seq_ = false;
    highest_seq_ = 0;
    for (uint32_t i = 0; i < kDedupWindow; ++i) {
        seen_seq_[i] = 0;
        seen_valid_[i] = false;
    }
    while (!queue_.empty()) queue_.pop();
}

bool MidiRxEngine::is_duplicate_and_mark(uint32_t seq) {
    if (has_seq_) {
        const int32_t diff = int32_t(seq - highest_seq_);
        if (diff <= -int32_t(kDedupWindow)) return true;  // too old
        if (diff > 0) highest_seq_ = seq;
    } else {
        has_seq_ = true;
        highest_seq_ = seq;
    }
    const uint32_t slot = seq & (kDedupWindow - 1);
    if (seen_valid_[slot] && seen_seq_[slot] == seq) {
        return true;
    }
    seen_valid_[slot] = true;
    seen_seq_[slot] = seq;
    return false;
}

bool MidiRxEngine::on_packet(const PacketHeader& h, const uint8_t* payload, size_t len) {
    return on_packet_with_cb(h, payload, len, nullptr);
}

bool MidiRxEngine::on_packet_with_cb(const PacketHeader& h, const uint8_t* payload, size_t len,
                                     const DispatchFn& on_unique) {
    if (h.type != PacketType::kMidiFrame || !payload) return false;
    MidiFrameHeader mh{};
    MidiEventPacket evs[kMaxMidiEventsPerPacket];
    if (!parse_midi_frame(payload, len, mh, evs, kMaxMidiEventsPerPacket)) return false;

    packets_rx_.fetch_add(1, std::memory_order_relaxed);
    std::vector<TimedMidiEvent> accepted;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (uint16_t i = 0; i < mh.event_count; ++i) {
            const auto& e = evs[i];
            if (is_duplicate_and_mark(e.event_seq)) {
                events_deduped_.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            TimedMidiEvent te{};
            te.event_seq = e.event_seq;
            const int64_t ts = int64_t(mh.base_media_ts_us) + int64_t(e.offset_us);
            te.media_ts_us = ts > 0 ? uint64_t(ts) : 0;
            te.status = e.status;
            te.data1 = e.data1;
            te.data2 = e.data2;
            te.port_index = e.port_index;
            if (on_unique) {
                accepted.push_back(te);
            } else {
                queue_.push(te);
            }
            events_unique_.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (on_unique) {
        for (const auto& te : accepted) {
            on_unique(te);
        }
    }
    return true;
}

uint32_t MidiRxEngine::pop_due_events(uint64_t playout_media_ts_us, const DispatchFn& on_event) {
    std::vector<TimedMidiEvent> due;
    {
        std::lock_guard<std::mutex> lk(mu_);
        while (!queue_.empty() && queue_.top().media_ts_us <= playout_media_ts_us) {
            due.push_back(queue_.top());
            queue_.pop();
        }
    }
    for (const auto& ev : due) {
        if (on_event) on_event(ev);
        events_dispatched_.fetch_add(1, std::memory_order_relaxed);
    }
    return uint32_t(due.size());
}

MidiRxStats MidiRxEngine::stats() const {
    return {packets_rx_.load(std::memory_order_relaxed),
            events_unique_.load(std::memory_order_relaxed),
            events_deduped_.load(std::memory_order_relaxed),
            events_dispatched_.load(std::memory_order_relaxed)};
}

}  // namespace lv
