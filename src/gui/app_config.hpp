#pragma once
// Persistent JSON configuration for LanVid Studio:
//  - Windows: %APPDATA%\LanVid\config.json
//  - Linux:   ~/.config/lanvid/config.json

#include <cstdint>
#include <string>

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

    int preset = 0;            // 0=1080p60 Low Latency, 1=Ultra 4K60, 2=Wi-Fi Adaptive, 3=Custom
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
    int active_tab = 0;        // 0=Sender, 1=Receiver
};

// Returns platform config file path (%APPDATA%\LanVid\config.json or ~/.config/lanvid/config.json).
std::string config_file_path();

// Loads existing config if present (preserving defaults for missing fields).
bool load_app_config(AppConfig& cfg);

// Saves config atomically (creating parent directories if needed).
bool save_app_config(const AppConfig& cfg);

}  // namespace lv::gui
