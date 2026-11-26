#pragma once
// VideoHubEngine — Clean headless coordinator composing CameraSlotEngine[16], HubDiscovery,
// IsoMultiRecorder, VirtualCamBridge, and StudioTallyIntercomHub for LanVid Video Hub.

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include "common/image.hpp"
#include "net/udp_socket.hpp"
#include "video_hub/camera_slot.hpp"
#include "video_hub/hub_discovery.hpp"
#include "video_hub/iso_recorder.hpp"
#include "video_hub/tally_intercom_hub.hpp"
#include "video_hub/vcam_bus.hpp"

namespace lv::hub {

struct VideoHubConfig {
    std::string hub_name = "LanVid Video Hub";
    std::string bind_ip = "0.0.0.0";
    uint16_t video_port = lv::kVideoStreamPort;        // 5006
    uint16_t tally_port = lv::kVideoTallyPort;         // 5022
    uint16_t discovery_port = lv::kVideoDiscoveryPort; // 5023
    int port_mode = 0;                                 // 0 = shared demux on video_port, 1 = video_port + 2*slot
    uint32_t max_slots = 8;                            // 1..16
    bool auto_assign = true;
    bool publish_vcam_bus = true;
    uint32_t vcam_count = 8;
    bool vcam_placeholder = true;
    std::string vcam_bus_name;                         // "" = LanVidVideoBus_v1
    std::string primary_nic_ip;
    std::string secondary_nic_ip;
    bool sync_remote_rec = false;
    uint16_t default_delay_ms = 33;
    bool default_fixed_delay = true;
    std::array<std::string, kMaxHubSlots> slot_labels{};
    std::array<std::string, kMaxHubSlots> slot_ips{};
    std::array<std::string, kMaxHubSlots> slot_uids{};
    std::array<uint8_t, kMaxHubSlots> slot_cam_numbers{};
    std::array<std::string, kMaxHubSlots> slot_cam_tags{};
    std::array<uint16_t, kMaxHubSlots> slot_delays_ms{};
    std::array<bool, kMaxHubSlots> slot_fixed_modes{};
};

class VideoHubEngine {
public:
    VideoHubEngine();
    ~VideoHubEngine();

    VideoHubEngine(const VideoHubEngine&) = delete;
    VideoHubEngine& operator=(const VideoHubEngine&) = delete;

    bool start(const VideoHubConfig& cfg);
    void stop();
    bool running() const { return running_.load(std::memory_order_acquire); }
    const VideoHubConfig& config() const { return cfg_; }

    uint16_t bound_video_port() const { return bound_video_port_; }
    uint16_t bound_tally_port() const { return tally_.bound_port(); }
    uint16_t bound_discovery_port() const { return cfg_.discovery_port; }
    uint32_t slot_count() const { return cfg_.max_slots; }

    // Periodic main-thread service tick (polls Tally/Intercom, syncs phone telemetry & VCam metadata).
    void poll();

    // ---- Camera Slots ----
    CameraSlotSnapshot slot_snapshot(uint32_t slot) const;
    std::array<CameraSlotSnapshot, kMaxHubSlots> all_snapshots() const;
    bool poll_slot_frame(uint32_t slot, lv::Image& out, uint64_t& inout_seq);
    CameraSlotEngine* slot_engine(uint32_t slot);

    void set_slot_label(uint32_t slot, const std::string& label);
    void pin_slot(uint32_t slot, const std::string& ip, const std::string& uid = "");
    void release_slot(uint32_t slot);
    void connect_peer_to_slot(uint32_t slot, const std::string& ip, uint16_t port, const std::string& uid,
                              const std::string& name);

    // ---- Switcher (Preview / Program + Auto-Tally) ----
    void set_program_slot(int slot);
    int program_slot() const { return program_slot_.load(std::memory_order_relaxed); }
    void set_preview_slot(int slot);
    int preview_slot() const { return preview_slot_.load(std::memory_order_relaxed); }
    void cut_preview_to_program();

    // ---- Bidirectional Config Sync & Badge Assignment ----
    bool assign_slot_badge(uint32_t slot, uint8_t cam_number, const std::string& custom_tag = "",
                           bool push_sync = true);
    bool apply_slot_remote_settings(uint32_t slot, const CameraRemoteSettings& s, bool push_sync = true);
    void set_slot_playout_delay(uint32_t slot, uint32_t delay_ms, bool fixed_mode, bool push_sync = true);
    void force_slot_idr(uint32_t slot);

    // ---- Parallel ISO Recording ----
    bool rec_start_all(const IsoMultiSessionConfig& sess, bool sync_remote_rec = false);
    void rec_stop_all(bool sync_remote_rec = false);
    bool rec_slot(uint32_t slot, bool on, bool sync_remote_rec = false);
    void set_sync_remote_rec(bool enabled);
    bool sync_remote_rec() const { return sync_remote_rec_.load(std::memory_order_relaxed); }

    // ---- Virtual Cameras & Network ----
    void set_vcam_config(uint32_t count, bool placeholder, int lip_sync_ms);
    void set_network_interfaces(const std::string& primary_ip, const std::string& secondary_ip);

    // ---- Test / Relay injection ----
    void inject_datagram(const uint8_t* data, size_t len, uint32_t src_ip_be, uint16_t src_port);

    HubDiscovery& discovery() { return discovery_; }
    const HubDiscovery& discovery() const { return discovery_; }
    IsoMultiRecorder& iso() { return iso_; }
    const IsoMultiRecorder& iso() const { return iso_; }
    VirtualCamBridge& vcam() { return vcam_; }
    const VirtualCamBridge& vcam() const { return vcam_; }
    StudioTallyIntercomHub& tally_intercom() { return tally_; }
    const StudioTallyIntercomHub& tally_intercom() const { return tally_; }

private:
    void demux_loop();
    int resolve_sender_slot(uint32_t ip_be, uint16_t port, bool allow_assign);
    void on_incoming_config_sync(const lv::VideoConfigSyncPayload& p);
    void push_tallies_to_phones();

    VideoHubConfig cfg_{};
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_demux_{false};
    uint16_t bound_video_port_ = 0;

    std::unique_ptr<lv::UdpSocket> demux_sock_;
    std::thread demux_thread_;

    std::array<std::unique_ptr<CameraSlotEngine>, kMaxHubSlots> slots_;
    mutable std::mutex route_mu_;
    std::unordered_map<uint64_t, uint32_t> endpoint_to_slot_;

    HubDiscovery discovery_;
    IsoMultiRecorder iso_;
    VirtualCamBridge vcam_;
    StudioTallyIntercomHub tally_;

    std::atomic<int> program_slot_{0};
    std::atomic<int> preview_slot_{1};
    std::atomic<bool> sync_remote_rec_{false};
    std::array<bool, kMaxHubSlots> phone_rec_active_{};
    uint64_t last_tally_sync_us_ = 0;
    uint64_t last_vcam_meta_us_ = 0;
};

}  // namespace lv::hub
