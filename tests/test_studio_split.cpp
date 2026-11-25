// Verifies that split desktop apps LanVid Audio (5004/5020/5021, role="audio") and
// LanVid Video (5006/5022/5023, role="video") run concurrently on the same host with:
//   - Separate config files (lanvid_audio_config.json vs lanvid_video_config.json)
//   - Zero UDP port conflict
//   - Accurate role-aware LAN discovery ([AUDIO] vs [VIDEO])
#include "mini_test.hpp"

#include <cstdio>
#include <filesystem>
#include <set>
#include <string>
#include <thread>

#include "gui/app_config.hpp"
#include "gui/audio_studio_ui.hpp"
#include "gui/video_studio_ui.hpp"
#include "net/discovery.hpp"

TEST(studio_audio_video_split_no_port_conflict) {
    namespace fs = std::filesystem;
    const fs::path tmp_dir = fs::temp_directory_path() / "lanvid_split_studio_test";
    std::error_code ec;
    fs::create_directories(tmp_dir, ec);
    const fs::path audio_cfg = tmp_dir / "lanvid_audio_config.json";
    const fs::path video_cfg = tmp_dir / "lanvid_video_config.json";
    fs::remove(audio_cfg, ec);
    fs::remove(video_cfg, ec);

    // 1. Canonical config paths must be distinct
    const std::string def_audio_cfg = lv::gui::config_file_path_for("lanvid_audio_config.json");
    const std::string def_video_cfg = lv::gui::config_file_path_for("lanvid_video_config.json");
    CHECK(def_audio_cfg != def_video_cfg);
    CHECK(def_audio_cfg.find("lanvid_audio_config.json") != std::string::npos);
    CHECK(def_video_cfg.find("lanvid_video_config.json") != std::string::npos);

    // 2. Start LanVid Audio & LanVid Video simultaneously on the same host
    lv::gui::LanVidAudioStudioApp audio_app(audio_cfg.string());
    lv::gui::LanVidVideoStudioApp video_app(video_cfg.string());

    REQUIRE(audio_app.start_services(lv::kAudioStreamPort, lv::kAudioMonitorPort, lv::kAudioDiscoveryPort));
    REQUIRE(video_app.start_services(lv::kVideoStreamPort, lv::kVideoTallyPort, lv::kVideoDiscoveryPort));

    CHECK(audio_app.services_running());
    CHECK(video_app.services_running());

    // 3. Verify all 6 UDP ports are strictly distinct (5004/5020/5021 vs 5006/5022/5023)
    const std::set<uint16_t> ports = {
        audio_app.bound_audio_port(),
        audio_app.bound_monitor_port(),
        audio_app.bound_discovery_port(),
        video_app.bound_video_port(),
        video_app.bound_tally_port(),
        video_app.bound_discovery_port(),
    };
    CHECK(ports.size() == 6);
    CHECK(audio_app.bound_audio_port() == 5004);
    CHECK(audio_app.bound_monitor_port() == 5020);
    CHECK(audio_app.bound_discovery_port() == 5021);
    CHECK(video_app.bound_video_port() == 5006);
    CHECK(video_app.bound_tally_port() == 5022);
    CHECK(video_app.bound_discovery_port() == 5023);

    // 4. Role-aware discovery must detect BOTH [AUDIO] (5004) and [VIDEO] (5006)
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    const auto found = lv::discovery_scan_roles(350, true, true, false);
    bool saw_audio = false;
    bool saw_video = false;
    for (const auto& r : found) {
        if (r.role == "audio" && r.stream_port == 5004) saw_audio = true;
        if (r.role == "video" && r.stream_port == 5006) saw_video = true;
    }
    std::printf("    [split studio] discovered=%zu | audio(:5004,role=audio)=%d | video(:5006,role=video)=%d\n",
                found.size(), int(saw_audio), int(saw_video));
    CHECK(saw_audio);
    CHECK(saw_video);

    // 5. Independent config persistence (no cross-clobbering)
    audio_app.config().aoip_channels = 32;
    audio_app.config().network_engine_mode = 2;  // Hybrid
    video_app.config().video_lip_sync_ms = 25;
    video_app.config().intercom_ducking_db = -14.0f;

    audio_app.shutdown();
    video_app.shutdown();

    lv::gui::AppConfig loaded_audio{}, loaded_video{};
    REQUIRE(lv::gui::load_app_config(loaded_audio, audio_cfg.string()));
    REQUIRE(lv::gui::load_app_config(loaded_video, video_cfg.string()));
    CHECK(loaded_audio.aoip_channels == 32);
    CHECK(loaded_audio.network_engine_mode == 2);
    CHECK(loaded_video.video_lip_sync_ms == 25);
    CHECK(loaded_video.intercom_ducking_db <= -13.5f && loaded_video.intercom_ducking_db >= -14.5f);

    fs::remove(audio_cfg, ec);
    fs::remove(video_cfg, ec);
    fs::remove(tmp_dir, ec);
}
