#pragma once
// Sender-side 64-stream audio engine: chunking, interleaved XOR FEC, retransmit history,
// NACK servicing and proactive duplication. Transport-agnostic (datagrams go out through SendFn)
// so it can be driven by a real UDP socket or a virtual-time network simulator.

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "audio/audio_reliability.hpp"
#include "net/packet.hpp"

namespace cl {

enum class AudioCodecProfile : uint8_t {
    kPcm32F     = 0,  // Uncompressed 32-bit float (Lossless)
    kPcm24      = 1,  // Lossless 24-bit packed PCM (-25%)
    kPcm16      = 2,  // Compact 16-bit PCM (-50%)
    kOpusAdpcm  = 3,  // 192 kbps/ch 4-bit IMA-ADPCM @ 48 kHz
    kBitrate256 = 4,  // 256 kbps/ch 5-bit IMA-ADPCM @ 48 kHz
    kBitrate192 = 5,  // 192 kbps/ch 4-bit IMA-ADPCM @ 48 kHz
    kBitrate128 = 6,  // 128 kbps/ch 5-bit IMA-ADPCM @ 24 kHz
    kBitrate96  = 7,  // 96 kbps/ch 4-bit IMA-ADPCM @ 24 kHz
    kBitrate64  = 8,  // 64 kbps/ch 4-bit IMA-ADPCM @ 16 kHz
};

inline AudioCodecProfile audio_codec_profile_from_bitrate(int kbps_per_ch) {
    if (kbps_per_ch <= 0) return AudioCodecProfile::kPcm32F;
    if (kbps_per_ch >= 224) return AudioCodecProfile::kBitrate256;
    if (kbps_per_ch >= 160) return AudioCodecProfile::kBitrate192;
    if (kbps_per_ch >= 112) return AudioCodecProfile::kBitrate128;
    if (kbps_per_ch >= 80) return AudioCodecProfile::kBitrate96;
    return AudioCodecProfile::kBitrate64;
}

struct AudioTxStats {
    uint64_t packets_sent = 0;
    uint64_t bytes_sent = 0;
    uint64_t parity_sent = 0;
    uint64_t retransmits = 0;
    uint64_t duplicates = 0;
    uint64_t secondary_packets = 0;
    uint64_t failover_packets = 0;
    uint64_t nacks_received = 0;
    uint64_t nack_misses = 0;  // requested packet already left the history
    uint32_t active_streams = 0;
    AudioReliabilityMode mode = AudioReliabilityMode::kBalanced;
};

class AudioTxEngine {
public:
    using SendFn = std::function<bool(const uint8_t* dgram, size_t len)>;

    AudioTxEngine(uint32_t sample_rate, size_t mtu, SendFn send);
    ~AudioTxEngine();

    void set_latency_ms(uint32_t ms);
    uint32_t latency_ms() const { return latency_ms_.load(std::memory_order_relaxed); }
    // Sender session epoch carried in PacketHeader::stream_epoch of every kAudioFrame packet (0 = none).
    // A receiver that sees it change resets FEC / dedup / jitter state and re-syncs the MasterClock.
    void set_epoch(uint16_t epoch) { epoch_.store(epoch, std::memory_order_relaxed); }
    uint16_t epoch() const { return epoch_.load(std::memory_order_relaxed); }
    // Live-switchable audio compression profile or target bitrate per channel (0=Lossless, 256/192/128/96/64 kbps).
    void set_codec_profile(AudioCodecProfile p) { codec_profile_.store(uint8_t(p), std::memory_order_release); }
    void set_bitrate_kbps_per_ch(int kbps) { set_codec_profile(audio_codec_profile_from_bitrate(kbps)); }
    AudioCodecProfile codec_profile() const { return AudioCodecProfile(codec_profile_.load(std::memory_order_acquire)); }
    // Convenience wrapper for 24-bit packed PCM compression.
    void set_wire_pcm24(bool enabled) {
        set_codec_profile(enabled ? AudioCodecProfile::kPcm24 : AudioCodecProfile::kPcm32F);
    }
    bool wire_pcm24() const { return codec_profile() == AudioCodecProfile::kPcm24; }
    AudioReliabilityProfile profile(AudioFormat2 fmt) const;

