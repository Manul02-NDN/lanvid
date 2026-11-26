#pragma once
// LanVid Audio — dedicated AoIP, Patchbay/64x64 Matrix, Personal Monitor, and Grid L2 (256ch) desktop app.
// Default ports (non-conflicting with LanVid Video): Audio 5004, Monitor 5020, Discovery 5021 (role = "audio").

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "audio/multi_device_matrix.hpp"
#include "audio/personal_monitor_bus.hpp"
#include "audio/shm_audio_host.hpp"
#include "grid/dsp_server.hpp"
#include "grid/grid_node.hpp"
#include "gui/app_config.hpp"
#include "gui/studio_common.hpp"
#include "lanvid/lanvid_sdk.h"
#include "lanvid_c_api.h"
#include "midi/midi_device_manager.hpp"
#include "net/discovery.hpp"
#include "net/mdns_advertiser.hpp"
#include "net/network_interfaces.hpp"

namespace lv::gui {

class LanVidAudioStudioApp final : public IStudioDesktopApp {
public:
    explicit LanVidAudioStudioApp(std::string config_path_override = "");
    ~LanVidAudioStudioApp() override;

    LanVidAudioStudioApp(const LanVidAudioStudioApp&) = delete;
    LanVidAudioStudioApp& operator=(const LanVidAudioStudioApp&) = delete;

    void set_platform_host(const PlatformHost& host) override { host_ = host; }
    void init() override;
    void shutdown() override;
    void render_frame() override;

    const char* app_display_name() const override { return "LanVid Audio"; }
    std::string active_config_path() const override { return config_path_; }

    // Headless service lifecycle (also callable from unit tests without opening a GUI window)
    bool start_services(uint16_t audio_port = lv::kAudioStreamPort,
                        uint16_t monitor_port = lv::kAudioMonitorPort,
                        uint16_t discovery_port = lv::kAudioDiscoveryPort);
    void stop_services();
    bool services_running() const { return services_running_.load(std::memory_order_acquire); }

    // Audio stream lifecycle (Classic AoIP Sender/Receiver/Duplex + Grid L2)
    bool start_stream();
    void stop_stream();
    bool stream_running() const { return sender_running_ || receiver_running_ || duplex_running_; }

    uint16_t bound_audio_port() const { return bound_audio_port_; }
    uint16_t bound_monitor_port() const { return monitor_srv_ ? monitor_srv_->bound_port() : 0; }
    uint16_t bound_discovery_port() const { return bound_disc_port_; }
    AppConfig& config() { return cfg_; }
    const AppConfig& config() const { return cfg_; }
    lv::MultiDeviceAudioMatrix& matrix() { return matrix_; }

private:
    // Initialization & subsystem wiring
    void init_matrix_from_config();
    void sync_matrix_to_config();
    void wire_network_midi_and_sync();
    void enforce_audio_ownership(uint64_t now_us_val);

    // Control Sync (bidirectional parameter sync with echo/loop guard)
    static void on_remote_control_sync(const LV_ControlSyncState* state, void* user_data);
    static void on_network_midi(const LV_MidiEvent* ev, void* user_data);
    void broadcast_control_sync(bool mute_all_panic = false);
    void apply_pending_remote_sync();
    void apply_live_audio_settings();
    void apply_monitor_dsp_to_clients();

    // Network & Device enumeration (cached, never polled every frame)
    void refresh_nics();
    void apply_nics();
    void refresh_device_caches(bool force = false);
    void refresh_grid_adapters();
    void start_lan_scan();
    bool start_grid_node();
    void stop_grid_node();

    // UI Rendering (Top Bar, Sidebar, 4 Pages with nested Pill Subtabs, Root Popups)
    void draw_top_bar();
    void draw_sidebar();
    void draw_subtab_pills(int page_idx, const char* const* labels, int count);
    void draw_root_popups();

    // Page 0: Network & Link
    void draw_network_page();
    void draw_network_connect_subtab();
    void draw_network_nodes_sync_subtab();

    // Page 1: Routing & Patchbay
    void draw_routing_page();
    void draw_routing_patchbay_subtab();
    void draw_routing_matrix_subtab();
    void draw_routing_midi_subtab();

    // Page 2: Personal Monitor
    void draw_monitor_page();
    void draw_monitor_phones_subtab();
    void draw_monitor_channels_dsp_subtab();

    // Page 3: Devices & System
    void draw_devices_page();
    void draw_devices_audio_subtab();
    void draw_devices_grid_subtab();
    void draw_devices_general_subtab();

