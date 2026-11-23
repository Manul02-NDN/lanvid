#pragma once
// Sender-side 64-stream audio engine: chunking, interleaved XOR FEC, retransmit history,
// NACK servicing and proactive duplication. Transport-agnostic (datagrams go out through SendFn)
// so it can be driven by a real UDP socket or a virtual-time network simulator.

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "audio/audio_reliability.hpp"
#include "net/packet.hpp"

namespace lv {

enum class AudioCodecProfile : uint8_t {
    kPcm32F    = 0,  // Uncompressed 32-bit float (4 B/sample, ~98 Mbps @ 64ch 48kHz)
    kPcm24     = 1,  // Lossless 24-bit packed PCM (3 B/sample, ~73 Mbps, -25%)
    kPcm16     = 2,  // Compact 16-bit PCM (2 B/sample, ~49 Mbps, -50%)
    kOpusAdpcm = 3,  // Ultra-Light 4-bit IMA-ADPCM (~12.5 Mbps, -85%, zero external deps)
};

struct AudioTxStats {
    uint64_t packets_sent = 0;
    uint64_t bytes_sent = 0;
    uint64_t parity_sent = 0;
    uint64_t retransmits = 0;
    uint64_t duplicates = 0;
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
    // Live-switchable audio compression profile (PCM32F / PCM24 / PCM16 / Ultra-Light ADPCM).
    void set_codec_profile(AudioCodecProfile p) { codec_profile_.store(uint8_t(p), std::memory_order_release); }
    AudioCodecProfile codec_profile() const { return AudioCodecProfile(codec_profile_.load(std::memory_order_acquire)); }
    // Convenience wrapper for 24-bit packed PCM compression.
    void set_wire_pcm24(bool enabled) {
        set_codec_profile(enabled ? AudioCodecProfile::kPcm24 : AudioCodecProfile::kPcm32F);
    }
    bool wire_pcm24() const { return codec_profile() == AudioCodecProfile::kPcm24; }
    AudioReliabilityProfile profile(AudioFormat2 fmt) const;

    // Pushes samples of ONE channel (contiguous in `fmt`). `media_ts` is the master-clock position of
    // the first sample. Thread-safe per channel. Returns 0 or a negative LV_ErrorCode-style value.
    int push_channel(uint16_t channel, const void* samples, uint32_t num_samples, AudioFormat2 fmt,
                     uint64_t media_ts, uint32_t wallclock_us);

    // Sends any partially filled chunk of every channel.
    void flush(uint32_t wallclock_us);

    void on_nack(const uint8_t* payload, size_t len);

    AudioTxStats stats() const;
    uint32_t sample_rate() const { return rate_; }

private:
    struct Stream;
    void emit_data(Stream& s, uint16_t ch, const uint8_t* samples, uint32_t n, AudioFormat2 fmt,
                   uint64_t ts, uint32_t wallclock_us, const AudioReliabilityProfile& prof);
    void emit_parity(Stream& s, uint16_t ch, uint32_t col, AudioFormat2 fmt, uint32_t n,
                     uint32_t wallclock_us);
    void send_payload(uint32_t seq, uint16_t stream, uint32_t wallclock_us, const uint8_t* payload,
                      size_t len);
    void flush_stream(Stream& s, uint16_t ch, uint32_t wallclock_us);
    void reconfigure(Stream& s, const AudioReliabilityProfile& prof);
    void update_loss_estimate();

    uint32_t rate_;
    size_t mtu_;
    SendFn send_;
    std::atomic<uint32_t> latency_ms_{20};
    std::atomic<uint8_t> codec_profile_{uint8_t(AudioCodecProfile::kPcm32F)};
    std::unique_ptr<Stream> streams_[kMaxAudioChannels];
    std::mutex create_mu_;

    std::atomic<uint64_t> packets_sent_{0}, bytes_sent_{0}, parity_sent_{0}, retransmits_{0},
        duplicates_{0}, nacks_rx_{0}, nack_miss_{0};
    std::atomic<bool> dup_active_{false};
    std::atomic<uint64_t> win_sent_{0}, win_nacked_{0};
    std::atomic<uint64_t> win_start_us_{0};
};

}  // namespace lv
