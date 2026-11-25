#pragma once
// LanVid Studio — unified Desktop GUI (Sender + Receiver) built with Dear ImGui + lanvid_core C API.

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "audio/intercom_engine.hpp"
#include "audio/multi_device_matrix.hpp"
#include "audio/personal_monitor_bus.hpp"
#include "grid/dsp_server.hpp"
#include "grid/grid_node.hpp"
#include "net/mdns_advertiser.hpp"
#include "audio/shm_audio_host.hpp"
#include "midi/midi_device_manager.hpp"
#include "gui/app_config.hpp"
#include "gui/gpu_texture.hpp"
#include "lanvid_c_api.h"
#include "lanvid/lanvid_sdk.h"
#include "net/discovery.hpp"
#include "net/network_interfaces.hpp"
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

    // Applies Soft Pastel Lavender (default) or Dark Studio visual style.
    static void apply_theme(bool dark = false);

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
    static void on_remote_control_sync(const LV_ControlSyncState* state, void* user_data);
    void broadcast_control_sync(bool mute_all_panic = false);
    void apply_pending_remote_sync();
    void start_vdev_tx();
    void stop_vdev_tx();
    void apply_receiver_output();
    // Exactly one consumer per playout matrix and one producer per sender stream (see enforce_audio_ownership).
    void enforce_audio_ownership(uint64_t now);
    enum class RxConsumer { kNone, kMeterPump, kVirtualDevice, kAudioMatrix };
    RxConsumer rx_consumer_ = RxConsumer::kNone;
    uint64_t ownership_us_ = 0;
    bool matrix_net_rx_ = false, matrix_net_tx_ = false, matrix_vbus_ = false;
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
    void draw_connect_tab();
    void draw_patchbay_tab();
    void draw_devices_tab();
    void draw_settings_tab();
    void draw_add_device_popups();
    void draw_studio_config_panel();
    // Non-null while the sender runs with video but its video source could not start (audio keeps going).
    const char* video_source_warning() const;
    int last_video_error_ = 0;
    // Jitter-buffer latency mode (Fixed (Ethernet) / Adaptive (Wi-Fi)) + live "ACTUAL BUFFER" telemetry.
    void draw_latency_mode_controls(const char* id, float combo_width);
    void draw_audio_bitrate_pills(const char* id);
    void draw_stream_mode_selector(const char* id);
    void apply_stream_mode();
    void draw_mobile_monitor_section();
    void draw_cameras_tally_intercom_section();
    void draw_network_adapters_section(const char* id_prefix, bool compact);
    void refresh_network_interfaces();
    void apply_network_interfaces();
    void draw_grid_engine_section();
    void apply_engine_mode(int mode);
    bool start_grid_node();
    void stop_grid_node();
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
    std::vector<NetworkInterfaceInfo> net_ifaces_;
    std::string local_lan_ip_ = "127.0.0.1";
    double nic_pri_tx_mbps_ = 0.0;
    double nic_sec_tx_mbps_ = 0.0;
    uint64_t nic_prev_pri_bytes_ = 0;
    uint64_t nic_prev_sec_bytes_ = 0;
    uint64_t nic_rate_us_ = 0;
    uint64_t nic_failover_pkts_ = 0;

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
    LV_MidiDejitterStats midi_dejitter_stats_{};
    bool midi_dejitter_valid_ = false;
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
    PersonalMonitorServer monitor_server_{48000};
    TallyIntercomEngine tally_intercom_;
    bool intercom_all_call_ = false;
    bool intercom_listen_muted_ = false;
    float intercom_ifb_gain_ = 1.0f;
    int intercom_mic_dev_idx_ = 0;
    int intercom_out_dev_idx_ = 0;
    MdnsAdvertiser monitor_mdns_;  // "_lanvid._udp" so LanVid Monitor phones find this studio in one tap
    std::atomic<bool> matrix_has_net_tx_routes_{false};
    double dpx_tx_mbps_ = 0.0;
    double dpx_rx_mbps_ = 0.0;
    uint64_t dpx_prev_tx_pkts_ = 0;
    uint64_t dpx_prev_rx_pkts_ = 0;
    uint64_t dpx_rate_us_ = 0;
    int matrix_src_slot_ = 1;  // default Network AoIP
    int matrix_dst_slot_ = 0;  // default Virtual Bus (DAW)
    int matrix_src_page_ = 0;  // 0..3 (blocks of 16 channels)
    int matrix_dst_page_ = 0;  // 0..3 (blocks of 16 channels)
    int matrix_active_preset_ = 0;  // 0, 1, 2, or -1 (custom)
    int matrix_patch_mode_ = 0;     // 0 = Audio Patch, 1 = MIDI Patch
    int midi_channel_filter_ = 0;   // 0 = All, 1..16 = Ch 1..16
    float matrix_new_gain_db_ = 0.0f;

    // Interactive Network Board (Connect) & Visual Audio Patchbay state
    int connect_selected_node_ = 1;      // 0=Local, 1=Remote Studio, 2=Audio I/O, 10+=Mobile Cam/Monitor
    bool connect_show_manual_ip_ = false;
    float connect_master_vol_ = 1.0f;
    float connect_ducking_db_ = -12.0f;
    bool connect_mute_panic_ = false;
    bool intercom_latch_mode_ = false;
    int patchbay_view_mode_ = 0;         // 0 = Visual Patchbay, 1 = Detailed 64x64 Matrix
    int patch_drag_src_slot_ = -1;
    int patch_sel_src_ = -1;
    int patch_sel_dst_ = -1;
    int patch_block_mode_ = 1;           // 0 = Stereo (Ch 1-2), 1 = 64ch 1:1
    bool patch_solo_active_ = false;

    // UI status / toast notifications & root-level popup triggers
    std::string toast_msg_;
    uint64_t toast_until_us_ = 0;
    int screenshot_counter_ = 0;
    bool req_open_add_audio_popup_ = false;
    bool req_open_add_midi_popup_ = false;
    bool req_open_clear_all_popup_ = false;
    uint64_t last_dev_refresh_us_ = 0;

    // Real-time bidirectional settings sync (anti-loop / echo guard)
    uint32_t sync_origin_id_ = 0;

    // LanVid Grid Layer 2 Ethernet (256ch) engine state (coexists cleanly with Classic UDP/L3)
    std::vector<lv::grid::L2AdapterInfo> grid_adapters_;
    std::string grid_backend_status_;
    std::string grid_last_err_;
    std::unique_ptr<lv::grid::GridNode> grid_node_;
    uint64_t grid_clock_sample_ = 0;
    uint64_t grid_last_sync_us_ = 0;
    uint64_t local_sync_revision_ = 0;
    uint64_t last_applied_remote_revision_ = 0;
    bool applying_remote_sync_ = false;
    uint64_t last_synced_us_ = 0;
    std::mutex remote_sync_mu_;
    bool has_pending_remote_sync_ = false;
    LV_ControlSyncState pending_remote_sync_{};
};

}  // namespace lv::gui