    PlatformHost host_{};
    std::string config_path_;
    AppConfig cfg_{};

    // Services & sockets
    std::atomic<bool> services_running_{false};
    uint16_t bound_audio_port_ = lv::kAudioStreamPort;
    uint16_t bound_disc_port_ = lv::kAudioDiscoveryPort;
    lv::UdpSocket audio_sock_;
    lv::DiscoveryResponder discovery_;
    std::unique_ptr<lv::PersonalMonitorServer> monitor_srv_;
    lv::MdnsAdvertiser mdns_;

    // Core AoIP C API handles
    LvSenderHandle* sender_ = nullptr;
    LvReceiverHandle* receiver_ = nullptr;
    LvDuplexHandle* duplex_ = nullptr;
    bool sender_running_ = false;
    bool receiver_running_ = false;
    bool duplex_running_ = false;
    LvSenderStats tx_stats_{};
    LvReceiverStats rx_stats_{};
    LV_DuplexStats dpx_stats_{};
    LV_MidiDejitterStats midi_dejitter_stats_{};
    bool midi_dejitter_valid_ = false;
    uint32_t actual_buf_ms_ = 0;

    // Multi-device Audio & MIDI Matrix
    lv::MultiDeviceAudioMatrix matrix_;
    std::atomic<bool> matrix_has_net_tx_routes_{false};
    bool matrix_net_rx_ = false;
    bool matrix_net_tx_ = false;
    bool matrix_vbus_ = false;
    int matrix_active_preset_ = 0;

    // Grid Layer 2 (256ch) + Failover
    std::unique_ptr<lv::grid::GridNode> grid_node_;
    std::unique_ptr<lv::grid::GridDspServer> dsp_server_;
    std::unique_ptr<lv::grid::GridFailoverController> grid_failover_;
    std::vector<lv::grid::L2AdapterInfo> grid_adapters_;
    bool grid_backend_ok_ = false;
    std::string grid_backend_status_;
    std::string grid_last_err_;

    // kControlSync state
    uint32_t sync_origin_id_ = 0;
    uint64_t local_sync_revision_ = 0;
    uint64_t last_applied_remote_revision_ = 0;
    bool applying_remote_sync_ = false;
    std::mutex remote_sync_mu_;
    LV_ControlSyncState pending_remote_sync_{};
    bool has_pending_remote_sync_ = false;
    uint64_t last_synced_us_ = 0;
    uint64_t ownership_us_ = 0;
    uint64_t telemetry_us_ = 0;

    // Cached device lists (refreshed on demand or every 3s)
    std::vector<lv::NetworkInterfaceInfo> nics_;
    std::vector<lv::SystemAudioDeviceEntry> cached_sys_outputs_;
    std::vector<lv::SystemAudioDeviceEntry> cached_sys_inputs_;
    std::vector<lv::AsioDriverEntry> cached_asio_drivers_;
    std::vector<lv::MidiDeviceInfo> cached_midi_inputs_;
    std::vector<lv::MidiDeviceInfo> cached_midi_outputs_;
    uint64_t last_device_refresh_us_ = 0;
    std::string local_lan_ip_ = "127.0.0.1";

    // LAN peer discovery
    std::mutex scan_mu_;
    std::vector<lv::DiscoveredReceiver> peers_;
    std::atomic<bool> scanning_{false};
    std::thread scan_thread_;

    // UI state
    int drag_src_slot_ = -1;
    int selected_link_src_ = -1;
    int selected_link_dst_ = -1;
    int matrix_view_src_slot_ = 0;
    int matrix_view_dst_slot_ = 1;
    int matrix_ch_range_ = 0;  // 0=1-16, 1=17-32, 2=33-48, 3=49-64
    int selected_monitor_slot_ = 0;
    bool master_mute_panic_ = false;
    bool config_dirty_ = false;
    uint64_t config_save_us_ = 0;

    // Root-level popup triggers
    bool open_add_ip_popup_ = false;
    bool open_add_out_dev_popup_ = false;
    bool open_add_in_dev_popup_ = false;
    bool open_qr_popup_ = false;

    char studio_name_buf_[64] = "LanVid Audio";
    char peer_ip_buf_[64] = "127.0.0.1";
    char manual_ip_buf_[64] = "192.168.1.100";
    char status_msg_[160] = "Готово до роботи";
    float anim_time_ = 0.0f;
};

}  // namespace lv::gui
