#pragma once
// obs_lanvid_core.hpp — Core C++ engine for the obs-lanvid OBS Studio plugin.
// Decouples LanVid network I/O (360p-4K60 video, 64ch AoIP audio, Auto-Tally, Duplex Intercom,
// ControlSync quality presets, and Program Output / Send Filter) from OBS/Qt UI layers so it
// can be compiled and thoroughly tested both inside OBS Studio and in headless CI.

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "audio/intercom_engine.hpp"
#include "lanvid/lanvid_sdk.h"
#include "net/packet.hpp"

namespace lv::obs {

// Supported video pixel formats mapped directly to OBS video_format (zero-copy friendly).
enum class VideoPixelFormat : int {
    kBGRA = 0,  // VIDEO_FORMAT_BGRA
    kNV12 = 1,  // VIDEO_FORMAT_NV12
    kI420 = 2,  // VIDEO_FORMAT_I420
};

enum class ColorSpace : int {
    kRec709 = 0,     // VIDEO_CS_709
    kBT2100_PQ = 1,  // VIDEO_CS_2100_PQ
    kBT2100_HLG = 2, // VIDEO_CS_2100_HLG
};

enum class JitterPreset : int {
    kUltraLow = 0,  // 5-10 ms (wired Ethernet / local Wi-Fi 6E)
    kBalanced = 1,  // 15-30 ms (default studio Wi-Fi)
    kSmooth = 2,    // 50-100 ms (broadcast safety buffer)
};

enum class AudioRouteMode : int {
    kStereoMix = 0,   // Ch 1-2 stereo mix
    kStereoPair = 1,  // Specific stereo pair (pair_index = 0..31 -> Ch 1-2 .. Ch 63-64)
    kMonoSum = 2,     // Mono sum of active channels
    kVideoOnly = 3,   // Audio disabled (e.g. routed via LanVid Virtual ASIO/CoreAudio)
};

enum class TallyControlMode : int {
    kAuto = 0,     // Track OBS Program / Preview / Studio Mode automatically
    kStandby = 1,  // Force Standby (Off)
    kPreview = 2,  // Force Preview (Green)
    kProgram = 3,  // Force Program ON AIR (Red)
};

enum class CameraQualityPreset : int {
    kProxy360p = 0,   // 640x360 @ 30fps, 3 Mbps
    kStudio1080p = 1, // 1920x1080 @ 60fps, 25 Mbps
    kMaster4K60 = 2,  // 3840x2160 @ 60fps, 85 Mbps
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
    const float* planes[2] = {nullptr, nullptr};  // planar L/R (or mono in planes[0])
    uint32_t frames = 0;
    uint32_t sample_rate = 48000;
    uint32_t channels = 2;
    uint64_t timestamp_ns = 0;
};

struct SourceConfig {
    std::string source_name = "LanVid Camera";
    std::string bind_ip = "0.0.0.0";
    uint16_t listen_port = 5004;
    uint32_t camera_id = 1;
    std::string peer_ip;
    JitterPreset jitter_preset = JitterPreset::kBalanced;
    AudioRouteMode audio_route = AudioRouteMode::kStereoMix;
    int audio_pair_index = 0;       // 0..31 (Ch 1-2 .. Ch 63-64)
    uint32_t audio_latency_ms = 20; // 5 / 10 / 20 / 40 ms
    uint32_t audio_channels = 2;    // 2..64
    bool auto_tally = true;
    bool only_on_air_when_live = false;
    bool sync_remote_rec = false;
};

struct SourceTelemetry {
    uint32_t camera_id = 0;
    std::string name;
    std::string peer_ip;
    uint16_t listen_port = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    double fps = 0.0;
    std::string codec_name = "DCT";
    double mbps = 0.0;
    double loss_pct = 0.0;
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
};

struct OutputConfig {
    std::string dest_ip = "127.0.0.1";
    uint16_t dest_port = 5004;
    uint32_t width = 1920;
    uint32_t height = 1080;
    uint32_t fps = 60;
    int codec = 0;          // 0 = LanVid DCT, 1 = H.264, 2 = HEVC
    int bitrate_kbps = 25000;  // 2000 .. 150000 kbps
    int quality = 82;
    int fec_group = 6;
    bool adaptive = true;
};

// -----------------------------------------------------------------------------
// Auto-Tally State Machine (tracks OBS Program / Preview / Studio Mode / Live)
// -----------------------------------------------------------------------------
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

// Converts a JitterPreset to target buffer latency in ms.
uint32_t jitter_preset_to_ms(JitterPreset preset);

// Builds a ControlSync payload for switching remote smartphone quality presets.
LV_ControlSyncState make_quality_preset_control_sync(CameraQualityPreset preset, uint64_t revision, uint32_t origin_id);

// -----------------------------------------------------------------------------
// LanVidObsSourceEngine — receives video (360p..4K60 + 9:16) & up to 64ch audio
// -----------------------------------------------------------------------------
class LanVidObsSourceEngine {
public:
    using VideoCallback = std::function<void(const VideoFrameView&)>;
    using AudioCallback = std::function<void(const AudioBlockView&)>;

