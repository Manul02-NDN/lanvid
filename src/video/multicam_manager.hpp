#pragma once
// MultiCamManager: receives up to 16 LanVid Camera phones at once (one slot each), keeps per-camera
// telemetry and live previews, publishes every camera to the OS virtual cameras (LanVidVideoBus_v1:
// "LanVid Cam 1..8" + "LanVid Program Out") and records each camera to its own file (ISO recording).
//
// Port modes (both stay compatible with the LanVid Camera app):
//   kSingleDemux    - every phone sends to the base port (5006, the default the app uses); packets are
//                     demultiplexed by sender (IP:port) into per-slot receivers sharing one socket.
//   kSequentialPorts- slot N listens on base + 2*N (5006, 5008, 5010 ...), for manually configured senders.
//
// Recording: H.264 / H.265 cameras are remuxed (zero-transcode) from the received access units;
// LanVid DCT cameras and the optional "PGM Master Mix" are encoded with the platform hardware encoder
// (VideoToolbox / Media Foundation / VA-API) into H.264.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "audio/intercom_engine.hpp"
#include "common/image.hpp"
#include "video/iso_recorder.hpp"
#include "video/video_bus.hpp"
#include "video/video_playout_buffer.hpp"

namespace lv {

class UdpReceiver;
class UdpSocket;

constexpr uint32_t kMultiCamMaxSlots = 16;

enum class MultiCamPortMode : uint8_t { kSingleDemux = 0, kSequentialPorts = 1 };

struct MultiCamConfig {
    std::string bind_ip = "0.0.0.0";
    uint16_t base_port = 5006;
    MultiCamPortMode port_mode = MultiCamPortMode::kSingleDemux;
    uint32_t max_slots = 8;            // 1..16
    bool auto_assign = true;           // bind new senders to the first free slot
    bool publish_video_bus = true;     // feed LanVidVideoBus_v1 (OS virtual cameras)
    std::string video_bus_name;        // "" = LanVidVideoBus_v1 (tests use their own name)
    uint32_t vcam_count = 8;           // number of "LanVid Cam N" devices exposed (0..8)
    bool vcam_placeholder = true;
    uint32_t audio_latency_ms = 40;
    uint32_t video_delay_ms = 33;      // VideoPlayoutBuffer target delay (5..250 ms)
    bool video_fixed_delay = true;     // true = Fixed Delay, false = Adaptive Wi-Fi
};

struct MultiCamSlotInfo {
    int index = 0;                     // 0-based; shown as "Cam index+1"
    uint32_t camera_id = 0;            // from the phone's Tally hello (0 = unknown)
    std::string label;                 // "CAM 1 • Wide"
    std::string source_name;           // phone name (Tally hello label / sender IP)
    std::string sender_ip;
    std::string pinned_ip;             // slot reserved for this phone ("" = any)
    uint16_t listen_port = 0;
    bool bound = false;                // a phone is assigned to this slot
    bool streaming = false;            // frames within the last second
    int width = 0, height = 0;
    double fps = 0.0;
    std::string codec;                 // "H.264" / "H.265" / "DCT"
    double bitrate_mbps = 0.0;
    double latency_ms = 0.0;           // network+decode latency estimate
    int battery_pct = -1;
    int temp_c = -100;
    float audio_peak_dbfs = -120.0f;
    uint8_t tally = 0;                 // kTallyStandby / Preview / Program
    bool recording = false;
    bool vcam_active = false;          // published to the OS virtual camera of this slot
    uint64_t frames = 0;
    // VideoPlayoutBuffer & Camera Badge Sync telemetry
    uint32_t playout_delay_ms = 33;
    double playout_effective_ms = 33.0;
    bool playout_fixed_mode = true;
    double playout_jitter_ms = 0.0;
    uint64_t playout_drops = 0;
    uint64_t playout_holds = 0;
    uint8_t assigned_cam_number = 0;   // 1..16 (0 = unassigned)
    std::string assigned_cam_tag;
    bool config_synced = false;
};

struct IsoSessionConfig {
    std::string output_dir;            // parent folder
    std::string session_name = "LanVid";
    IsoContainer container = IsoContainer::kMp4;
    bool record_program = false;       // extra PGM_Master.<ext>
    uint32_t slot_mask = 0xFFFFu;      // slots included in REC ALL
};

struct IsoSlotStats {
    bool recording = false;
    bool waiting_for_keyframe = false;
    std::string path;
    uint64_t bytes = 0;
    uint64_t duration_us = 0;
    uint64_t frames = 0;
    double write_mbps = 0.0;           // MB/s to disk (moving average)
};

class MultiCamManager {
public:
    MultiCamManager();
    ~MultiCamManager();
    MultiCamManager(const MultiCamManager&) = delete;
    MultiCamManager& operator=(const MultiCamManager&) = delete;

    bool start(const MultiCamConfig& cfg);
    void stop();
    bool running() const { return running_.load(std::memory_order_acquire); }
    const MultiCamConfig& config() const { return cfg_; }
    uint16_t base_port() const { return bound_base_port_; }

