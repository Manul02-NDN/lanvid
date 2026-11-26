#pragma once
// obs_lanvid_core.hpp — Zero-Crash C++ Core Engine & Singleton Runtime for obs-lanvid.
//
// Architectural guarantees:
//   * Single background runtime (`LanVidObsRuntime`) manages Tally/Intercom, continuous
//     non-blocking multi-source discovery (mDNS / UDP Broadcast :5021/:5023/:5099 / Tally phones /
//     Shared Memory `LanVidVideoBus_v1`), and port-conflict checks.
//   * `obs_properties_t`, `video_render`, and Qt UI NEVER execute blocking network calls.
//   * Sources are held as `std::weak_ptr<LanVidObsSourceEngine>` — destroying an OBS source or
//     switching scenes can never cause use-after-free or callback deadlocks.
//   * Each source runs an isolated `VideoPlayoutBuffer` (Fixed Delay 5..250 ms or Adaptive Wi-Fi)
//     with non-blocking Frame Hold, Smart IDR requests, and branded Lavender Standby placeholder.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "audio/intercom_engine.hpp"
#include "common/image.hpp"
#include "lanvid/lanvid_sdk.h"
#include "net/packet.hpp"
#include "video/video_bus.hpp"
#include "video/video_playout_buffer.hpp"

namespace lv {
class UdpReceiver;
}

namespace lv::obs {

enum class VideoPixelFormat : int {
    kBGRA = 0,
    kNV12 = 1,
    kI420 = 2,
};

enum class ColorSpace : int {
    kRec709 = 0,
    kBT2100_PQ = 1,
    kBT2100_HLG = 2,
};

enum class ConnectionMode : int {
    kAutoDiscovery = 0,  // Select from LAN phones, studios, or local LanVidVideoBus_v1 SHM
    kDedicatedPort = 1,  // Bind dedicated UDP port (5004/5006/5008...) + optional peer IP filter
};

enum class JitterPreset : int {
    kFixed16ms = 0,
    kFixed33ms = 1,
    kFixed50ms = 2,
    kFixed80ms = 3,
    kAdaptiveWifi = 4,
    kCustomFixed = 5,
    // Compatibility aliases
    kUltraLow = kFixed16ms,
    kBalanced = kFixed33ms,
    kSmooth = kFixed50ms,
};

enum class AudioRouteMode : int {
    kStereoMix = 0,
    kStereoPair = 1,
    kMonoSum = 2,
    kVideoOnly = 3,
};

enum class TallyControlMode : int {
    kAuto = 0,
    kStandby = 1,
    kPreview = 2,
    kProgram = 3,
};

enum class CameraQualityPreset : int {
    kProxy360p = 0,   // 640x360 @ 30fps, 4 Mbps
    kHd720p = 1,      // 1280x720 @ 60fps, 12 Mbps
    kStudio1080p = 2, // 1920x1080 @ 60fps, 25 Mbps
    kMaster4K60 = 3,  // 3840x2160 @ 60fps, 85 Mbps
};

enum class DiscoveryOrigin : int {
    kTallyPhone = 0,     // Connected LanVid Camera phone on Tally :5022
    kSharedMemory = 1,   // Local LanVid Video slot via LanVidVideoBus_v1 ("LanVid Cam 1..8" / Program)
    kLanBroadcast = 2,   // LAN UDP Broadcast / mDNS (:5021 / :5023 / :5099)
};

struct DiscoveredCameraItem {
    std::string id;                // Stable selection token e.g. "shm:0", "tally:192.168.1.20:5006", "lan:..."
    std::string display_name;      // e.g. "[CAM 1] iPhone 16 Pro (192.168.1.20:5006)" or "[SHM] LanVid Cam 1"
    std::string ip;
    uint16_t port = 5006;
    uint32_t camera_id = 0;
    uint8_t assigned_cam_number = 0;
    std::string assigned_cam_tag;
    int shm_slot = -1;             // 0..8 when origin == kSharedMemory
    DiscoveryOrigin origin = DiscoveryOrigin::kLanBroadcast;
    std::string role;
    bool online = true;
};

struct PortConflictInfo {
    bool has_conflict = false;
    uint16_t port = 0;
    std::string conflicting_source;
};

struct RemoteCameraControl {
    CameraQualityPreset quality = CameraQualityPreset::kStudio1080p;
    uint8_t fps = 60;
    uint8_t codec = uint8_t(VideoCodecId::kH264);
    uint32_t bitrate_kbps = 25000;
    uint8_t lens = uint8_t(CameraLens::kLensWide);
    uint16_t video_delay_ms = 33;
    bool fixed_delay_mode = true;
};

struct VideoFrameView {
    const uint8_t* data[4] = {nullptr, nullptr, nullptr, nullptr};
    uint32_t linesize[4] = {0, 0, 0, 0};
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t timestamp_ns = 0;
    VideoPixelFormat format = VideoPixelFormat::kBGRA;
    ColorSpace color_space = ColorSpace::kRec709;
    bool full_range = true;
};

struct AudioBlockView {
    const float* planes[2] = {nullptr, nullptr};
    uint32_t frames = 0;
    uint32_t sample_rate = 48000;
    uint32_t channels = 2;
    uint64_t timestamp_ns = 0;
};

struct SourceConfig {
    std::string source_name = "LanVid Camera";
    ConnectionMode connection_mode = ConnectionMode::kAutoDiscovery;
    std::string device_id;         // Selected DiscoveredCameraItem::id in kAutoDiscovery mode
    std::string bind_ip = "0.0.0.0";
    uint16_t listen_port = 5006;
    std::string peer_ip;           // Optional sender IP filter in dedicated/auto mode
    uint32_t camera_id = 1;
    uint8_t assigned_cam_number = 1;  // 1..16 badge sent to phone
    std::string assigned_cam_tag = "CAM 1";
    int shm_slot = -1;             // >= 0 reads directly from LanVidVideoBus_v1
    std::string shm_bus_name;      // "" = default LanVidVideoBus_v1

