// Coexistence & isolation unit tests for the 3 separate LanVid desktop apps:
//   1) LanVid Audio    (Audio 5004, Monitor 5020, Discovery 5021, role="audio")
//   2) LanVid Video    (Video 5006, Tally 5022,   Discovery 5023, role="video")
//   3) LanVid Intercom (Intercom 5024, Discovery 5025, role="intercom")
// All 3 services run concurrently on localhost with zero UDP port conflicts,
// separate JSON config files, and full C ABI Duplex Intercom + Tally parity.

#include "mini_test.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "audio_service.hpp"
#include "intercom_service.hpp"
#include "lanvid/lanvid_sdk.h"
#include "net/discovery.hpp"
#include "video_service.hpp"

namespace fs = std::filesystem;

namespace {

std::string make_temp_cfg_dir(const char* tag) {
    const fs::path p = fs::temp_directory_path() /
                       (std::string("lanvid_split_") + tag + "_" +
                        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(p);
    return p.string();
}

}  // namespace

TEST(separate_apps_coexistence_no_port_conflict) {
    // Verify canonical non-overlapping ports across all 3 standalone apps
    CHECK(lv::kAudioStreamPort == 5004);
    CHECK(lv::kVideoStreamPort == 5006);
    CHECK(lv::kAudioMonitorPort == 5020);
    CHECK(lv::kAudioDiscoveryPort == 5021);
    CHECK(lv::kVideoTallyPort == 5022);
    CHECK(lv::kVideoDiscoveryPort == 5023);
    CHECK(lv::apps::kIntercomDefaultPort == 5024);
    CHECK(lv::apps::kIntercomDiscoveryPort == 5025);

    const std::string dir = make_temp_cfg_dir("coexist");
    lv::apps::LanVidAudioService audio_svc(dir + "/lanvid_audio_config.json");
    lv::apps::LanVidVideoService video_svc(dir + "/lanvid_video_config.json");
    lv::apps::LanVidIntercomService intercom_svc(dir + "/lanvid_intercom_config.json");

    // Start all 3 services simultaneously on their default ports (with high-port fallback if a live app holds defaults).
    bool audio_ok = audio_svc.start_services();
    bool video_ok = video_svc.start_services();
    bool intercom_ok = intercom_svc.start_services();
    if (!audio_ok || !video_ok || !intercom_ok) {
        intercom_svc.stop_services();
        video_svc.stop_services();
        audio_svc.stop_services();
        audio_ok = audio_svc.start_services(25004, 25020, 25021);
        video_ok = video_svc.start_services(25006, 25022, 25023);
        intercom_ok = intercom_svc.start_services(25024, 25025);
    }

    REQUIRE(audio_ok);
    REQUIRE(video_ok);
    REQUIRE(intercom_ok);

    CHECK(audio_svc.services_running());
    CHECK(video_svc.services_running());
    CHECK(intercom_svc.services_running());

    const uint16_t a_mon = audio_svc.bound_monitor_port();
    const uint16_t a_disc = audio_svc.bound_discovery_port();
    const uint16_t v_vid = video_svc.bound_video_port();
    const uint16_t v_tal = video_svc.bound_tally_port();
    const uint16_t v_disc = video_svc.bound_discovery_port();
    const uint16_t i_com = intercom_svc.bound_intercom_port();
    const uint16_t i_disc = intercom_svc.bound_discovery_port();

    CHECK(a_mon != 0);
    CHECK(a_disc != 0);
    CHECK(v_vid != 0);
    CHECK(v_tal != 0);
    CHECK(v_disc != 0);
    CHECK(i_com != 0);
    CHECK(i_disc != 0);

    // Every socket across the 3 services must hold a distinct bound port
    const std::vector<uint16_t> ports = {a_mon, a_disc, v_vid, v_tal, v_disc, i_com, i_disc};
    for (size_t i = 0; i < ports.size(); ++i) {
        for (size_t j = i + 1; j < ports.size(); ++j) {
            CHECK(ports[i] != ports[j]);
        }
    }

    // Verify independent JSON persistence for all 3 apps
    audio_svc.config().studio_name = "Audio Suite";
    audio_svc.config().channels = 32;
    REQUIRE(audio_svc.save_config());

    video_svc.config().hub_name = "Video Hub";
    video_svc.config().max_slots = 12;
    REQUIRE(video_svc.save_config());

    intercom_svc.config().desk_name = "Director Desk";
    intercom_svc.config().auto_ducking_db = -18.0f;
    REQUIRE(intercom_svc.save_config());

    {
        lv::apps::LanVidAudioService audio_reload(dir + "/lanvid_audio_config.json");
        lv::apps::LanVidVideoService video_reload(dir + "/lanvid_video_config.json");
        lv::apps::LanVidIntercomService intercom_reload(dir + "/lanvid_intercom_config.json");
        REQUIRE(audio_reload.load_config());
        REQUIRE(video_reload.load_config());
        REQUIRE(intercom_reload.load_config());

        CHECK(audio_reload.config().studio_name == "Audio Suite");
        CHECK(audio_reload.config().channels == 32);
        CHECK(video_reload.config().hub_name == "Video Hub");
        CHECK(video_reload.config().max_slots == 12);
        CHECK(intercom_reload.config().desk_name == "Director Desk");
        CHECK(std::abs(intercom_reload.config().auto_ducking_db - (-18.0f)) < 0.05f);
    }

    intercom_svc.stop_services();
    video_svc.stop_services();
    audio_svc.stop_services();

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST(separate_apps_config_and_intercom_lifecycle) {
    const std::string dir = make_temp_cfg_dir("intercom");
    lv::apps::LanVidIntercomService intercom(dir + "/lanvid_intercom_config.json");
    REQUIRE(intercom.start_services(0, 0));

    const uint16_t desk_port = intercom.bound_intercom_port();
    REQUIRE(desk_port != 0);

    // Connect a beltpack camera client on slot 1 using the pure C ABI
    LvTallyIntercomHandle* beltpack = lv_tally_intercom_create_camera("127.0.0.1", desk_port, 1, "CAM 1");
    REQUIRE(beltpack != nullptr);

    struct TallyTracker {
        std::atomic<uint8_t> tally{0};
        std::atomic<uint8_t> rec{0};
    } tracker;
    lv_camera_set_tally_callback(
        beltpack,
        [](const LV_TallyState* st, void* ud) {
            if (!st || !ud) return;
            auto* t = static_cast<TallyTracker*>(ud);
            t->tally.store(st->tally_state, std::memory_order_release);
            t->rec.store((st->flags & 1u) ? 1u : 0u, std::memory_order_release);
        },
        &tracker);

    // Configure Partyline Group A on slot 0 (camera_id = 1) and enable Program Tally + Auto-Ducking
    intercom.set_operator_group(0, lv::apps::PartylineGroup::kGroupA);
    intercom.set_operator_talk(0, true, true);
    intercom.set_operator_tally(0, 2 /* PGM */, true);
    intercom.set_partyline_talk(lv::apps::PartylineGroup::kGroupA, true);
    intercom.set_director_state(true, false, 1.0f, 0.0f);

    // Pump Director voice -> Beltpack and verify roundtrip arrival
    std::vector<float> tx_tone(480);
    for (int i = 0; i < 480; ++i) {
        tx_tone[static_cast<size_t>(i)] = 0.45f * std::sin(2.0f * 3.14159265f * 1000.0f * i / 48000.0f);
    }

    int beltpack_rx = 0;
    std::vector<float> rx_buf(480, 0.0f);
    for (int iter = 0; iter < 50; ++iter) {
        if (iter == 5 || iter == 15) {
            intercom.set_operator_tally(0, 2 /* PGM */, true);
        }
        intercom.push_director_mic_pcm(tx_tone.data(), 480);
        intercom.poll();

        lv_intercom_push_mic_pcm(beltpack, tx_tone.data(), 480);
        lv_tally_intercom_poll(beltpack);

        const int got = lv_intercom_pull_rx_pcm(beltpack, rx_buf.data(), 480);
        if (got > 0) beltpack_rx += got;

        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    CHECK(beltpack_rx > 0);
    CHECK(tracker.tally.load(std::memory_order_acquire) == 2);
    CHECK(tracker.rec.load(std::memory_order_acquire) == 1);

    lv_tally_intercom_destroy(beltpack);
    intercom.stop_services();

    std::error_code ec;
    fs::remove_all(dir, ec);
}
