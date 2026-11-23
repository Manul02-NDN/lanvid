#pragma once
// UDP Receiver: reads LVSP datagrams in batches (recvmmsg on Linux), handles PING/PONG clock sync,
// 100 ms AIMD Feedback, XOR FEC video reassembly + concurrent decode, and lock-free audio buffering.

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

#include "common/clock.hpp"
#include "audio/audio_jitter_buffer.hpp"
#include "audio/audio_rx.hpp"
#include "audio/audio_sync_matrix.hpp"
#include "audio/master_clock.hpp"
#include "net/assembler.hpp"
#include "net/udp_socket.hpp"
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
    double audio_target_ms = 12.0;           // 10-15 ms jitter buffer target
    uint32_t audio_sample_rate = 48000;      // 64-channel bus rate (48000 / 96000)
    uint32_t audio_latency_ms = 20;          // 2..2000 ms multichannel playout latency
    bool audio_only = false;                 // drop video slices before reassembly/decoding (AoIP bridge)
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
    ~UdpReceiver() = default;

    void set_audio_callback(AudioCallback cb) { audio_cb_ = std::move(cb); }

    bool init();
    uint16_t local_port() const { return sock_.local_port(); }

    void step_once();
    void run(std::atomic<bool>& stop_flag);
    void flush();

    double estimate_latency_ms(uint64_t recv_complete_us, uint32_t sender_ts_us) const;

    const AssemblerStats& assembler_stats() const { return assembler_.stats(); }
    const SeqLossTracker& seq_tracker() const { return assembler_.seq_tracker(); }
    VideoCodecId active_codec() const { return assembler_.active_codec(); }
    std::string decoder_backend_name() const { return assembler_.decoder_backend_name(); }
    bool decoder_is_hardware() const { return assembler_.decoder_is_hardware(); }
    ClockSyncInfo clock_sync() const { return sync_; }
    uint64_t feedbacks_sent() const { return feedbacks_sent_; }
    uint64_t plis_sent() const { return plis_sent_; }
    uint64_t video_packets_ignored() const { return video_ignored_; }
    uint32_t jitter_us() const { return uint32_t(jitter_us_); }

    AudioJitterBuffer& audio_buffer() { return audio_buf_; }
    AudioStats audio_stats() const { return audio_buf_.stats(); }

    // ---- 64-channel phase-coherent audio bus ----
    void set_audio_latency_ms(uint32_t ms) { arx_.set_latency_ms(ms); }
    uint32_t audio_latency_ms() const { return arx_.latency_ms(); }
    // Renders `frames` synchronised frames for channels [0, num_channels) (interleaved or planar).
    void pop_audio_multichannel_interleaved(float* out, uint32_t num_channels, uint32_t frames) {
        matrix_.pop_interleaved(mclock_, now_us(), out, num_channels, frames);
    }
    void pop_audio_multichannel_planar(float* const* out, uint32_t num_channels, uint32_t frames) {
        matrix_.pop_planar(mclock_, now_us(), out, num_channels, frames);
    }
    AudioSyncMatrix& audio_matrix() { return matrix_; }
    const MasterClock& master_clock() const { return mclock_; }
    AudioChannelRxStats audio_channel_rx_stats(uint16_t ch) const { return arx_.channel_stats(ch); }

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

    ReceiverConfig cfg_;
    FrameCallback user_on_frame_;
    AudioCallback audio_cb_;
    UdpSocket sock_;
    ThreadPool pool_;
    FrameAssembler assembler_;
    AudioJitterBuffer audio_buf_;
    AudioSyncMatrix matrix_;
    MasterClock mclock_;
    AudioRxEngine arx_;

    Endpoint sender_ep_{};
    bool has_sender_ = false;
    uint64_t last_ping_us_ = 0;
    uint32_t ping_seq_ = 0;
    ClockSyncInfo sync_{};

    uint64_t last_fb_us_ = 0;
    uint64_t fb_prev_rx_ = 0;
    uint64_t fb_prev_lost_ = 0;
    uint64_t fb_prev_fec_rec_ = 0;
    uint64_t fb_prev_fec_par_ = 0;
    uint32_t last_completed_fid_ = 0;
    bool has_prev_lat_ = false;
    double prev_lat_us_ = 0.0;
    double jitter_us_ = 0.0;
    uint64_t fb_dec_us_sum_ = 0;
    uint64_t fb_dec_cnt_ = 0;
    uint64_t feedbacks_sent_ = 0;
    uint64_t plis_sent_ = 0;
    uint64_t video_ignored_ = 0;

    uint8_t recv_batch_buf_[kRecvBatchSize][kRecvSlotCap];
};

}  // namespace lv
