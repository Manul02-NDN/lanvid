#pragma once
// Headless audio hub engine for LanVid Audio (AoIP, 64x64 Patchbay, Grid L2, Personal Monitor, MIDI2).
// Ports: Audio 5004, Personal Monitor 5020, Discovery 5021 (role = "audio").

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "audio/multi_device_matrix.hpp"
#include "common_ui/json_config.hpp"
#include "grid/grid_node.hpp"
#include "lanvid/lanvid_sdk.h"
#include "net/discovery.hpp"
#include "net/mdns_advertiser.hpp"

namespace lv::apps {

struct AudioAppConfig {
    std::string studio_name = "LanVid Audio";
    std::string peer_ip = "127.0.0.1";
    int peer_port = 5004;
    int listen_port = 5004;
    int monitor_port = 5020;
    int discovery_port = 5021;
    int channels = 8;               // 2 / 8 / 16 / 32 / 64
    int latency_ms = 20;            // 2..2000 ms
    int audio_codec = 0;            // 0=PCM32F, 1=PCM24, 2=PCM16, 3=Opus
    int bitrate_kbps = 0;           // 0=Lossless, 256/192/128/96/64
    bool adaptive_jitter = true;
    bool wifi_redundancy = false;
    bool midi_only = false;
    bool monitor_flow_enabled = true;
    bool dark_theme = false;
    int active_page = 0;
    int active_subtab = 0;

    // Grid L2 (256ch)
    int network_engine_mode = 0;    // 0=Classic UDP, 1=Grid L2, 2=Hybrid
    std::string grid_nic_name;
    int grid_channels = 256;
    bool grid_clock_master = true;

    std::string primary_nic_ip;
    std::string secondary_nic_ip;
    bool dual_nic_enabled = false;
    std::vector<std::string> monitor_channel_names;
};

class LanVidAudioService {
public:
    explicit LanVidAudioService(std::string config_path_override = "");
    ~LanVidAudioService();

    LanVidAudioService(const LanVidAudioService&) = delete;
    LanVidAudioService& operator=(const LanVidAudioService&) = delete;

    bool start_services(uint16_t audio_port = lv::kAudioStreamPort,
                        uint16_t monitor_port = lv::kAudioMonitorPort,
                        uint16_t discovery_port = lv::kAudioDiscoveryPort);
    void stop_services();
    bool services_running() const { return services_running_.load(std::memory_order_acquire); }

    bool start_duplex_stream();
    void stop_duplex_stream();
    bool duplex_running() const { return duplex_running_.load(std::memory_order_acquire); }

    void apply_live_settings();
    void set_channel_routing(bool is_tx, int src_ch, int dst_ch);
    void reset_routing_identity();
    void set_channel_mute_solo(bool is_tx, uint16_t ch, int mute, int solo);
    void send_test_midi_note(uint8_t note, bool on);
    void toggle_grid_l2(int mode);

    void scan_nodes();
    std::vector<LV_AudioNode> discovered_nodes() const;
    std::vector<LV_MonitorClientInfo> monitor_clients() const;
    std::vector<LvNetInterfaceInfo> network_interfaces() const;
    LV_DuplexStats duplex_stats() const;
    std::vector<lv::SystemAudioDeviceEntry> system_outputs() const;
    std::vector<lv::SystemAudioDeviceEntry> system_inputs() const;
    std::vector<lv::grid::L2AdapterInfo> grid_adapters() const;

    uint16_t bound_audio_port() const { return bound_audio_port_; }
    uint16_t bound_monitor_port() const { return monitor_srv_ ? lv_monitor_server_port(monitor_srv_) : 0; }
    uint16_t bound_discovery_port() const { return bound_disc_port_; }
    std::string config_path() const { return config_path_; }

    AudioAppConfig& config() { return cfg_; }
    const AudioAppConfig& config() const { return cfg_; }
    lv::MultiDeviceAudioMatrix& matrix() { return matrix_; }
    bool grid_open() const { return grid_node_ && grid_node_->is_open(); }
    std::string grid_status() const { return grid_status_; }

    bool load_config();
    bool save_config() const;

    std::array<int8_t, 64> tx_routing{};
    std::array<int8_t, 64> rx_routing{};
    std::array<bool, 64> ch_mute{};
    std::array<bool, 64> ch_solo{};

private:
    std::string config_path_;
    AudioAppConfig cfg_{};

    std::atomic<bool> services_running_{false};
    std::atomic<bool> duplex_running_{false};
    uint16_t bound_audio_port_ = lv::kAudioStreamPort;
    uint16_t bound_disc_port_ = lv::kAudioDiscoveryPort;

    LvDuplexHandle* duplex_ = nullptr;
    LvMonitorServerHandle* monitor_srv_ = nullptr;
    lv::DiscoveryResponder discovery_;
    lv::MdnsAdvertiser mdns_;
    lv::MultiDeviceAudioMatrix matrix_;
    std::unique_ptr<lv::grid::GridNode> grid_node_;
    std::string grid_status_ = "Off";

    mutable std::mutex scan_mu_;
    std::vector<LV_AudioNode> cached_nodes_;
    std::vector<lv::grid::L2AdapterInfo> cached_adapters_;
};

}  // namespace lv::apps
