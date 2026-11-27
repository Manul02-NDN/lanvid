#include "net/video_sender.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <thread>
#include <vector>

#include "codec/dct.hpp"
#include "common/clock.hpp"
#include "common/network_interfaces.hpp"
#include "net/packet.hpp"

namespace lv {

UdpSender::UdpSender(const SenderConfig& cfg)
    : cfg_(cfg),
      pool_(cfg.threads),
      active_codec_(uint8_t(cfg.codec)),
      target_fps_(cfg.fps > 1.0 ? cfg.fps : 60.0),
      target_bitrate_kbps_(cfg.bitrate_kbps > 0 ? cfg.bitrate_kbps : 8000u),
      enc_backend_name_(std::string("LanVid DCT (") + dct_kernels_best().name + ")"),
      aq_(cfg.adaptive),
      fec_group_size_(cfg.fec_group_size),
      sim_loss_pct_(cfg.sim_loss_pct) {
    if (cfg_.bitrate_kbps > 0) {
        cfg_.adaptive.initial_bitrate_kbps = cfg_.bitrate_kbps;
        aq_.reset(cfg_.adaptive);
    }
}

bool UdpSender::init() {
    if (!Endpoint::parse(cfg_.dest_ip, cfg_.dest_port, dst_)) return false;
    if (!sock_.open()) return false;
    primary_nic_ip_ = cfg_.bind_ip;
    secondary_nic_ip_ = cfg_.secondary_bind_ip;

    auto bind_sock_for_ip = [](UdpSocket& s, const std::string& ip, uint16_t port) -> bool {
        if (ip.empty() || ip == "0.0.0.0" || ip == "auto") {
            return s.bind("0.0.0.0", port);
        }
        NetworkInterfaceInfo info{};
        const bool found = find_interface_by_ip(ip, info);
        return s.bind_to_interface(ip, port, found ? info.if_index : 0u, found ? info.system_name : "");
    };

    if (!bind_sock_for_ip(sock_, primary_nic_ip_, cfg_.bind_port)) {
        if (!primary_nic_ip_.empty() && primary_nic_ip_ != "0.0.0.0") {
            if (!sock_.bind("0.0.0.0", cfg_.bind_port)) return false;
        } else {
            return false;
        }
    }
    sock_.set_buffer_sizes(std::max(cfg_.socket_buf_bytes, UdpSocket::kMinSocketBuffer),
                           UdpSocket::kMinSocketBuffer);
    sock_.set_qos_class(QosTrafficClass::kVideo);
    sock_.set_nonblocking(true);
    if (UdpSocket::is_multicast_ip(cfg_.dest_ip)) {
        sock_.set_multicast_loop_and_ttl(true, 2);
    }

    if (!secondary_nic_ip_.empty() && secondary_nic_ip_ != "0.0.0.0") {
        set_network_interfaces(primary_nic_ip_, secondary_nic_ip_);
    }

    last_pace_us_ = now_us();
    tokens_bytes_ = 64.0 * 1024.0;
    return true;
}

bool UdpSender::add_destination(const std::string& ip, uint16_t port) {
    if (ip.empty() || port == 0) return false;
    Endpoint ep{};
    if (!Endpoint::parse(ip, port, ep) || !ep.valid()) return false;
    if (ep == dst_) {
        force_idr_.store(true, std::memory_order_release);
        return true;
    }
    std::lock_guard<std::mutex> lk(dsts_mu_);
    for (const auto& cur : extra_dsts_) {
        if (cur == ep) {
            force_idr_.store(true, std::memory_order_release);
            return true;
        }
    }
    if (extra_dsts_.size() + 1 >= kMaxFanoutDestinations) return false;
    if (UdpSocket::is_multicast_ip(ip) && sock_.is_open()) {
        sock_.set_multicast_loop_and_ttl(true, 2);
    }
    extra_dsts_.push_back(ep);
    extra_dst_count_.store(uint32_t(extra_dsts_.size()), std::memory_order_release);
    force_idr_.store(true, std::memory_order_release);
    return true;
}

bool UdpSender::remove_destination(const std::string& ip, uint16_t port) {
    Endpoint ep{};
    if (!Endpoint::parse(ip, port, ep)) return false;
    std::lock_guard<std::mutex> lk(dsts_mu_);
    const auto old_sz = extra_dsts_.size();
    extra_dsts_.erase(std::remove(extra_dsts_.begin(), extra_dsts_.end(), ep), extra_dsts_.end());
    extra_dst_count_.store(uint32_t(extra_dsts_.size()), std::memory_order_release);
    return extra_dsts_.size() != old_sz;
}

void UdpSender::clear_extra_destinations() {
    std::lock_guard<std::mutex> lk(dsts_mu_);
    extra_dsts_.clear();
    extra_dst_count_.store(0, std::memory_order_release);
}

std::vector<Endpoint> UdpSender::destinations() const {
    std::vector<Endpoint> out;
    if (dst_.valid()) out.push_back(dst_);
    std::lock_guard<std::mutex> lk(dsts_mu_);
    out.insert(out.end(), extra_dsts_.begin(), extra_dsts_.end());
    return out;
}

bool UdpSender::set_network_interfaces(const std::string& primary_ip, const std::string& secondary_ip) {
    std::lock_guard<std::mutex> lk(nic_mu_);
    primary_nic_ip_ = (primary_ip == "auto" || primary_ip == "0.0.0.0") ? std::string() : primary_ip;
    secondary_nic_ip_ = (secondary_ip == "auto" || secondary_ip == "0.0.0.0") ? std::string() : secondary_ip;
    cfg_.bind_ip = primary_nic_ip_;
    cfg_.secondary_bind_ip = secondary_nic_ip_;

    sock_secondary_.close();
    dual_nic_active_.store(false, std::memory_order_release);

    if (!secondary_nic_ip_.empty()) {
        if (sock_secondary_.open()) {
            NetworkInterfaceInfo info{};
            const bool found = find_interface_by_ip(secondary_nic_ip_, info);
            if (sock_secondary_.bind_to_interface(secondary_nic_ip_, 0, found ? info.if_index : 0u,
                                                  found ? info.system_name : "")) {
                sock_secondary_.set_buffer_sizes(std::max(cfg_.socket_buf_bytes / 2, UdpSocket::kMinSocketBuffer),
                                                 UdpSocket::kMinSocketBuffer);
                sock_secondary_.set_qos_class(QosTrafficClass::kVideo);
                sock_secondary_.set_nonblocking(true);
                dual_nic_active_.store(true, std::memory_order_release);
                return true;
            }
            sock_secondary_.close();
        }
        return false;
    }
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
                                   int total_slices, const uint8_t* data, size_t size,
                                   uint8_t extra_flags) {
    PacketizerConfig pcfg;
    pcfg.mtu = cfg_.mtu;
    pcfg.enable_crc = cfg_.enable_crc;
    pcfg.fec_group_size = fec_group_size_.load(std::memory_order_relaxed);
    pcfg.extra_flags = extra_flags;

    std::unique_lock<std::mutex> send_lk(pace_mu_);
    if (cfg_.pace_mbps <= 0.0 && cfg_.mtu <= 1500) {
        constexpr int kMaxBatch = 32;
        uint8_t batch_storage[kMaxBatch][1500];
        DatagramView views[kMaxBatch];
        int count = 0;

        auto flush_batch = [&]() {
            if (count == 0) return;
            int sent = primary_link_down_.load(std::memory_order_acquire)
                           ? 0
                           : sock_.send_batch_qos(views, count, dst_, QosTrafficClass::kVideo);
            if (sent < count && dual_nic_active_.load(std::memory_order_acquire)) {
                std::lock_guard<std::mutex> lk(nic_mu_);
                if (sock_secondary_.is_open()) {
                    const int s2 = sock_secondary_.send_batch_qos(views, count, dst_, QosTrafficClass::kVideo);
                    if (s2 > 0) {
                        sec_pkts_sent_.fetch_add(uint64_t(s2), std::memory_order_relaxed);
                        failover_pkts_.fetch_add(uint64_t(s2), std::memory_order_relaxed);
                        size_t b2 = 0;
                        for (int i = 0; i < s2; ++i) b2 += views[i].len;
                        sec_bytes_sent_.fetch_add(b2, std::memory_order_relaxed);
                        if (sent <= 0) sent = s2;
                    }
                }
            }
            // Zero-re-encode fan-out to additional video destinations
            if (extra_dst_count_.load(std::memory_order_acquire) > 0) {
                std::vector<Endpoint> extras;
                {
                    std::lock_guard<std::mutex> dlk(dsts_mu_);
                    extras = extra_dsts_;
                }
                for (const auto& edst : extras) {
                    if (!primary_link_down_.load(std::memory_order_acquire)) {
                        (void)sock_.send_batch_qos(views, count, edst, QosTrafficClass::kVideo);
                    }
                }
            }
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
                            const int sent = send_with_failover(dgram, len, QosTrafficClass::kVideo);
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

bool UdpSender::prepare_encoder(int width, int height) {
    if (active_codec() == VideoCodecId::kLanVidDct) return true;
    return ensure_video_encoder(width, height);
}

bool UdpSender::ensure_video_encoder(int width, int height) {
    const VideoCodecId want_codec = active_codec();
    const double want_fps = target_fps_.load(std::memory_order_acquire);
    if (video_enc_ && enc_w_ == width && enc_h_ == height && enc_codec_ == want_codec &&
        std::abs(enc_fps_ - want_fps) < 0.5) {
        return true;
    }
    video_enc_.reset();
    VideoEncoderConfig ecfg;
    ecfg.width = width;
    ecfg.height = height;
    ecfg.fps = want_fps > 1.0 ? want_fps : 60.0;
    ecfg.bitrate_kbps = target_bitrate_kbps_.load(std::memory_order_relaxed);
    ecfg.keyframe_interval = std::max<uint32_t>(24u, uint32_t(ecfg.fps + 0.5));
    ecfg.prefer_hardware = true;

    video_enc_ = create_video_encoder(want_codec, ecfg);
    if (!video_enc_) return false;
    enc_w_ = width;
    enc_h_ = height;
    enc_codec_ = want_codec;
    enc_fps_ = ecfg.fps;
    applied_bitrate_kbps_ = ecfg.bitrate_kbps;
    {
        std::lock_guard<std::mutex> lk(enc_meta_mu_);
        enc_backend_name_ = video_enc_->backend_name();
        enc_is_hw_ = video_enc_->is_hardware();
    }
    return true;
}

bool UdpSender::send_frame(const Image& img, const CodecParams& params, uint32_t frame_id,
                           uint32_t capture_ts_us) {
    const VideoCodecId codec = active_codec();
    if (codec != VideoCodecId::kLanVidDct) {
        if (!ensure_video_encoder(img.width, img.height)) return false;
        if (force_idr_.exchange(false, std::memory_order_acq_rel)) {
            video_enc_->force_keyframe();
        }
        const uint32_t br = target_bitrate_kbps_.load(std::memory_order_relaxed);
        if (br != applied_bitrate_kbps_) {
            video_enc_->set_bitrate_kbps(br);
            applied_bitrate_kbps_ = br;
        }
        if (!video_enc_->encode_image(img, enc_pkt_) || enc_pkt_.data.empty()) return false;
        const uint8_t extra_flags =
            uint8_t(encode_codec_flags(codec) |
                    (enc_pkt_.is_keyframe ? kFlagKeyFrame : 0) |
                    (enc_pkt_.has_codec_config ? kFlagCodecConfig : 0));
        send_encoded_slice(frame_id, capture_ts_us, 0, 1, enc_pkt_.data.data(), enc_pkt_.data.size(),
                           extra_flags);
        frames_sent_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    auto on_slice = [&](int slice_id, int total_slices, const uint8_t* data, size_t size) {
        send_encoded_slice(frame_id, capture_ts_us, slice_id, total_slices, data, size, 0);
    };
    if (!codec_.encode_streaming(img, params, on_slice, &pool_)) return false;
    frames_sent_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool UdpSender::send_frame_yuv420(const Yuv420View& yuv, const CodecParams& params, uint32_t frame_id,
                                  uint32_t capture_ts_us) {
    const VideoCodecId codec = active_codec();
    if (codec != VideoCodecId::kLanVidDct) {
        if (!ensure_video_encoder(params.width, params.height)) return false;
        if (force_idr_.exchange(false, std::memory_order_acq_rel)) {
            video_enc_->force_keyframe();
        }
        const uint32_t br = target_bitrate_kbps_.load(std::memory_order_relaxed);
        if (br != applied_bitrate_kbps_) {
            video_enc_->set_bitrate_kbps(br);
            applied_bitrate_kbps_ = br;
        }
        if (!video_enc_->encode_yuv420(yuv, params.width, params.height, enc_pkt_) ||
            enc_pkt_.data.empty()) {
            return false;
        }
        const uint8_t extra_flags =
            uint8_t(encode_codec_flags(codec) |
                    (enc_pkt_.is_keyframe ? kFlagKeyFrame : 0) |
                    (enc_pkt_.has_codec_config ? kFlagCodecConfig : 0));
        send_encoded_slice(frame_id, capture_ts_us, 0, 1, enc_pkt_.data.data(), enc_pkt_.data.size(),
                           extra_flags);
        frames_sent_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    auto on_slice = [&](int slice_id, int total_slices, const uint8_t* data, size_t size) {
        send_encoded_slice(frame_id, capture_ts_us, slice_id, total_slices, data, size, 0);
    };
    if (!codec_.encode_streaming_yuv420(yuv, params, on_slice, &pool_)) return false;
    frames_sent_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool UdpSender::send_frame_native(const void* native_buf, int width, int height, uint32_t frame_id,
                                  uint32_t capture_ts_us) {
    const VideoCodecId codec = active_codec();
    if (!native_buf || codec == VideoCodecId::kLanVidDct) return false;
    const int even_w = width & ~1;
    const int even_h = height & ~1;
    if (!ensure_video_encoder(even_w, even_h)) return false;
    if (force_idr_.exchange(false, std::memory_order_acq_rel)) {
        video_enc_->force_keyframe();
    }
    const uint32_t br = target_bitrate_kbps_.load(std::memory_order_relaxed);
    if (br != applied_bitrate_kbps_) {
        video_enc_->set_bitrate_kbps(br);
        applied_bitrate_kbps_ = br;
    }
    if (!video_enc_->encode_native_buffer(native_buf, even_w, even_h, enc_pkt_) ||
        enc_pkt_.data.empty()) {
        return false;
    }
    const uint8_t extra_flags =
        uint8_t(encode_codec_flags(codec) |
                (enc_pkt_.is_keyframe ? kFlagKeyFrame : 0) |
                (enc_pkt_.has_codec_config ? kFlagCodecConfig : 0));
    send_encoded_slice(frame_id, capture_ts_us, 0, 1, enc_pkt_.data.data(), enc_pkt_.data.size(),
                       extra_flags);
    frames_sent_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool UdpSender::send_audio_raw(uint32_t audio_seq_id, uint32_t timestamp_us, const void* samples,
                               uint16_t num_frames, uint32_t sample_rate, uint8_t channels,
                               AudioSampleFormat fmt) {
    if (!samples || num_frames == 0 || channels < 1 || channels > kMaxStereoChannels) return false;
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
    h.frame_id = audio_seq_id;
    h.timestamp_us = timestamp_us;
    h.seq = uint16_t(audio_seq_id);
    h.payload_size = uint16_t(payload_len);

    uint8_t dgram[2048];
    const size_t n = write_packet(dgram, sizeof(dgram), h, payload, payload_len);
    if (!n) return false;

    std::lock_guard<std::mutex> lk(pace_mu_);
    const int sent = send_with_failover(dgram, n, QosTrafficClass::kAudio);
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
    c.initial_bitrate_kbps = target_bitrate_kbps_.load(std::memory_order_relaxed);
    cfg_.adaptive = c;
    aq_.reset(c);
}

void UdpSender::set_bitrate_kbps(uint32_t bitrate_kbps) {
    const uint32_t clamped = std::clamp<uint32_t>(bitrate_kbps, 250u, 150000u);
    target_bitrate_kbps_.store(clamped, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(aq_mu_);
    aq_.set_target_bitrate_kbps(clamped);
}

void UdpSender::set_codec(VideoCodecId codec) {
    active_codec_.store(uint8_t(codec), std::memory_order_release);
    force_idr_.store(true, std::memory_order_release);
    if (codec == VideoCodecId::kLanVidDct) {
        std::lock_guard<std::mutex> lk(enc_meta_mu_);
        enc_backend_name_ = std::string("LanVid DCT (") + dct_kernels_best().name + ")";
        enc_is_hw_ = false;
    }
}

void UdpSender::set_fps(double fps) {
    const double clamped = std::clamp(fps, 1.0, 240.0);
    target_fps_.store(clamped, std::memory_order_release);
    force_idr_.store(true, std::memory_order_release);
}

std::string UdpSender::encoder_backend_name() const {
    std::lock_guard<std::mutex> lk(enc_meta_mu_);
    return enc_backend_name_;
}

bool UdpSender::encoder_is_hardware() const {
    std::lock_guard<std::mutex> lk(enc_meta_mu_);
    return enc_is_hw_;
}

int UdpSender::send_with_failover(const uint8_t* dgram, size_t len, QosTrafficClass qos) {
    int sent = -1;
    if (!primary_link_down_.load(std::memory_order_acquire)) {
        sent = sock_.send_to_qos(dgram, len, dst_, qos);
    }
    if (dual_nic_active_.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> nlk(nic_mu_);
        if (sock_secondary_.is_open()) {
            const int s2 = sock_secondary_.send_to_qos(dgram, len, dst_, qos);
            if (s2 == int(len)) {
                sec_pkts_sent_.fetch_add(1, std::memory_order_relaxed);
                sec_bytes_sent_.fetch_add(uint64_t(len), std::memory_order_relaxed);
                if (sent != int(len)) {
                    failover_pkts_.fetch_add(1, std::memory_order_relaxed);
                    sent = s2;
                }
            }
        }
    }
    if (extra_dst_count_.load(std::memory_order_acquire) > 0) {
        std::vector<Endpoint> extras;
        {
            std::lock_guard<std::mutex> dlk(dsts_mu_);
            extras = extra_dsts_;
        }
        for (const auto& edst : extras) {
            if (!primary_link_down_.load(std::memory_order_acquire)) {
                (void)sock_.send_to_qos(dgram, len, edst, qos);
            }
        }
    }
    return sent;
}

void UdpSender::service_control() {
    std::lock_guard<std::mutex> lk(ctl_mu_);
    service_control_locked();
}

void UdpSender::service_control_locked() {
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
            if (out_n && sock_.send_to_qos(out, out_n, peer, QosTrafficClass::kSyncControl) == int(out_n)) {
                pongs_sent_.fetch_add(1, std::memory_order_relaxed);
            }
        } else if (h.type == PacketType::kFeedback) {
            FeedbackPayload fb;
            if (!parse_feedback(payload, h.payload_size, fb)) continue;
            if (fb.request_idr) {
                force_idr_.store(true, std::memory_order_release);
                pli_received_.fetch_add(1, std::memory_order_relaxed);
            }
            {
                std::lock_guard<std::mutex> alk(aq_mu_);
                if (aq_.on_feedback(fb, now) && active_codec() != VideoCodecId::kLanVidDct) {
                    target_bitrate_kbps_.store(aq_.current_bitrate_kbps(), std::memory_order_relaxed);
                }
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
            fb.fec_recovered_permille,
            active_codec(),
            target_bitrate_kbps_.load(std::memory_order_relaxed),
            pli_received_.load(std::memory_order_relaxed)};
}

}  // namespace lv
