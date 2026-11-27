#pragma once
// LanVid Video + Stereo Audio UDP Sender:
// Encodes video slices in parallel (DCT AVX2/NEON or HW H.264/H.265), supports slice-level XOR FEC,
// batch UDP send, 1-2 channel stereo audio (kAudio), pacing, PING/PONG, AIMD/PLI, and Dual-NIC failover.

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "codec/frame_codec.hpp"
#include "codec/video_encoder.hpp"
#include "common/clock.hpp"
#include "common/image.hpp"
#include "common/udp_socket.hpp"
#include "net/adaptive_quality.hpp"
#include "net/packetizer.hpp"
#include "parallel/thread_pool.hpp"

namespace lv {

struct SenderConfig {
    std::string dest_ip = "127.0.0.1";
    uint16_t dest_port = 5000;
    std::string bind_ip;             // Empty / "0.0.0.0" = OS default route; else Primary NIC IPv4
    std::string secondary_bind_ip;   // Optional Secondary NIC IPv4 for Dual-NIC 1+1 failover
    uint16_t bind_port = 0;
    size_t mtu = kDefaultMtu;
    bool enable_crc = true;
    uint16_t fec_group_size = 0;  // 0 = disabled; e.g. 6 or 8 = 1 XOR parity per group
    double sim_loss_pct = 0.0;    // 0..100: simulated random packet drop
    double pace_mbps = 0.0;       // 0 = unpaced; >0 = token-bucket bitrate cap in Mbps
    int socket_buf_bytes = 8 * 1024 * 1024;
    uint32_t threads = 0;         // 0 = hardware concurrency
    VideoCodecId codec = VideoCodecId::kLanVidDct;
    uint32_t bitrate_kbps = 8000;            // Target bitrate for H.264 / H.265
    uint32_t keyframe_interval_frames = 120; // IDR interval for H.264 / H.265
    double fps = 60.0;
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
    VideoCodecId active_codec = VideoCodecId::kLanVidDct;
    uint32_t active_bitrate_kbps = 0;
    uint64_t pli_received = 0;
};

class UdpSender {
public:
    explicit UdpSender(const SenderConfig& cfg);
    ~UdpSender() = default;

    bool init();
    // Pre-initializes H.264/H.265 HW encoder before frame 0 so the first frame has zero init spike.
    bool prepare_encoder(int width, int height);
    uint16_t local_port() const { return sock_.local_port(); }

    // Encodes `img` and transmits all slices over UDP. Returns false on invalid codec params.
    bool send_frame(const Image& img, const CodecParams& params, uint32_t frame_id,
                    uint32_t capture_ts_us);

    // Zero-RGB-conversion path: encodes YUV_420_888 planes directly and streams over UDP.
    bool send_frame_yuv420(const Yuv420View& yuv, const CodecParams& params, uint32_t frame_id,
                           uint32_t capture_ts_us);

    // True zero-copy path: passes platform hardware frame handle (e.g. CVPixelBufferRef) to VideoToolbox.
    bool send_frame_native(const void* native_buf, int width, int height, uint32_t frame_id,
                           uint32_t capture_ts_us);

    // Transmits one low-latency stereo/mono audio chunk (1-2 ch PCM16) over the same UDP socket.
    bool send_audio(uint32_t audio_seq_id, uint32_t timestamp_us, const int16_t* pcm16,
                    uint16_t num_frames, uint32_t sample_rate = 48000, uint8_t channels = 2);

    // Transmits one 1-2 channel stereo/mono audio chunk in PCM16 or Float32 format.
    bool send_audio_raw(uint32_t audio_seq_id, uint32_t timestamp_us, const void* samples,
                        uint16_t num_frames, uint32_t sample_rate, uint8_t channels,
                        AudioSampleFormat fmt);

