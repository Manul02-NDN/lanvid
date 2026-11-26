// LanVid Video Hub — Comprehensive unit & integration tests for the rewritten src/video_hub/
// covering: HubDiscovery + persistent device_uid -> CAM N badge sync (Echo Guard), isolated
// CameraSlotEngine + VideoPlayoutBuffer (Fixed Delay without freezes), IsoMultiRecorder across
// 3+ cameras, and StudioTallyIntercomHub + VirtualCamBridge (LanVidVideoBus_v1).

#include "mini_test.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "codec/video_encoder.hpp"
#include "common/clock.hpp"
#include "common/image.hpp"
#include "gui/app_config.hpp"
#include "gui/video_hub_ui.hpp"
#include "video/video_bus.hpp"
#include "video_hub/video_hub_engine.hpp"

namespace fs = std::filesystem;

namespace {

std::string make_tmp_hub_dir(const char* tag) {
    const fs::path d = fs::temp_directory_path() / (std::string("lanvid_hub_") + tag + "_" + std::to_string(lv::now_us()));
    fs::create_directories(d);
    return d.string();
}

}  // namespace

TEST(video_hub_auto_discovery_and_persistent_cam_badge_sync) {
    // 1. Persistent device_uid -> slot assignment survives reconnect / reload
    lv::hub::HubDiscovery disc;
    const int s0 = disc.resolve_or_assign_slot("iphone-uid-A", "192.168.1.21", 8, true);
    const int s1 = disc.resolve_or_assign_slot("pixel-uid-B", "192.168.1.22", 8, true);
    CHECK(s0 == 0 && s1 == 1);
    // Reconnect from changed DHCP IP still returns to CAM 1 (slot 0) via device_uid
    CHECK(disc.resolve_or_assign_slot("iphone-uid-A", "192.168.1.99", 8, true) == 0);

    // 2. Full VideoHubEngine <-> Phone bidirectional kVideoConfigSync & Echo Guard
    lv::hub::VideoHubEngine hub;
    lv::hub::VideoHubConfig hcfg{};
    hcfg.video_port = 0;
    hcfg.tally_port = 0;
    hcfg.discovery_port = 0;
    hcfg.max_slots = 8;
    hcfg.publish_vcam_bus = false;
    REQUIRE(hub.start(hcfg));
    const uint16_t tport = hub.bound_tally_port();
    REQUIRE(tport != 0);

    lv::TallyIntercomEngine phone;
    std::atomic<int> phone_sync_rx{0};
    lv::VideoConfigSyncPayload last_phone_cfg{};
    phone.set_video_config_sync_callback([&](const lv::VideoConfigSyncPayload& p) {
        if ((p.flags & lv::kVideoConfigFlagRequestIdr) != 0) return;
        last_phone_cfg = p;
        phone_sync_rx.fetch_add(1);
    });
    REQUIRE(phone.start_camera("127.0.0.1", tport, 11, "iPhone 16 Pro"));

    for (int i = 0; i < 60 && hub.slot_snapshot(0).camera_id == 0; ++i) {
        phone.poll();
        hub.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }
    REQUIRE(hub.slot_snapshot(0).camera_id == 11);

    // Assign CAM 3 + 60 Mbps + 33 ms Fixed delay -> phone receives update once (Echo Guard active)
    lv::hub::CameraRemoteSettings rs{};
    rs.assigned_cam_number = 3;
    rs.assigned_cam_tag = "CAM 3";
    rs.resolution_preset = lv::kQuality4K;
    rs.target_fps = 60;
    rs.bitrate_kbps = 60000;
    rs.video_delay_ms = 33;
    rs.fixed_delay_mode = true;
    rs.camera_lens = lv::kLensTele;
    REQUIRE(hub.apply_slot_remote_settings(0, rs, true));

    const int before = phone_sync_rx.load();
    for (int i = 0; i < 60 && last_phone_cfg.assigned_cam_number != 3; ++i) {
        hub.poll();
        phone.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }
    CHECK(last_phone_cfg.assigned_cam_number == 3);
    CHECK(std::string(last_phone_cfg.assigned_cam_tag) == "CAM 3");
    CHECK(last_phone_cfg.bitrate_kbps == 60000);
    CHECK(phone_sync_rx.load() >= before);

    // Reverse sync from phone (switching to Ultra-Wide 50fps) -> reflected in hub slot 0
    lv::VideoConfigSyncPayload rev = last_phone_cfg;
    rev.config_revision += 5;
    rev.origin_id = phone.origin_id();
    rev.camera_id = 11;
    rev.camera_lens = lv::kLensUltraWide;
    rev.target_fps = 50;
    rev.flags = lv::kVideoConfigFlagFromCamera;
    REQUIRE(phone.send_video_config_sync(11, rev));
    for (int i = 0; i < 50 && hub.slot_snapshot(0).settings.camera_lens != lv::kLensUltraWide; ++i) {
        phone.poll();
        hub.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }
    CHECK(hub.slot_snapshot(0).settings.camera_lens == lv::kLensUltraWide);
    CHECK(hub.slot_snapshot(0).settings.target_fps == 50);

    phone.stop();
    hub.stop();
}

