#pragma once
// Headless video hub service for LanVid Video (Up to 16 cameras, VideoPlayoutBuffer,
// kVideoConfigSync, IsoMultiRecorder, LanVidVideoBus_v1, Tally/Switcher).
// Ports: Video 5006, Tally 5022, Discovery 5023 (role = "video").

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "common_ui/json_config.hpp"
#include "hub/video_hub_engine.hpp"
#include "lanvid/lanvid_sdk.h"

namespace lv::apps {

struct VideoAppConfig {
    std::string hub_name = "LanVid Video";
    int video_port = lv::kVideoStreamPort;        // 5006
    int tally_port = lv::kVideoTallyPort;         // 5022
    int discovery_port = lv::kVideoDiscoveryPort; // 5023
    int port_mode = 0;                            // 0=shared demux :5006, 1=dedicated per-slot
    int max_slots = 8;                            // 1..16
    int multiview_layout = 2;                     // 1..4 (1x1..4x4)
    int default_delay_ms = 33;                    // 5..250 ms
    bool default_fixed_delay = true;
    int vcam_count = 8;                           // 0..8
    bool vcam_placeholder = true;
    int lip_sync_ms = 0;                          // -200..+200 ms
    std::string iso_output_dir;
    std::string iso_session_name = "LanVid";
    int iso_container = 0;                        // 0=MP4, 1=MKV, 2=MOV
    bool iso_record_program = false;
    int iso_slot_mask = 0xFFFF;
    bool sync_remote_rec = false;
    bool dark_theme = false;
    int active_page = 0;
    int active_subtab_cam = 0;
    int active_subtab_rec = 0;
    int active_subtab_out = 0;
    std::string primary_nic_ip;
    std::string secondary_nic_ip;
    bool dual_nic_enabled = false;
    std::vector<std::string> slot_labels;
    std::vector<std::string> slot_ips;
    std::vector<std::string> slot_uids;
    std::vector<int> slot_cam_numbers;
    std::vector<std::string> slot_cam_tags;
    std::vector<int> slot_delays_ms;
    std::vector<int> slot_fixed_modes;
};

class LanVidVideoService {
public:
    explicit LanVidVideoService(std::string config_path_override = "");
    ~LanVidVideoService();

    LanVidVideoService(const LanVidVideoService&) = delete;
    LanVidVideoService& operator=(const LanVidVideoService&) = delete;

    bool start_services(uint16_t video_port = lv::kVideoStreamPort,
                        uint16_t tally_port = lv::kVideoTallyPort,
                        uint16_t discovery_port = lv::kVideoDiscoveryPort);
    void stop_services();
    bool services_running() const { return services_running_.load(std::memory_order_acquire); }
    void poll();

    uint16_t bound_video_port() const { return hub_.bound_video_port(); }
    uint16_t bound_tally_port() const { return hub_.bound_tally_port(); }
    uint16_t bound_discovery_port() const { return disc_port_; }
    std::string config_path() const { return config_path_; }

    std::vector<LvNetInterfaceInfo> network_interfaces() const;
    VideoAppConfig& config() { return cfg_; }
    const VideoAppConfig& config() const { return cfg_; }
    lv::hub::VideoHubEngine& engine() { return hub_; }
    const lv::hub::VideoHubEngine& engine() const { return hub_; }

    void sync_hub_to_config();
    bool load_config();
    bool save_config() const;

private:
    std::string config_path_;
    VideoAppConfig cfg_{};
    lv::hub::VideoHubEngine hub_;
    std::atomic<bool> services_running_{false};
    uint16_t disc_port_ = lv::kVideoDiscoveryPort;
};

}  // namespace lv::apps
