#pragma once
// Host side of CrossLinkAudioBus_v1: a (non-real-time) pump thread that moves audio between the
// shared-memory bus and the network stack.
//
//   TX: driver/DAW output ring -> PushFn (e.g. lv_sender_push_audio_multichannel)
//   RX: PopFn (e.g. lv_receiver_pop_audio_multichannel) -> driver/DAW input ring
//
// RX is filled up to a target level, so the network playout is pulled exactly at the DAW device
// rate (the AudioSyncMatrix resampler then absorbs master-vs-device clock drift).
// TX is re-anchored to the master clock (PushFn `discontinuity`) when the DAW restarts, really
// stops (> kStallMs without data), or its device clock stays more than kDriftMs away from real
// time for longer than kDriftHoldMs. Short scheduling hiccups are absorbed (an absolute-clock
// driver catches up by itself), because re-anchoring on them would punch holes in the stream.

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "audio/shm_audio_bridge.hpp"

namespace cl {

struct ShmHostStatus {
    bool bus_open = false;
    bool driver_connected = false;
    uint32_t sample_rate = 0;
    uint32_t buffer_size = 0;
    uint32_t tx_fill = 0;  // frames waiting in TX
    uint32_t rx_fill = 0;  // frames buffered in RX
    uint64_t tx_frames = 0;
    uint64_t rx_frames = 0;
    uint64_t tx_overruns = 0;
    uint64_t rx_underruns = 0;
    uint64_t reanchors = 0;
    std::string driver_name;
};

class ShmAudioHost {
public:
    using PushFn = std::function<void(const float* const* planes, uint32_t channels, uint32_t frames,
                                      bool discontinuity)>;
    using PopFn = std::function<void(float* const* planes, uint32_t channels, uint32_t frames)>;

    struct Config {
        uint32_t channels = 64;
        uint32_t block_frames = 128;
        uint32_t rx_target_frames = 0;  // 0 = auto: max(3 x driver buffer, 512)
        std::string bus_name;           // empty = kShmBusName
    };

    static constexpr uint32_t kStallMs = 250;
    static constexpr uint32_t kDriftMs = 50;
    static constexpr uint32_t kDriftHoldMs = 500;

    ShmAudioHost() = default;
    ~ShmAudioHost() { stop(); }

    // Either callback may be empty (sender-only or receiver-only host).
    bool start(const Config& cfg, PushFn tx, PopFn rx);
    void stop();
    bool running() const { return running_.load(std::memory_order_acquire); }

    void publish_network_state(uint32_t latency_ms, bool clock_synced, double skew_ppm);
    ShmHostStatus status() const;

private:
    void loop();

    Config cfg_;
    PushFn tx_;
    PopFn rx_;
    mutable std::mutex bus_mu_;
    ShmAudioBus bus_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> tx_frames_{0}, rx_frames_{0}, reanchors_{0};
};

}  // namespace cl