    JitterPreset jitter_preset = JitterPreset::kFixed33ms;
    uint32_t video_delay_ms = 33;  // 5..250 ms
    bool fixed_delay_mode = true;
    bool show_placeholder = true;  // Lavender standby card when signal absent > 750 ms

    AudioRouteMode audio_route = AudioRouteMode::kStereoMix;
    int audio_pair_index = 0;
    uint32_t audio_latency_ms = 20;
    uint32_t audio_channels = 2;

    bool auto_tally = true;
    bool only_on_air_when_live = false;
    bool sync_remote_rec = false;
};

struct SourceTelemetry {
    uint64_t uid = 0;
    uint32_t camera_id = 0;
    uint8_t assigned_cam_number = 0;
    std::string assigned_cam_tag;
    std::string name;
    std::string peer_ip;
    uint16_t listen_port = 0;
    int shm_slot = -1;
    uint32_t width = 0;
    uint32_t height = 0;
    double fps = 0.0;
    std::string codec_name = "DCT";
    double mbps = 0.0;
    double loss_pct = 0.0;
    double playout_delay_ms = 33.0;
    double playout_jitter_ms = 0.0;
    uint64_t frames_rx = 0;
    uint64_t dropped_frames = 0;
    uint8_t battery_pct = 100;
    int8_t temp_c = 30;
    uint8_t effective_tally = lv::kTallyStandby;
    TallyControlMode tally_mode = TallyControlMode::kAuto;
    bool remote_rec_active = false;
    bool operator_speaking = false;
    bool talk_to_cam = false;
    bool listen_from_cam = true;
    bool connected = false;
    bool config_synced = false;
    bool has_port_conflict = false;
};

struct OutputConfig {
    std::string dest_ip = "127.0.0.1";
    uint16_t dest_port = 5010;
    uint32_t width = 1920;
    uint32_t height = 1080;
    uint32_t fps = 60;
    int codec = 0;             // 0 = LanVid DCT, 1 = H.264, 2 = H.265
    int bitrate_kbps = 25000;  // 2000 .. 150000 kbps
    int quality = 82;
    int fec_group = 6;
    bool adaptive = true;

    bool operator==(const OutputConfig& o) const {
        return dest_ip == o.dest_ip && dest_port == o.dest_port && width == o.width && height == o.height &&
               fps == o.fps && codec == o.codec && bitrate_kbps == o.bitrate_kbps && quality == o.quality &&
               fec_group == o.fec_group && adaptive == o.adaptive;
    }
    bool operator!=(const OutputConfig& o) const { return !(*this == o); }
};

struct ObsTallyInputs {
    bool in_program = false;
    bool in_preview = false;
    bool studio_mode = false;
    bool obs_streaming = false;
    bool obs_recording = false;
    bool only_on_air_when_live = false;
    bool sync_remote_rec = false;
    TallyControlMode mode = TallyControlMode::kAuto;
};

struct ObsTallyDecision {
    uint8_t tally_state = lv::kTallyStandby;
    bool rec_trigger = false;
};

ObsTallyDecision evaluate_auto_tally(const ObsTallyInputs& in);
uint32_t jitter_preset_to_ms(JitterPreset preset);
LV_ControlSyncState make_quality_preset_control_sync(CameraQualityPreset preset, uint64_t revision, uint32_t origin_id);

// -----------------------------------------------------------------------------
// LanVidObsSourceEngine — Isolated, ref-counted source engine (360p-4K60 + 64ch audio)
// -----------------------------------------------------------------------------
class LanVidObsSourceEngine : public std::enable_shared_from_this<LanVidObsSourceEngine> {
public:
    using VideoCallback = std::function<void(const VideoFrameView&)>;
    using AudioCallback = std::function<void(const AudioBlockView&)>;