    // Glitch-Free Wi-Fi Redundancy (1+1 Duplicate, Dante-inspired):
    // Proactively sends an identical backup packet staggered by `stagger_us` (default 1.5 ms)
    // — or onto an optional secondary subnet/interface (`set_secondary_send`) — eliminating NACK RTT
    // stalls on isolated Wi-Fi packet drops. Receiver deduplicates instantly by `seq`.
    void set_wifi_redundancy(bool enabled, uint32_t stagger_us = 1500);
    bool wifi_redundancy() const { return wifi_redundancy_.load(std::memory_order_acquire); }
    uint32_t wifi_redundancy_stagger_us() const { return dup_stagger_us_.load(std::memory_order_relaxed); }
    void set_secondary_send(SendFn secondary_send);
    bool has_secondary_send() const { return has_secondary_send_.load(std::memory_order_acquire); }
    void set_network_interfaces(const std::string& primary_ip, const std::string& secondary_ip);
    std::string primary_nic_ip() const;
    std::string secondary_nic_ip() const;
    // Flushes any queued 1+1 staggered duplicates whose due timestamp <= `now_wallclock_us`.
    void service_redundancy(uint32_t now_wallclock_us);

    // Pushes samples of ONE channel (contiguous in `fmt`). `media_ts` is the master-clock position of
    // the first sample. Thread-safe per channel. Returns 0 or a negative CL_ErrorCode-style value.
    int push_channel(uint16_t channel, const void* samples, uint32_t num_samples, AudioFormat2 fmt,
                     uint64_t media_ts, uint32_t wallclock_us);

    // Sends any partially filled chunk of every channel.
    void flush(uint32_t wallclock_us);

    void on_nack(const uint8_t* payload, size_t len);

    AudioTxStats stats() const;
    uint32_t sample_rate() const { return rate_; }

private:
    struct Stream;
    struct PendingDup {
        uint32_t due_wc_us = 0;
        std::vector<uint8_t> dgram;
    };
    void emit_data(Stream& s, uint16_t ch, const uint8_t* samples, uint32_t n, AudioFormat2 fmt,
                   uint64_t ts, uint32_t wallclock_us, const AudioReliabilityProfile& prof);
    void emit_parity(Stream& s, uint16_t ch, uint32_t col, AudioFormat2 fmt, uint32_t n,
                     uint32_t wallclock_us);
    bool send_payload(uint32_t seq, uint16_t stream, uint32_t wallclock_us, const uint8_t* payload,
                      size_t len);
    void queue_staggered_duplicate(uint32_t seq, uint16_t stream, uint32_t wallclock_us,
                                   const uint8_t* payload, size_t len, bool force_immediate = false,
                                   bool is_failover = false);
    void flush_stream(Stream& s, uint16_t ch, uint32_t wallclock_us);
    void reconfigure(Stream& s, const AudioReliabilityProfile& prof);
    void update_loss_estimate();

    uint32_t rate_;
    size_t mtu_;
    SendFn send_;
    mutable std::mutex dup_mu_;
    SendFn secondary_send_;
    std::atomic<bool> has_secondary_send_{false};
    std::string primary_nic_ip_;
    std::string secondary_nic_ip_;
    std::deque<PendingDup> pending_dups_;
    std::atomic<bool> wifi_redundancy_{false};
    std::atomic<uint32_t> dup_stagger_us_{1500};  // 1.5 ms default time diversity
    std::atomic<uint32_t> latency_ms_{20};
    std::atomic<uint16_t> epoch_{0};
    std::atomic<uint8_t> codec_profile_{uint8_t(AudioCodecProfile::kPcm32F)};
    std::unique_ptr<Stream> streams_[kMaxAudioChannels];
    std::atomic<Stream*> stream_ptrs_[kMaxAudioChannels]{};
    std::mutex create_mu_;

    std::atomic<uint64_t> packets_sent_{0}, bytes_sent_{0}, parity_sent_{0}, retransmits_{0},
        duplicates_{0}, secondary_pkts_{0}, failover_pkts_{0}, nacks_rx_{0}, nack_miss_{0};
    std::atomic<bool> dup_active_{false};
    std::atomic<uint64_t> win_sent_{0}, win_nacked_{0};
    std::atomic<uint64_t> win_start_us_{0};
};

}  // namespace cl
