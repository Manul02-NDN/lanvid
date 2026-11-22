#pragma once
// LanVid Studio — unified Desktop GUI (Sender + Receiver) built with Dear ImGui + lanvid_core C API.

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "audio/shm_audio_host.hpp"
#include "gui/app_config.hpp"
#include "gui/gpu_texture.hpp"
#include "lanvid_c_api.h"
#include "lanvid/lanvid_sdk.h"
#include "net/discovery.hpp"
#include "platform/audio_capture.hpp"
#include "platform/camera_capture.hpp"
#include "platform/screen_capture.hpp"

namespace lv::gui {

class LanVidStudioApp {
public:
    explicit LanVidStudioApp(PlatformHost host);
    ~LanVidStudioApp();

    LanVidStudioApp(const LanVidStudioApp&) = delete;
    LanVidStudioApp& operator=(const LanVidStudioApp&) = delete;

    void init();
    void shutdown();

    // Renders one Dear ImGui frame covering the entire OS window.
    void render_frame();

    // Applies modern dark studio visual style.
    static void apply_theme();

private:
    void refresh_devices();
    void start_lan_scan();
    void start_sender();
    void stop_sender();
    void start_receiver();
    void stop_receiver();
    void apply_preset(int preset_idx);
    void poll_telemetry_and_frames();
    void start_tone();
    void stop_tone();
    void draw_aoip_sender_controls();
    void draw_aoip_meter_grid();
    void apply_receiver_bus();
    void start_vdev_tx();
    void stop_vdev_tx();
    void apply_receiver_output();
    void draw_vdev_status(const ShmHostStatus& st, bool tx_side);

    void draw_top_bar();
    void draw_sender_tab();
    void draw_receiver_tab();
    void draw_fullscreen_receiver();
    void draw_aspect_image(IGpuTexture* tex, const ImVec2& avail, const char* empty_label);

    PlatformHost host_{};
    AppConfig cfg_{};
    bool config_dirty_ = false;
    uint64_t last_save_us_ = 0;

    // Enumerated hardware & network
    std::vector<MonitorInfo> monitors_;
    std::vector<CameraInfo> cameras_;
    std::vector<AudioDeviceInfo> loop_devs_;
    std::vector<AudioDeviceInfo> mic_devs_;
    std::vector<NetInterfaceInfo> ifaces_;
    std::string local_lan_ip_ = "127.0.0.1";

    // Async LAN discovery state
    std::mutex disc_mu_;
    std::vector<DiscoveredReceiver> discovered_;
    std::thread disc_thread_;
    std::atomic<bool> scanning_{false};

    // Core C API handles
    LvSenderHandle* sender_ = nullptr;
    LvReceiverHandle* receiver_ = nullptr;
    bool sender_running_ = false;
    bool receiver_running_ = false;

    // 64-channel AoIP bus
    std::thread tone_thread_;
    std::atomic<bool> tone_stop_{true};
    std::atomic<int> tone_channels_{8};
    LV_AudioChannelStats ch_stats_[LV_AUDIO_MAX_CHANNELS]{};
    uint64_t ch_stats_us_ = 0;
    ShmAudioHost vdev_tx_;  // DAW -> network (sender side)
    ShmAudioHost vdev_rx_;  // network -> DAW (receiver side)

    // Editable text buffers
    char ip_buf_[64]{};
    char file_buf_[512]{};

    // Live GPU textures + CPU transfer buffers
    IGpuTexture* sender_tex_ = nullptr;
    IGpuTexture* receiver_tex_ = nullptr;
    std::vector<uint8_t> sender_frame_buf_;
    std::vector<uint8_t> receiver_frame_buf_;
    uint64_t last_tx_preview_fid_ = 0;
    uint32_t last_rx_fid_ = 0;

    // Telemetry + rolling sparkline history (120 samples @ 2 Hz = 60 sec)
    static constexpr int kHistLen = 120;
    LvSenderStats tx_stats_{};
    LvReceiverStats rx_stats_{};
    float tx_mbps_hist_[kHistLen]{};
    float tx_loss_hist_[kHistLen]{};
    float rx_lat_hist_[kHistLen]{};
    float rx_mbps_hist_[kHistLen]{};
    int hist_idx_ = 0;
    uint64_t last_stat_poll_us_ = 0;

    // UI status / toast notifications
    std::string toast_msg_;
    uint64_t toast_until_us_ = 0;
    int screenshot_counter_ = 0;
};

}  // namespace lv::gui
