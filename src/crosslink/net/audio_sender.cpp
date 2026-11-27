#include "net/audio_sender.hpp"

#include <algorithm>
#include <cstring>

#include "common/network_interfaces.hpp"

namespace cl {

AudioSender::AudioSender(const AudioSenderConfig& cfg)
    : cfg_(cfg), latency_ms_(std::clamp<uint32_t>(cfg.latency_ms, 2, 2000)) {
    epoch_us_ = now_us();
    uint64_t x = epoch_us_ ^ (uint64_t(reinterpret_cast<uintptr_t>(this)) << 7) ^ 0x9E3779B97F4A7C15ull;
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    stream_epoch_ = uint16_t(x) ? uint16_t(x) : uint16_t(1);
}

bool AudioSender::init() {
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
    sock_.set_low_latency_qos();
    sock_.set_nonblocking(true);

    atx_ = std::make_unique<AudioTxEngine>(cfg_.sample_rate, cfg_.mtu, [this](const uint8_t* d, size_t n) {
        if (primary_link_down_.load(std::memory_order_acquire)) return false;
        const bool ok = sock_.send_to_qos(d, n, dst_, QosTrafficClass::kAudio) == int(n);
        if (!ok) send_errors_.fetch_add(1, std::memory_order_relaxed);
        return ok;
    });
    atx_->set_latency_ms(latency_ms_.load(std::memory_order_relaxed));
    atx_->set_codec_profile(AudioCodecProfile(audio_codec_.load(std::memory_order_acquire)));
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

    return true;
}

bool AudioSender::set_network_interfaces(const std::string& primary_ip, const std::string& secondary_ip) {
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

uint64_t AudioSender::master_samples_now() const {
    const uint64_t us = now_us() - epoch_us_;
    return (us / 1000000) * cfg_.sample_rate + (us % 1000000) * cfg_.sample_rate / 1000000;
}

uint64_t AudioSender::resolve_auto_ts(uint64_t next, uint32_t num_samples) const {
    const uint64_t now = master_samples_now();
    const uint64_t ideal = now > num_samples ? now - num_samples : 0;
    if (next == 0) return ideal;
    const int64_t drift = int64_t(ideal) - int64_t(next);
    const int64_t behind_tol = std::max<int64_t>(int64_t(cfg_.sample_rate) / 50, 3 * int64_t(num_samples));
    const int64_t ahead_tol = int64_t(cfg_.sample_rate);
    if (drift > behind_tol || drift < -ahead_tol) {
        auto_reanchors_.fetch_add(1, std::memory_order_relaxed);
        return ideal;
    }
    return next;
}

uint64_t AudioSender::next_auto_media_ts(uint32_t num_samples) const {
    return resolve_auto_ts(next_auto_ts_[0].load(std::memory_order_relaxed), num_samples);
}

int AudioSender::push_audio_channel(uint16_t channel, const void* samples, uint32_t num_samples,
                                    AudioFormat2 fmt, uint64_t media_ts) {
    if (midi_only_.load(std::memory_order_relaxed)) {
        service_control_try();
        return 0;
    }
    if (!atx_ || channel >= kMaxAudioChannels) return -1;
    service_control_try();
    auto& next = next_auto_ts_[channel];
    if (media_ts == kAutoMediaTs) media_ts = resolve_auto_ts(next.load(std::memory_order_relaxed), num_samples);
    next.store(media_ts + num_samples, std::memory_order_relaxed);
    return atx_->push_channel(channel, samples, num_samples, fmt, media_ts, now_us32());
}

void AudioSender::set_latency_ms(uint32_t ms) {
    const uint32_t clamped = std::clamp<uint32_t>(ms, 2, 2000);
    latency_ms_.store(clamped, std::memory_order_relaxed);
    if (atx_) atx_->set_latency_ms(clamped);
}

void AudioSender::flush_audio() {
    if (atx_) atx_->flush(now_us32());
}

bool AudioSender::push_midi(uint8_t status, uint8_t d1, uint8_t d2, uint8_t port, uint64_t media_ts_us) {
    if (!midi_tx_) return false;
    const uint64_t ts = media_ts_us ? media_ts_us : master_us_now();
    midi_tx_->push_event(status, d1, d2, port, ts);
    last_midi_us_.store(now_us(), std::memory_order_relaxed);
    return midi_tx_->flush(ts, now_us32());
}

int AudioSender::send_with_failover(const uint8_t* dgram, size_t len, QosTrafficClass qos) {
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
    return sent;
}

bool AudioSender::send_control_sync(const ControlSyncPayload& cs) {
    uint8_t body[kControlSyncSize];
    write_control_sync(body, cs);
    PacketHeader h;
    h.type = PacketType::kControlSync;
    h.flags = kFlagHasCrc;
    h.seq_id = uint32_t(cs.revision & 0xFFFFFFFFu);
    h.timestamp_us = now_us32();
    h.stream_epoch = stream_epoch_;
    uint8_t out[128];
    const size_t n = write_packet(out, sizeof(out), h, body, sizeof(body));
    if (!n) return false;
    int ok = 0;
    for (int rep = 0; rep < 3; ++rep) {
        if (send_with_failover(out, n, QosTrafficClass::kSyncControl) == int(n)) ++ok;
    }
    if (ok > 0) cs_tx_count_.fetch_add(1, std::memory_order_relaxed);
    return ok > 0;
}

void AudioSender::service_control() {
    std::lock_guard<std::mutex> lk(ctl_mu_);
    service_control_locked();
}

void AudioSender::service_control_locked() {
    if (atx_) atx_->service_redundancy(now_us32());
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

            uint8_t pong_body[kPingPongSize + kPongMasterTailSize];
            write_ping_pong(pong_body, pp);
            write_pong_master_tail(pong_body + kPingPongSize, now - epoch_us_, now_us() - epoch_us_);

            PacketHeader rh;
            rh.type = PacketType::kPong;
            rh.flags = kFlagHasCrc;
            rh.seq_id = h.seq_id;
            rh.timestamp_us = pp.t3_server_send_us;
            rh.stream_epoch = stream_epoch_;
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
                if (cs.revision <= cs_last_rx_rev_) continue;
                cs_last_rx_rev_ = cs.revision;
                cb = cs_cb_;
            }
            cs_rx_count_.fetch_add(1, std::memory_order_relaxed);
            if (cb) cb(cs);
        }
    }
}

AudioSenderStats AudioSender::stats() const {
    const AudioTxStats a = audio_tx_stats();
    const MidiTxStats m = midi_stats();
    return {a.packets_sent,
            a.bytes_sent,
            a.parity_sent,
            a.retransmits,
            pongs_sent_.load(std::memory_order_relaxed),
            send_errors_.load(std::memory_order_relaxed),
            m.events_pushed,
            cs_tx_count_.load(std::memory_order_relaxed),
            cs_rx_count_.load(std::memory_order_relaxed),
            failover_packets(),
            sec_bytes_sent_.load(std::memory_order_relaxed)};
}

}  // namespace cl
