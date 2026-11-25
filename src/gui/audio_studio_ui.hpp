#pragma once
// LanVid Audio — dedicated AoIP, Patchbay/64x64 Matrix, Personal Monitor, and Grid L2 (256ch) desktop app.
// Default ports (non-conflicting with LanVid Video): Audio 5004, Monitor 5020, Discovery 5021 (role = "audio").

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "audio/multi_device_matrix.hpp"
#include "audio/personal_monitor_bus.hpp"
#include "grid/dsp_server.hpp"
#include "grid/grid_node.hpp"
#include "gui/app_config.hpp"
#include "gui/studio_common.hpp"
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

    uint16_t bound_audio_port() const { return bound_audio_port_; }
    uint16_t bound_monitor_port() const { return monitor_srv_ ? monitor_srv_->bound_port() : 0; }
    uint16_t bound_discovery_port() const { return bound_disc_port_; }
    AppConfig& config() { return cfg_; }
    const AppConfig& config() const { return cfg_; }

private:
    void refresh_nics();
    void apply_nics();
    void start_lan_scan();
    void draw_sidebar();
    void draw_top_bar();
    void draw_connect_tab();
    void draw_patchbay_tab();
    void draw_monitor_tab();
    void draw_devices_grid_tab();

    PlatformHost host_{};
    std::string config_path_;
    AppConfig cfg_{};

    int active_tab_ = 0;         // 0=Connect, 1=Patchbay/Matrix, 2=Personal Monitor, 3=Devices & Grid
    bool patchbay_matrix_mode_ = false;
    int drag_src_slot_ = -1;

    std::atomic<bool> services_running_{false};
    uint16_t bound_audio_port_ = lv::kAudioStreamPort;
    uint16_t bound_disc_port_ = lv::kAudioDiscoveryPort;

    lv::UdpSocket audio_sock_;
    lv::DiscoveryResponder discovery_;
    std::unique_ptr<lv::PersonalMonitorServer> monitor_srv_;
    lv::MdnsAdvertiser mdns_;
    lv::MultiDeviceAudioMatrix matrix_;
    std::unique_ptr<lv::grid::GridNode> grid_node_;
    std::unique_ptr<lv::grid::GridDspServer> dsp_server_;

    std::vector<lv::NetworkInterfaceInfo> nics_;
    std::mutex scan_mu_;
    std::vector<lv::DiscoveredReceiver> peers_;
    std::atomic<bool> scanning_{false};
    std::thread scan_thread_;

    char studio_name_buf_[64] = "LanVid Audio";
    char peer_ip_buf_[64] = "127.0.0.1";
    float anim_time_ = 0.0f;
};

}  // namespace lv::gui
