#pragma once
// LanVid Video Hub — Modular Multi-Camera Hub, VideoPlayoutBuffer, Bidirectional Sync,
// ISO Multi-Recorder, Virtual Cameras (LanVidVideoBus_v1), Tally & Duplex Intercom UI.

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "audio/multi_device_matrix.hpp"
#include "gui/app_config.hpp"
#include "gui/studio_common.hpp"
#include "net/discovery.hpp"
#include "net/network_interfaces.hpp"
#include "video_hub/video_hub_engine.hpp"

namespace lv::gui {

class LanVidVideoHubApp final : public IStudioDesktopApp {
public:
    explicit LanVidVideoHubApp(std::string config_path_override = "");
    ~LanVidVideoHubApp() override;

    LanVidVideoHubApp(const LanVidVideoHubApp&) = delete;
    LanVidVideoHubApp& operator=(const LanVidVideoHubApp&) = delete;

    void set_platform_host(const PlatformHost& host) override { host_ = host; }
    void init() override;
    void shutdown() override;
    void render_frame() override;

    const char* app_display_name() const override { return "LanVid Video Hub"; }
    std::string active_config_path() const override { return config_path_; }

    // Headless service lifecycle (for tests & automation without opening a window)
    bool start_services(uint16_t video_port = lv::kVideoStreamPort,
                        uint16_t tally_port = lv::kVideoTallyPort,
                        uint16_t discovery_port = lv::kVideoDiscoveryPort);
    void stop_services();
    bool services_running() const { return services_running_.load(std::memory_order_acquire); }

    uint16_t bound_video_port() const { return hub_.bound_video_port(); }
    uint16_t bound_tally_port() const { return hub_.bound_tally_port(); }
    uint16_t bound_discovery_port() const { return cfg_.video_port_mode == 0 ? disc_port_ : 0; }

    AppConfig& config() { return cfg_; }
    const AppConfig& config() const { return cfg_; }
    lv::hub::VideoHubEngine& engine() { return hub_; }
    const lv::hub::VideoHubEngine& engine() const { return hub_; }

private:
    void sync_config_to_hub();
    void sync_hub_to_config();
    void refresh_nics();
    void refresh_audio_devices();
    void update_slot_texture(uint32_t slot);
    void draw_slot_preview_box(uint32_t slot, float w, float h, const char* empty_hint);
    void draw_top_bar();
    void draw_sidebar();
    void draw_subtab_pills(int page_idx, const char* const* labels, int count);

    void draw_cameras_hub_page();
    void draw_multiview_subtab();
    void draw_discovery_qr_subtab();

    void draw_inspector_page();

    void draw_record_switcher_page();
    void draw_iso_record_subtab();
    void draw_live_switcher_subtab();

    void draw_outputs_intercom_page();
    void draw_vcam_obs_subtab();
    void draw_intercom_settings_subtab();

    PlatformHost host_{};
    std::string config_path_;
    AppConfig cfg_{};
    lv::hub::VideoHubEngine hub_;

    std::atomic<bool> services_running_{false};
    uint16_t disc_port_ = lv::kVideoDiscoveryPort;
    int selected_slot_ = 0;
    int connect_target_slot_ = 0;

    bool director_ptt_ = false;
    bool director_latch_ = false;
    bool director_listen_mute_ = false;
    bool sync_remote_rec_ = false;

    std::array<IGpuTexture*, lv::hub::kMaxHubSlots> slot_tex_{};
    std::array<uint64_t, lv::hub::kMaxHubSlots> slot_tex_seq_{};
    std::vector<uint8_t> rgba_scratch_;

    std::vector<lv::NetworkInterfaceInfo> nics_;
    std::vector<lv::SystemAudioDeviceEntry> sys_mics_;
    std::vector<lv::SystemAudioDeviceEntry> sys_outs_;

    bool config_dirty_ = false;
    uint64_t config_save_us_ = 0;
    uint64_t last_disk_check_us_ = 0;
    double cached_disk_free_gb_ = 0.0;

    char studio_name_buf_[64]{};
    char manual_ip_buf_[64]{};
    int manual_port_ = lv::kVideoStreamPort;
    char iso_dir_buf_[512]{};
    char iso_sess_buf_[64]{};
    char slot_label_buf_[64]{};
    char slot_tag_buf_[16]{};
    int slot_tag_edit_idx_ = -1;
};

}  // namespace lv::gui
