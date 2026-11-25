#pragma once
// UDP Receiver: reads LVSP datagrams in batches (recvmmsg on Linux), handles PING/PONG clock sync,
// 100 ms AIMD Feedback, XOR FEC video reassembly + concurrent decode, and lock-free audio buffering.

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

#include "common/clock.hpp"
#include "audio/audio_jitter_buffer.hpp"
#include "audio/audio_rx.hpp"
#include "audio/audio_sync_matrix.hpp"
#include "audio/master_clock.hpp"
#include "midi/midi_dejitter_queue.hpp"
#include "midi/midi_sync_engine.hpp"
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
    bool audio_adaptive_jitter = true;       // true = Adaptive Wi-Fi jitter buffer, false = Fixed latency
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
    uint32_t jitter_us() const {
        std::lock_guard<std::mutex> lk(fb_mu_);
        return uint32_t(jitter_us_);
    }

    AudioJitterBuffer& audio_buffer() { return audio_buf_; }
    AudioStats audio_stats() const { return audio_buf_.stats(); }

    // ---- 64-channel phase-coherent audio bus ----
    void set_audio_latency_ms(uint32_t ms) {
        arx_.set_latency_ms(ms);
        midi_dejitter_.set_latency(double(arx_.latency_ms()), arx_.adaptive_jitter_enabled());
    }
    uint32_t audio_latency_ms() const { return arx_.latency_ms(); }
    // Latency mode (live): Fixed (Ethernet / live monitoring) or Adaptive (Wi-Fi, grows up to 200 ms).
    void set_audio_adaptive_jitter(bool enabled) {
        arx_.set_adaptive_jitter_enabled(enabled);
        midi_dejitter_.set_latency(double(arx_.latency_ms()), arx_.adaptive_jitter_enabled());
    }
    bool audio_adaptive_jitter() const { return arx_.adaptive_jitter_enabled(); }
    // Dante-inspired Deterministic Timestamp-Driven Playout: locks latency strictly to target (5..10 ms)
    // with +/-0.1% (+/-1000 ppm) micro-rate trim and zero cushion expansion on small jitter.
    void set_deterministic_playout(bool enabled, double max_trim_ppm = 1000.0) {
        matrix_.set_deterministic_playout(enabled, max_trim_ppm);
    }
    bool deterministic_playout() const { return matrix_.deterministic_playout(); }
    // Measured playout delay of the 64-channel jitter buffer (ms) and the delay it currently steers to.
    double audio_effective_latency_ms() const { return arx_.current_effective_latency_ms(); }
    uint32_t audio_effective_target_ms() const { return matrix_.effective_latency_ms(); }
    // Renders `frames` synchronised frames for channels [0, num_channels) (interleaved or planar), then
    // dispatches every MIDI2 event that falls inside this block (sample offset given to the callback).
    void pop_audio_multichannel_interleaved(float* out, uint32_t num_channels, uint32_t frames);
    void pop_audio_multichannel_planar(float* const* out, uint32_t num_channels, uint32_t frames);

    // ---- MIDI2 (same UDP stream) ----
    // `block_offset_frames`: sample position of the event inside the audio block just rendered by a
    // pop_audio_* call (sample-accurate scheduling), or -1 when dispatched without an audio consumer.
    using MidiCallback = std::function<void(const TimedMidiEvent& ev, int32_t block_offset_frames)>;
    void set_midi_callback(MidiCallback cb);
    void set_midi_only(bool enabled) {
        midi_only_.store(enabled, std::memory_order_relaxed);
        midi_dejitter_.set_midi_only_mode(enabled, 1.5);
    }
    bool midi_only() const { return midi_only_.load(std::memory_order_relaxed); }
    MidiRxStats midi_stats() const { return midi_rx_.stats(); }
    MidiDejitterStats midi_dejitter_stats() const { return midi_dejitter_.stats(); }
    uint32_t active_midi_notes() const;

    // ---- Real-Time Control Sync (kControlSync, bidirectional, 3x Wi-Fi redundant) ----
    using ControlSyncCallback = std::function<void(const ControlSyncPayload&)>;
    void set_control_sync_callback(ControlSyncCallback cb) {
        std::lock_guard<std::mutex> lk(cs_mu_);
        cs_cb_ = std::move(cb);
    }
    bool send_control_sync(const ControlSyncPayload& cs);
    uint64_t control_sync_tx_count() const { return cs_tx_count_.load(std::memory_order_relaxed); }
    uint64_t control_sync_rx_count() const { return cs_rx_count_.load(std::memory_order_relaxed); }

    // ---- Stream epoch (sender restart) ----
    uint16_t stream_epoch() const { return cur_epoch_.load(std::memory_order_relaxed); }
    uint64_t stream_epoch_resets() const { return epoch_resets_.load(std::memory_order_relaxed); }
    const UdpSocket& socket() const { return sock_; }
    AudioSyncMatrix& audio_matrix() { return matrix_; }
    const MasterClock& master_clock() const { return mclock_; }
    AudioChannelRxStats audio_channel_rx_stats(uint16_t ch) const { return arx_.channel_stats(ch); }
    uint64_t audio2_bytes_rx() const { return audio2_bytes_rx_.load(std::memory_order_relaxed); }
    uint64_t audio2_packets_rx() const { return audio2_packets_rx_.load(std::memory_order_relaxed); }

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
    bool accept_epoch(uint16_t epoch);
    void reset_stream_epoch();
    void dispatch_midi(uint64_t playout_us, double block_start_samples, uint32_t frames);
    void dispatch_midi_local(uint64_t local_now_us);
    void track_note(const TimedMidiEvent& ev);
    void release_all_notes();

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
    std::atomic<int64_t> sync_offset_pub_{0};
    std::atomic<bool> sync_synced_pub_{false};

    uint64_t last_fb_us_ = 0;
    uint64_t fb_prev_rx_ = 0;
    uint64_t fb_prev_lost_ = 0;
    uint64_t fb_prev_fec_rec_ = 0;
    uint64_t fb_prev_fec_par_ = 0;
    mutable std::mutex fb_mu_;  // frame-callback stats (decode worker) vs. feedback (network thread)
    uint32_t last_completed_fid_ = 0;
    bool has_prev_lat_ = false;
    double prev_lat_us_ = 0.0;
    double jitter_us_ = 0.0;
    uint64_t fb_dec_us_sum_ = 0;
    uint64_t fb_dec_cnt_ = 0;
    uint64_t feedbacks_sent_ = 0;
    uint64_t plis_sent_ = 0;
    uint64_t video_ignored_ = 0;
    std::atomic<uint64_t> audio2_bytes_rx_{0};
    std::atomic<uint64_t> audio2_packets_rx_{0};

    // MIDI2 + De-Jitter Playout Queue + session epoch
    MidiRxEngine midi_rx_;
    MidiDejitterQueue midi_dejitter_;
    std::atomic<bool> midi_only_{false};
    mutable std::mutex midi_mu_;
    MidiCallback midi_cb_;
    uint8_t active_notes_[kMaxMidiPorts][16][128] = {};
    std::atomic<uint64_t> last_pop_us_{0};
    std::atomic<uint16_t> cur_epoch_{0};
    uint16_t prev_epoch_ = 0;
    std::atomic<uint64_t> epoch_resets_{0};
    mutable std::mutex cs_mu_;
    ControlSyncCallback cs_cb_;
    uint64_t cs_last_rx_rev_ = 0;
    std::atomic<uint64_t> cs_tx_count_{0};
    std::atomic<uint64_t> cs_rx_count_{0};

    uint8_t recv_batch_buf_[kRecvBatchSize][kRecvSlotCap];
};

}  // namespace lv
