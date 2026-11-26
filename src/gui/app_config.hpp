#pragma once
// Persistent JSON configuration for LanVid Studio:
//  - Windows: %APPDATA%\LanVid\config.json
//  - Linux:   ~/.config/lanvid/config.json

#include <cstdint>
#include <string>
#include <vector>

#include "audio/multi_device_matrix.hpp"

namespace lv::gui {

struct AppConfig {
    std::string dest_ip = "127.0.0.1";
    int dest_port = 5000;
    int listen_port = 5000;

    int source_type = 2;       // 1=pattern, 2=screen, 3=camera, 4=file
    int monitor_index = 0;
    int capture_backend = 0;   // 0=auto, 1=dxgi, 2=gdi
    int camera_index = 0;
    std::string file_path;
    int pattern_width = 1920;
    int pattern_height = 1080;

    int audio_mode = 0;        // 0=none, 1=loopback, 2=mic
    int audio_device_index = -1;
    double audio_chunk_ms = 5.0;

    // 64-channel AoIP bus
    bool audio_only = false;   // true=disable video capture/encode/decode (same as CLI --audio-only)
    bool midi_only = false;    // true=MIDI Only ultra-low latency (~1.5 ms, 0 audio/video packets)
    int audio_codec = 0;       // 0=PCM32F, 1=PCM24, 2=PCM16, 3=192k, 4=256k, 5=192k, 6=128k, 7=96k, 8=64k
    int audio_bitrate_kbps = 0;// 0=Lossless (PCM), or 256 / 192 / 128 / 96 / 64 kbps per channel
    int aoip_channels = 8;     // 2 / 8 / 16 / 32 / 64
    int aoip_latency_ms = 20;  // 2 (ultra-low) .. 2000 (bulletproof)
    int adaptive_jitter = 1;   // 1 = Adaptive (Wi-Fi) jitter buffer, 0 = Fixed (Ethernet / live monitoring)
    bool aoip_tone = false;    // sender: N-channel test tone bus
    int aoip_source = 0;       // sender: 0=Built-in Test Tone Generator, 1=Virtual Audio Device
    int aoip_output = 0;       // receiver: 0=meters only, 1=Virtual Audio Device
    std::string dpx_peer_host = "127.0.0.1";  // full-duplex peer node
    int dpx_peer_port = 5004;
    int dpx_listen_port = 5005;

    int preset = 0;            // 0=1080p60 Low Latency, 1=Ultra 4K60, 2=Wi-Fi Adaptive, 3=Custom
    int codec = 0;             // 0=LanVid DCT, 1=H.264, 2=H.265 (HEVC)
    int bitrate_mbps = 8;      // 1..50 Mbps target for H.264/H.265
    int keyframe_interval = 120;
    int quality = 80;
    int fps = 60;
    int fec_group = 0;
    bool adaptive_quality = false;
    int q_min = 35;
    int q_max = 85;

    float volume = 1.0f;
    int conceal_mode = 1;      // 0=black, 1=prev_frame
    double audio_target_ms = 12.0;
    bool show_hud = true;
    std::string preview_backend = "auto";  // "auto", "d3d11", "gdi"
    int active_tab = 0;        // 0=Connect, 1=Patchbay, 2=Devices, 3=Settings
    int connect_role = 0;      // 0=Send, 1=Receive, 2=Duplex (Both directions)
    bool dark_theme = false;   // false=Soft Pastel Lavender, true=Dark Studio
    bool matrix_stereo_pairs = true;  // true=Stereo pairs (1-2, 3-4...), false=Per-channel
    bool sync_settings = true;        // real-time bidirectional settings sync across connected peers
    bool sync_matrix_routing = false; // also sync matrix routing presets across peers
    bool wifi_redundancy = false;     // Glitch-Free Wi-Fi Redundancy (1+1 Duplicate +1.5 ms stagger)

    // Physical NIC selection & Dual-NIC 1+1 Failover (Ethernet + Wi-Fi)
    std::string primary_nic_id;       // Empty = Auto (OS default route)
    std::string primary_nic_ip;
    std::string secondary_nic_id;     // Empty = Disabled / same-NIC Wi-Fi 1+1
    std::string secondary_nic_ip;
    bool dual_nic_redundancy_enabled = false;