    // Physical NIC selection & Dual-NIC 1+1 failover.
    bool set_network_interfaces(const std::string& primary_ip, const std::string& secondary_ip);
    bool dual_nic_active() const { return dual_nic_active_.load(std::memory_order_acquire); }
    uint64_t failover_packets() const { return failover_pkts_.load(std::memory_order_relaxed); }
    uint64_t secondary_packets_sent() const { return sec_pkts_sent_.load(std::memory_order_relaxed); }
    uint64_t secondary_bytes_sent() const { return sec_bytes_sent_.load(std::memory_order_relaxed); }
    void set_primary_link_down(bool down) { primary_link_down_.store(down, std::memory_order_release); }
    bool primary_link_down() const { return primary_link_down_.load(std::memory_order_acquire); }

    const UdpSocket& socket() const { return sock_; }

    // Non-blocking poll for incoming control packets (PING -> PONG, FEEDBACK -> AIMD / PLI).
    void service_control();
    void service_control_try() {
        std::unique_lock<std::mutex> lk(ctl_mu_, std::try_to_lock);
        if (lk.owns_lock()) service_control_locked();
    }

    void set_sim_loss_pct(double pct) { sim_loss_pct_.store(pct, std::memory_order_relaxed); }
    double sim_loss_pct() const { return sim_loss_pct_.load(std::memory_order_relaxed); }

    // Live runtime FEC, bitrate, PLI, and AIMD reconfiguration (thread-safe).
    void set_fec_group_size(uint16_t group_size) {
        fec_group_size_.store(group_size, std::memory_order_relaxed);
    }
    uint16_t fec_group_size() const { return fec_group_size_.load(std::memory_order_relaxed); }
    void set_adaptive_quality(bool enabled, int q_min, int q_max, int current_q);
    void set_bitrate_kbps(uint32_t bitrate_kbps);
    void set_codec(VideoCodecId codec);
    void set_fps(double fps);
    void force_keyframe() { force_idr_.store(true, std::memory_order_release); }

    int active_quality() const {
        std::lock_guard<std::mutex> lk(aq_mu_);
        return aq_.current_quality();
    }
    uint32_t active_bitrate_kbps() const { return target_bitrate_kbps_.load(std::memory_order_relaxed); }
    VideoCodecId active_codec() const { return VideoCodecId(active_codec_.load(std::memory_order_acquire)); }
    std::string encoder_backend_name() const;
    bool encoder_is_hardware() const;
    const AdaptiveQualityController& adaptive_controller() const { return aq_; }

    SenderStats stats() const;

private:
    void service_control_locked();
    int send_with_failover(const uint8_t* dgram, size_t len, QosTrafficClass qos);
    bool should_drop_sim();
    bool ensure_video_encoder(int width, int height);
    void pace_bytes(size_t bytes, std::unique_lock<std::mutex>& lk);
    void send_encoded_slice(uint32_t frame_id, uint32_t capture_ts_us, int slice_id, int total_slices,
                            const uint8_t* data, size_t size, uint8_t extra_flags = 0);

    SenderConfig cfg_;
    std::mutex ctl_mu_;
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
    ThreadPool pool_;
    FrameCodec codec_;
    std::unique_ptr<IVideoEncoder> video_enc_;
    int enc_w_ = 0;
    int enc_h_ = 0;
    VideoCodecId enc_codec_ = VideoCodecId::kLanVidDct;
    double enc_fps_ = 0.0;
    std::atomic<uint8_t> active_codec_{uint8_t(VideoCodecId::kLanVidDct)};
    std::atomic<double> target_fps_{60.0};
    uint32_t applied_bitrate_kbps_ = 0;
    std::atomic<uint32_t> target_bitrate_kbps_{8000};
    std::atomic<bool> force_idr_{false};
    EncodedVideoPacket enc_pkt_;
    mutable std::mutex enc_meta_mu_;
    std::string enc_backend_name_;
    bool enc_is_hw_ = false;
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
    std::atomic<uint64_t> pli_received_{0};
};

using VideoSender = UdpSender;
using VideoTxEngine = UdpSender;

}  // namespace lv
