#include "mini_test.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "crosslink.hpp"
#include "crosslink/crosslink_sdk.h"

TEST(crosslink_c_api_version_and_abi) {
    REQUIRE(cl_init() == 1);
    CHECK(std::string(cl_get_version_string()) == CL_SDK_VERSION_STRING);
    CHECK(cl_get_version_number() ==
          ((uint32_t(CL_SDK_VERSION_MAJOR) << 16) | (uint32_t(CL_SDK_VERSION_MINOR) << 8) |
           uint32_t(CL_SDK_VERSION_PATCH)));
    CHECK(cl_check_abi_compatibility(CL_SDK_VERSION_MAJOR, CL_SDK_VERSION_MINOR) == 1);
    CHECK(cl_check_abi_compatibility(CL_SDK_VERSION_MAJOR + 1, 0) == 0);
    CHECK(cl_check_abi_compatibility(CL_SDK_VERSION_MAJOR, CL_SDK_VERSION_MINOR + 1) == 0);
    CHECK(std::strlen(cl_error_string(CL_OK)) > 0);
    CHECK(std::strlen(cl_get_cpu_simd_info()) > 0);
    cl_shutdown();
}

TEST(crosslink_cpp_raii_aoip_midi_controlsync_and_discovery) {
    crosslink::Runtime rt;
    REQUIRE(rt.check_abi());

    std::atomic<int> midi_rx{0};
    std::atomic<int> cs_rx{0};
    std::atomic<int> mc_blocks{0};

    crosslink::Receiver rx;
    rx.on_midi([&](const CL_MidiEvent& ev) {
        if (ev.status == 0x90 && ev.data1 == 67 && ev.data2 == 112)
            midi_rx.fetch_add(1, std::memory_order_relaxed);
    });
    rx.on_control_sync([&](const CL_ControlSyncState& cs) {
        if (cs.audio_channels == 16 && cs.audio_bitrate_kbps == 192)
            cs_rx.fetch_add(1, std::memory_order_relaxed);
    });
    rx.on_multichannel_audio([&](const CL_MultichannelAudioBlock& b) {
        if (b.data && b.num_channels == 8 && b.num_frames == 240)
            mc_blocks.fetch_add(1, std::memory_order_relaxed);
    });

    auto rcfg = crosslink::Receiver::default_config(0, 10, 8, 48000);
    std::snprintf(rcfg.bind_ip, sizeof(rcfg.bind_ip), "127.0.0.1");
    rcfg.discovery_port = 15221;
    std::snprintf(rcfg.device_name, sizeof(rcfg.device_name), "CrossLinkUnitAudioNode");
    REQUIRE(rx.start(rcfg).is_ok());

    const uint16_t port = rx.local_port();
    REQUIRE(port > 0);

    // Structured & JSON LAN Audio Node discovery on :15221
    const auto nodes = crosslink::Discovery::scan(300, 15221);
    bool found = false;
    for (const auto& n : nodes) {
        if (n.rx_port == port && std::string(n.name) == "CrossLinkUnitAudioNode") found = true;
    }
    CHECK(found);

    char* json = cl_discovery_scan_json(250, 15221);
    REQUIRE(json != nullptr);
    CHECK(std::string(json).find("CrossLinkUnitAudioNode") != std::string::npos);
    cl_free_string(json);

    crosslink::Sender tx;
    auto scfg = crosslink::Sender::default_config("127.0.0.1", port, 10, 48000);
    REQUIRE(tx.start(scfg).is_ok());

    constexpr int kCh = 8;
    constexpr int kFrames = 240;
    std::vector<float> buf(kCh * kFrames, 0.0f);
    for (int f = 0; f < kFrames; ++f) {
        for (int c = 0; c < kCh; ++c)
            buf[f * kCh + c] = 0.2f * std::sin(float(f) * 0.08f + float(c));
    }

    CL_ControlSyncState cs{};
    cs.revision = 1;
    cs.origin_id = 0x1234u;
    cs.audio_channels = 16;
    cs.audio_bitrate_kbps = 192;
    cs.target_buffer_ms = 10;
    cs.latency_mode = 1;
    REQUIRE(tx.send_control_sync(cs).is_ok());
    REQUIRE(tx.push_midi(0x90, 67, 112, 0).is_ok());

    for (int i = 0; i < 12; ++i) {
        REQUIRE(tx.push_multichannel(buf.data(), kCh, kFrames).is_ok());
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    for (int i = 0; i < 60 && (midi_rx.load() == 0 || cs_rx.load() == 0 || mc_blocks.load() == 0); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    CHECK(midi_rx.load() >= 1);
    CHECK(cs_rx.load() >= 1);
    CHECK(mc_blocks.load() >= 1);
    CHECK(rx.channel_stats(0).packets_rx > 0);
    CHECK(rx.channel_stats(7).packets_rx > 0);

    tx.stop();
    rx.stop();
}

TEST(crosslink_c_api_personal_monitor_loopback) {
    ClMonitorServerHandle* srv = cl_monitor_server_create(48000);
    ClMonitorClientHandle* cli = cl_monitor_client_create(48000);
    REQUIRE(srv && cli);
    REQUIRE(cl_monitor_server_start(srv, 0) == CL_OK);
    const uint16_t port = cl_monitor_server_port(srv);
    REQUIRE(port > 0);

    cl_monitor_server_set_channel_name(srv, 0, "LeadVox");
    cl_monitor_server_set_channel_name(srv, 1, "KeysL");
    REQUIRE(cl_monitor_client_connect(cli, "127.0.0.1", port, "DrummerIEM", 128, 12) == CL_OK);

    std::vector<float> ch0(240, 0.3f), ch1(240, -0.3f);
    const float* planes[2] = {ch0.data(), ch1.data()};

    CL_MonitorTelemetry tel{};
    CL_MonitorMetaChannel meta[8]{};
    int meta_count = 0;
    for (int i = 0; i < 40; ++i) {
        cl_monitor_server_push_audio_planar(srv, planes, 2, 240);
        cl_monitor_client_request_meta(cli);
        cl_monitor_client_poll(cli);
        meta_count = cl_monitor_client_get_meta(cli, meta, 8, &tel);
        if (tel.connected && tel.audio_packets_rx > 0 && meta_count >= 2) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    CHECK(tel.connected == 1);
    CHECK(tel.audio_packets_rx > 0);
    CHECK(meta_count >= 2);
    CHECK(std::string(meta[0].name) == "LeadVox");

    std::vector<float> stereo(240 * 2, 0.0f);
    cl_monitor_client_pop_stereo(cli, stereo.data(), 240);

    cl_monitor_client_disconnect(cli);
    cl_monitor_server_stop(srv);
    cl_monitor_client_destroy(cli);
    cl_monitor_server_destroy(srv);
}
