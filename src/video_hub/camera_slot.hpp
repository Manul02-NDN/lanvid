#pragma once
// CameraSlotEngine — Isolated per-camera pipeline (up to 16 slots) for LanVid Video Hub.
//
// Design guarantees:
//   * Each slot owns its own receiver (either SDK C ABI LvReceiverHandle or external-socket
//     UdpReceiver on the shared port), its own worker thread, its own VideoPlayoutBuffer,
//     and its own frame/stats mutexes (zero cross-camera lock contention).
//   * VideoPlayoutBuffer eliminates Wi-Fi micro-freezes via Fixed Delay (5..250 ms, default 33 ms)
//     or Adaptive Wi-Fi (up to 200 ms), non-blocking Frame Hold, and 500 ms rate-limited IDR requests.
//   * Bidirectional kVideoConfigSync state with Echo Guard & revision deduplication.

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "common/image.hpp"
#include "lanvid/lanvid_sdk.h"
#include "net/packet.hpp"
#include "net/receiver.hpp"
#include "video/video_playout_buffer.hpp"

namespace lv::hub {

constexpr uint32_t kMaxHubSlots = 16;
constexpr size_t kTelemetryHistoryLen = 120;

struct CameraRemoteSettings {
    uint8_t assigned_cam_number = 1;           // 1..16
    std::string assigned_cam_tag = "CAM 1";    // up to 15 chars shown on phone HUD badge
    uint8_t badge_accent_rgba[4] = {139, 92, 246, 255};
    uint8_t resolution_preset = lv::kQuality1080p;  // 0=360p, 1=720p, 2=1080p, 3=4K
    uint8_t target_fps = 60;                   // 24 / 25 / 30 / 50 / 60
    uint8_t codec = uint8_t(lv::VideoCodecId::kH264);
    uint32_t bitrate_kbps = 25000;             // 5000 .. 150000
    uint8_t camera_lens = lv::kLensWide;       // 0=Wide, 1=Ultra, 2=Tele, 3=Front
    uint16_t video_delay_ms = 33;              // 5..250 ms
    bool fixed_delay_mode = true;              // true=Fixed, false=Adaptive Wi-Fi
};

struct CameraSlotSnapshot {
    uint32_t slot_index = 0;
    uint32_t camera_id = 0;
    std::string device_uid;
    std::string label;
    std::string source_name;
    std::string sender_ip;
    std::string pinned_ip;
    uint16_t sender_port = 0;
    uint16_t listen_port = 0;

    bool bound = false;
    bool streaming = false;
    bool tally_connected = false;
    bool config_synced = false;

    uint32_t width = 0;
    uint32_t height = 0;
    double fps = 0.0;
    std::string codec_name = "—";
    std::string decoder_backend;
    bool hardware_decode = false;
    double bitrate_mbps = 0.0;
    double loss_pct = 0.0;
    uint64_t frames_rx = 0;

    // VideoPlayoutBuffer live telemetry
    double playout_delay_ms = 33.0;
    double playout_jitter_ms = 0.0;
    uint64_t playout_held_frames = 0;
    uint64_t playout_dropped_frames = 0;
    uint64_t playout_idr_requests = 0;

    // Phone telemetry & Tally
    uint8_t battery_pct = 100;
    int8_t temp_c = 30;
    float audio_peak_dbfs = -120.0f;
    uint8_t tally = lv::kTallyStandby;
    bool rec_trigger_active = false;
    bool operator_speaking = false;
    bool talk_to_cam = false;
    bool listen_from_cam = true;

    CameraRemoteSettings settings{};
    std::array<float, kTelemetryHistoryLen> jitter_history{};
    std::array<float, kTelemetryHistoryLen> loss_history{};
    size_t history_count = 0;
};

class CameraSlotEngine {
public:
    using BitstreamCallback = std::function<void(uint32_t slot, int codec, const uint8_t* data, size_t size,
                                                 bool is_keyframe, uint64_t arrival_us)>;
    using AudioCallback = std::function<void(uint32_t slot, const lv::AudioHeader& hdr, const uint8_t* pcm)>;
    using IdrCallback = std::function<void(uint32_t slot, uint32_t camera_id)>;
    using PlayoutFrameCallback = std::function<void(uint32_t slot, const lv::Image& frame, double fps,
                                                    uint64_t timestamp_us)>;

    explicit CameraSlotEngine(uint32_t slot_index);
    ~CameraSlotEngine();

    CameraSlotEngine(const CameraSlotEngine&) = delete;
    CameraSlotEngine& operator=(const CameraSlotEngine&) = delete;