    uint32_t slot_count() const { return cfg_.max_slots; }
    MultiCamSlotInfo slot_info(uint32_t slot) const;
    std::vector<MultiCamSlotInfo> slots() const;
    bool set_slot_label(uint32_t slot, const std::string& label);
    // Reserves `slot` for a phone (by IP); the next stream from it lands there. "" clears the pin.
    bool pin_slot(uint32_t slot, const std::string& ip);
    void release_slot(uint32_t slot);

    // Latest decoded frame of a slot as BGRA. Returns false if there is no new frame since `seq`.
    bool latest_frame(uint32_t slot, Image& out, uint64_t& seq) const;

    // Merges Tally / battery / temperature / label from the Tally & Intercom server (matched by IP).
    void update_camera_telemetry(const std::vector<StudioCameraInfo>& cams);

    // Program / preview selection (Program feeds "LanVid Program Out" and the PGM Master recording).
    void set_program_slot(int slot);
    int program_slot() const { return program_slot_.load(std::memory_order_relaxed); }
    void set_preview_slot(int slot) { preview_slot_.store(slot, std::memory_order_relaxed); }
    int preview_slot() const { return preview_slot_.load(std::memory_order_relaxed); }

    // ---- ISO recording ----
    bool rec_start_all(const IsoSessionConfig& session);
    void rec_stop_all();
    bool rec_slot(uint32_t slot, bool on);
    bool recording_any() const;
    IsoSlotStats rec_stats(uint32_t slot) const;
    IsoSlotStats rec_program_stats() const;
    std::string session_dir() const;
    uint64_t session_elapsed_us() const;
    std::string session_timecode() const;  // "HH:MM:SS:FF" of the running session
    static uint64_t disk_free_bytes(const std::string& dir);
    static std::string default_output_dir();

    // ---- OS virtual cameras ----
    void set_vcam(uint32_t count, bool placeholder);
    bool video_bus_open() const;

    // ---- VideoPlayoutBuffer & Camera Badge Sync ----
    void set_slot_playout_config(uint32_t slot, uint32_t delay_ms, bool fixed_mode);
    VideoPlayoutStats slot_playout_stats(uint32_t slot) const;
    void force_slot_idr(uint32_t slot);
    void set_idr_callback(std::function<void(uint32_t slot, uint32_t camera_id)> cb);
    bool set_slot_assigned_camera(uint32_t slot, uint8_t cam_number, const std::string& tag,
                                  const uint8_t rgba[4] = nullptr);
    void set_slot_config_synced(uint32_t slot, bool synced);

    // Feeds one datagram as if it arrived on the base port from `src_ip:src_port` (tests / relays).
    void inject_datagram(const uint8_t* data, size_t len, uint32_t src_ip_be, uint16_t src_port);

    struct Slot;

private:
    Slot* slot_ptr(uint32_t i) const;
    int slot_for_sender(uint32_t ip_be, uint16_t port, bool allow_assign);
    bool ensure_receiver(uint32_t i);
    void demux_loop();
    void on_frame(uint32_t i, const struct CompletedFrame& cf);
    void on_bitstream(uint32_t i, int codec, const uint8_t* data, size_t size, bool key, uint64_t arrival_us);
    void on_audio(uint32_t i, const struct AudioHeader& hdr, const uint8_t* pcm);
    void publish_slot_playout(uint32_t i, uint64_t now) const;
    void iso_worker_loop();
    void drain_iso_queue();
    bool open_slot_recorder(uint32_t i, IsoVideoCodec codec, int w, int h);
    void request_idr(uint32_t i);
    std::string slot_file_path(uint32_t i) const;

    MultiCamConfig cfg_{};
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_{false};
    uint16_t bound_base_port_ = 0;
    std::unique_ptr<UdpSocket> demux_sock_;
    std::thread demux_thread_;
    std::vector<std::unique_ptr<Slot>> slots_;
    mutable std::mutex slots_mu_;  // slot binding / receivers

    std::atomic<int> program_slot_{0};
    std::atomic<int> preview_slot_{-1};

    // session
    mutable std::mutex rec_mu_;
    bool session_active_ = false;
    IsoSessionConfig session_{};
    std::string session_dir_;
    std::atomic<uint64_t> session_t0_us_{0};
    uint64_t session_unix_s_ = 0;
    uint64_t session_tc_frame_ = 0;
    std::unique_ptr<IsoRecorder> program_rec_;
    std::unique_ptr<class IVideoEncoder> program_enc_;
    Image program_scaled_img_;

    // Decoupled background ISO encode/write queue (never blocks video preview or vCam)
    struct IsoJob {
        uint32_t slot = 0;
        Image image;
        uint64_t timestamp_us = 0;
        bool is_program = false;
    };
    std::mutex iso_q_mu_;
    std::condition_variable iso_q_cv_;
    std::condition_variable iso_idle_cv_;
    std::deque<IsoJob> iso_q_;
    bool iso_busy_ = false;
    std::atomic<bool> iso_stop_{false};
    std::thread iso_thread_;

    std::mutex idr_cb_mu_;
    std::function<void(uint32_t slot, uint32_t camera_id)> idr_cb_;

    mutable VideoBus bus_;
    mutable std::vector<uint8_t> nv12_scratch_;
    mutable std::mutex bus_mu_;
};

}  // namespace lv
