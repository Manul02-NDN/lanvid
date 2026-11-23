#pragma once
// High-throughput UDP streaming sender: encodes slices in parallel on ThreadPool, supports
// slice-level XOR FEC, batch UDP send, low-latency audio packets, pacing, PING/PONG, and AIMD.

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "common/clock.hpp"
#include "audio/audio_tx.hpp"
#include "midi/midi_sync_engine.hpp"
#include "codec/frame_codec.hpp"
#include "codec/video_encoder.hpp"
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
    VideoCodecId codec = VideoCodecId::kLanVidDct;
    uint32_t bitrate_kbps = 8000;            // Target bitrate for H.264 / H.265
    uint32_t keyframe_interval_frames = 120; // IDR interval for H.264 / H.265
    double fps = 60.0;
    AdaptiveQualityConfig adaptive{};
    uint32_t audio_sample_rate = 48000;  // 48000 / 96000 for the 64-channel audio bus
    uint32_t audio_latency_ms = 20;      // 2..2000: selects chunk size / FEC / NACK profile
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

    // Transmits one low-latency audio chunk (2.5-5 ms PCM16) over the same UDP socket.
    // Thread-safe (may be called directly from the WASAPI audio capture thread).
    bool send_audio(uint32_t audio_seq_id, uint32_t timestamp_us, const int16_t* pcm16,
                    uint16_t num_frames, uint32_t sample_rate = 48000, uint8_t channels = 2);

    // Transmits one audio chunk in either PCM16 or Float32 format.
    bool send_audio_raw(uint32_t audio_seq_id, uint32_t timestamp_us, const void* samples,
                        uint16_t num_frames, uint32_t sample_rate, uint8_t channels,
                        AudioSampleFormat fmt);

    // ---- 64-channel AoIP (LVSP audio v2) ----
    // Pushes samples of one channel (0..63). `media_ts` = master-clock sample position of the first
    // sample, or kAutoMediaTs to continue the channel (anchored to the master clock on first push).
    static constexpr uint64_t kAutoMediaTs = UINT64_MAX;
    int push_audio_channel(uint16_t channel, const void* samples, uint32_t num_samples,
                           AudioFormat2 fmt, uint64_t media_ts);
    // Next contiguous timestamp for a multi-channel push (anchored to the master clock on first use).
    uint64_t next_auto_media_ts(uint32_t num_samples) const;
    void set_next_auto_media_ts(uint64_t ts) { for (auto& n : next_auto_ts_) n.store(ts, std::memory_order_relaxed); }
    AudioReliabilityProfile audio2_profile() const {
        return atx_ ? atx_->profile(AudioFormat2::kPcm16)
                    : audio_profile_for_latency(cfg_.audio_latency_ms, cfg_.audio_sample_rate, cfg_.mtu, AudioFormat2::kPcm16);
    }
    void set_audio_latency_ms(uint32_t ms);
    uint32_t audio_latency_ms() const { return atx_ ? atx_->latency_ms() : cfg_.audio_latency_ms; }
    void set_audio_codec_profile(AudioCodecProfile p) {
        audio_codec_ = p;
        if (atx_) atx_->set_codec_profile(p);
    }
    AudioCodecProfile audio_codec_profile() const { return atx_ ? atx_->codec_profile() : audio_codec_; }
    void flush_audio();
    // Current master-clock position in samples (sender epoch = construction time).
    uint64_t master_samples_now() const;
    uint64_t master_us_now() const { return now_us() - epoch_us_; }
    AudioTxStats audio2_stats() const { return atx_ ? atx_->stats() : AudioTxStats{}; }

    // ---- MIDI2 over the same UDP stream ----
    // Sends one MIDI short message stamped with the sender MasterClock (media_ts_us = 0 -> now).
    // Flushed immediately; every event is repeated in the next packets and in 100 ms heartbeats
    // (16-event redundancy ring), so a lost Note Off is recovered on lossy Wi-Fi.
    bool push_midi(uint8_t status, uint8_t d1, uint8_t d2, uint8_t port = 0, uint64_t media_ts_us = 0);
    MidiTxStats midi_stats() const { return midi_tx_ ? midi_tx_->stats() : MidiTxStats{}; }

    // Session epoch (random, non-zero) carried by every AUDIO2 / MIDI2 packet. A receiver resets its
    // transport state and MasterClock when it changes (sender restart).
    uint16_t stream_epoch() const { return stream_epoch_; }
    const UdpSocket& socket() const { return sock_; }

    // Non-blocking poll for incoming control packets (PING -> PONG, FEEDBACK -> AIMD).
    void service_control();
    // Same, but skips if another thread (an audio push) is already servicing the socket.
    void service_control_try() {
        std::unique_lock<std::mutex> lk(ctl_mu_, std::try_to_lock);
        if (lk.owns_lock()) service_control();
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
    void force_keyframe() { force_idr_.store(true, std::memory_order_release); }

    int active_quality() const { return aq_.current_quality(); }
    uint32_t active_bitrate_kbps() const { return target_bitrate_kbps_.load(std::memory_order_relaxed); }
    VideoCodecId active_codec() const { return cfg_.codec; }
    std::string encoder_backend_name() const;
    bool encoder_is_hardware() const;
    const AdaptiveQualityController& adaptive_controller() const { return aq_; }

    SenderStats stats() const;

private:
    bool should_drop_sim();
    bool ensure_video_encoder(int width, int height);
    void pace_bytes(size_t bytes, std::unique_lock<std::mutex>& lk);
    void send_encoded_slice(uint32_t frame_id, uint32_t capture_ts_us, int slice_id, int total_slices,
                            const uint8_t* data, size_t size, uint8_t extra_flags = 0);

    SenderConfig cfg_;
    uint64_t epoch_us_ = 0;
    uint16_t stream_epoch_ = 1;
    std::unique_ptr<MidiTxEngine> midi_tx_;
    std::atomic<uint64_t> last_midi_us_{0};
    std::atomic<uint64_t> last_midi_hb_us_{0};
    std::mutex ctl_mu_;
    std::unique_ptr<AudioTxEngine> atx_;
    AudioCodecProfile audio_codec_ = AudioCodecProfile::kPcm32F;
    std::atomic<uint64_t> next_auto_ts_[kMaxAudioChannels]{};
    Endpoint dst_{};
    UdpSocket sock_;
    ThreadPool pool_;
    FrameCodec codec_;
    std::unique_ptr<IVideoEncoder> video_enc_;
    int enc_w_ = 0;
    int enc_h_ = 0;
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

}  // namespace lv
