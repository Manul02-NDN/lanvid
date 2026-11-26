#pragma once
// ImGui presentation layer for standalone LanVid Video app.

#include <array>
#include <string>
#include <vector>

#include "common_ui/app_host.hpp"
#include "video_service.hpp"

namespace lv::apps {

class LanVidVideoApp final : public lv::ui::IDesktopApp {
public:
    explicit LanVidVideoApp(std::string config_path_override = "");
    ~LanVidVideoApp() override;

    void set_platform_host(const lv::ui::PlatformHost& host) override { host_ = host; }
    void init() override;
    void shutdown() override;
    void render_frame() override;

    const char* app_display_name() const override { return "LanVid Video"; }
    std::string active_config_path() const override { return svc_.config_path(); }

    LanVidVideoService& service() { return svc_; }

private:
    void update_slot_texture(uint32_t slot);
    void draw_slot_preview(uint32_t slot, float w, float h, const char* empty_hint);
    void draw_top_bar();
    void draw_sidebar();
    void draw_cameras_page();
    void draw_inspector_page();
    void draw_record_switcher_page();
    void draw_vcam_obs_page();

    lv::ui::PlatformHost host_{};
    LanVidVideoService svc_;
    std::vector<LvNetInterfaceInfo> nics_;

    std::array<lv::ui::IGpuTexture*, lv::hub::kMaxHubSlots> tex_{};
    std::array<uint64_t, lv::hub::kMaxHubSlots> tex_seq_{};
    std::vector<uint8_t> rgba_tmp_;

    int selected_slot_ = 0;
    int target_slot_ = 0;
    uint64_t last_disk_us_ = 0;
    double free_gb_ = 0.0;

    char manual_ip_buf_[64]{};
    int manual_port_ = lv::kVideoStreamPort;
    char iso_dir_buf_[512]{};
    char iso_sess_buf_[64]{};
    char label_buf_[64]{};
    char tag_buf_[16]{};
    int tag_edit_idx_ = -1;
};

}  // namespace lv::apps