    // Dual-Engine Architecture: 0 = Classic (UDP/IP Layer 3, default), 1 = Grid (Layer 2 Ethernet 256ch),
    // 2 = Hybrid (Grid L2 studio backbone + Classic L3 Wi-Fi/Mobile bridge)
    int network_engine_mode = 0;
    std::string grid_nic_name;        // Selected Ethernet adapter name for Layer 2
    bool grid_vlan_enabled = false;   // IEEE 802.1Q / 802.1p QoS tagging
    int grid_vlan_id = 0;             // 0..4094
    int grid_vlan_pcp = 6;            // 0..7 (6 = Voice/Audio priority)
    int grid_channels = 256;          // 64 / 128 / 256 channels in Grid L2 mode
    bool grid_clock_master = true;    // Whether this node broadcasts L2 Clock Master sync pulses

    // Multi-Device Audio Matrix persistence
    int matrix_buffer_size = 256;
    bool matrix_feedback_guard = true;
    std::vector<MatrixSlotConfig> matrix_slots;
    std::vector<MatrixCrosspoint> matrix_crosspoints;
    std::vector<MidiSlotConfig> midi_slots;
    std::vector<MidiCrosspoint> midi_crosspoints;

    // Video Studio specific preferences (LanVid Video)
    int video_lip_sync_ms = 0;            // -200..+200 ms A/V lip-sync correction
    bool video_obs_output_enabled = true; // Forward active camera / PGM to obs-lanvid
    float intercom_ducking_db = -12.0f;   // Program audio ducking when director speaks
    bool video_dscp_af41 = true;          // DSCP AF41 (34) for video streams
    bool tally_dscp_ef = true;            // DSCP EF/CS7 (46) for Tally & Intercom

    // Multi-Cam (MultiCamManager), ISO recording and OS virtual cameras (LanVid Video)
    int video_max_slots = 8;              // 1..16 camera slots
    int video_port_mode = 0;              // 0 = one port (5006) demultiplexed by sender, 1 = 5006/5008/5010...
    int video_multiview_layout = 2;       // 1 = 1x1, 2 = 2x2, 3 = 3x3
    std::vector<std::string> video_slot_labels;  // "CAM 1 • Wide", ... (index = slot)
    std::vector<std::string> video_slot_ips;     // pinned phone IP per slot ("" = auto)
    std::string iso_output_dir;           // "" = ~/Movies/LanVid (macOS) / Videos\LanVid (Windows)
    std::string iso_session_name = "LanVid";
    int iso_container = 0;                // 0 = MP4, 1 = MKV, 2 = MOV
    bool iso_record_program = false;      // extra "PGM Master Mix" file
    int iso_slot_mask = 0xFFFF;           // slots included in REC ALL
    int vcam_count = 8;                   // "LanVid Cam 1..N" exposed to the OS (0..8) + Program Out
    bool vcam_placeholder = true;         // lavender "No Signal / Standby" card when a camera is gone
    bool video_hw_decode = true;          // hardware H.264 / H.265 decoding (VideoToolbox / DXVA / VA-API)
    int video_audio_route = 0;            // camera audio: 0 = LanVid Audio, 1 = system output, 2 = off
    int intercom_mic_index = -1;          // director microphone (-1 = system default)
    int intercom_out_index = -1;          // director headphones (-1 = system default)
    float intercom_ifb_volume = 1.0f;     // IFB (director -> camera) gain
};

// Returns platform config file path (%APPDATA%\LanVid\<filename> or ~/.config/lanvid/<filename>).
std::string config_file_path_for(const std::string& filename);

// Returns default legacy config file path (config.json).
std::string config_file_path();

// Loads existing config if present (preserving defaults for missing fields).
bool load_app_config(AppConfig& cfg);
bool load_app_config(AppConfig& cfg, const std::string& custom_path);

// Saves config atomically (creating parent directories if needed).
bool save_app_config(const AppConfig& cfg);
bool save_app_config(const AppConfig& cfg, const std::string& custom_path);

}  // namespace lv::gui
