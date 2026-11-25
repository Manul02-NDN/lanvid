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
    //    (fall back to +100 port block if live desktop instances already hold 5004/5006 on this machine)
    CHECK(lv::kAudioStreamPort == 5004);
    CHECK(lv::kAudioMonitorPort == 5020);
    CHECK(lv::kAudioDiscoveryPort == 5021);
    CHECK(lv::kVideoStreamPort == 5006);
    CHECK(lv::kVideoTallyPort == 5022);
    CHECK(lv::kVideoDiscoveryPort == 5023);

    lv::gui::LanVidAudioStudioApp audio_app(audio_cfg.string());
    lv::gui::LanVidVideoStudioApp video_app(video_cfg.string());

    uint16_t a_stream = lv::kAudioStreamPort, a_mon = lv::kAudioMonitorPort, a_disc = lv::kAudioDiscoveryPort;
    uint16_t v_stream = lv::kVideoStreamPort, v_tally = lv::kVideoTallyPort, v_disc = lv::kVideoDiscoveryPort;
    if (!audio_app.start_services(a_stream, a_mon, a_disc)) {
        a_stream = 5104; a_mon = 5120; a_disc = 5121;
        REQUIRE(audio_app.start_services(a_stream, a_mon, a_disc));
    }
    if (!video_app.start_services(v_stream, v_tally, v_disc)) {
        v_stream = 5106; v_tally = 5122; v_disc = 5123;
        REQUIRE(video_app.start_services(v_stream, v_tally, v_disc));
    }

    CHECK(audio_app.services_running());
    CHECK(video_app.services_running());

    // 3. Verify all 6 UDP ports are strictly distinct
    const std::set<uint16_t> ports = {
        audio_app.bound_audio_port(),
        audio_app.bound_monitor_port(),
        audio_app.bound_discovery_port(),
        video_app.bound_video_port(),
        video_app.bound_tally_port(),
        video_app.bound_discovery_port(),
    };
    CHECK(ports.size() == 6);
    CHECK(audio_app.bound_audio_port() == a_stream);
    CHECK(audio_app.bound_monitor_port() == a_mon);
    CHECK(audio_app.bound_discovery_port() == a_disc);
    CHECK(video_app.bound_video_port() == v_stream);
    CHECK(video_app.bound_tally_port() == v_tally);
    CHECK(video_app.bound_discovery_port() == v_disc);

    // 4. Role-aware discovery must detect BOTH [AUDIO] and [VIDEO]
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    const auto found_a = lv::discovery_scan(250, a_disc);
    const auto found_v = lv::discovery_scan(250, v_disc);
    bool saw_audio = false;
    bool saw_video = false;
    for (const auto& r : found_a) {
        if (r.role == "audio" && r.stream_port == a_stream) saw_audio = true;
    }
    for (const auto& r : found_v) {
        if (r.role == "video" && r.stream_port == v_stream) saw_video = true;
    }
    std::printf("    [split studio] audio(:%u,role=audio)=%d | video(:%u,role=video)=%d\n",
                unsigned(a_stream), int(saw_audio), unsigned(v_stream), int(saw_video));
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