TEST(video_hub_slot_fixed_delay_playout_no_freezes) {
    lv::hub::CameraSlotEngine slot(0);
    slot.set_playout_delay(33, true);
    std::atomic<int> idrs{0};
    slot.set_idr_callback([&](uint32_t, uint32_t) { idrs.fetch_add(1); });

    auto mk = [](uint8_t val) {
        lv::Image img;
        img.allocate(32, 32, lv::PixelOrder::kBGRA);
        std::memset(img.data.data(), val, img.data.size());
        return img;
    };

    const uint64_t base_us = 20'000'000ULL;
    const uint32_t jit_ms[] = {0, 15, 3, 21, 5, 11, 2, 19, 6, 8};
    uint32_t next_i = 0;
    uint64_t seq = 0;
    lv::Image out;
    int released = 0;
    for (uint64_t step_ms = 0; step_ms <= 600; step_ms += 10) {
        const uint64_t sim_us = base_us + step_ms * 1000ULL;
        while (next_i < 16) {
            const uint32_t mts = 500'000u + next_i * 33333u;
            const uint64_t arr = base_us + uint64_t(next_i) * 33333ULL + uint64_t(jit_ms[next_i % 10]) * 1000ULL;
            if (arr > sim_us) break;
            slot.push_decoded_frame(mk(uint8_t(30 + next_i)), next_i + 1, mts, arr, (next_i % 8 == 0), false, 30.0,
                                    "H.264");
            ++next_i;
        }
        if (slot.poll_playout_frame(sim_us, out, seq)) ++released;
    }
    CHECK(released >= 14);

    // Late/corrupt frames trigger rate-limited IDR while holding last clean frame
    for (int k = 0; k < 4; ++k) {
        slot.push_decoded_frame(mk(0xAA), 90 + k, 2'000'000u + k * 50'000u, base_us + 700'000ULL + k * 50'000ULL,
                                false, true);
    }
    const auto st = slot.snapshot();
    CHECK(st.settings.fixed_delay_mode);
    CHECK(st.playout_dropped_frames == 4);
    CHECK(st.playout_idr_requests == 1);
    CHECK(idrs.load() == 1);
}

TEST(video_hub_parallel_iso_recording_three_cameras) {
    const std::string out_dir = make_tmp_hub_dir("iso3");
    lv::hub::IsoMultiRecorder iso;
    std::array<std::string, lv::hub::kMaxHubSlots> labels{};
    labels[0] = "Wide";
    labels[1] = "Close";
    labels[2] = "Top";

    lv::hub::IsoMultiSessionConfig sc{};
    sc.output_dir = out_dir;
    sc.session_name = "HubIso";
    sc.container = lv::IsoContainer::kMp4;
    sc.record_program = false;
    sc.slot_mask = 0x07;
    sc.slot_count = 3;
    REQUIRE(iso.start_session(sc, labels));

    lv::VideoEncoderConfig ec{};
    ec.width = 128;
    ec.height = 96;
    ec.fps = 30.0;
    ec.bitrate_kbps = 800;
    ec.keyframe_interval = 15;
    ec.prefer_hardware = false;
    auto enc = lv::create_video_encoder(lv::VideoCodecId::kH264, ec);
    REQUIRE(enc && enc->init(ec));

    lv::Image img;
    img.allocate(128, 96, lv::PixelOrder::kBGRA);
    const uint64_t t0 = lv::now_us();
    for (int f = 0; f < 30; ++f) {
        std::memset(img.data.data(), uint8_t(40 + f * 3), img.data.size());
        lv::EncodedVideoPacket pkt;
        if (enc->encode_image(img, pkt) && !pkt.data.empty()) {
            const uint64_t ts = t0 + uint64_t(f) * 33'333ULL;
            for (uint32_t s = 0; s < 3; ++s) {
                iso.feed_encoded_access_unit(s, lv::IsoVideoCodec::kH264, pkt.data.data(), pkt.data.size(),
                                             pkt.is_keyframe, ts);
            }
        }
    }

    const auto st0 = iso.slot_stats(0);
    const auto st1 = iso.slot_stats(1);
    const auto st2 = iso.slot_stats(2);
    CHECK(st0.recording && st1.recording && st2.recording);
    CHECK(st0.frames == 30 && st1.frames == 30 && st2.frames == 30);
    iso.stop_session();

    std::error_code ec_fs;
    CHECK(fs::exists(st0.path, ec_fs) && fs::file_size(st0.path, ec_fs) > 1000);
    CHECK(fs::exists(st1.path, ec_fs) && fs::file_size(st1.path, ec_fs) > 1000);
    CHECK(fs::exists(st2.path, ec_fs) && fs::file_size(st2.path, ec_fs) > 1000);
    fs::remove_all(out_dir, ec_fs);
}

