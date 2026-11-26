#pragma once
// LanVid Video — Multi-Cam studio: up to 16 phones (MultiCamManager), per-camera ISO recording, OS virtual
// cameras ("LanVid Cam 1..8" + "LanVid Program Out"), Program/Preview switcher with Tally, Director
// Intercom/IFB and OBS outputs. Soft Pastel Lavender / Glass UI with 4 pages + pill sub-tabs:
//   1. Камери та Підключення  — Multiview Grid | Discovery (+QR)
//   2. Запис та Ефір          — ISO Recorder   | Program / Preview & Tally
//   3. Віртуальні камери      — OS capture devices | OBS & audio bridge
//   4. Інтерком та Налаштування — Duplex Intercom | Network & System
// Default ports (non-conflicting with LanVid Audio): Video 5006, Tally/Intercom 5022, Discovery 5023.

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "audio/audio_jitter_buffer.hpp"
#include "audio/intercom_engine.hpp"
#include "common/image.hpp"
#include "gui/app_config.hpp"
#include "gui/studio_common.hpp"
#include "net/discovery.hpp"
#include "net/mdns_advertiser.hpp"
#include "net/network_interfaces.hpp"
#include "video/multicam_manager.hpp"

namespace lv {
class AudioCapture;
class AudioPlayback;
}  // namespace lv

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
    lv::MultiCamManager& multicam() { return multicam_; }

    // Program / Preview switching with Tally sync to the phones (also used by tests).
    void take_program(int slot);
    void set_preview(int slot);
    // Applies config slot labels / pins / vcam count to the running MultiCamManager.
    void apply_slot_config();

    // Virtual camera driver state for the current OS ("Installed", "Not installed", ...).
    static std::string vcam_driver_status();
    static bool install_vcam_driver(std::string& message);

private:
    void refresh_nics();
    void apply_nics();
    void start_lan_scan();
    void poll_services();
    void update_slot_texture(uint32_t idx, uint32_t tex_index);
    void sync_tally();
    void persist_slot_labels();

    // layout
    void draw_top_bar();
    void draw_sidebar();
    bool draw_pill_tabs(const char* id, const char* const* labels, int count, int& active);
    // pages
    void page_cameras();
    void page_record_live();
    void page_outputs();
    void page_intercom_settings();
    // sub-pages
    void draw_multiview();
    void draw_camera_card(uint32_t slot, float w, float h);
    void draw_discovery();
    void draw_iso_recorder();
    void draw_switcher();
    void draw_virtual_cameras();
    void draw_integrations();
    void draw_intercom();
    void draw_system_settings();

    void draw_preview_image(int tex_index, float w, float h, const char* overlay, const ImVec4& border);
    std::string quick_connect_url() const;
    void start_intercom_audio();
    void stop_intercom_audio();

    PlatformHost host_{};
    std::string config_path_;
    AppConfig cfg_{};

    int page_ = 0;           // 0..3 sidebar pages
    int sub_[4] = {0, 0, 0, 0};

    std::atomic<bool> services_running_{false};
    uint16_t bound_video_port_ = lv::kVideoStreamPort;
    uint16_t bound_disc_port_ = lv::kVideoDiscoveryPort;

    lv::MultiCamManager multicam_;
    static constexpr uint32_t kTexCount = lv::kMultiCamMaxSlots + 2;  // slots + PGM + PVW
    std::array<IGpuTexture*, kTexCount> tex_{};
    std::array<uint64_t, kTexCount> tex_seq_{};
    std::array<int, kTexCount> tex_slot_{};
    lv::Image scratch_;

    lv::TallyIntercomEngine intercom_;
    lv::DiscoveryResponder discovery_;
    lv::DiscoveryResponder legacy_discovery_;
    lv::MdnsAdvertiser mdns_;

    // switcher / tally
    int pgm_slot_ = 0;
    int pvw_slot_ = 1;
    double auto_t0_ = -1.0;  // AUTO transition start (seconds), -1 = idle
    std::vector<uint8_t> last_tally_sent_;

    // per-camera UI state
    int editing_slot_ = -1;
    char label_buf_[64] = {};
    std::array<int, lv::kMultiCamMaxSlots> lens_{};
    std::array<int, lv::kMultiCamMaxSlots> quality_{};
    std::array<bool, lv::kMultiCamMaxSlots> af_{};
    std::array<int, lv::kMultiCamMaxSlots> focus_{};
    std::array<bool, lv::kMultiCamMaxSlots> talk_{};
    std::array<bool, lv::kMultiCamMaxSlots> latch_{};

    // discovery / manual connect
    char manual_ip_[64] = {};
    int manual_slot_ = 0;
    std::vector<lv::NetworkInterfaceInfo> nics_;
    std::mutex scan_mu_;
    std::vector<lv::DiscoveredReceiver> peers_;
    std::atomic<bool> scanning_{false};
    std::thread scan_thread_;

    // recorder
    char iso_dir_[512] = {};
    char iso_session_[64] = {};

    // intercom audio (director mic / headphones)
    bool all_call_ = true;
    bool director_ptt_ = false;
    bool director_latch_ = false;
    std::unique_ptr<lv::AudioCapture> mic_;
    std::unique_ptr<lv::AudioPlayback> phones_;
    lv::AudioJitterBuffer phones_buf_{48000, 30.0};
    std::vector<float> ifb_scratch_;
    std::string intercom_audio_status_;

    std::string vcam_status_;
    std::string vcam_message_;
    char studio_name_buf_[64] = "LanVid Video";
    float anim_time_ = 0.0f;
    double clock_s_ = 0.0;
};

}  // namespace lv::gui
