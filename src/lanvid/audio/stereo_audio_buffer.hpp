#pragma once
// Lock-free SPSC stereo PCM16 audio ring buffer with 10-15 ms target jitter buffering
// and adaptive 1-sample drop/duplicate clock-drift compensation.

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "net/packet.hpp"

namespace lv {

struct AudioStats {
    uint64_t packets_rx = 0;
    uint64_t packets_lost = 0;
    uint64_t bytes_rx = 0;
    uint64_t underruns = 0;
    uint64_t overruns = 0;
    uint64_t drift_drops = 0;
    uint64_t drift_dups = 0;
    double buffered_ms = 0.0;
};

class AudioJitterBuffer {
public:
    static constexpr uint32_t kDefaultSampleRate = 48000;
    static constexpr size_t kCapacityFrames = 4096;  // power of 2 (~85.3 ms at 48 kHz)
    static constexpr size_t kCapacityMask = kCapacityFrames - 1;

    explicit AudioJitterBuffer(uint32_t sample_rate = kDefaultSampleRate, double target_ms = 12.0);

    void reset();

    // Producer (UDP recv thread): pushes a decoded PacketType::kAudio payload (PCM16 or F32).
    void push_packet(uint32_t audio_seq_id, const AudioHeader& hdr, const uint8_t* pcm_data,
                     size_t dgram_bytes);

    // Producer helper for unit tests: pushes raw interleaved stereo PCM16 frames directly.
    void push_pcm16(const int16_t* stereo_frames, size_t num_frames);

    // Consumer (audio playback thread): fills `out_stereo` with `num_frames` stereo PCM16 frames.
    // Performs pre-buffering, smooth fade-out on underrun, and 1-sample drift drop/dup.
    void pop_pcm16(int16_t* out_stereo, size_t num_frames);

    // Output gain 0.0 (mute) .. 2.0 (boost); thread-safe (called from UI thread).
    void set_volume(float vol);
    float volume() const { return volume_.load(std::memory_order_relaxed); }

    // Runtime target buffering delay (5..60 ms) for stereo audio playout; thread-safe.
    void set_target_ms(double target_ms);

    uint32_t sample_rate() const { return sample_rate_; }
    size_t available_frames() const;
    double buffered_ms() const;
    AudioStats stats() const;

private:
    uint32_t sample_rate_ = kDefaultSampleRate;
    std::atomic<size_t> target_frames_{576};  // 12 ms at 48 kHz
    std::atomic<size_t> max_frames_{1680};    // 35 ms cap before trimming excess latency
    size_t drift_band_ = 144;                 // +/- 3 ms deadband before 1-sample drift adjustment

    alignas(64) std::atomic<uint64_t> write_pos_{0};
    alignas(64) std::atomic<uint64_t> read_pos_{0};
    int16_t ring_[kCapacityFrames * 2]{};  // interleaved L, R

    // Producer-only sequence tracking
    bool has_seq_ = false;
    uint32_t expected_seq_ = 0;

    // Consumer-only playback & drift state
    bool prebuffering_ = true;
    double avg_fill_ = 0.0;
    int16_t last_l_ = 0;
    int16_t last_r_ = 0;
    uint32_t drift_cooldown_ = 0;

    // Telemetry counters
    std::atomic<uint64_t> packets_rx_{0};
    std::atomic<uint64_t> packets_lost_{0};
    std::atomic<uint64_t> bytes_rx_{0};
    std::atomic<uint64_t> underruns_{0};
    std::atomic<uint64_t> overruns_{0};
    std::atomic<uint64_t> drift_drops_{0};
    std::atomic<uint64_t> drift_dups_{0};
    std::atomic<float> volume_{1.0f};
};

}  // namespace lv
