#pragma once
// LanVid Video + Stereo Audio UDP Receiver:
// Reads LVSP datagrams in batches, handles PING/PONG clock sync, 100 ms AIMD/PLI feedback,
// XOR FEC video reassembly + concurrent decode, and lock-free 1-2 channel stereo audio buffering.

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

#include "audio/stereo_audio_buffer.hpp"
#include "common/clock.hpp"
#include "common/udp_socket.hpp"
#include "net/assembler.hpp"
#include "parallel/thread_pool.hpp"

namespace lv {

struct ReceiverConfig {
    std::string bind_ip = "0.0.0.0";
    uint16_t bind_port = 5000;
    int socket_buf_bytes = 16 * 1024 * 1024;
    int recv_timeout_ms = 2;
    uint32_t decode_threads = 0;  // 0 = hardware concurrency
    AssemblerConfig assembler{};
    bool enable_clock_sync = true;
    uint64_t ping_interval_us = 500000;      // 500 ms
    bool enable_feedback = true;
    uint64_t feedback_interval_us = 100000;  // 100 ms
    double audio_target_ms = 12.0;           // 5-40 ms stereo audio jitter buffer target
};

struct ClockSyncInfo {
    bool synced = false;
    int64_t offset_us = 0;
    uint32_t min_rtt_us = 0;
};

class UdpReceiver {
public:
    using FrameCallback = FrameAssembler::FrameCallback;
    using AudioCallback = std::function<void(uint32_t seq_id, uint32_t timestamp_us,
                                             const AudioHeader& hdr, const uint8_t* pcm_data)>;

    UdpReceiver(const ReceiverConfig& cfg, FrameCallback on_frame);
    ~UdpReceiver();

    void set_audio_callback(AudioCallback cb) { audio_cb_ = std::move(cb); }
    void set_video_bitstream_callback(FrameAssembler::BitstreamCallback cb) {
        assembler_.set_bitstream_callback(std::move(cb));
    }

    bool init();
    // Shared-socket mode (MultiCamManager demux on one port).
    bool init_external(UdpSocket* shared_socket);
    void ingest_datagram(const uint8_t* data, size_t len, const Endpoint& src, uint64_t now);
    void service(uint64_t now);
    uint16_t local_port() const { return ext_sock_ ? ext_sock_->local_port() : sock_.local_port(); }

    void step_once();
    void run(std::atomic<bool>& stop_flag);
    void flush();

    double estimate_latency_ms(uint64_t recv_complete_us, uint32_t sender_ts_us) const;

    AssemblerStats assembler_stats() const { return assembler_.stats(); }
    void request_keyframe() { send_pli_now(now_us()); }
    bool sender_endpoint(Endpoint& out) const {
        std::lock_guard<std::mutex> lk(sender_ep_mu_);
        if (!has_sender_) return false;
        out = sender_ep_;
        return true;
    }
    const SeqLossTracker& seq_tracker() const { return assembler_.seq_tracker(); }
    VideoCodecId active_codec() const { return assembler_.active_codec(); }
    std::string decoder_backend_name() const { return assembler_.decoder_backend_name(); }
    bool decoder_is_hardware() const { return assembler_.decoder_is_hardware(); }
    ClockSyncInfo clock_sync() const { return sync_; }
    uint64_t feedbacks_sent() const { return feedbacks_sent_.load(std::memory_order_relaxed); }
    uint64_t plis_sent() const { return plis_sent_.load(std::memory_order_relaxed); }
    uint32_t jitter_us() const {
        std::lock_guard<std::mutex> lk(fb_mu_);
        return uint32_t(jitter_us_);
    }

    AudioJitterBuffer& audio_buffer() { return audio_buf_; }
    AudioStats audio_stats() const { return audio_buf_.stats(); }
    void set_audio_target_ms(double target_ms) { audio_buf_.set_target_ms(target_ms); }

    const UdpSocket& socket() const { return sock_; }
    void set_conceal_mode(ConcealMode m) { assembler_.set_conceal_mode(m); }
    ConcealMode conceal_mode() const { return assembler_.conceal_mode(); }

private:
    static constexpr int kRecvBatchSize = 16;
    static constexpr size_t kRecvSlotCap = 2048;

    void on_frame_internal(const CompletedFrame& cf);
    void maybe_send_ping(uint64_t now);
    void maybe_send_feedback(uint64_t now);
    void send_pli_now(uint64_t now);
    void handle_pong(const uint8_t* payload, size_t len, uint32_t t4_us);
    void handle_datagram(const uint8_t* dgram, size_t n, const Endpoint& src, uint64_t now);

    ReceiverConfig cfg_;
    FrameCallback user_on_frame_;
    AudioCallback audio_cb_;
    UdpSocket sock_;
    UdpSocket* ext_sock_ = nullptr;
    UdpSocket& tx() { return ext_sock_ ? *ext_sock_ : sock_; }
    ThreadPool pool_;
    FrameAssembler assembler_;
    AudioJitterBuffer audio_buf_;

    mutable std::mutex sender_ep_mu_;
    Endpoint sender_ep_{};
    bool has_sender_ = false;
    void remember_sender(const Endpoint& src) {
        std::lock_guard<std::mutex> lk(sender_ep_mu_);
        if (!has_sender_ || !(sender_ep_ == src)) {
            sender_ep_ = src;
            has_sender_ = true;
        }
    }

    uint64_t last_ping_us_ = 0;
    uint32_t ping_seq_ = 0;
    ClockSyncInfo sync_{};
    std::atomic<int64_t> sync_offset_pub_{0};
    std::atomic<bool> sync_synced_pub_{false};

    uint64_t last_fb_us_ = 0;
    uint64_t fb_prev_rx_ = 0;
    uint64_t fb_prev_lost_ = 0;
    uint64_t fb_prev_fec_rec_ = 0;
    uint64_t fb_prev_fec_par_ = 0;
    mutable std::mutex fb_mu_;
    uint32_t last_completed_fid_ = 0;
    bool has_prev_lat_ = false;
    double prev_lat_us_ = 0.0;
    double jitter_us_ = 0.0;
    uint64_t fb_dec_us_sum_ = 0;
    uint64_t fb_dec_cnt_ = 0;
    std::atomic<uint64_t> feedbacks_sent_{0};
    std::atomic<uint64_t> plis_sent_{0};

    uint8_t recv_batch_buf_[kRecvBatchSize][kRecvSlotCap];
};

using VideoReceiver = UdpReceiver;
using VideoRxEngine = UdpReceiver;

}  // namespace lv