    uint32_t slot_index() const { return slot_index_; }

    // Dedicated SDK receiver mode (LvReceiverHandle via C ABI on `listen_port`).
    bool start_dedicated_port(const std::string& bind_ip, uint16_t listen_port, const std::string& hub_name);
    // Shared base-port demux mode (external socket; datagrams queued via enqueue_datagram).
    bool start_demux_receiver(lv::UdpSocket* shared_sock, uint16_t shared_port);
    void stop();
    bool is_running() const { return running_.load(std::memory_order_acquire); }

    void set_bitstream_callback(BitstreamCallback cb);
    void set_audio_callback(AudioCallback cb);
    void set_idr_callback(IdrCallback cb);
    void set_playout_callback(PlayoutFrameCallback cb);

    // Enqueues one raw UDP datagram from the demux socket into this slot's private queue.
    void enqueue_datagram(const uint8_t* data, size_t len, const lv::Endpoint& src, uint64_t now_us);
    // Binds sender identity (IP:port or device_uid) to this slot cleanly.
    void bind_sender(const std::string& ip, uint16_t port, const std::string& uid = "");
    void release_sender();

    // Injects an already-decoded or raw frame directly (useful for unit tests & synthetic pipelines).
    void push_decoded_frame(lv::Image img, uint32_t frame_id, uint32_t media_ts_us32, uint64_t arrival_us,
                            bool is_keyframe, bool corrupt = false, double fps_hint = 30.0,
                            const std::string& codec = "DCT");

    // Polls latest smooth frame from VideoPlayoutBuffer without blocking.
    bool poll_playout_frame(uint64_t now_us, lv::Image& out, uint64_t& inout_seq);

    // Configuration & Badge assignment
    void set_label(const std::string& label);
    void set_pinned_ip(const std::string& ip);
    void set_device_uid(const std::string& uid);
    void set_playout_delay(uint32_t delay_ms, bool fixed_mode);
    void force_idr();

    void set_remote_settings(const CameraRemoteSettings& s);
    CameraRemoteSettings remote_settings() const;
    lv::VideoConfigSyncPayload build_config_sync_payload(uint32_t origin_id);
    bool apply_incoming_config_sync(const lv::VideoConfigSyncPayload& p, uint32_t local_origin_id);

    void merge_tally_telemetry(uint32_t camera_id, const std::string& device_name, const std::string& ip,
                               bool connected, uint8_t battery_pct, int8_t temp_c, uint8_t tally,
                               bool rec_trigger, bool op_speaking);
    void set_intercom_routing(bool talk_to_cam, bool listen_from_cam);
    void set_audio_peak(float peak_dbfs);
    void mark_config_synced(bool synced);

    uint16_t listen_port() const;
    CameraSlotSnapshot snapshot() const;

private:
    struct QueuedDatagram {
        std::vector<uint8_t> data;
        lv::Endpoint src{};
        uint64_t arrival_us = 0;
    };

    void worker_loop();
    void update_telemetry_and_history(uint64_t now_us);
    void push_history_locked(float jitter_ms, float loss_pct);

    static void on_sdk_video(const LV_VideoFrameView* frame, void* user_data);
    static void on_sdk_audio(const LV_AudioChunkView* audio, void* user_data);

    const uint32_t slot_index_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_{false};
    bool sdk_mode_ = false;
    uint16_t bound_port_ = 0;

    LvReceiverHandle* sdk_rx_ = nullptr;
    std::unique_ptr<lv::UdpReceiver> demux_rx_;

    std::mutex dgram_mu_;
    std::condition_variable dgram_cv_;
    std::deque<QueuedDatagram> dgram_q_;
    std::thread worker_;

    lv::VideoPlayoutBuffer playout_;
    std::mutex playout_pub_mu_;
    uint64_t playout_pub_seq_ = 0;

    mutable std::mutex state_mu_;
    CameraSlotSnapshot snap_{};
    uint64_t local_config_rev_ = 1;
    uint64_t last_remote_rev_ = 0;
    uint64_t prev_frames_rx_ = 0;
    uint64_t prev_bytes_rx_ = 0;
    uint64_t last_rate_sample_us_ = 0;
    uint64_t last_rx_frame_us_ = 0;
    std::vector<float> audio_peak_tmp_;

    std::mutex cb_mu_;
    BitstreamCallback bitstream_cb_;
    AudioCallback audio_cb_;
    IdrCallback idr_cb_;
    PlayoutFrameCallback playout_cb_;
};

}  // namespace lv::hub