    LanVidObsSourceEngine();
    ~LanVidObsSourceEngine();

    LanVidObsSourceEngine(const LanVidObsSourceEngine&) = delete;
    LanVidObsSourceEngine& operator=(const LanVidObsSourceEngine&) = delete;

    bool start(const SourceConfig& cfg, VideoCallback on_video, AudioCallback on_audio);
    void stop();
    bool is_running() const { return running_.load(std::memory_order_acquire); }

    void update_config(const SourceConfig& cfg);
    void request_idr_keyframe();
    void reset_buffers();

    // Sends a quality preset (360p Proxy / 1080p Studio / 4K60 Master) via kControlSync.
    bool send_quality_preset(CameraQualityPreset preset);

    // Updates OBS scene visibility for Auto-Tally calculation.
    void set_obs_visibility(bool in_program, bool in_preview);
    void set_tally_mode(TallyControlMode mode);
    void set_manual_rec(bool rec_active);

    // Feeds an external/synthetic video frame (also used by unit tests & zero-copy hooks).
    void ingest_video_frame_bgra(const uint8_t* bgra, uint32_t width, uint32_t height, uint32_t stride,
                                 uint64_t timestamp_ns, ColorSpace cs = ColorSpace::kRec709);

    // Routes interleaved N-channel float32 audio (up to 64ch) according to AudioRouteMode.
    void route_multichannel_audio(const float* interleaved, uint16_t num_channels, uint32_t num_frames,
                                  uint32_t sample_rate, uint64_t timestamp_ns);

    uint16_t bound_port() const;
    SourceConfig config() const;
    SourceTelemetry telemetry() const;
    ObsTallyDecision compute_current_tally(bool studio_mode, bool streaming, bool recording) const;

private:
    void worker_loop();

    mutable std::mutex mu_;
    SourceConfig cfg_;
    VideoCallback on_video_;
    AudioCallback on_audio_;

    LvReceiverHandle* rx_ = nullptr;
    std::atomic<bool> running_{false};
    std::atomic<bool> reset_requested_{false};
    std::thread worker_;

    // Auto-Tally per-source state
    bool in_program_ = false;
    bool in_preview_ = false;
    TallyControlMode tally_mode_ = TallyControlMode::kAuto;
    bool manual_rec_ = false;
    uint8_t last_effective_tally_ = lv::kTallyStandby;
    uint64_t control_sync_rev_ = 1;

    // Live frame & audio stats
    uint32_t last_w_ = 0;
    uint32_t last_h_ = 0;
    uint32_t last_frame_id_ = 0;
    uint64_t frames_rx_ = 0;
    uint64_t dropped_frames_ = 0;
    double measured_fps_ = 0.0;
    double measured_mbps_ = 0.0;
    double measured_loss_pct_ = 0.0;

    std::vector<uint8_t> frame_buf_;
    std::vector<float> audio_interleaved_;
    std::vector<float> audio_l_;
    std::vector<float> audio_r_;
};

// -----------------------------------------------------------------------------
// LanVidObsOutputEngine — sends OBS Program / Dedicated Filter to LanVid network
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
// LanVidObsManager — Global registry for OBS Sources, Auto-Tally & Duplex Intercom
// -----------------------------------------------------------------------------
class LanVidObsManager {
public:
    static LanVidObsManager& instance();

    // Starts/stops the shared Studio Tally & Duplex Intercom UDP server (default port 5022).
    bool ensure_studio_started(uint16_t port = lv::kDefaultTallyIntercomPort);
    void shutdown();
    uint16_t studio_port() const;

    void register_source(LanVidObsSourceEngine* src);
    void unregister_source(LanVidObsSourceEngine* src);
    uint16_t allocate_next_port();

    // OBS Frontend global state updates (from obs_frontend_add_event_callback)
    void set_obs_global_state(bool studio_mode, bool streaming, bool recording);
    void sync_all_tallies();
    void poll_studio();

    // Duplex Intercom controls (from Qt6 Control Dock)
    void set_director_talk(uint32_t camera_id, bool talk_to_cam, bool listen_from_cam, bool all_call);
    void set_director_mic_active(bool talk_active, bool listen_muted, float rx_gain, float sidetone_gain);
    void push_director_mic_pcm(const float* mono_48k, int frames);
    int pull_director_rx_pcm(float* mono_48k, int frames);

    // Global actions
    void panic_reset_all_buffers();
    std::vector<SourceTelemetry> snapshot_cameras() const;
    std::vector<LV_DiscoveredPeer> scan_lan_devices(int timeout_ms = 350) const;

    lv::TallyIntercomEngine& tally_engine() { return tally_intercom_; }

private:
    LanVidObsManager() = default;
    ~LanVidObsManager();

    mutable std::mutex mu_;
    std::vector<LanVidObsSourceEngine*> sources_;
    lv::TallyIntercomEngine tally_intercom_;
    std::atomic<uint16_t> next_port_{5004};
    bool obs_studio_mode_ = false;
    bool obs_streaming_ = false;
    bool obs_recording_ = false;
};

}  // namespace lv::obs
