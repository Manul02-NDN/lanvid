#pragma once
// CrossLink Audio UDP Sender:
// Pure 64-channel AoIP transport (PCM16/24/F32/IMA-ADPCM/Opus CELT + XOR FEC + NACK),
// MasterClock PONG synchronization, MIDI2 with 16-event redundancy ring, ControlSync,
// Wi-Fi 1+1 redundancy, and Dual-NIC 1+1 failover. Zero video code.

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "audio/audio_tx.hpp"
#include "common/clock.hpp"
#include "common/udp_socket.hpp"
#include "midi/midi_sync_engine.hpp"
#include "net/packet.hpp"

namespace cl {

struct AudioSenderConfig {
    std::string dest_ip = "127.0.0.1";
    uint16_t dest_port = 5004;
    std::string bind_ip;             // Empty / "0.0.0.0" = OS default route
    std::string secondary_bind_ip;   // Optional Secondary NIC IPv4 for Dual-NIC 1+1 failover
    uint16_t bind_port = 0;
    size_t mtu = kDefaultMtu;
    int socket_buf_bytes = 8 * 1024 * 1024;
    uint32_t sample_rate = 48000;    // 48000 / 96000
    uint32_t latency_ms = 20;        // 2..2000 ms
};

struct AudioSenderStats {
    uint64_t audio_packets_sent = 0;
    uint64_t audio_bytes_sent = 0;
    uint64_t fec_packets_sent = 0;
    uint64_t retransmits_sent = 0;
    uint64_t pongs_sent = 0;
    uint64_t send_errors = 0;
    uint64_t midi_events_sent = 0;
    uint64_t control_sync_tx = 0;
    uint64_t control_sync_rx = 0;
    uint64_t failover_packets = 0;
    uint64_t secondary_bytes_sent = 0;
};

class AudioSender {
public:
    static constexpr uint64_t kAutoMediaTs = UINT64_MAX;

    explicit AudioSender(const AudioSenderConfig& cfg);
    ~AudioSender() = default;

    bool init();
    uint16_t local_port() const { return sock_.local_port(); }

    int push_audio_channel(uint16_t channel, const void* samples, uint32_t num_samples,
                           AudioFormat2 fmt, uint64_t media_ts = kAutoMediaTs);
    uint64_t next_auto_media_ts(uint32_t num_samples) const;
    uint64_t auto_ts_reanchors() const { return auto_reanchors_.load(std::memory_order_relaxed); }
    void set_next_auto_media_ts(uint64_t ts) {
        for (auto& n : next_auto_ts_) n.store(ts, std::memory_order_relaxed);
    }

    AudioReliabilityProfile audio_profile() const {
        return atx_ ? atx_->profile(AudioFormat2::kPcm16)
                    : audio_profile_for_latency(cfg_.latency_ms, cfg_.sample_rate, cfg_.mtu, AudioFormat2::kPcm16);
    }

    void set_latency_ms(uint32_t ms);
    uint32_t latency_ms() const { return atx_ ? atx_->latency_ms() : latency_ms_.load(std::memory_order_relaxed); }

    void set_wifi_redundancy(bool enabled, uint32_t stagger_us = 1500) {
        wifi_redundancy_.store(enabled, std::memory_order_release);
        if (atx_) atx_->set_wifi_redundancy(enabled, stagger_us);
    }
    bool wifi_redundancy() const { return wifi_redundancy_.load(std::memory_order_acquire); }

    bool set_network_interfaces(const std::string& primary_ip, const std::string& secondary_ip);
    bool dual_nic_active() const { return dual_nic_active_.load(std::memory_order_acquire); }
    uint64_t failover_packets() const {
        return failover_pkts_.load(std::memory_order_relaxed) +
               (atx_ ? atx_->stats().failover_packets : 0ull);
    }
    uint64_t secondary_packets_sent() const { return sec_pkts_sent_.load(std::memory_order_relaxed); }
    uint64_t secondary_bytes_sent() const { return sec_bytes_sent_.load(std::memory_order_relaxed); }
    void set_primary_link_down(bool down) { primary_link_down_.store(down, std::memory_order_release); }
    bool primary_link_down() const { return primary_link_down_.load(std::memory_order_acquire); }

