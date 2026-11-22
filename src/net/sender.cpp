#include "net/sender.hpp"

#include <algorithm>
#include <cstring>
#include <thread>
#include <vector>

#include "common/clock.hpp"
#include "net/packet.hpp"

namespace lv {

UdpSender::UdpSender(const SenderConfig& cfg)
    : cfg_(cfg),
      pool_(cfg.threads),
      aq_(cfg.adaptive),
      fec_group_size_(cfg.fec_group_size),
      sim_loss_pct_(cfg.sim_loss_pct) {}

bool UdpSender::init() {
    if (!Endpoint::parse(cfg_.dest_ip, cfg_.dest_port, dst_)) return false;
    if (!sock_.open()) return false;
    if (!sock_.bind("0.0.0.0", cfg_.bind_port)) return false;
    sock_.set_buffer_sizes(cfg_.socket_buf_bytes, 256 * 1024);
    sock_.set_nonblocking(true);
    last_pace_us_ = now_us();
    tokens_bytes_ = 64.0 * 1024.0;
    return true;
}

bool UdpSender::should_drop_sim() {
    const double pct = sim_loss_pct_.load(std::memory_order_relaxed);
    if (pct <= 0.0) return false;
    if (pct >= 100.0) return true;
    uint64_t x = rng_state_.fetch_add(0x9E3779B97F4A7C15ull, std::memory_order_relaxed);
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    const uint64_t v = (x * 0x2545F4914F6CDD1Dull) >> 32;
    const double u = double(v) * (100.0 / 4294967296.0);
    return u < pct;
}

void UdpSender::pace_bytes(size_t bytes, std::unique_lock<std::mutex>& lk) {
    if (cfg_.pace_mbps <= 0.0) return;
    const double bytes_per_us = cfg_.pace_mbps * (1e6 / 8.0) / 1e6;
    const double max_burst = std::max(double(cfg_.mtu) * 8.0, 16384.0);

    for (;;) {
        const uint64_t now = now_us();
        if (now > last_pace_us_) {
            tokens_bytes_ = std::min(max_burst, tokens_bytes_ + double(now - last_pace_us_) * bytes_per_us);
            last_pace_us_ = now;
        }
        if (tokens_bytes_ >= double(bytes)) {
            tokens_bytes_ -= double(bytes);
            return;
        }
        const uint64_t wait_us = uint64_t((double(bytes) - tokens_bytes_) / bytes_per_us);
        lk.unlock();
        if (wait_us > 1500) std::this_thread::sleep_for(std::chrono::microseconds(wait_us / 2));
        else std::this_thread::yield();
        lk.lock();
    }
}

void UdpSender::send_encoded_slice(uint32_t frame_id, uint32_t capture_ts_us, int slice_id,
                                   int total_slices, const uint8_t* data, size_t size) {
    PacketizerConfig pcfg;
    pcfg.mtu = cfg_.mtu;
    pcfg.enable_crc = cfg_.enable_crc;
    pcfg.fec_group_size = fec_group_size_.load(std::memory_order_relaxed);

    std::unique_lock<std::mutex> send_lk(pace_mu_);
    if (cfg_.pace_mbps <= 0.0 && cfg_.mtu <= 1500) {
        // Collect this slice's packets and emit in one batch (uses sendmmsg on Linux).
        constexpr int kMaxBatch = 32;
        uint8_t batch_storage[kMaxBatch][1500];
        DatagramView views[kMaxBatch];
        int count = 0;

        auto flush_batch = [&]() {
            if (count == 0) return;
            const int sent = sock_.send_batch(views, count, dst_);
            if (sent > 0) {
                packets_sent_.fetch_add(uint64_t(sent), std::memory_order_relaxed);
                size_t ok_bytes = 0;
                for (int i = 0; i < sent; ++i) ok_bytes += views[i].len;
                bytes_sent_.fetch_add(ok_bytes, std::memory_order_relaxed);
            }
            if (sent < count) {
                send_errors_.fetch_add(uint64_t(count - std::max(0, sent)), std::memory_order_relaxed);
            }
            count = 0;
        };

        packetize_slice(frame_id, capture_ts_us, uint16_t(slice_id), uint16_t(total_slices), data,
                        size, pcfg, seq_, [&](const uint8_t* dgram, size_t len) {
                            if (should_drop_sim()) {
                                packets_dropped_sim_.fetch_add(1, std::memory_order_relaxed);
                                return;
                            }
                            if (count == kMaxBatch) flush_batch();
                            std::memcpy(batch_storage[count], dgram, len);
                            views[count] = {batch_storage[count], len};
                            ++count;
                        });
        flush_batch();
    } else {
        packetize_slice(frame_id, capture_ts_us, uint16_t(slice_id), uint16_t(total_slices), data,
                        size, pcfg, seq_, [&](const uint8_t* dgram, size_t len) {
                            if (should_drop_sim()) {
                                packets_dropped_sim_.fetch_add(1, std::memory_order_relaxed);
                                return;
                            }
                            if (cfg_.pace_mbps > 0.0) pace_bytes(len, send_lk);
                            const int sent = sock_.send_to(dgram, len, dst_);
                            if (sent == int(len)) {
                                packets_sent_.fetch_add(1, std::memory_order_relaxed);
                                bytes_sent_.fetch_add(uint64_t(len), std::memory_order_relaxed);
                            } else {
                                send_errors_.fetch_add(1, std::memory_order_relaxed);
                            }
                        });
    }
    slices_sent_.fetch_add(1, std::memory_order_relaxed);
}

bool UdpSender::send_frame(const Image& img, const CodecParams& params, uint32_t frame_id,
                           uint32_t capture_ts_us) {
    auto on_slice = [&](int slice_id, int total_slices, const uint8_t* data, size_t size) {
        send_encoded_slice(frame_id, capture_ts_us, slice_id, total_slices, data, size);
    };
    if (!codec_.encode_streaming(img, params, on_slice, &pool_)) return false;
    frames_sent_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool UdpSender::send_frame_yuv420(const Yuv420View& yuv, const CodecParams& params, uint32_t frame_id,
                                  uint32_t capture_ts_us) {
    auto on_slice = [&](int slice_id, int total_slices, const uint8_t* data, size_t size) {
        send_encoded_slice(frame_id, capture_ts_us, slice_id, total_slices, data, size);
    };
    if (!codec_.encode_streaming_yuv420(yuv, params, on_slice, &pool_)) return false;
    frames_sent_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool UdpSender::send_audio_raw(uint32_t audio_seq_id, uint32_t timestamp_us, const void* samples,
                               uint16_t num_frames, uint32_t sample_rate, uint8_t channels,
                               AudioSampleFormat fmt) {
    if (!samples || num_frames == 0 || channels < 1 || channels > 2) return false;
    const size_t sample_bytes = (fmt == AudioSampleFormat::kFloat32) ? sizeof(float) : sizeof(int16_t);
    const size_t pcm_bytes = size_t(num_frames) * channels * sample_bytes;
    const size_t payload_len = kAudioHeaderSize + pcm_bytes;
    if (kPacketHeaderSize + payload_len > 2048) return false;

    uint8_t payload[2048];
    AudioHeader ah;
    ah.sample_rate = sample_rate;
    ah.channels = channels;
    ah.sample_format = fmt;
    ah.num_frames = num_frames;
    write_audio_header(payload, ah);
    std::memcpy(payload + kAudioHeaderSize, samples, pcm_bytes);

    PacketHeader h;
    h.version = kProtocolVersion;
    h.type = PacketType::kAudio;
    h.flags = cfg_.enable_crc ? kFlagHasCrc : 0;
    h.frame_id = audio_seq_id;  // independent audio sequence; never touches video seq_
    h.timestamp_us = timestamp_us;
    h.seq = uint16_t(audio_seq_id);
    h.payload_size = uint16_t(payload_len);

    uint8_t dgram[2048];
    const size_t n = write_packet(dgram, sizeof(dgram), h, payload, payload_len);
    if (!n) return false;

    std::lock_guard<std::mutex> lk(pace_mu_);
    const int sent = sock_.send_to(dgram, n, dst_);
    if (sent == int(n)) {
        audio_packets_sent_.fetch_add(1, std::memory_order_relaxed);
        audio_bytes_sent_.fetch_add(uint64_t(n), std::memory_order_relaxed);
        return true;
    }
    send_errors_.fetch_add(1, std::memory_order_relaxed);
    return false;
}

bool UdpSender::send_audio(uint32_t audio_seq_id, uint32_t timestamp_us, const int16_t* pcm16,
                           uint16_t num_frames, uint32_t sample_rate, uint8_t channels) {
    return send_audio_raw(audio_seq_id, timestamp_us, pcm16, num_frames, sample_rate, channels,
                          AudioSampleFormat::kPcm16);
}

void UdpSender::set_adaptive_quality(bool enabled, int q_min, int q_max, int current_q) {
    std::lock_guard<std::mutex> lk(aq_mu_);
    AdaptiveQualityConfig c = cfg_.adaptive;
    c.enabled = enabled;
    c.q_min = std::clamp(q_min, 1, 100);
    c.q_max = std::clamp(q_max, c.q_min, 100);
    c.initial_q = std::clamp(current_q, c.q_min, c.q_max);
    cfg_.adaptive = c;
    aq_.reset(c);
}

void UdpSender::service_control() {
    uint8_t buf[256];
    Endpoint peer{};
    for (int i = 0; i < 32; ++i) {
        const int n = sock_.recv_from(buf, sizeof(buf), peer);
        if (n <= 0) break;
        const uint64_t now = now_us();
        const uint32_t t2 = uint32_t(now);
        PacketHeader h;
        const uint8_t* payload = nullptr;
        if (parse_packet(buf, size_t(n), h, &payload) != PacketParseStatus::kOk) continue;

        if (h.type == PacketType::kPing) {
            PingPongPayload pp;
            if (!parse_ping_pong(payload, h.payload_size, pp)) continue;
            pp.t2_server_recv_us = t2;
            pp.t3_server_send_us = now_us32();

            uint8_t pp_bytes[kPingPongSize];
            write_ping_pong(pp_bytes, pp);

            PacketHeader rh;
            rh.type = PacketType::kPong;
            rh.flags = kFlagHasCrc;
            rh.frame_id = h.frame_id;
            rh.timestamp_us = pp.t3_server_send_us;
            uint8_t out[64];
            const size_t out_n = write_packet(out, sizeof(out), rh, pp_bytes, sizeof(pp_bytes));
            if (out_n && sock_.send_to(out, out_n, peer) == int(out_n)) {
                pongs_sent_.fetch_add(1, std::memory_order_relaxed);
            }
        } else if (h.type == PacketType::kFeedback) {
            FeedbackPayload fb;
            if (!parse_feedback(payload, h.payload_size, fb)) continue;
            {
                std::lock_guard<std::mutex> alk(aq_mu_);
                aq_.on_feedback(fb, now);
            }
            feedbacks_received_.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

SenderStats UdpSender::stats() const {
    FeedbackPayload fb{};
    int cur_q = 75;
    {
        std::lock_guard<std::mutex> alk(aq_mu_);
        fb = aq_.last_feedback();
        cur_q = aq_.current_quality();
    }
    return {frames_sent_.load(std::memory_order_relaxed),
            slices_sent_.load(std::memory_order_relaxed),
            packets_sent_.load(std::memory_order_relaxed),
            packets_dropped_sim_.load(std::memory_order_relaxed),
            send_errors_.load(std::memory_order_relaxed),
            bytes_sent_.load(std::memory_order_relaxed),
            audio_packets_sent_.load(std::memory_order_relaxed),
            audio_bytes_sent_.load(std::memory_order_relaxed),
            pongs_sent_.load(std::memory_order_relaxed),
            feedbacks_received_.load(std::memory_order_relaxed),
            cur_q,
            fb.packet_loss_permille,
            fb.fec_recovered_permille};
}

}  // namespace lv
