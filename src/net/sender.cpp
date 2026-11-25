#include "net/sender.hpp"

#include <algorithm>
#include <cstring>
#include <thread>
#include <vector>

#include "codec/dct.hpp"
#include "common/clock.hpp"
#include "net/network_interfaces.hpp"
#include "net/packet.hpp"

namespace lv {

UdpSender::UdpSender(const SenderConfig& cfg)
    : cfg_(cfg),
      pool_(cfg.threads),
      target_bitrate_kbps_(cfg.bitrate_kbps > 0 ? cfg.bitrate_kbps : 8000u),
      enc_backend_name_(std::string("LanVid DCT (") + dct_kernels_best().name + ")"),
      aq_(cfg.adaptive),
      fec_group_size_(cfg.fec_group_size),
      sim_loss_pct_(cfg.sim_loss_pct) {
    epoch_us_ = now_us();
    {
        // Non-zero 16-bit session epoch: differs between sender instances / restarts.
        uint64_t x = epoch_us_ ^ (uint64_t(reinterpret_cast<uintptr_t>(this)) << 7) ^ 0x9E3779B97F4A7C15ull;
        x ^= x >> 33;
        x *= 0xff51afd7ed558ccdull;
        x ^= x >> 33;
        stream_epoch_ = uint16_t(x) ? uint16_t(x) : uint16_t(1);
    }
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
        // Fallback to OS default route if configured NIC IP disappeared
        if (!primary_nic_ip_.empty() && primary_nic_ip_ != "0.0.0.0") {
            if (!sock_.bind("0.0.0.0", cfg_.bind_port)) return false;
        } else {
            return false;
        }
    }
    sock_.set_buffer_sizes(std::max(cfg_.socket_buf_bytes, UdpSocket::kMinSocketBuffer), UdpSocket::kMinSocketBuffer);
    sock_.set_low_latency_qos();  // DSCP EF (Wi-Fi WMM AC_VO) for audio / MIDI / clock sync
    sock_.set_nonblocking(true);

    atx_ = std::make_unique<AudioTxEngine>(cfg_.audio_sample_rate, cfg_.mtu, [this](const uint8_t* d, size_t n) {
        if (primary_link_down_.load(std::memory_order_acquire)) return false;
        return sock_.send_to_qos(d, n, dst_, QosTrafficClass::kAudio) == int(n);
    });
    atx_->set_latency_ms(cfg_.audio_latency_ms);
    atx_->set_codec_profile(audio_codec_);
    atx_->set_epoch(stream_epoch_);
    atx_->set_wifi_redundancy(wifi_redundancy_.load(std::memory_order_acquire));
    midi_tx_ = std::make_unique<MidiTxEngine>(
        [this](const uint8_t* d, size_t n) {
            const bool pri_ok = !primary_link_down_.load(std::memory_order_acquire) &&
                                sock_.send_to_qos(d, n, dst_, QosTrafficClass::kSyncControl) == int(n);
            if (dual_nic_active_.load(std::memory_order_acquire)) {
                std::lock_guard<std::mutex> lk(nic_mu_);
                if (sock_secondary_.is_open() &&
                    sock_secondary_.send_to_qos(d, n, dst_, QosTrafficClass::kSyncControl) == int(n)) {
                    sec_pkts_sent_.fetch_add(1, std::memory_order_relaxed);
                    sec_bytes_sent_.fetch_add(n, std::memory_order_relaxed);
                    if (!pri_ok) {
                        failover_pkts_.fetch_add(1, std::memory_order_relaxed);
                        return true;
                    }
                }
            }
            return pri_ok;
        },
        stream_epoch_);

    if (!secondary_nic_ip_.empty() && secondary_nic_ip_ != "0.0.0.0") {
        set_network_interfaces(primary_nic_ip_, secondary_nic_ip_);
    }

    last_pace_us_ = now_us();
    tokens_bytes_ = 64.0 * 1024.0;
    return true;
}

