#pragma once
// ImGui presentation layer for standalone LanVid Audio app.

#include <string>
#include <vector>

#include "audio_service.hpp"
#include "common_ui/app_host.hpp"

namespace lv::apps {

class LanVidAudioApp final : public lv::ui::IDesktopApp {
public:
    explicit LanVidAudioApp(std::string config_path_override = "");
    ~LanVidAudioApp() override;

    void set_platform_host(const lv::ui::PlatformHost& host) override { host_ = host; }
    void init() override;
    void shutdown() override;
    void render_frame() override;

    const char* app_display_name() const override { return "LanVid Audio"; }
    std::string active_config_path() const override { return svc_.config_path(); }

    LanVidAudioService& service() { return svc_; }

private:
    void draw_top_bar();
    void draw_sidebar();
    void draw_aoip_grid_page();
    void draw_patchbay_page();
    void draw_monitor_page();
    void draw_midi_settings_page();

    lv::ui::PlatformHost host_{};
    LanVidAudioService svc_;
    std::vector<LvNetInterfaceInfo> nics_;
    char peer_ip_buf_[64]{};
    char studio_name_buf_[64]{};
};

}  // namespace lv::apps
