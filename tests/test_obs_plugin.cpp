#include "mini_test.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

#include "obs_api_compat.hpp"
#include "obs_lanvid_core.hpp"

TEST(obs_plugin_video_360p_to_4k60_dynamic_switch_and_64ch_audio_routing) {
    using namespace lv::obs;

    // 1. Dynamic video resolution switching: 360p -> 1080p -> 9:16 vertical -> 4K UHD -> 4K DCI
    LanVidObsSourceEngine src;
    SourceConfig cfg{};
    cfg.source_name = "CAM 1 • OBS MAIN";
    cfg.listen_port = 0;  // ephemeral UDP port
    cfg.camera_id = 1;
    cfg.jitter_preset = JitterPreset::kUltraLow;
    cfg.audio_route = AudioRouteMode::kStereoPair;
    cfg.audio_pair_index = 2;  // Ch 5-6 (0-based indices 4 & 5)
    cfg.audio_channels = 64;

    struct RecFrame {
        uint32_t w, h;
        VideoPixelFormat fmt;
        ColorSpace cs;
    };
    std::vector<RecFrame> rx_frames;
    std::atomic<float> last_l{0.0f}, last_r{0.0f};
    std::atomic<uint32_t> audio_callbacks{0};

    CHECK(src.start(
        cfg,
        [&](const VideoFrameView& fv) {
            rx_frames.push_back({fv.width, fv.height, fv.format, fv.color_space});
        },
        [&](const AudioBlockView& av) {
            audio_callbacks.fetch_add(1, std::memory_order_relaxed);
            if (av.frames > 0 && av.planes[0] && av.planes[1] &&
                (std::fabs(av.planes[0][0]) > 1e-6f || std::fabs(av.planes[1][0]) > 1e-6f)) {
                last_l.store(av.planes[0][0], std::memory_order_relaxed);
                last_r.store(av.planes[1][0], std::memory_order_relaxed);
            }
        }));
    CHECK(src.bound_port() != 0);

    struct ResSpec {
        uint32_t w, h;
        ColorSpace cs;
    };
    const ResSpec specs[] = {
        {640, 360, ColorSpace::kRec709},       // 360p
        {1920, 1080, ColorSpace::kRec709},     // 1080p
        {1080, 1920, ColorSpace::kRec709},     // 9:16 smartphone vertical
        {3840, 2160, ColorSpace::kBT2100_PQ},  // 4K UHD HDR PQ
        {4096, 2160, ColorSpace::kBT2100_HLG}, // 4K DCI HDR HLG
    };

    std::vector<uint8_t> dummy_bgra(4096u * 2160u * 4u, 0x80u);
    uint64_t ts_ns = 1'000'000ULL;
    for (const auto& sp : specs) {
        src.ingest_video_frame_bgra(dummy_bgra.data(), sp.w, sp.h, sp.w * 4u, ts_ns, sp.cs);
        ts_ns += 16'666'667ULL;  // 60 fps
    }

    CHECK(rx_frames.size() == 5);
    CHECK(rx_frames[0].w == 640 && rx_frames[0].h == 360);
    CHECK(rx_frames[1].w == 1920 && rx_frames[1].h == 1080);
    CHECK(rx_frames[2].w == 1080 && rx_frames[2].h == 1920);
    CHECK(rx_frames[3].w == 3840 && rx_frames[3].h == 2160 && rx_frames[3].cs == ColorSpace::kBT2100_PQ);
    CHECK(rx_frames[4].w == 4096 && rx_frames[4].h == 2160 && rx_frames[4].cs == ColorSpace::kBT2100_HLG);

    const auto tel = src.telemetry();
    CHECK(tel.width == 4096 && tel.height == 2160 && tel.frames_rx == 5);

    // 2. 64-channel AoIP audio routing: Stereo Pair (Ch 5-6), Mono Sum, Video Only
    constexpr uint16_t kCh = 64;
    constexpr uint32_t kFrames = 128;
    std::vector<float> interleaved(size_t(kFrames) * kCh, 0.0f);
    for (uint32_t f = 0; f < kFrames; ++f) {
        interleaved[size_t(f) * kCh + 4] = 0.75f;   // Ch 5
        interleaved[size_t(f) * kCh + 5] = -0.50f;  // Ch 6
    }
    const uint32_t cb_before = audio_callbacks.load(std::memory_order_relaxed);
    src.route_multichannel_audio(interleaved.data(), kCh, kFrames, 48000, ts_ns);
    CHECK(audio_callbacks.load(std::memory_order_relaxed) >= cb_before + 1);
    CHECK(std::fabs(last_l.load(std::memory_order_relaxed) - 0.75f) < 1e-5f);
    CHECK(std::fabs(last_r.load(std::memory_order_relaxed) - (-0.50f)) < 1e-5f);

    // Switch to Video Only -> no audio callback fired
    cfg.audio_route = AudioRouteMode::kVideoOnly;
    cfg.listen_port = src.bound_port();
    src.update_config(cfg);
    const uint32_t cb_vo_before = audio_callbacks.load(std::memory_order_relaxed);
    src.route_multichannel_audio(interleaved.data(), kCh, kFrames, 48000, ts_ns + 1000);
    CHECK(audio_callbacks.load(std::memory_order_relaxed) == cb_vo_before);

    // 3. LanVid Program Output loopback test
    LanVidObsOutputEngine out;
    OutputConfig ocfg{};
    ocfg.dest_ip = "127.0.0.1";
    ocfg.dest_port = src.bound_port();
    ocfg.width = 640;
    ocfg.height = 360;
    ocfg.fps = 60;
    ocfg.codec = 0;
    ocfg.bitrate_kbps = 12000;
    CHECK(out.start(ocfg));
    CHECK(out.push_bgra(dummy_bgra.data(), 640, 360, 640 * 4));
    CHECK(out.frames_sent() == 1);
    out.stop();

    src.stop();
    std::printf("    [obs-lanvid] 360p->1080p->9:16->4K UHD->4K DCI dynamic switch + 64ch Ch5-6 routing OK\n");
}

