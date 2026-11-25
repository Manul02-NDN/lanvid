#pragma once
// LanVid Video — dedicated Multi-Camera, Tally (STBY/PVW/PGM), Director Intercom/IFB & OBS Output desktop app.
// Default ports (non-conflicting with LanVid Audio): Video 5006, Tally/Intercom 5022, Discovery 5023 (role = "video").

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "audio/intercom_engine.hpp"
#include "gui/app_config.hpp"
#include "gui/studio_common.hpp"
#include "lanvid_c_api.h"
#include "net/discovery.hpp"
#include "net/mdns_advertiser.hpp"
#include "net/network_interfaces.hpp"

namespace lv::gui {

class LanVidVideoStudioApp final : public IStudioDesktopApp {
public:
    explicit LanVidVideoStudioApp(std::string config_path_override = "");
    ~LanVidVideoStudioApp() override;

    LanVidVideoStudioApp(const LanVidVideoStudioApp&) = delete;
    LanVidVideoStudioApp& operator=(const LanVidVideoStudioApp&) = delete;

    void set_platform_host(const PlatformHost& host) override { host_ = host; }
    void init() override;
    void shutdown() override;
    void render_frame() override;

    const char* app_display_name() const override { return "LanVid Video"; }
    std::string active_config_path() const override { return config_path_; }

    // Headless service lifecycle (also callable from unit tests without opening a GUI window)
    bool start_services(uint16_t video_port = lv::kVideoStreamPort,
                        uint16_t tally_port = lv::kVideoTallyPort,
                        uint16_t discovery_port = lv::kVideoDiscoveryPort);
    void stop_services();
    bool services_running() const { return services_running_.load(std::memory_order_acquire); }

    uint16_t bound_video_port() const { return bound_video_port_; }
    uint16_t bound_tally_port() const { return intercom_.bound_port(); }
    uint16_t bound_discovery_port() const { return bound_disc_port_; }
    AppConfig& config() { return cfg_; }
    const AppConfig& config() const { return cfg_; }

private:
    void refresh_nics();
    void apply_nics();
    void start_lan_scan();
    void poll_video_and_tally();
    void draw_top_bar();
    void draw_sidebar();
    void draw_cameras_tally_tab();
    void draw_intercom_tab();
    void draw_outputs_obs_tab();
    void draw_settings_tab();

    PlatformHost host_{};
    std::string config_path_;
    AppConfig cfg_{};

    int active_tab_ = 0;  // 0=Cameras & Tally, 1=Intercom & IFB, 2=Outputs & OBS, 3=Network & Settings

    std::atomic<bool> services_running_{false};
    uint16_t bound_video_port_ = lv::kVideoStreamPort;
    uint16_t bound_disc_port_ = lv::kVideoDiscoveryPort;

    LvReceiverHandle* rx_ = nullptr;
    IGpuTexture* preview_tex_ = nullptr;
    std::vector<uint8_t> frame_buf_;
    uint32_t last_frame_seq_ = 0;
    LvReceiverStats rx_stats_{};

    lv::TallyIntercomEngine intercom_;
    lv::DiscoveryResponder discovery_;
    lv::MdnsAdvertiser mdns_;

    bool director_ptt_ = false;
    bool director_latch_ = false;
    bool all_call_ = true;
    int selected_pgm_cam_ = 1;
    int selected_pvw_cam_ = 2;

    std::vector<lv::NetworkInterfaceInfo> nics_;
    std::mutex scan_mu_;
    std::vector<lv::DiscoveredReceiver> peers_;
    std::atomic<bool> scanning_{false};
    std::thread scan_thread_;

    char studio_name_buf_[64] = "LanVid Video";
    float anim_time_ = 0.0f;
};

}  // namespace lv::gui