    static std::shared_ptr<LanVidObsSourceEngine> create();
    LanVidObsSourceEngine();
    ~LanVidObsSourceEngine();

    LanVidObsSourceEngine(const LanVidObsSourceEngine&) = delete;
    LanVidObsSourceEngine& operator=(const LanVidObsSourceEngine&) = delete;

    uint64_t uid() const { return uid_; }
    bool start(const SourceConfig& cfg, VideoCallback on_video, AudioCallback on_audio);
    void stop();
    bool is_running() const { return running_.load(std::memory_order_acquire); }

    void update_config(const SourceConfig& cfg);
    void request_idr_keyframe();
    void reset_buffers();

    bool assign_camera_number(uint8_t cam_number, const std::string& tag);
    bool send_quality_preset(CameraQualityPreset preset);
    bool send_remote_camera_control(const RemoteCameraControl& ctl);

    void set_obs_visibility(bool in_program, bool in_preview);
    void set_tally_mode(TallyControlMode mode);
    void set_manual_rec(bool rec_active);

    // Feeds an external/synthetic video frame (unit tests & direct zero-copy hooks).
    void ingest_video_frame_bgra(const uint8_t* bgra, uint32_t width, uint32_t height, uint32_t stride,
                                 uint64_t timestamp_ns, ColorSpace cs = ColorSpace::kRec709,
                                 bool is_keyframe = true, uint32_t media_ts_us32 = 0);

    // Routes interleaved N-channel float32 audio (up to 64ch) according to AudioRouteMode.
    void route_multichannel_audio(const float* interleaved, uint16_t num_channels, uint32_t num_frames,
                                  uint32_t sample_rate, uint64_t timestamp_ns);

    uint16_t bound_port() const;
    SourceConfig config() const;
    SourceTelemetry telemetry() const;
    ObsTallyDecision compute_current_tally(bool studio_mode, bool streaming, bool recording) const;

private:
    void worker_loop();
    void emit_video_image(const Image& img, uint64_t ts_ns, ColorSpace cs);
    void emit_placeholder_if_needed(uint64_t now_us);

    uint64_t uid_ = 0;
    mutable std::mutex mu_;
    SourceConfig cfg_;
    std::mutex cb_mu_;
    VideoCallback on_video_;
    AudioCallback on_audio_;

    std::unique_ptr<lv::UdpReceiver> rx_;
    lv::VideoBus shm_reader_;
    lv::VideoPlayoutBuffer playout_;
    uint64_t playout_seq_ = 0;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_worker_{false};
    std::atomic<bool> reset_requested_{false};
    std::thread worker_;

    bool in_program_ = false;
    bool in_preview_ = false;
    TallyControlMode tally_mode_ = TallyControlMode::kAuto;
    bool manual_rec_ = false;
    uint8_t last_effective_tally_ = lv::kTallyStandby;
    uint64_t config_sync_rev_ = 1;
    bool config_synced_ = false;

    uint32_t last_w_ = 0;
    uint32_t last_h_ = 0;
    uint64_t frames_rx_ = 0;
    uint64_t dropped_frames_ = 0;
    double measured_fps_ = 0.0;
    double measured_mbps_ = 0.0;
    double measured_loss_pct_ = 0.0;
    std::string codec_name_ = "DCT";
    std::string detected_peer_ip_;
    uint64_t last_rx_frame_us_ = 0;
    uint64_t last_placeholder_us_ = 0;

    std::vector<uint8_t> placeholder_buf_;
    std::vector<uint8_t> shm_buf_;
    std::vector<float> audio_interleaved_;
    std::vector<float> audio_l_;
    std::vector<float> audio_r_;
};

// -----------------------------------------------------------------------------
// LanVidObsOutputEngine — Safe OBS Program / Filter -> LanVid Network sender
// -----------------------------------------------------------------------------
class LanVidObsOutputEngine {
public:
    LanVidObsOutputEngine();
    ~LanVidObsOutputEngine();

