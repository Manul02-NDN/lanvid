#include "net/receiver.hpp"

#include <algorithm>
#include <cmath>

#include "common/clock.hpp"
#include "common/rt_thread.hpp"
#include "net/packet.hpp"

namespace lv {

UdpReceiver::UdpReceiver(const ReceiverConfig& cfg, FrameCallback on_frame)
    : cfg_(cfg),
      user_on_frame_(std::move(on_frame)),
      pool_(cfg.decode_threads),
      assembler_(cfg.assembler, [this](const CompletedFrame& cf) { on_frame_internal(cf); }, &pool_),
      audio_buf_(AudioJitterBuffer::kDefaultSampleRate, cfg.audio_target_ms),
      matrix_(cfg.audio_sample_rate),
      arx_(matrix_, mclock_, [this](const uint8_t* d, size_t n) {
          return has_sender_ && sock_.send_to(d, n, sender_ep_) == int(n);
      }) {
    arx_.set_latency_ms(cfg.audio_latency_ms);
    arx_.set_adaptive_jitter_enabled(cfg.audio_adaptive_jitter);
    assembler_.set_pli_callback([this](uint32_t) { send_pli_now(now_us()); });
}

bool UdpReceiver::init() {
    if (!sock_.open()) return false;
    if (!sock_.bind(cfg_.bind_ip, cfg_.bind_port)) return false;
    sock_.set_buffer_sizes(UdpSocket::kMinSocketBuffer, std::max(cfg_.socket_buf_bytes, UdpSocket::kMinSocketBuffer));
    sock_.set_low_latency_qos();  // DSCP EF on pings / NACKs / feedback back to the sender
    sock_.set_recv_timeout_ms(cfg_.recv_timeout_ms);
    return true;
}

void UdpReceiver::on_frame_internal(const CompletedFrame& cf) {
    last_completed_fid_ = cf.frame_id;
    fb_dec_us_sum_ += cf.decode_time_us;
    ++fb_dec_cnt_;

    const double lat_us =
        std::max(0.0, estimate_latency_ms(cf.complete_time_us, cf.sender_timestamp_us) * 1000.0);
    if (has_prev_lat_) {
        const double d = std::abs(lat_us - prev_lat_us_);
        jitter_us_ += (d - jitter_us_) / 16.0;
    } else {
        has_prev_lat_ = true;
    }
    prev_lat_us_ = lat_us;

    if (user_on_frame_) user_on_frame_(cf);
}

void UdpReceiver::maybe_send_ping(uint64_t now) {
    if (!cfg_.enable_clock_sync || !has_sender_) return;
    // Fast pings (100 ms) until the MasterClock has a well-converged minimum-RTT set, then the normal rate.
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
    h.frame_id = ++ping_seq_;
    h.timestamp_us = pp.t1_client_send_us;
    uint8_t dgram[64];
    const size_t n = write_packet(dgram, sizeof(dgram), h, pp_bytes, sizeof(pp_bytes));
    if (n) sock_.send_to(dgram, n, sender_ep_);
}

void UdpReceiver::maybe_send_feedback(uint64_t now) {
    if (!cfg_.enable_feedback || !has_sender_) return;
    if (last_fb_us_ == 0) {
        last_fb_us_ = now;
        return;
    }
    if ((now - last_fb_us_) < cfg_.feedback_interval_us) return;
    last_fb_us_ = now;

    const auto& seq = assembler_.seq_tracker();
    const auto& as = assembler_.stats();

    const uint64_t cur_rx = seq.received();
    const uint64_t cur_lost = seq.lost(8);
    const uint64_t d_rx = cur_rx >= fb_prev_rx_ ? (cur_rx - fb_prev_rx_) : 0;
    const uint64_t d_lost = cur_lost >= fb_prev_lost_ ? (cur_lost - fb_prev_lost_) : 0;
    const uint64_t d_exp = d_rx + d_lost;

    const uint64_t cur_fec_rec = as.fec_recovered_packets;
    const uint64_t cur_fec_par = as.fec_parity_received;
    const uint64_t d_fec_rec = cur_fec_rec >= fb_prev_fec_rec_ ? (cur_fec_rec - fb_prev_fec_rec_) : 0;
    const uint64_t d_fec_par = cur_fec_par >= fb_prev_fec_par_ ? (cur_fec_par - fb_prev_fec_par_) : 0;
    const uint64_t d_data_est = d_exp > d_fec_par ? (d_exp - d_fec_par) : d_exp;

    fb_prev_rx_ = cur_rx;
    fb_prev_lost_ = cur_lost;
    fb_prev_fec_rec_ = cur_fec_rec;
    fb_prev_fec_par_ = cur_fec_par;

    FeedbackPayload fb;
    fb.last_completed_frame_id = last_completed_fid_;
    fb.packet_loss_permille =
        d_exp ? uint16_t(std::min<uint64_t>(1000, (d_lost * 1000 + d_exp / 2) / d_exp)) : 0;
    fb.fec_recovered_permille =
        d_data_est ? uint16_t(std::min<uint64_t>(1000, (d_fec_rec * 1000 + d_data_est / 2) / d_data_est)) : 0;
    fb.jitter_us = uint32_t(std::min<double>(jitter_us_, 1e7));
    fb.decode_time_us = fb_dec_cnt_ ? uint32_t(fb_dec_us_sum_ / fb_dec_cnt_) : 0;
    fb_dec_us_sum_ = 0;
    fb_dec_cnt_ = 0;

    uint8_t fb_bytes[kFeedbackSize];
    write_feedback(fb_bytes, fb);

    PacketHeader h;
    h.type = PacketType::kFeedback;
    h.flags = kFlagHasCrc;
    h.frame_id = fb.last_completed_frame_id;
    h.timestamp_us = uint32_t(now);
    uint8_t dgram[64];
    const size_t n = write_packet(dgram, sizeof(dgram), h, fb_bytes, sizeof(fb_bytes));
    if (n && sock_.send_to(dgram, n, sender_ep_) == int(n)) {
        ++feedbacks_sent_;
    }
}

void UdpReceiver::send_pli_now(uint64_t now) {
    if (!has_sender_) return;
    FeedbackPayload fb;
    fb.last_completed_frame_id = last_completed_fid_;
    fb.packet_loss_permille = 0;
    fb.fec_recovered_permille = 0;
    fb.jitter_us = uint32_t(std::min<double>(jitter_us_, 1e7));
    fb.decode_time_us = 0;
    fb.request_idr = true;

    uint8_t fb_bytes[kFeedbackSize];
    write_feedback(fb_bytes, fb);

    PacketHeader h;
    h.type = PacketType::kFeedback;
    h.flags = kFlagHasCrc;
    h.frame_id = fb.last_completed_frame_id;
    h.timestamp_us = uint32_t(now);
    uint8_t dgram[64];
    const size_t n = write_packet(dgram, sizeof(dgram), h, fb_bytes, sizeof(fb_bytes));
    if (n && sock_.send_to(dgram, n, sender_ep_) == int(n)) {
        ++feedbacks_sent_;
        ++plis_sent_;
    }
}

void UdpReceiver::handle_pong(const uint8_t* payload, size_t len, uint32_t t4_us) {
    PingPongPayload pp;
    if (!parse_ping_pong(payload, len, pp)) return;
    if (len >= kPingPongSize + kPongMasterTailSize) {
        uint64_t t2m = 0, t3m = 0;
        if (parse_pong_master_tail(payload + kPingPongSize, len - kPingPongSize, t2m, t3m)) {
            const uint64_t t4_64 = now_us();
            const uint64_t t1_64 = t4_64 - uint64_t(uint32_t(t4_us - pp.t1_client_send_us));
            mclock_.on_exchange(t1_64, t2m, t3m, t4_64);
        }
    }
    const int64_t rtt = int64_t(diff_us32(t4_us, pp.t1_client_send_us)) -
                        int64_t(diff_us32(pp.t3_server_send_us, pp.t2_server_recv_us));
    if (rtt < 0 || rtt > 500000) return;

    const int64_t offset = (int64_t(diff_us32(pp.t2_server_recv_us, pp.t1_client_send_us)) +
                            int64_t(diff_us32(pp.t3_server_send_us, t4_us))) / 2;

    if (!sync_.synced || uint32_t(rtt) <= sync_.min_rtt_us + 50) {
        sync_.synced = true;
        sync_.min_rtt_us = sync_.min_rtt_us == 0 ? uint32_t(rtt) : std::min(sync_.min_rtt_us, uint32_t(rtt));
        sync_.offset_us = offset;
    } else {
        sync_.min_rtt_us += 5;
    }
}

void UdpReceiver::step_once() {
    RecvDatagram batch[kRecvBatchSize];
    for (int i = 0; i < kRecvBatchSize; ++i) {
        batch[i] = {recv_batch_buf_[i], kRecvSlotCap, 0, {}};
    }

    const int got = sock_.recv_batch(batch, kRecvBatchSize);
    const uint64_t now = now_us();

    for (int i = 0; i < got; ++i) {
        const size_t n = batch[i].len;
        if (n == 0) continue;

        PacketHeader h;
        const uint8_t* payload = nullptr;
        const PacketParseStatus st = parse_packet(batch[i].data, n, h, &payload);
        if (st == PacketParseStatus::kOk) {
            if (h.type == PacketType::kPong) {
                handle_pong(payload, h.payload_size, uint32_t(now));
            } else if (h.type == PacketType::kVideoSlice) {
                if (cfg_.audio_only) {  // headless AoIP bridge: never reassemble / decode video
                    ++video_ignored_;
                    continue;
                }
                if (!has_sender_ || !(sender_ep_ == batch[i].src)) {
                    sender_ep_ = batch[i].src;
                    has_sender_ = true;
                }
                assembler_.ingest_video_packet(h, payload, n, now);
            } else if (h.type == PacketType::kAudio2) {
                if (!accept_epoch(h.total_slices)) continue;  // late packet of the previous session
                if (!has_sender_ || !(sender_ep_ == batch[i].src)) {
                    sender_ep_ = batch[i].src;
                    has_sender_ = true;
                }
                audio2_bytes_rx_.fetch_add(uint64_t(n), std::memory_order_relaxed);
                audio2_packets_rx_.fetch_add(1, std::memory_order_relaxed);
                arx_.on_packet(h, payload, h.payload_size, now);
            } else if (h.type == PacketType::kMidiFrame) {
                if (h.payload_size < 4) continue;
                const uint32_t sid = uint32_t(payload[0]) | (uint32_t(payload[1]) << 8) |
                                     (uint32_t(payload[2]) << 16) | (uint32_t(payload[3]) << 24);
                if (!accept_epoch(uint16_t(sid))) continue;
                if (!has_sender_ || !(sender_ep_ == batch[i].src)) {
                    sender_ep_ = batch[i].src;
                    has_sender_ = true;
                }
                midi_rx_.on_packet(h, payload, h.payload_size);
            } else if (h.type == PacketType::kAudio) {
                if (!has_sender_ || !(sender_ep_ == batch[i].src)) {
                    sender_ep_ = batch[i].src;
                    has_sender_ = true;
                }
                AudioHeader ah;
                if (parse_audio_header(payload, h.payload_size, ah)) {
                    audio_buf_.push_packet(h.frame_id, ah, payload + kAudioHeaderSize, n);
                    if (audio_cb_) audio_cb_(h.frame_id, h.timestamp_us, ah, payload + kAudioHeaderSize);
                }
            }
        } else {
            assembler_.record_bad_packet(st, n);
        }
    }

    assembler_.poll(now);
    arx_.service(now);
    // MIDI without an audio consumer (no pop_audio_* for 50 ms): schedule on the MasterClock playout
    // estimate (or immediately while the clock is not synced yet).
    if (now - last_pop_us_.load(std::memory_order_relaxed) > 50'000) {
        uint64_t playout = UINT64_MAX;
        if (mclock_.synced()) {
            const int64_t m = mclock_.master_us(now) - int64_t(arx_.latency_ms()) * 1000;
            playout = m > 0 ? uint64_t(m) : 0;
        }
        dispatch_midi(playout, -1.0, 0);
    }
    maybe_send_ping(now);
    maybe_send_feedback(now);
}

// ---- audio pull + sample-accurate MIDI dispatch ----

void UdpReceiver::pop_audio_multichannel_interleaved(float* out, uint32_t num_channels, uint32_t frames) {
    matrix_.pop_interleaved(mclock_, now_us(), out, num_channels, frames);
    last_pop_us_.store(now_us(), std::memory_order_relaxed);
    const double end = matrix_.playout_cursor();
    dispatch_midi(matrix_.playout_media_ts_us(), end - double(frames), frames);
}

void UdpReceiver::pop_audio_multichannel_planar(float* const* out, uint32_t num_channels, uint32_t frames) {
    matrix_.pop_planar(mclock_, now_us(), out, num_channels, frames);
    last_pop_us_.store(now_us(), std::memory_order_relaxed);
    const double end = matrix_.playout_cursor();
    dispatch_midi(matrix_.playout_media_ts_us(), end - double(frames), frames);
}

void UdpReceiver::set_midi_callback(MidiCallback cb) {
    std::lock_guard<std::mutex> lk(midi_mu_);
    midi_cb_ = std::move(cb);
}

void UdpReceiver::track_note(const TimedMidiEvent& ev) {
    const uint8_t hi = ev.status & 0xF0, ch = ev.status & 0x0F, port = ev.port_index & 0x0F;
    if (hi == 0x90 && ev.data2 > 0) {
        active_notes_[port][ch][ev.data1 & 0x7F] = 1;
    } else if (hi == 0x80 || (hi == 0x90 && ev.data2 == 0)) {
        active_notes_[port][ch][ev.data1 & 0x7F] = 0;
    } else if (hi == 0xB0 && (ev.data1 == 123 || ev.data1 == 120)) {  // All Notes / Sound Off
        for (auto& n : active_notes_[port][ch]) n = 0;
    }
}

void UdpReceiver::dispatch_midi(uint64_t playout_us, double block_start_samples, uint32_t frames) {
    std::lock_guard<std::mutex> lk(midi_mu_);
    const double rate = double(matrix_.sample_rate());
    midi_rx_.pop_due_events(playout_us, [&](const TimedMidiEvent& ev) {
        track_note(ev);
        if (!midi_cb_) return;
        int32_t off = -1;
        if (frames > 0) {
            const double pos = double(ev.media_ts_us) * rate / 1e6 - block_start_samples;
            off = int32_t(std::clamp(std::floor(pos), 0.0, double(frames - 1)));
        }
        midi_cb_(ev, off);
    });
}

uint32_t UdpReceiver::active_midi_notes() const {
    std::lock_guard<std::mutex> lk(midi_mu_);
    uint32_t n = 0;
    for (const auto& port : active_notes_)
        for (const auto& ch : port)
            for (uint8_t v : ch) n += v;
    return n;
}

void UdpReceiver::release_all_notes() {
    std::lock_guard<std::mutex> lk(midi_mu_);
    for (uint8_t port = 0; port < kMaxMidiPorts; ++port)
        for (uint8_t ch = 0; ch < 16; ++ch)
            for (uint8_t note = 0; note < 128; ++note) {
                if (!active_notes_[port][ch][note]) continue;
                active_notes_[port][ch][note] = 0;
                if (midi_cb_) {
                    TimedMidiEvent off{};
                    off.status = uint8_t(0x80 | ch);
                    off.data1 = note;
                    off.data2 = 0;
                    off.port_index = port;
                    midi_cb_(off, 0);
                }
            }
}

// ---- stream epoch (sender restart): reset transport state without clicks or stuck notes ----

bool UdpReceiver::accept_epoch(uint16_t epoch) {
    if (epoch == 0) return true;  // pre-epoch sender
    const uint16_t cur = cur_epoch_.load(std::memory_order_relaxed);
    if (epoch == cur) return true;
    if (cur == 0) {
        cur_epoch_.store(epoch, std::memory_order_relaxed);
        return true;
    }
    if (epoch == prev_epoch_) return false;  // in-flight packet of the session we just left
    prev_epoch_ = cur;
    cur_epoch_.store(epoch, std::memory_order_relaxed);
    reset_stream_epoch();
    return true;
}

void UdpReceiver::reset_stream_epoch() {
    arx_.reset_transport();          // FEC groups, sequence windows, NACK schedule, jitter
    matrix_.request_epoch_reset();   // crossfade to silence, re-anchor on the new timeline
    mclock_.reset();                 // the new sender has a new MasterClock epoch
    sync_ = {};
    last_ping_us_ = 0;               // re-sync immediately
    midi_rx_.reset();                // event_seq dedup window + queued events of the old session
    release_all_notes();             // Note Off for everything still sounding -> no stuck notes
    epoch_resets_.fetch_add(1, std::memory_order_relaxed);
}

void UdpReceiver::run(std::atomic<bool>& stop_flag) {
    RealtimeThreadScope rt(2000, 400, 1500);  // network RX: audio/MIDI packets + clock sync
    while (!stop_flag.load(std::memory_order_relaxed)) {
        step_once();
    }
}

void UdpReceiver::flush() { assembler_.flush(now_us()); }

double UdpReceiver::estimate_latency_ms(uint64_t recv_complete_us, uint32_t sender_ts_us) const {
    const int64_t raw = int64_t(diff_us32(uint32_t(recv_complete_us), sender_ts_us));
    const int64_t adj = sync_.synced ? (raw + sync_.offset_us) : raw;
    return double(adj) / 1000.0;
}

}  // namespace lv
