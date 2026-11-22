#include "mini_test.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "codec/frame_codec.hpp"
#include "codec/slice_codec.hpp"
#include "common/clock.hpp"
#include "common/image.hpp"
#include "lanvid.hpp"
#include "lanvid_c_api.h"

TEST(c_api_init_and_device_enum) {
    REQUIRE(lv_init() == 1);
    const char* simd = lv_get_cpu_simd_info();
    REQUIRE(simd != nullptr);
    CHECK(std::strlen(simd) > 0);

    char* json = lv_enumerate_devices();
    REQUIRE(json != nullptr);
    const std::string s(json);
    lv_free_string(json);

    CHECK(s.find("\"os\":") != std::string::npos);
    CHECK(s.find("\"monitors\":[") != std::string::npos);
    CHECK(s.find("\"cameras\":[") != std::string::npos);
    CHECK(s.find("\"audio_loopback\":[") != std::string::npos);
    CHECK(s.find("\"audio_mic\":[") != std::string::npos);
    CHECK(s.find("\"network_interfaces\":[") != std::string::npos);
    lv_shutdown();
}

TEST(c_api_yuv420_direct_encode_decode) {
    // Tests direct YUV_420_888 encoding for BOTH planar I420 (uv_pixel_stride=1)
    // and Android Camera2 semi-planar NV12 (uv_pixel_stride=2) with zero RGB roundtrip.
    constexpr int W = 160, H = 96;
    std::vector<uint8_t> y(W * H);
    std::vector<uint8_t> u_planar((W / 2) * (H / 2));
    std::vector<uint8_t> v_planar((W / 2) * (H / 2));
    std::vector<uint8_t> uv_interleaved((W / 2) * (H / 2) * 2);

    for (int r = 0; r < H; ++r) {
        for (int c = 0; c < W; ++c) {
            y[size_t(r) * W + c] = uint8_t((c * 255) / W);
        }
    }
    for (int r = 0; r < H / 2; ++r) {
        for (int c = 0; c < W / 2; ++c) {
            const uint8_t cb = uint8_t(90 + (r * 70) / (H / 2));
            const uint8_t cr = uint8_t(165 - (c * 70) / (W / 2));
            u_planar[size_t(r) * (W / 2) + c] = cb;
            v_planar[size_t(r) * (W / 2) + c] = cr;
            uv_interleaved[(size_t(r) * (W / 2) + c) * 2 + 0] = cb;
            uv_interleaved[(size_t(r) * (W / 2) + c) * 2 + 1] = cr;
        }
    }

    lv::CodecParams cp;
    cp.width = W;
    cp.height = H;
    cp.quality = 85;
    cp.chroma = lv::ChromaFormat::k420;
    cp.slice_height = 16;

    lv::FrameCodec fc;
    lv::EncodedFrame ef_planar, ef_nv12;
    ef_planar.params = ef_nv12.params = cp;
    REQUIRE(lv::compute_geometry(cp, ef_planar.geom));
    ef_nv12.geom = ef_planar.geom;
    ef_planar.slices.resize(size_t(ef_planar.geom.total_slices));
    ef_nv12.slices.resize(size_t(ef_nv12.geom.total_slices));

    lv::Yuv420View view_planar{y.data(), u_planar.data(), v_planar.data(), W, W / 2, W / 2, 1};
    lv::Yuv420View view_nv12{y.data(), uv_interleaved.data(), uv_interleaved.data() + 1, W, W, W, 2};

    REQUIRE(fc.encode_streaming_yuv420(
        view_planar, cp,
        [&](int sid, int, const uint8_t* data, size_t sz) { ef_planar.slices[sid].assign(data, data + sz); },
        nullptr));
    REQUIRE(fc.encode_streaming_yuv420(
        view_nv12, cp,
        [&](int sid, int, const uint8_t* data, size_t sz) { ef_nv12.slices[sid].assign(data, data + sz); },
        nullptr));

    // Planar I420 and semi-planar NV12 representing the same image must produce bit-identical slices!
    for (size_t i = 0; i < ef_planar.slices.size(); ++i) {
        CHECK(ef_planar.slices[i] == ef_nv12.slices[i]);
    }

    lv::Image dec;
    dec.allocate(W, H, lv::PixelOrder::kRGBA);
    const auto res = fc.decode(ef_nv12, dec, nullptr);
    CHECK(res.slices_ok == ef_nv12.geom.total_slices);
    CHECK(res.slices_failed == 0);
}