    bool start(const OutputConfig& cfg);
    void stop();
    bool is_running() const { return running_.load(std::memory_order_acquire); }

    bool push_bgra(const uint8_t* bgra, uint32_t width, uint32_t height, uint32_t stride);
    bool push_i420(const uint8_t* y, const uint8_t* u, const uint8_t* v,
                   int stride_y, int stride_u, int stride_v, uint32_t width, uint32_t height);
    bool push_nv12(const uint8_t* y, const uint8_t* uv, int stride_y, int stride_uv,
                   uint32_t width, uint32_t height);
    bool push_audio_pcm16(const int16_t* stereo_interleaved, uint32_t frames);

    OutputConfig config() const;
    uint64_t frames_sent() const { return frames_sent_.load(std::memory_order_relaxed); }

private:
    mutable std::mutex mu_;
    OutputConfig cfg_;
    LvSenderHandle* tx_ = nullptr;
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> frames_sent_{0};
};

// -----------------------------------------------------------------------------
// LanVidObsRuntime — Thread-safe Global Singleton (Discovery, Port Checks, Tally & Intercom)
// -----------------------------------------------------------------------------
class LanVidObsRuntime {
public:
    static LanVidObsRuntime& instance();

    bool startup(uint16_t tally_port = lv::kDefaultTallyIntercomPort);
    bool ensure_studio_started(uint16_t port = lv::kDefaultTallyIntercomPort) { return startup(port); }
    void shutdown();
    bool is_started() const { return started_.load(std::memory_order_acquire); }
    uint16_t studio_port() const;
    bool tally_port_fallback_active() const { return tally_fallback_.load(std::memory_order_relaxed); }

    void register_source(const std::shared_ptr<LanVidObsSourceEngine>& src);
    void unregister_source(uint64_t uid);
    uint16_t allocate_next_port();
    PortConflictInfo check_port_conflict(uint16_t port, uint64_t exclude_uid = 0) const;

    // Continuous non-blocking discovery cache (safe to call from OBS UI / Qt / video threads)
    std::vector<DiscoveredCameraItem> discovered_cameras() const;
    void trigger_discovery_refresh();
    void refresh_discovery_now();
    std::string diagnose_network_and_ports() const;

    // Test hooks for deterministic multi-camera verification
    void inject_discovered_camera_for_test(const DiscoveredCameraItem& item);
    void clear_test_cameras();

    void set_obs_global_state(bool studio_mode, bool streaming, bool recording);
    void sync_all_tallies();
    void poll_studio();

    void set_director_talk(uint32_t camera_id, bool talk_to_cam, bool listen_from_cam, bool all_call);
    void set_director_mic_active(bool talk_active, bool listen_muted, float rx_gain, float sidetone_gain);
    void push_director_mic_pcm(const float* mono_48k, int frames);
    int pull_director_rx_pcm(float* mono_48k, int frames);

    void panic_reset_all_buffers();
    std::vector<SourceTelemetry> snapshot_cameras() const;
    std::vector<LV_DiscoveredPeer> scan_lan_devices(int timeout_ms = 250) const;

    lv::TallyIntercomEngine& tally_engine() { return tally_intercom_; }

private:
    LanVidObsRuntime() = default;
    ~LanVidObsRuntime();

    void runtime_loop();
    void run_discovery_pass();
    std::vector<std::shared_ptr<LanVidObsSourceEngine>> active_sources_locked() const;

    mutable std::mutex mu_;
    mutable std::vector<std::weak_ptr<LanVidObsSourceEngine>> sources_;
    lv::TallyIntercomEngine tally_intercom_;
    std::atomic<bool> started_{false};
    std::atomic<bool> stop_{false};
    std::atomic<bool> tally_fallback_{false};
    std::atomic<uint16_t> next_port_{5004};
    std::condition_variable cv_;
    std::mutex cv_mu_;
    std::atomic<bool> refresh_requested_{false};
    std::thread runtime_thread_;

    mutable std::mutex disc_mu_;
    std::vector<DiscoveredCameraItem> cached_cameras_;
    std::vector<DiscoveredCameraItem> injected_test_cameras_;
    lv::VideoBus disc_shm_bus_;

    bool obs_studio_mode_ = false;
    bool obs_streaming_ = false;
    bool obs_recording_ = false;
};

using LanVidObsManager = LanVidObsRuntime;

}  // namespace lv::obs
