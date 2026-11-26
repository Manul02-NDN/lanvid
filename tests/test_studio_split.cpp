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

TEST(studio_audio_config_and_lifecycle) {
    namespace fs = std::filesystem;
    const fs::path tmp_dir = fs::temp_directory_path() / "lanvid_audio_lifecycle_test";
    std::error_code ec;
    fs::create_directories(tmp_dir, ec);
    const fs::path cfg_path = tmp_dir / "lanvid_audio_config.json";
    fs::remove(cfg_path, ec);

    {
        lv::gui::LanVidAudioStudioApp app(cfg_path.string());
        auto& c = app.config();
        c.studio_name = "Kyiv Studio A";
        c.active_tab = 2;
        c.active_subtab = {1, 2, 1, 0};
        c.network_engine_mode = 2;
        c.audio_sample_rate = 96000;
        c.grid_block_samples = 16;
        c.grid_nic_mac = "AA:BB:CC:DD:EE:FF";
        c.grid_dsp_primary = "11:22:33:44:55:66";
        c.grid_dsp_backup = "66:55:44:33:22:11";
        c.monitor_channel_names = {"Vocal", "Click", "Guitar"};
        c.monitor_channel_mask = 0x7ull;
        c.monitor_dsp_hpf_enabled = true;
        c.monitor_dsp_hpf_hz = 110.0f;
        c.monitor_dsp_eq_enabled = true;
        c.monitor_dsp_eq_low_db = 3.5f;
        c.monitor_dsp_comp_enabled = true;
        c.monitor_dsp_comp_ratio = 4.5f;
        c.autostart_session = false;

        REQUIRE(lv::gui::save_app_config(c, cfg_path.string()));
    }

    {
        lv::gui::AppConfig loaded{};
        REQUIRE(lv::gui::load_app_config(loaded, cfg_path.string()));
        CHECK(loaded.studio_name == "Kyiv Studio A");
        CHECK(loaded.active_tab == 2);
        CHECK(loaded.active_subtab[0] == 1);
        CHECK(loaded.active_subtab[1] == 2);
        CHECK(loaded.active_subtab[2] == 1);
        CHECK(loaded.active_subtab[3] == 0);
        CHECK(loaded.network_engine_mode == 2);
        CHECK(loaded.audio_sample_rate == 96000);
        CHECK(loaded.grid_block_samples == 16);
        CHECK(loaded.grid_nic_mac == "AA:BB:CC:DD:EE:FF");
        CHECK(loaded.grid_dsp_primary == "11:22:33:44:55:66");
        CHECK(loaded.grid_dsp_backup == "66:55:44:33:22:11");
        CHECK(loaded.monitor_channel_names.size() >= 3);
        CHECK(loaded.monitor_channel_names[0] == "Vocal");
        CHECK(loaded.monitor_channel_names[1] == "Click");
        CHECK(loaded.monitor_channel_mask == 0x7ull);
        CHECK(loaded.monitor_dsp_hpf_enabled);
        CHECK(loaded.monitor_dsp_hpf_hz > 109.0f && loaded.monitor_dsp_hpf_hz < 111.0f);
        CHECK(loaded.monitor_dsp_eq_enabled);
        CHECK(loaded.monitor_dsp_eq_low_db > 3.4f && loaded.monitor_dsp_eq_low_db < 3.6f);
        CHECK(loaded.monitor_dsp_comp_enabled);
        CHECK(loaded.monitor_dsp_comp_ratio > 4.4f && loaded.monitor_dsp_comp_ratio < 4.6f);
    }

    {
        lv::gui::LanVidAudioStudioApp app(cfg_path.string());
        uint16_t a_port = 5204, m_port = 5220, d_port = 5221;
        REQUIRE(app.start_services(a_port, m_port, d_port));
        CHECK(app.services_running());
        CHECK(app.matrix().is_running());
        app.config().connect_role = 1;  // Receive
        CHECK(app.start_stream());
        CHECK(app.stream_running());
        app.stop_stream();
        CHECK(!app.stream_running());
        app.shutdown();
        CHECK(!app.services_running());
    }

    fs::remove(cfg_path, ec);
    fs::remove(tmp_dir, ec);
}