    void set_audio_codec_profile(AudioCodecProfile p) {
        audio_codec_.store(uint8_t(p), std::memory_order_relaxed);
        if (atx_) atx_->set_codec_profile(p);
    }
    void set_audio_bitrate_kbps(int kbps_per_ch) {
        set_audio_codec_profile(audio_codec_profile_from_bitrate(kbps_per_ch));
    }
    AudioCodecProfile audio_codec_profile() const {
        return atx_ ? atx_->codec_profile() : AudioCodecProfile(audio_codec_.load(std::memory_order_relaxed));
    }

    void flush_audio();
    uint64_t master_samples_now() const;
    uint64_t master_us_now() const { return now_us() - epoch_us_; }
    AudioTxStats audio_tx_stats() const { return atx_ ? atx_->stats() : AudioTxStats{}; }

    // MIDI2 over the same UDP stream
    bool push_midi(uint8_t status, uint8_t d1, uint8_t d2, uint8_t port = 0, uint64_t media_ts_us = 0);
    MidiTxStats midi_stats() const { return midi_tx_ ? midi_tx_->stats() : MidiTxStats{}; }
    void set_midi_only(bool enabled) { midi_only_.store(enabled, std::memory_order_relaxed); }
    bool midi_only() const { return midi_only_.load(std::memory_order_relaxed); }

    // Bidirectional Control Sync (kControlSync, 3x redundant)
    using ControlSyncCallback = std::function<void(const ControlSyncPayload&)>;
    bool send_control_sync(const ControlSyncPayload& cs);
    void set_control_sync_callback(ControlSyncCallback cb) {
        std::lock_guard<std::mutex> lk(cs_mu_);
        cs_cb_ = std::move(cb);
    }
    uint64_t control_sync_tx_count() const { return cs_tx_count_.load(std::memory_order_relaxed); }
    uint64_t control_sync_rx_count() const { return cs_rx_count_.load(std::memory_order_relaxed); }

    uint16_t stream_epoch() const { return stream_epoch_; }
    const UdpSocket& socket() const { return sock_; }

    void service_control();
    void service_control_try() {
        std::unique_lock<std::mutex> lk(ctl_mu_, std::try_to_lock);
        if (lk.owns_lock()) service_control_locked();
    }

    AudioSenderStats stats() const;

private:
    void service_control_locked();
    int send_with_failover(const uint8_t* dgram, size_t len, QosTrafficClass qos);
    uint64_t resolve_auto_ts(uint64_t next, uint32_t num_samples) const;

    AudioSenderConfig cfg_;
    uint64_t epoch_us_ = 0;
    uint16_t stream_epoch_ = 1;
    std::unique_ptr<AudioTxEngine> atx_;
    std::unique_ptr<MidiTxEngine> midi_tx_;
    std::atomic<uint64_t> last_midi_us_{0};
    std::atomic<uint64_t> last_midi_hb_us_{0};

    mutable std::mutex cs_mu_;
    ControlSyncCallback cs_cb_;
    uint64_t cs_last_rx_rev_ = 0;
    std::atomic<uint64_t> cs_tx_count_{0};
    std::atomic<uint64_t> cs_rx_count_{0};

    std::mutex ctl_mu_;
    std::atomic<uint8_t> audio_codec_{uint8_t(AudioCodecProfile::kPcm32F)};
    std::atomic<uint32_t> latency_ms_{20};
    std::atomic<uint64_t> next_auto_ts_[kMaxAudioChannels]{};
    mutable std::atomic<uint64_t> auto_reanchors_{0};

    Endpoint dst_{};
    UdpSocket sock_;
    UdpSocket sock_secondary_;
    mutable std::mutex nic_mu_;
    std::string primary_nic_ip_;
    std::string secondary_nic_ip_;
    std::atomic<bool> dual_nic_active_{false};
    std::atomic<bool> primary_link_down_{false};
    std::atomic<uint64_t> sec_pkts_sent_{0};
    std::atomic<uint64_t> sec_bytes_sent_{0};
    std::atomic<uint64_t> failover_pkts_{0};
    std::atomic<uint64_t> pongs_sent_{0};
    std::atomic<uint64_t> send_errors_{0};
    std::atomic<bool> midi_only_{false};
    std::atomic<bool> wifi_redundancy_{false};
};

}  // namespace cl