TEST(video_hub_tally_intercom_and_vcam_bus) {
    const std::string bus_name = "lv_hub_vbus_" + std::to_string(lv::now_us());
    lv::hub::VideoHubEngine hub;
    lv::hub::VideoHubConfig cfg{};
    cfg.video_port = 0;
    cfg.tally_port = 0;
    cfg.discovery_port = 0;
    cfg.max_slots = 4;
    cfg.publish_vcam_bus = true;
    cfg.vcam_bus_name = bus_name;
    cfg.vcam_count = 4;
    REQUIRE(hub.start(cfg));
    const uint16_t tport = hub.bound_tally_port();
    REQUIRE(tport != 0);

    // Phone connects on Tally + Intercom
    lv::TallyIntercomEngine phone;
    std::atomic<uint8_t> phone_tally{lv::kTallyStandby};
    phone.camera_set_tally_callback([&](const lv::TallyStatePayload& t) { phone_tally.store(t.tally_state); });
    REQUIRE(phone.start_camera("127.0.0.1", tport, 1, "iPhone Main"));
    for (int i = 0; i < 60 && hub.slot_snapshot(0).camera_id == 0; ++i) {
        phone.poll();
        hub.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }
    REQUIRE(hub.slot_snapshot(0).camera_id == 1);

    // Switch slot 0 out of Program, into Preview (Green), then Program (Red ON AIR)
    hub.set_program_slot(1);
    hub.set_preview_slot(0);
    for (int i = 0; i < 40 && phone_tally.load() != lv::kTallyPreview; ++i) {
        hub.poll();
        phone.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }
    CHECK(phone_tally.load() == lv::kTallyPreview);

    hub.set_program_slot(0);
    for (int i = 0; i < 40 && phone_tally.load() != lv::kTallyProgram; ++i) {
        hub.poll();
        phone.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }
    CHECK(phone_tally.load() == lv::kTallyProgram);

    // Intercom All Call -> audio arrives at phone IFB
    hub.tally_intercom().set_director_mic_state(true, false, 1.0f, 0.0f);
    hub.tally_intercom().set_all_call(true);
    std::vector<float> voice(480), rx(480, 0.0f);
    for (size_t i = 0; i < 480; ++i) voice[i] = float(0.45 * std::sin(2.0 * 3.14159 * 600.0 * double(i) / 48000.0));
    double rms = 0.0;
    for (int k = 0; k < 15 && rms < 0.05; ++k) {
        hub.tally_intercom().push_director_mic_pcm(voice.data(), 480);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        hub.poll();
        phone.poll();
        const int n = phone.pull_rx_pcm(rx.data(), 480);
        if (n > 0) {
            double e = 0.0;
            for (int m = 0; m < n; ++m) e += double(rx[m]) * double(rx[m]);
            rms = std::sqrt(e / double(n));
        }
    }
    CHECK(rms > 0.05);

    // VirtualCamBridge publishes slot 0 + Program Out to LanVidVideoBus_v1
    lv::Image frame;
    frame.allocate(320, 180, lv::PixelOrder::kBGRA);
    std::memset(frame.data.data(), 0x7Au, frame.data.size());
    hub.vcam().publish_frame(0, frame, true, lv::now_us(), 60.0);

    lv::VideoBus reader;
    REQUIRE(reader.open(bus_name.c_str()));
    std::vector<uint8_t> out_buf(lv::kVideoBusFrameBytes);
    lv::VideoBus::FrameInfo fi{};
    CHECK(reader.read_latest(0, out_buf.data(), out_buf.size(), fi));
    CHECK(fi.width == 320 && fi.height == 180);
    CHECK(reader.read_latest(lv::kVideoBusProgramSlot, out_buf.data(), out_buf.size(), fi));
    CHECK(fi.width == 320 && fi.height == 180);
    reader.close();

    phone.stop();
    hub.stop();
}
