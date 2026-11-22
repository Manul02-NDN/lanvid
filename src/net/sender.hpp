#pragma once
// High-throughput UDP streaming sender: encodes slices in parallel on ThreadPool, supports
// slice-level XOR FEC, batch UDP send, low-latency audio packets, pacing, PING/PONG, and AIMD.

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

#include "codec/frame_codec.hpp"
#include "common/image.hpp"
#include "net/adaptive_quality.hpp"
#include "net/packetizer.hpp"
#include "net/udp_socket.hpp"
#include "parallel/thread_pool.hpp"

namespace lv {

struct SenderConfig {
    std::string dest_ip = "127.0.0.1";
    uint16_t dest_port = 5000;
    uint16_t bind_port = 0;
    size_t mtu = kDefaultMtu;
    bool enable_crc = true;
    uint16_t fec_group_size = 0;  // 0 = disabled; e.g. 6 or 8 = 1 XOR parity per group
    double sim_loss_pct = 0.0;    // 0..100: simulated random packet drop
    double pace_mbps = 0.0;       // 0 = unpaced; >0 = token-bucket bitrate cap in Mbps
    int socket_buf_bytes = 8 * 1024 * 1024;
    uint32_t threads = 0;         // 0 = hardware concurrency
    AdaptiveQualityConfig adaptive{};
};

struct SenderStats {
    uint64_t frames_sent = 0;
    uint64_t slices_sent = 0;
    uint64_t packets_sent = 0;
    uint64_t packets_dropped_sim = 0;
    uint64_t send_errors = 0;
    uint64_t bytes_sent = 0;
    uint64_t audio_packets_sent = 0;
    uint64_t audio_bytes_sent = 0;
    uint64_t pongs_sent = 0;
    uint64_t feedbacks_received = 0;
    int active_quality = 75;
    uint16_t last_rx_loss_permille = 0;
    uint16_t last_rx_fec_permille = 0;
};

class UdpSender {
public:
    explicit UdpSender(const SenderConfig& cfg);
    ~UdpSender() = default;

    bool init();
    uint16_t local_port() const { return sock_.local_port(); }

    // Encodes `img` and transmits all slices over UDP. Returns false on invalid codec params.
    bool send_frame(const Image& img, const CodecParams& params, uint32_t frame_id,
                    uint32_t capture_ts_us);

    // Zero-RGB-conversion path: encodes YUV_420_888 planes directly and streams over UDP.
    bool send_frame_yuv420(const Yuv420View& yuv, const CodecParams& params, uint32_t frame_id,
                           uint32_t capture_ts_us);

    // Transmits one low-latency audio chunk (2.5-5 ms PCM16) over the same UDP socket.
    // Thread-safe (may be called directly from the WASAPI audio capture thread).
    bool send_audio(uint32_t audio_seq_id, uint32_t timestamp_us, const int16_t* pcm16,
                    uint16_t num_frames, uint32_t sample_rate = 48000, uint8_t channels = 2);

    // Transmits one audio chunk in either PCM16 or Float32 format.
    bool send_audio_raw(uint32_t audio_seq_id, uint32_t timestamp_us, const void* samples,
                        uint16_t num_frames, uint32_t sample_rate, uint8_t channels,
                        AudioSampleFormat fmt);

    // Non-blocking poll for incoming control packets (PING -> PONG, FEEDBACK -> AIMD).
    void service_control();

    void set_sim_loss_pct(double pct) { sim_loss_pct_.store(pct, std::memory_order_relaxed); }
    double sim_loss_pct() const { return sim_loss_pct_.load(std::memory_order_relaxed); }

    // Live runtime FEC and AIMD reconfiguration (thread-safe).
    void set_fec_group_size(uint16_t group_size) {
        fec_group_size_.store(group_size, std::memory_order_relaxed);
    }
    uint16_t fec_group_size() const { return fec_group_size_.load(std::memory_order_relaxed); }
    void set_adaptive_quality(bool enabled, int q_min, int q_max, int current_q);

    int active_quality() const { return aq_.current_quality(); }
    const AdaptiveQualityController& adaptive_controller() const { return aq_; }

    SenderStats stats() const;

private:
    bool should_drop_sim();
    void pace_bytes(size_t bytes, std::unique_lock<std::mutex>& lk);
    void send_encoded_slice(uint32_t frame_id, uint32_t capture_ts_us, int slice_id, int total_slices,
                            const uint8_t* data, size_t size);

    SenderConfig cfg_;
    Endpoint dst_{};
    UdpSocket sock_;
    ThreadPool pool_;
    FrameCodec codec_;
    AdaptiveQualityController aq_;
    mutable std::mutex aq_mu_;
    std::atomic<uint16_t> seq_{0};
    std::atomic<uint16_t> fec_group_size_{0};
    std::atomic<double> sim_loss_pct_{0.0};

    std::mutex pace_mu_;
    double tokens_bytes_ = 0.0;
    uint64_t last_pace_us_ = 0;
    std::atomic<uint64_t> rng_state_{0x9E3779B97F4A7C15ull};

    std::atomic<uint64_t> frames_sent_{0};
    std::atomic<uint64_t> slices_sent_{0};
    std::atomic<uint64_t> packets_sent_{0};
    std::atomic<uint64_t> packets_dropped_sim_{0};
    std::atomic<uint64_t> send_errors_{0};
    std::atomic<uint64_t> bytes_sent_{0};
    std::atomic<uint64_t> audio_packets_sent_{0};
    std::atomic<uint64_t> audio_bytes_sent_{0};
    std::atomic<uint64_t> pongs_sent_{0};
    std::atomic<uint64_t> feedbacks_received_{0};
};

}  // namespace lv
