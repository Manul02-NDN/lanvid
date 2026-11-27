#include "net/audio_receiver.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

#include "common/clock.hpp"
#include "common/rt_thread.hpp"

namespace cl {

AudioReceiver::AudioReceiver(const AudioReceiverConfig& cfg)
    : cfg_(cfg),
      matrix_(cfg.sample_rate),
      arx_(matrix_, mclock_, [this](const uint8_t* d, size_t n) {
          Endpoint ep{};
          if (!sender_endpoint(ep)) return false;
          return sock_.send_to_qos(d, n, ep, QosTrafficClass::kSyncControl) == int(n);
      }),
      midi_dejitter_(cfg.sample_rate) {
    arx_.set_latency_ms(cfg.latency_ms);
    arx_.set_adaptive_jitter_enabled(cfg.adaptive_jitter);
    midi_dejitter_.set_latency(double(arx_.latency_ms()), arx_.adaptive_jitter_enabled());
}

bool AudioReceiver::init() {
    if (!sock_.open()) return false;
    if (!sock_.bind(cfg_.bind_ip, cfg_.bind_port)) return false;
    sock_.set_buffer_sizes(UdpSocket::kMinSocketBuffer,
                           std::max(cfg_.socket_buf_bytes, UdpSocket::kMinSocketBuffer));
    sock_.set_low_latency_qos();
    sock_.set_recv_timeout_ms(cfg_.recv_timeout_ms);
    return true;
}

void AudioReceiver::maybe_send_ping(uint64_t now) {
    Endpoint ep{};
    if (!cfg_.enable_clock_sync || !sender_endpoint(ep)) return;
    const uint64_t interval = mclock_.accepted_samples() < 20 ? std::min<uint64_t>(cfg_.ping_interval_us, 100000)
                                                              : cfg_.ping_interval_us;
    if (last_ping_us_ != 0 && (now - last_ping_us_) < interval) return;
    last_ping_us_ = now;

    PingPongPayload pp;
    pp.t1_client_send_us = uint32_t(now);
    uint8_t pp_bytes[kPingPongSize];
    write_ping_pong(pp_bytes, pp);

    PacketHeader h;
    h.type = PacketType::kPing;
    h.flags = kFlagHasCrc;
    h.seq_id = ++ping_seq_;
    h.timestamp_us = pp.t1_client_send_us;
    uint8_t dgram[64];
    const size_t n = write_packet(dgram, sizeof(dgram), h, pp_bytes, sizeof(pp_bytes));
    if (n) sock_.send_to_qos(dgram, n, ep, QosTrafficClass::kSyncControl);
}

void AudioReceiver::handle_pong(const uint8_t* payload, size_t len) {
    PingPongPayload pp;
    if (!parse_ping_pong(payload, len, pp)) return;
    const uint64_t t4_64 = now_us();
    const uint32_t t4_cur = uint32_t(t4_64);
    const int64_t rtt = int64_t(diff_us32(t4_cur, pp.t1_client_send_us)) -
                        int64_t(diff_us32(pp.t3_server_send_us, pp.t2_server_recv_us));
    if (rtt < 0 || rtt > 500000) return;

    if (len >= kPingPongSize + kPongMasterTailSize) {
        uint64_t t2m = 0, t3m = 0;
        if (parse_pong_master_tail(payload + kPingPongSize, len - kPingPongSize, t2m, t3m)) {
            const uint32_t elapsed_u32 = uint32_t(t4_cur - pp.t1_client_send_us);
            if (elapsed_u32 <= 1'000'000u && t4_64 >= elapsed_u32) {
                const uint64_t t1_64 = t4_64 - uint64_t(elapsed_u32);
                mclock_.on_exchange(t1_64, t2m, t3m, t4_64);
            }
        }
    }
}

void AudioReceiver::step_once() {
    RecvDatagram batch[kRecvBatchSize];
    for (int i = 0; i < kRecvBatchSize; ++i) {
        batch[i] = {recv_batch_buf_[i], kRecvSlotCap, 0, {}};
    }
    const int got = sock_.recv_batch(batch, kRecvBatchSize);
    const uint64_t now = now_us();
    for (int i = 0; i < got; ++i) {
        if (batch[i].len) handle_datagram(batch[i].data, batch[i].len, batch[i].src, now);
    }
    service(now);
}

void AudioReceiver::handle_datagram(const uint8_t* dgram, size_t n, const Endpoint& src, uint64_t now) {
    PacketHeader h;
    const uint8_t* payload = nullptr;
    if (parse_packet(dgram, n, h, &payload) != PacketParseStatus::kOk) {
        bad_packets_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    if (h.type == PacketType::kPong) {
        handle_pong(payload, h.payload_size);
    } else if (h.type == PacketType::kAudioFrame) {
        if (!accept_epoch(h.stream_epoch)) return;
        remember_sender(src);
        audio_bytes_rx_.fetch_add(uint64_t(n), std::memory_order_relaxed);
        audio_packets_rx_.fetch_add(1, std::memory_order_relaxed);
        arx_.on_packet(h, payload, h.payload_size, now);
    } else if (h.type == PacketType::kMidiFrame) {
        if (h.payload_size < 4) return;
        const uint32_t sid = uint32_t(payload[0]) | (uint32_t(payload[1]) << 8) |
                             (uint32_t(payload[2]) << 16) | (uint32_t(payload[3]) << 24);
        if (!accept_epoch(uint16_t(sid))) return;
        remember_sender(src);
        midi_rx_.on_packet_with_cb(h, payload, h.payload_size, [&](const TimedMidiEvent& ev) {
            const bool msync = mclock_.synced();
            const int64_t moff = msync ? (mclock_.master_us(now) - int64_t(now)) : 0;
            midi_dejitter_.push(ev, now, msync, moff);
        });
    } else if (h.type == PacketType::kControlSync) {
        remember_sender(src);
        ControlSyncPayload cs{};
        if (!parse_control_sync(payload, h.payload_size, cs)) return;
        ControlSyncCallback cb;
        {
            std::lock_guard<std::mutex> clk(cs_mu_);
            if (cs.revision <= cs_last_rx_rev_) return;
            cs_last_rx_rev_ = cs.revision;
            cb = cs_cb_;
        }
        cs_rx_count_.fetch_add(1, std::memory_order_relaxed);
        if (cb) cb(cs);
    }
}

void AudioReceiver::service(uint64_t now) {
    arx_.service(now);
    const uint64_t last_pop = last_pop_us_.load(std::memory_order_relaxed);
    if (midi_only_.load(std::memory_order_relaxed) ||
        (now > last_pop && (now - last_pop) > 50'000)) {
        midi_dejitter_.set_effective_buffer_ms(double(arx_.latency_ms()),
                                               !midi_only_.load(std::memory_order_relaxed) && mclock_.synced());
        dispatch_midi_local(now);
    }
    maybe_send_ping(now);
}

void AudioReceiver::pop_audio_multichannel_interleaved(float* out, uint32_t num_channels, uint32_t frames) {
    const uint64_t t = now_us();
    matrix_.pop_interleaved(mclock_, t, out, num_channels, frames);
    last_pop_us_.store(t, std::memory_order_relaxed);
    const double end = matrix_.playout_cursor();
    const bool audio_live = (end > 0.0) && !midi_only_.load(std::memory_order_relaxed);
    midi_dejitter_.set_effective_buffer_ms(arx_.current_effective_latency_ms(), audio_live);
    if (audio_live) {
        dispatch_midi(matrix_.playout_media_ts_us(), end - double(frames), frames);
    } else {
        dispatch_midi_local(t);
    }
}

void AudioReceiver::pop_audio_multichannel_planar(float* const* out, uint32_t num_channels, uint32_t frames) {
    const uint64_t t = now_us();
    matrix_.pop_planar(mclock_, t, out, num_channels, frames);
    last_pop_us_.store(t, std::memory_order_relaxed);
    const double end = matrix_.playout_cursor();
    const bool audio_live = (end > 0.0) && !midi_only_.load(std::memory_order_relaxed);
    midi_dejitter_.set_effective_buffer_ms(arx_.current_effective_latency_ms(), audio_live);
    if (audio_live) {
        dispatch_midi(matrix_.playout_media_ts_us(), end - double(frames), frames);
    } else {
        dispatch_midi_local(t);
    }
}

void AudioReceiver::set_midi_callback(MidiCallback cb) {
    std::lock_guard<std::mutex> lk(midi_mu_);
    midi_cb_ = std::move(cb);
}

void AudioReceiver::track_note(const TimedMidiEvent& ev) {
    const uint8_t hi = ev.status & 0xF0, ch = ev.status & 0x0F, port = ev.port_index & 0x0F;
    if (hi == 0x90 && ev.data2 > 0) {
        active_notes_[port][ch][ev.data1 & 0x7F] = 1;
    } else if (hi == 0x80 || (hi == 0x90 && ev.data2 == 0)) {
        active_notes_[port][ch][ev.data1 & 0x7F] = 0;
    } else if (hi == 0xB0 && (ev.data1 == 123 || ev.data1 == 120)) {
        for (auto& n : active_notes_[port][ch]) n = 0;
    }
}

void AudioReceiver::dispatch_midi(uint64_t playout_us, double block_start_samples, uint32_t frames) {
    struct DueItem {
        TimedMidiEvent ev;
        int32_t offset;
    };
    DueItem due[64];
    size_t due_n = 0;
    MidiCallback cb;
    {
        std::lock_guard<std::mutex> lk(midi_mu_);
        cb = midi_cb_;
        const double rate = double(matrix_.sample_rate());
        midi_dejitter_.pop_due_media(playout_us, [&](const TimedMidiEvent& ev, bool) {
            track_note(ev);
            if (!cb) return;
            int32_t off = -1;
            if (frames > 0) {
                const double pos = double(ev.media_ts_us) * rate / 1e6 - block_start_samples;
                off = int32_t(std::clamp(std::floor(pos), 0.0, double(frames - 1)));
            }
            if (due_n < 64) due[due_n++] = {ev, off};
        });
    }
    if (cb) {
        for (size_t i = 0; i < due_n; ++i) cb(due[i].ev, due[i].offset);
    }
}

void AudioReceiver::dispatch_midi_local(uint64_t local_now_us) {
    TimedMidiEvent due[64];
    size_t due_n = 0;
    MidiCallback cb;
    {
        std::lock_guard<std::mutex> lk(midi_mu_);
        cb = midi_cb_;
        midi_dejitter_.pop_due_local(local_now_us, [&](const TimedMidiEvent& ev, bool) {
            track_note(ev);
            if (cb && due_n < 64) due[due_n++] = ev;
        });
    }
    if (cb) {
        for (size_t i = 0; i < due_n; ++i) cb(due[i], -1);
    }
}

uint32_t AudioReceiver::active_midi_notes() const {
    std::lock_guard<std::mutex> lk(midi_mu_);
    uint32_t n = 0;
    for (const auto& port : active_notes_)
        for (const auto& ch : port)
            for (uint8_t v : ch) n += v;
    return n;
}

void AudioReceiver::release_all_notes() {
    std::vector<TimedMidiEvent> offs;
    MidiCallback cb;
    {
        std::lock_guard<std::mutex> lk(midi_mu_);
        cb = midi_cb_;
        for (uint8_t port = 0; port < kMaxMidiPorts; ++port)
            for (uint8_t ch = 0; ch < 16; ++ch)
                for (uint8_t note = 0; note < 128; ++note) {
                    if (!active_notes_[port][ch][note]) continue;
                    active_notes_[port][ch][note] = 0;
                    if (cb) {
                        TimedMidiEvent off{};
                        off.status = uint8_t(0x80 | ch);
                        off.data1 = note;
                        off.data2 = 0;
                        off.port_index = port;
                        offs.push_back(off);
                    }
                }
    }
    if (cb) {
        for (const auto& off : offs) cb(off, 0);
    }
}

bool AudioReceiver::accept_epoch(uint16_t epoch) {
    if (epoch == 0) return true;
    const uint16_t cur = cur_epoch_.load(std::memory_order_relaxed);
    if (epoch == cur) return true;
    if (cur == 0) {
        cur_epoch_.store(epoch, std::memory_order_relaxed);
        return true;
    }
    if (epoch == prev_epoch_) return false;
    prev_epoch_ = cur;
    cur_epoch_.store(epoch, std::memory_order_relaxed);
    reset_stream_epoch();
    return true;
}

void AudioReceiver::reset_stream_epoch() {
    arx_.reset_transport();
    matrix_.request_epoch_reset();
    mclock_.reset();
    last_ping_us_ = 0;
    midi_rx_.reset();
    midi_dejitter_.reset();
    release_all_notes();
    epoch_resets_.fetch_add(1, std::memory_order_relaxed);
}

void AudioReceiver::run(std::atomic<bool>& stop_flag) {
    RealtimeThreadScope rt(2000, 400, 1500);
    while (!stop_flag.load(std::memory_order_relaxed)) {
        step_once();
    }
}

bool AudioReceiver::send_control_sync(const ControlSyncPayload& cs) {
    Endpoint ep{};
    if (!sender_endpoint(ep)) return false;
    uint8_t body[kControlSyncSize];
    write_control_sync(body, cs);
    PacketHeader h;
    h.type = PacketType::kControlSync;
    h.flags = kFlagHasCrc;
    h.seq_id = uint32_t(cs.revision & 0xFFFFFFFFu);
    h.timestamp_us = now_us32();
    uint8_t out[128];
    const size_t n = write_packet(out, sizeof(out), h, body, sizeof(body));
    if (!n) return false;
    int ok = 0;
    for (int rep = 0; rep < 3; ++rep) {
        if (sock_.send_to_qos(out, n, ep, QosTrafficClass::kSyncControl) == int(n)) ++ok;
    }
    if (ok > 0) cs_tx_count_.fetch_add(1, std::memory_order_relaxed);
    return ok > 0;
}

}  // namespace cl