TEST(obs_plugin_auto_tally_and_duplex_intercom_control_sync) {
    using namespace lv::obs;

    // 1. Exhaustive Auto-Tally state machine verification
    {
        ObsTallyInputs in{};
        in.mode = TallyControlMode::kAuto;
        in.studio_mode = true;

        // Standby (not in Preview or Program)
        auto d = evaluate_auto_tally(in);
        CHECK(d.tally_state == lv::kTallyStandby && !d.rec_trigger);

        // Preview in Studio Mode -> Green
        in.in_preview = true;
        d = evaluate_auto_tally(in);
        CHECK(d.tally_state == lv::kTallyPreview);

        // Program with Only-On-Air-When-Live guard ON while idle -> stays Preview (Green)
        in.in_program = true;
        in.only_on_air_when_live = true;
        in.obs_streaming = false;
        in.obs_recording = false;
        d = evaluate_auto_tally(in);
        CHECK(d.tally_state == lv::kTallyPreview);

        // OBS starts Streaming -> Program (Red ON AIR)
        in.obs_streaming = true;
        d = evaluate_auto_tally(in);
        CHECK(d.tally_state == lv::kTallyProgram && !d.rec_trigger);

        // OBS starts Recording with Sync Remote REC -> Program + rec_trigger=true
        in.obs_recording = true;
        in.sync_remote_rec = true;
        d = evaluate_auto_tally(in);
        CHECK(d.tally_state == lv::kTallyProgram && d.rec_trigger);
    }

    // 2. Live UDP Auto-Tally + Duplex Intercom through LanVidObsManager <-> Smartphone Camera
    auto& mgr = LanVidObsManager::instance();
    mgr.shutdown();
    CHECK(mgr.ensure_studio_started(0));
    const uint16_t studio_port = mgr.studio_port();
    CHECK(studio_port != 0);

    LanVidObsSourceEngine src;
    SourceConfig scfg{};
    scfg.source_name = "CAM 1 • OBS MAIN";
    scfg.listen_port = 0;
    scfg.camera_id = 1;
    scfg.auto_tally = true;
    scfg.sync_remote_rec = true;
    CHECK(src.start(scfg, nullptr, nullptr));

    lv::TallyIntercomEngine phone_cam;
    std::atomic<uint8_t> phone_tally{lv::kTallyStandby};
    std::atomic<bool> phone_rec{false};
    phone_cam.camera_set_tally_callback([&](const lv::TallyStatePayload& t) {
        phone_tally.store(t.tally_state);
        phone_rec.store((t.flags & lv::kTallyFlagRecTrigger) != 0);
    });
    phone_cam.camera_update_telemetry(88, 37);
    CHECK(phone_cam.start_camera("127.0.0.1", studio_port, 1, "iPhone 16 Pro"));

    // Register phone with OBS Studio manager
    for (int i = 0; i < 30; ++i) {
        phone_cam.poll();
        mgr.poll_studio();
        if (!mgr.tally_engine().studio_cameras().empty()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
    CHECK(!mgr.tally_engine().studio_cameras().empty());

    // OBS switches CAM 1 into Program + Recording -> phone gets PGM + Remote REC
    mgr.set_obs_global_state(/*studio_mode=*/true, /*streaming=*/true, /*recording=*/true);
    src.set_obs_visibility(/*in_program=*/true, /*in_preview=*/false);
    for (int i = 0; i < 25 && (phone_tally.load() != lv::kTallyProgram || !phone_rec.load()); ++i) {
        mgr.poll_studio();
        phone_cam.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(phone_tally.load() == lv::kTallyProgram);
    CHECK(phone_rec.load());

    // 3. Duplex Intercom: OBS Director Talkback -> Phone IFB & Phone PTT -> OBS Director
    mgr.set_director_mic_active(true, false, 1.0f, 0.0f);
    mgr.set_director_talk(1, true, true, false);
    phone_cam.set_intercom_state(true, false, 1.0f, 0.0f);

    std::vector<float> dir_voice(480), op_voice(480);
    for (size_t i = 0; i < 480; ++i) {
        dir_voice[i] = float(0.45 * std::sin(2.0 * 3.1415926535 * 550.0 * double(i) / 48000.0));
        op_voice[i] = float(0.40 * std::sin(2.0 * 3.1415926535 * 880.0 * double(i) / 48000.0));
    }
    std::vector<float> rx_phone(480, 0.0f), rx_obs(480, 0.0f);
    double phone_rms = 0.0, obs_rms = 0.0;
    for (int step = 0; step < 15; ++step) {
        mgr.push_director_mic_pcm(dir_voice.data(), 480);
        phone_cam.push_mic_pcm(op_voice.data(), 480);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        phone_cam.poll();
        mgr.poll_studio();
        if (phone_rms < 0.05) {
            const int n = phone_cam.pull_rx_pcm(rx_phone.data(), 480);
            if (n > 0) {
                double e = 0.0;
                for (int k = 0; k < n; ++k) e += double(rx_phone[k]) * double(rx_phone[k]);
                phone_rms = std::sqrt(e / double(n));
            }
        }
        if (obs_rms < 0.05) {
            const int n = mgr.pull_director_rx_pcm(rx_obs.data(), 480);
            if (n > 0) {
                double e = 0.0;
                for (int k = 0; k < n; ++k) e += double(rx_obs[k]) * double(rx_obs[k]);
                obs_rms = std::sqrt(e / double(n));
            }
        }
        if (phone_rms > 0.05 && obs_rms > 0.05) break;
    }
    CHECK(phone_rms > 0.05);
    CHECK(obs_rms > 0.05);
    CHECK(phone_cam.is_director_speaking());

    // 4. kControlSync quality preset payload check (360p Proxy / 1080p Studio / 4K60 Master)
    const auto p360 = make_quality_preset_control_sync(CameraQualityPreset::kProxy360p, 1, 0x4F425301);
    const auto p1080 = make_quality_preset_control_sync(CameraQualityPreset::kStudio1080p, 2, 0x4F425301);
    const auto p4k = make_quality_preset_control_sync(CameraQualityPreset::kMaster4K60, 3, 0x4F425301);
    CHECK(p360.active_preset_id == 0 && p360.target_buffer_ms == 20);
    CHECK(p1080.active_preset_id == 1 && p1080.target_buffer_ms == 15);
    CHECK(p4k.active_preset_id == 2 && p4k.target_buffer_ms == 10 && p4k.latency_mode == 0);

    // Verify Dock snapshot merges phone battery/temp + Tally
    const auto snap = mgr.snapshot_cameras();
    CHECK(snap.size() == 1);
    CHECK(snap[0].battery_pct == 88 && snap[0].temp_c == 37);
    CHECK(snap[0].effective_tally == lv::kTallyProgram && snap[0].remote_rec_active);

    phone_cam.stop();
    src.stop();
    mgr.shutdown();

    std::printf("    [obs-lanvid] Auto-Tally PGM+REC + Duplex Intercom (phone_rms=%.3f obs_rms=%.3f) + ControlSync OK\n",
                phone_rms, obs_rms);
}
