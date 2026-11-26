#pragma once
// ImGui presentation layer for standalone LanVid Intercom desk.

#include <string>
#include <vector>

#include "common_ui/app_host.hpp"
#include "intercom_service.hpp"

namespace lv::apps {

class LanVidIntercomApp final : public lv::ui::IDesktopApp {
public:
    explicit LanVidIntercomApp(std::string config_path_override = "");
    ~LanVidIntercomApp() override;

    void set_platform_host(const lv::ui::PlatformHost& host) override { host_ = host; }
    void init() override;
    void shutdown() override;
    void render_frame() override;

    const char* app_display_name() const override { return "LanVid Intercom"; }
    std::string active_config_path() const override { return svc_.config_path(); }

    LanVidIntercomService& service() { return svc_; }

private:
    void draw_top_bar();
    void draw_sidebar();
    void draw_operator_matrix();

    lv::ui::PlatformHost host_{};
    LanVidIntercomService svc_;
    std::vector<LvNetInterfaceInfo> nics_;
    std::vector<lv::SystemAudioDeviceEntry> mics_;
    std::vector<lv::SystemAudioDeviceEntry> hps_;
    bool ptt_active_ = false;
    bool latch_active_ = false;
};

}  // namespace lv::apps
