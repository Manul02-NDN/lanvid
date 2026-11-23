#include "net/receiver.hpp"

#include <algorithm>
#include <cmath>

#include "common/clock.hpp"
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
    assembler_.set_pli_callback([this](uint32_t) { send_pli_now(now_us()); });
}

bool UdpReceiver::init() {
    if (!sock_.open()) return false;
    if (!sock_.bind(cfg_.bind_ip, cfg_.bind_port)) return false;
    sock_.set_buffer_sizes(256 * 1024, cfg_.socket_buf_bytes);
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
    if (last_ping_us_ != 0 && (now - last_ping_us_) < cfg_.ping_interval_us) return;
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
                if (!has_sender_ || !(sender_ep_ == batch[i].src)) {
                    sender_ep_ = batch[i].src;
                    has_sender_ = true;
                }
                arx_.on_packet(h, payload, h.payload_size, now);
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
    maybe_send_ping(now);
    maybe_send_feedback(now);
}

void UdpReceiver::run(std::atomic<bool>& stop_flag) {
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
