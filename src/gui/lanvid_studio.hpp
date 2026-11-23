#pragma once
// LanVid Studio — unified Desktop GUI (Sender + Receiver) built with Dear ImGui + lanvid_core C API.

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "audio/multi_device_matrix.hpp"
#include "audio/shm_audio_host.hpp"
#include "midi/midi_device_manager.hpp"
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

    // Applies Soft Pastel Lavender / Blue-Purple (Soft Glass / Claymorphic) visual style.
    static void apply_theme();

private:
    enum class IconKind {
        kGear,
        kPlay,
        kPause,
        kStop,
        kPanic,
        kWaveform,
        kHeadphones,
        kMidiPlug,
        kSend,
        kReceive,
        kDuplex,
        kMatrix
    };

    static bool draw_gradient_icon_tile(const char* id, IconKind kind, float size = 34.0f, int badge_count = -1,
                                        bool clickable = false);
    static void draw_ring_gauge(const char* title, const char* value_str, const char* sub_str, float fraction,
                                float card_w, float card_h, bool warn = false, bool health_mode = false);

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
    void wire_network_midi();
    static void on_network_midi(const LV_MidiEvent* ev, void* user_data);
    void start_vdev_tx();
    void stop_vdev_tx();
    void apply_receiver_output();
    void draw_vdev_status(const ShmHostStatus& st, bool tx_side);
    void draw_duplex_tab();
    void draw_patchbay_block();
    void draw_audio_matrix_tab();
    void init_audio_matrix();
    void sync_matrix_to_config();
    void start_duplex();
    void stop_duplex();
    void scan_aoip_nodes();

    void draw_top_bar();
    void draw_sidebar();
    void draw_studio_config_panel();
    // Jitter-buffer latency mode (Fixed (Ethernet) / Adaptive (Wi-Fi)) + live "ACTUAL BUFFER" telemetry.
    void draw_latency_mode_controls(const char* id, float combo_width);
    void draw_actual_buffer_line();
    void apply_latency_mode();
    void poll_latency_telemetry(uint64_t now);
    void draw_segmented_vu(float dbfs, float width, float height);
    void draw_sender_tab();
    void draw_receiver_tab();
    void draw_fullscreen_receiver();
    void draw_aspect_image(IGpuTexture* tex, const ImVec2& avail, const char* empty_label);

    PlatformHost host_{};
    AppConfig cfg_{};
    bool config_dirty_ = false;
    bool initial_tab_applied_ = false;
    uint64_t last_save_us_ = 0;

    // Enumerated hardware & network
    std::vector<MonitorInfo> monitors_;
    std::vector<CameraInfo> cameras_;
    std::vector<AudioDeviceInfo> loop_devs_;
    std::vector<AudioDeviceInfo> mic_devs_;
    std::vector<AsioDriverEntry> asio_devs_;
    std::vector<SystemAudioDeviceEntry> sys_out_devs_;  // CoreAudio outputs (macOS)
    std::vector<SystemAudioDeviceEntry> sys_in_devs_;   // CoreAudio inputs (macOS)
    std::vector<MidiDeviceInfo> midi_in_devs_;
    std::vector<MidiDeviceInfo> midi_out_devs_;
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

    // Full-duplex 64x64 node + patchbay
    LvDuplexHandle* duplex_ = nullptr;
    bool duplex_running_ = false;
    LV_DuplexStats dpx_stats_{};
    double actual_buf_ms_ = 0.0;       // measured jitter-buffer delay of the active receive path
    uint32_t actual_target_ms_ = 0;    // delay it steers to (> latency when Adaptive has grown it)
    bool actual_buf_valid_ = false;
    uint64_t actual_buf_us_ = 0;
    uint64_t dpx_stats_us_ = 0;
    char dpx_peer_buf_[64]{};
    std::mutex nodes_mu_;
    std::vector<LV_AudioNode> nodes_;
    std::thread nodes_thread_;
    std::atomic<bool> nodes_scanning_{false};
    int patch_dir_ = 0;  // 0 = TX (DAW -> peer), 1 = RX (peer -> DAW)
    int patch_src_ = 1, patch_dst_ = 1;
    bool mute_[2][64]{};
    bool solo_[2][64]{};
    int8_t route_[2][64]{};

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

    // Multi-Device Audio Matrix (64x64 across up to 8 slots)
    MultiDeviceAudioMatrix audio_matrix_;
    int matrix_src_slot_ = 1;  // default Network AoIP
    int matrix_dst_slot_ = 0;  // default Virtual Bus (DAW)
    int matrix_src_page_ = 0;  // 0..3 (blocks of 16 channels)
    int matrix_dst_page_ = 0;  // 0..3 (blocks of 16 channels)
    int matrix_active_preset_ = 0;  // 0, 1, 2, or -1 (custom)
    int matrix_patch_mode_ = 0;     // 0 = Audio Patch, 1 = MIDI Patch
    int midi_channel_filter_ = 0;   // 0 = All, 1..16 = Ch 1..16
    float matrix_new_gain_db_ = 0.0f;

    // UI status / toast notifications
    std::string toast_msg_;
    uint64_t toast_until_us_ = 0;
    int screenshot_counter_ = 0;
};

}  // namespace lv::gui