bool UdpSender::set_network_interfaces(const std::string& primary_ip, const std::string& secondary_ip) {
    std::lock_guard<std::mutex> lk(nic_mu_);
    primary_nic_ip_ = (primary_ip == "auto" || primary_ip == "0.0.0.0") ? std::string() : primary_ip;
    secondary_nic_ip_ = (secondary_ip == "auto" || secondary_ip == "0.0.0.0") ? std::string() : secondary_ip;
    cfg_.bind_ip = primary_nic_ip_;
    cfg_.secondary_bind_ip = secondary_nic_ip_;

    sock_secondary_.close();
    dual_nic_active_.store(false, std::memory_order_release);

    if (atx_) {
        atx_->set_network_interfaces(primary_nic_ip_, secondary_nic_ip_);
        atx_->set_secondary_send(nullptr);
    }

    if (!secondary_nic_ip_.empty()) {
        if (sock_secondary_.open()) {
            NetworkInterfaceInfo info{};
            const bool found = find_interface_by_ip(secondary_nic_ip_, info);
            if (sock_secondary_.bind_to_interface(secondary_nic_ip_, 0, found ? info.if_index : 0u,
                                                  found ? info.system_name : "")) {
                sock_secondary_.set_buffer_sizes(std::max(cfg_.socket_buf_bytes / 2, UdpSocket::kMinSocketBuffer),
                                                 UdpSocket::kMinSocketBuffer);
                sock_secondary_.set_low_latency_qos();
                sock_secondary_.set_nonblocking(true);
                dual_nic_active_.store(true, std::memory_order_release);
                if (atx_) {
                    atx_->set_secondary_send([this](const uint8_t* d, size_t n) {
                        std::lock_guard<std::mutex> slk(nic_mu_);
                        if (!sock_secondary_.is_open()) return false;
                        const bool ok = sock_secondary_.send_to_qos(d, n, dst_, QosTrafficClass::kAudio) == int(n);
                        if (ok) {
                            sec_pkts_sent_.fetch_add(1, std::memory_order_relaxed);
                            sec_bytes_sent_.fetch_add(n, std::memory_order_relaxed);
                        }
                        return ok;
                    });
                }
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
        // Collect this slice's packets and emit in one batch (uses sendmmsg on Linux).
        constexpr int kMaxBatch = 32;
        uint8_t batch_storage[kMaxBatch][1500];
        DatagramView views[kMaxBatch];
        int count = 0;

        auto flush_batch = [&]() {
            if (count == 0) return;
            // Video traffic tagged DSCP AF41 (WMM AC_VI) so camera/screen bursts never delay EF/CS7 audio & sync!
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
                            const int sent = sock_.send_to_qos(dgram, len, dst_, QosTrafficClass::kVideo);
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
    if (cfg_.codec == VideoCodecId::kLanVidDct) return true;
    return ensure_video_encoder(width, height);
}

bool UdpSender::ensure_video_encoder(int width, int height) {
    if (video_enc_ && enc_w_ == width && enc_h_ == height) return true;
    VideoEncoderConfig ecfg;
    ecfg.width = width;
    ecfg.height = height;
    ecfg.fps = cfg_.fps > 1.0 ? cfg_.fps : 60.0;
    ecfg.bitrate_kbps = target_bitrate_kbps_.load(std::memory_order_relaxed);
    ecfg.keyframe_interval = cfg_.keyframe_interval_frames > 0 ? cfg_.keyframe_interval_frames : 120u;
    ecfg.prefer_hardware = true;

    video_enc_ = create_video_encoder(cfg_.codec, ecfg);
    if (!video_enc_) return false;
    enc_w_ = width;
    enc_h_ = height;
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
    if (cfg_.codec != VideoCodecId::kLanVidDct) {
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
            uint8_t(encode_codec_flags(cfg_.codec) |
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
    if (cfg_.codec != VideoCodecId::kLanVidDct) {
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
            uint8_t(encode_codec_flags(cfg_.codec) |
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

bool UdpSender::send_audio_raw(uint32_t audio_seq_id, uint32_t timestamp_us, const void* samples,
                               uint16_t num_frames, uint32_t sample_rate, uint8_t channels,
                               AudioSampleFormat fmt) {
    if (midi_only_.load(std::memory_order_relaxed)) return true;
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
    const int sent = sock_.send_to_qos(dgram, n, dst_, QosTrafficClass::kAudio);
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
    const uint32_t clamped = std::clamp<uint32_t>(bitrate_kbps, 250u, 100000u);
    target_bitrate_kbps_.store(clamped, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(aq_mu_);
    aq_.set_target_bitrate_kbps(clamped);
}

std::string UdpSender::encoder_backend_name() const {
    std::lock_guard<std::mutex> lk(enc_meta_mu_);
    return enc_backend_name_;
}

bool UdpSender::encoder_is_hardware() const {
    std::lock_guard<std::mutex> lk(enc_meta_mu_);
    return enc_is_hw_;
}

uint64_t UdpSender::master_samples_now() const {
    const uint64_t us = now_us() - epoch_us_;
    return (us / 1000000) * cfg_.audio_sample_rate + (us % 1000000) * cfg_.audio_sample_rate / 1000000;
}

uint64_t UdpSender::resolve_auto_ts(uint64_t next, uint32_t num_samples) const {
    const uint64_t now = master_samples_now();
    const uint64_t ideal = now > num_samples ? now - num_samples : 0;  // block just captured ends "now"
    if (next == 0) return ideal;                                       // anchor on first push
    const int64_t drift = int64_t(ideal) - int64_t(next);              // > 0: producer behind the clock
    // Behind: re-anchor after max(20 ms, 3 blocks). Ahead (pushing faster than real time, e.g. a file
    // pre-buffer) is legal up to 1 s - the receiver ring holds 2.5 s - and only then re-anchored.
    const int64_t behind_tol = std::max<int64_t>(int64_t(cfg_.audio_sample_rate) / 50, 3 * int64_t(num_samples));
    const int64_t ahead_tol = int64_t(cfg_.audio_sample_rate);
    if (drift > behind_tol || drift < -ahead_tol) {
        auto_reanchors_.fetch_add(1, std::memory_order_relaxed);
        return ideal;
    }
    return next;
}

uint64_t UdpSender::next_auto_media_ts(uint32_t num_samples) const {
    return resolve_auto_ts(next_auto_ts_[0].load(std::memory_order_relaxed), num_samples);
}

int UdpSender::push_audio_channel(uint16_t channel, const void* samples, uint32_t num_samples,
                                  AudioFormat2 fmt, uint64_t media_ts) {
    if (midi_only_.load(std::memory_order_relaxed)) {
        std::unique_lock<std::mutex> lk(ctl_mu_, std::try_to_lock);
        if (lk.owns_lock()) service_control();
        return 0;
    }
    if (!atx_ || channel >= kMaxAudioChannels) return -1;
    {   // answer PING / NACK from whichever pushing thread gets here first
        std::unique_lock<std::mutex> lk(ctl_mu_, std::try_to_lock);
        if (lk.owns_lock()) service_control();
    }
    auto& next = next_auto_ts_[channel];
    if (media_ts == kAutoMediaTs) media_ts = resolve_auto_ts(next.load(std::memory_order_relaxed), num_samples);
    next.store(media_ts + num_samples, std::memory_order_relaxed);
    return atx_->push_channel(channel, samples, num_samples, fmt, media_ts, now_us32());
}

void UdpSender::set_audio_latency_ms(uint32_t ms) {
    cfg_.audio_latency_ms = std::clamp<uint32_t>(ms, 2, 2000);
    if (atx_) atx_->set_latency_ms(ms);
}

void UdpSender::flush_audio() {
    if (atx_) atx_->flush(now_us32());
}

bool UdpSender::push_midi(uint8_t status, uint8_t d1, uint8_t d2, uint8_t port, uint64_t media_ts_us) {
    if (!midi_tx_) return false;
    const uint64_t ts = media_ts_us ? media_ts_us : master_us_now();
    midi_tx_->push_event(status, d1, d2, port, ts);
    last_midi_us_.store(now_us(), std::memory_order_relaxed);
    return midi_tx_->flush(ts, now_us32());
}

bool UdpSender::send_control_sync(const ControlSyncPayload& cs) {
    uint8_t body[kControlSyncSize];
    write_control_sync(body, cs);
    PacketHeader h;
    h.type = PacketType::kControlSync;
    h.flags = kFlagHasCrc;
    h.frame_id = uint32_t(cs.revision & 0xFFFFFFFFu);
    h.timestamp_us = now_us32();
    uint8_t out[128];
    const size_t n = write_packet(out, sizeof(out), h, body, sizeof(body));
    if (!n) return false;
    int ok = 0;
    for (int rep = 0; rep < 3; ++rep) {
        if (sock_.send_to_qos(out, n, dst_, QosTrafficClass::kSyncControl) == int(n)) ++ok;
    }
    if (ok > 0) cs_tx_count_.fetch_add(1, std::memory_order_relaxed);
    return ok > 0;
}

void UdpSender::service_control() {
    if (atx_) atx_->service_redundancy(now_us32());
    // MIDI2 heartbeat: re-send recent events every 100 ms for 2 s after the last one (stuck-note guard).
    if (midi_tx_) {
        const uint64_t t = now_us();
        const uint64_t last = last_midi_us_.load(std::memory_order_relaxed);
        if (last && t - last < 2'000'000 && t - last_midi_hb_us_.load(std::memory_order_relaxed) >= 100'000) {
            last_midi_hb_us_.store(t, std::memory_order_relaxed);
            midi_tx_->flush(master_us_now(), now_us32(), true);
        }
    }
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
            // Extended PONG: sender master-clock t2/t3 (64-bit us since epoch) for MasterClock sync.
            uint8_t pong_body[kPingPongSize + kPongMasterTailSize];
            std::memcpy(pong_body, pp_bytes, sizeof(pp_bytes));
            write_pong_master_tail(pong_body + kPingPongSize, now - epoch_us_, now_us() - epoch_us_);
            uint8_t out[96];
            const size_t out_n = write_packet(out, sizeof(out), rh, pong_body, sizeof(pong_body));
            if (out_n && sock_.send_to_qos(out, out_n, peer, QosTrafficClass::kSyncControl) == int(out_n)) {
                pongs_sent_.fetch_add(1, std::memory_order_relaxed);
            }
        } else if (h.type == PacketType::kAudioNack) {
            if (atx_) atx_->on_nack(payload, h.payload_size);
        } else if (h.type == PacketType::kControlSync) {
            ControlSyncPayload cs{};
            if (!parse_control_sync(payload, h.payload_size, cs)) continue;
            ControlSyncCallback cb;
            {
                std::lock_guard<std::mutex> clk(cs_mu_);
                if (cs.revision <= cs_last_rx_rev_) continue;  // dedup / anti-loop
                cs_last_rx_rev_ = cs.revision;
                cb = cs_cb_;
            }
            cs_rx_count_.fetch_add(1, std::memory_order_relaxed);
            if (cb) cb(cs);
        } else if (h.type == PacketType::kFeedback) {
            FeedbackPayload fb;
            if (!parse_feedback(payload, h.payload_size, fb)) continue;
            if (fb.request_idr) {
                force_idr_.store(true, std::memory_order_release);
                pli_received_.fetch_add(1, std::memory_order_relaxed);
            }
            {
                std::lock_guard<std::mutex> alk(aq_mu_);
                if (aq_.on_feedback(fb, now) && cfg_.codec != VideoCodecId::kLanVidDct) {
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
            cfg_.codec,
            target_bitrate_kbps_.load(std::memory_order_relaxed),
            pli_received_.load(std::memory_order_relaxed)};
}

}  // namespace lv