TEST(udp_loopback_c_api_and_discovery) {
    REQUIRE(lv_init() == 1);

    LvReceiverHandle* rx = lv_receiver_create();
    REQUIRE(rx != nullptr);

    LvReceiverConfig rcfg;
    lv_receiver_config_default(&rcfg);
    std::snprintf(rcfg.bind_ip, sizeof(rcfg.bind_ip), "127.0.0.1");
    rcfg.bind_port = 0;  // ephemeral port
    rcfg.decode_threads = 4;
    rcfg.enable_audio = 0;
    rcfg.external_audio_pull = 1;
    rcfg.enable_discovery = 1;
    rcfg.discovery_port = 15099;
    std::snprintf(rcfg.device_name, sizeof(rcfg.device_name), "TestReceiverUnit");
    REQUIRE(lv_receiver_start(rx, &rcfg) == 1);

    const uint16_t rx_port = lv_receiver_get_local_port(rx);
    REQUIRE(rx_port > 0);

    // 1. Verify LAN discovery finds our active receiver on custom discovery port.
    char* disc_json = lv_discovery_scan_port(350, 15099);
    REQUIRE(disc_json != nullptr);
    const std::string disc(disc_json);
    lv_free_string(disc_json);
    CHECK(disc.find("TestReceiverUnit") != std::string::npos);
    CHECK(disc.find("\"port\":" + std::to_string(rx_port)) != std::string::npos);

    // 2. Push external RGBA + YUV420 frames + PCM16 audio via C API and verify receipt.
    LvSenderHandle* tx = lv_sender_create();
    REQUIRE(tx != nullptr);
    LvSenderConfig scfg;
    lv_sender_config_default(&scfg);
    std::snprintf(scfg.dest_ip, sizeof(scfg.dest_ip), "127.0.0.1");
    scfg.dest_port = rx_port;
    scfg.source_type = LV_SOURCE_EXTERNAL;
    scfg.width = 160;
    scfg.height = 96;
    scfg.quality = 82;
    scfg.threads = 4;
    REQUIRE(lv_sender_start(tx, &scfg) == 1);

    std::vector<uint8_t> rgba(160 * 96 * 4, 180);
    std::vector<uint8_t> y(160 * 96, 200), u(80 * 48, 128), v(80 * 48, 128);
    int16_t pcm[240 * 2];
    for (int i = 0; i < 240 * 2; ++i) pcm[i] = 10000;

    for (int i = 0; i < 4; ++i) {
        REQUIRE(lv_sender_push_raw_frame(tx, rgba.data(), 160, 96, 160 * 4, LV_PIXFMT_RGBA, 0) == 1);
        REQUIRE(lv_sender_push_yuv420_frame(tx, y.data(), u.data(), v.data(), 160, 96, 160, 80, 80, 1, 0) == 1);
        REQUIRE(lv_sender_push_audio_pcm16(tx, pcm, 240, 48000, 2, 0) == 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    std::vector<uint8_t> out_rgba(160 * 96 * 4);
    int ow = 0, oh = 0;
    uint32_t seq = 0;
    bool got = false;
    for (int i = 0; i < 80; ++i) {
        if (lv_receiver_get_latest_frame(rx, out_rgba.data(), out_rgba.size(), &ow, &oh, &seq, 1) == 1) {
            got = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(got);
    CHECK(ow == 160 && oh == 96);

    LvReceiverStats rst{};
    for (int i = 0; i < 80; ++i) {
        if (lv_receiver_get_stats(rx, &rst) == 1 && rst.audio_packets_rx >= 4) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    // Check volume control & external AAudio-style pull
    lv_receiver_set_volume(rx, 0.5f);
    int16_t apcm[240 * 2]{};
    CHECK(lv_receiver_pop_audio_pcm16(rx, apcm, 240) == 240);

    CHECK(lv_receiver_get_stats(rx, &rst) == 1);
    CHECK(rst.frames_complete >= 1);
    CHECK(rst.audio_packets_rx == 4);

    lv_sender_stop(tx);
    lv_sender_destroy(tx);
    lv_receiver_stop(rx);
    lv_receiver_destroy(rx);
    lv_shutdown();
}

TEST(sdk_callbacks_and_version_check) {
    // 1. Version & ABI compatibility checks
    CHECK(std::string(lv_get_version_string()) == "1.0.0");
    CHECK(lv_get_version_number() == ((1u << 16) | (0u << 8) | 0u));
    CHECK(lv_check_abi_compatibility(LV_SDK_VERSION_MAJOR, LV_SDK_VERSION_MINOR) == 1);
    CHECK(lv_check_abi_compatibility(LV_SDK_VERSION_MAJOR + 1, 0) == 0);
    CHECK(lv_check_abi_compatibility(LV_SDK_VERSION_MAJOR, LV_SDK_VERSION_MINOR + 1) == 0);
    CHECK(std::strlen(lv_error_string(LV_OK)) > 0);
    CHECK(std::strlen(lv_error_string(LV_ERR_SOCKET)) > 0);

    // 2. Test C++17 Header-Only RAII Wrapper (lanvid.hpp) with zero-copy callbacks!
    lanvid::Runtime rt;
    CHECK(rt.check_abi());

    std::atomic<int> video_cb_frames{0};
    std::atomic<int> audio_cb_chunks{0};
    std::atomic<int> last_state{LV_STATE_STOPPED};
    std::atomic<bool> saw_connected{false};
    std::atomic<uint8_t> last_q{0};

    lanvid::Receiver rx;
    rx.on_video([&](const LV_VideoFrameView& f) {
        if (f.data && f.width == 160 && f.height == 96) {
            video_cb_frames.fetch_add(1, std::memory_order_relaxed);
            last_q.store(f.quality, std::memory_order_relaxed);
        }
    });
    rx.on_audio([&](const LV_AudioChunkView& a) {
        if (a.data && a.num_frames == 240 && a.sample_rate == 48000) {
            audio_cb_chunks.fetch_add(1, std::memory_order_relaxed);
        }
    });
    rx.on_state([&](LV_StreamState st, LV_ErrorCode, const char*) {
        last_state.store(int(st), std::memory_order_relaxed);
        if (st == LV_STATE_CONNECTED) saw_connected.store(true, std::memory_order_relaxed);
    });

    auto rcfg = lanvid::Receiver::default_config(0, LV_PIXEL_FORMAT_RGBA8888);
    std::snprintf(rcfg.bind_ip, sizeof(rcfg.bind_ip), "127.0.0.1");
    rcfg.discovery_port = 15101;
    std::snprintf(rcfg.device_name, sizeof(rcfg.device_name), "SdkCallbackTest");
    REQUIRE(rx.start(rcfg).is_ok());

    const uint16_t port = rx.local_port();
    REQUIRE(port > 0);

    // Structured peer discovery (zero-JSON LV_DiscoveredPeer API)
    const auto peers = lanvid::Discovery::scan(300, 15101);
    bool found_self = false;
    for (const auto& p : peers) {
        if (p.port == port && p.name == "SdkCallbackTest") found_self = true;
    }
    CHECK(found_self);

    lanvid::Sender tx;
    auto scfg = lanvid::Sender::default_config("127.0.0.1", port);
    scfg.width = 160;
    scfg.height = 96;
    scfg.quality = 72;
    scfg.fec_group_size = 4;
    scfg.threads = 4;
    REQUIRE(tx.start(scfg).is_ok());

    // Push NV12 via CVPixelBuffer planes + packed BGRA + live quality/FEC change + F32 audio!
    std::vector<uint8_t> y(160 * 96, 190), uv(160 * 48, 128), bgra(160 * 96 * 4, 150);
    float f32_pcm[240 * 2]{};
    for (int i = 0; i < 240; ++i) f32_pcm[i * 2] = f32_pcm[i * 2 + 1] = 0.25f;

    LV_YuvPlanes nv21{y.data(), uv.data(), nullptr, 160, 160, 160, 2, LV_PIXEL_FORMAT_NV21};
    REQUIRE(tx.push_yuv(nv21, 160, 96).is_ok());
    REQUIRE(tx.push_frame(bgra.data(), 160, 96, 160 * 4, LV_PIXEL_FORMAT_BGRA8888).is_ok());
    REQUIRE(tx.set_quality(90).is_ok());
    REQUIRE(tx.set_fec_group(6).is_ok());
    REQUIRE(tx.set_adaptive_quality(false, 35, 90).is_ok());
    REQUIRE(tx.push_frame(bgra.data(), 160, 96, 160 * 4, LV_PIXEL_FORMAT_BGRA8888).is_ok());
    REQUIRE(tx.push_audio(f32_pcm, 240, 48000, 2, LV_AUDIO_FORMAT_F32).is_ok());
    REQUIRE(tx.push_audio(f32_pcm, 240, 48000, 2, LV_AUDIO_FORMAT_F32).is_ok());

    for (int i = 0; i < 80 && (video_cb_frames.load() < 3 || audio_cb_chunks.load() < 2); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    CHECK(video_cb_frames.load() == 3);
    CHECK(audio_cb_chunks.load() == 2);
    CHECK(saw_connected.load());
    CHECK(last_q.load() == 90);  // live quality change took effect immediately!

    rx.stop();
    CHECK(last_state.load() == LV_STATE_STOPPED);
    tx.stop();
}
