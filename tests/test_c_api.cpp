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
#include "codec/video_codec_common.hpp"
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
    CHECK(std::string(lv_get_version_string()) == LV_SDK_VERSION_STRING);
    CHECK(lv_get_version_number() ==
          ((uint32_t(LV_SDK_VERSION_MAJOR) << 16) | (uint32_t(LV_SDK_VERSION_MINOR) << 8) |
           uint32_t(LV_SDK_VERSION_PATCH)));
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

TEST(android_yuv420_padded_stride_and_front_camera_rotation) {
    // Simulate Android front camera: 1920x1080 YUV_420_888 (SENSOR_ORIENTATION=270, LENS_FACING_FRONT)
    // with 128-byte garbage (0xFF) row padding, uvPixelStride=2 (NV12), and Android's exact
    // last-chroma-row quirk where uPlane ends at (width - 1) bytes on the last row.
    // After 270 deg CW rotation, width becomes 1080 (non-multiple-of-16: 67*16 + 8) x 1920!
    constexpr int W = 1920;
    constexpr int H = 1080;
    constexpr int kPad = 128;
    constexpr int y_stride = W + kPad;
    constexpr int uv_stride = W + kPad;
    constexpr int cw = W / 2;
    constexpr int ch = H / 2;

    std::vector<uint8_t> y_plane(size_t(y_stride) * size_t(H), 0xFF);
    // Exact Android NV12 shared UV buffer size: last row has no padding after the final V byte.
    const size_t uv_buf_sz = size_t(uv_stride) * size_t(ch - 1) + size_t(W);
    std::vector<uint8_t> uv_plane(uv_buf_sz, 0xFF);

    // Smooth gradient pattern (no 0xFF values anywhere in active area so any padding leak stands out)
    auto expected_y = [](int sx, int sy) -> uint8_t {
        return uint8_t(32 + ((sx * 160) / W) + ((sy * 48) / H));
    };
    auto expected_u = [](int cx, int cy) -> uint8_t {
        return uint8_t(64 + ((cx * 120) / cw));
    };
    auto expected_v = [](int cx, int cy) -> uint8_t {
        return uint8_t(64 + ((cy * 120) / ch));
    };

    for (int sy = 0; sy < H; ++sy) {
        uint8_t* row = y_plane.data() + size_t(sy) * size_t(y_stride);
        for (int sx = 0; sx < W; ++sx) row[sx] = expected_y(sx, sy);
    }
    for (int cy = 0; cy < ch; ++cy) {
        uint8_t* row = uv_plane.data() + size_t(cy) * size_t(uv_stride);
        for (int cx = 0; cx < cw; ++cx) {
            row[cx * 2 + 0] = expected_u(cx, cy);
            row[cx * 2 + 1] = expected_v(cx, cy);
        }
    }

    lv::Yuv420View src_yuv{};
    src_yuv.y = y_plane.data();
    src_yuv.u = uv_plane.data();
    src_yuv.v = uv_plane.data() + 1;
    src_yuv.y_stride = y_stride;
    src_yuv.u_stride = uv_stride;
    src_yuv.v_stride = uv_stride;
    src_yuv.uv_pixel_stride = 2;

    std::vector<uint8_t> i420_buf;
    int dst_w = 0, dst_h = 0;
    lv::Yuv420View rot_yuv{};
    REQUIRE(lv::yuv420_transform_to_i420(src_yuv, W, H, 270, true, i420_buf, dst_w, dst_h, rot_yuv));
    REQUIRE(dst_w == H);  // 1080 (non-multiple-of-16!)
    REQUIRE(dst_h == W);  // 1920
    REQUIRE(rot_yuv.y_stride == dst_w);
    REQUIRE(rot_yuv.uv_pixel_stride == 1);

    // 1. Exact bit-for-bit verification of every Y, U, V sample (zero padding contamination)
    for (int sy = 0; sy < H; ++sy) {
        const int dx = dst_w - 1 - sy;  // 270 CW puts sy -> dx_rot=sy, then mirror -> dst_w-1-sy
        for (int sx = 0; sx < W; ++sx) {
            const int dy = W - 1 - sx;
            const uint8_t got = rot_yuv.y[size_t(dy) * size_t(rot_yuv.y_stride) + size_t(dx)];
            REQUIRE(got == expected_y(sx, sy));
        }
    }
    const int dst_cw = dst_w / 2;
    for (int cy = 0; cy < ch; ++cy) {
        const int dcx = dst_cw - 1 - cy;
        for (int cx = 0; cx < cw; ++cx) {
            const int dcy = cw - 1 - cx;
            const uint8_t gu = rot_yuv.u[size_t(dcy) * size_t(rot_yuv.u_stride) + size_t(dcx)];
            const uint8_t gv = rot_yuv.v[size_t(dcy) * size_t(rot_yuv.v_stride) + size_t(dcx)];
            REQUIRE(gu == expected_u(cx, cy));
            REQUIRE(gv == expected_v(cx, cy));
        }
    }

    // 2. Also verify yuv420_to_nv12 last-row bounds safety on padded NV12 input
    std::vector<uint8_t> nv12_out(size_t(W) * size_t(H) * 3 / 2, 0);
    lv::yuv420_to_nv12(src_yuv, W, H, nv12_out.data(), W, nv12_out.data() + size_t(W) * size_t(H), W);
    CHECK(nv12_out[size_t(W) * size_t(H) + size_t(ch - 1) * size_t(W) + size_t(W - 2)] ==
          expected_u(cw - 1, ch - 1));
    CHECK(nv12_out[size_t(W) * size_t(H) + size_t(ch - 1) * size_t(W) + size_t(W - 1)] ==
          expected_v(cw - 1, ch - 1));

    // 3. Full LanVid DCT encode -> decode roundtrip at rotated 1080x1920 (non-16-aligned width!)
    lv::CodecParams cp;
    cp.width = dst_w;
    cp.height = dst_h;
    cp.quality = 85;
    cp.chroma = lv::ChromaFormat::k420;
    cp.slice_height = 16;

    lv::FrameCodec fc;
    lv::EncodedFrame ef;
    ef.params = cp;
    REQUIRE(lv::compute_geometry(cp, ef.geom));
    ef.slices.resize(size_t(ef.geom.total_slices));
    REQUIRE(fc.encode_streaming_yuv420(
        rot_yuv, cp,
        [&](int sid, int, const uint8_t* data, size_t size) {
            ef.slices[size_t(sid)].assign(data, data + size);
        },
        nullptr));

    lv::Image dec;
    const auto dr = fc.decode(ef, dec, nullptr);
    REQUIRE(dr.slices_ok == ef.geom.total_slices);
    REQUIRE(dec.width == 1080 && dec.height == 1920);

    // Compare decoded RGB against reference YCbCr->RGB of rotated I420 (PSNR > 35 dB)
    lv::Image ref;
    ref.allocate(dst_w, dst_h, dec.order);
    const lv::Planes ref_pl{const_cast<uint8_t*>(rot_yuv.y), const_cast<uint8_t*>(rot_yuv.u),
                            const_cast<uint8_t*>(rot_yuv.v), rot_yuv.y_stride, rot_yuv.u_stride};
    lv::ycbcr_to_rgba(ref_pl, dst_h, dst_w, lv::ChromaFormat::k420, lv::ColorMatrix::kBT601Full,
                      ref.data.data(), ref.stride, dec.order);

    double mse = 0.0;
    const size_t n = size_t(dst_w) * size_t(dst_h);
    for (size_t i = 0; i < n; ++i) {
        for (int c = 0; c < 3; ++c) {
            const double d = double(dec.data[i * 4 + c]) - double(ref.data[i * 4 + c]);
            mse += d * d;
        }
    }
    mse /= double(n * 3);
    const double psnr = mse > 0.0 ? 10.0 * std::log10((255.0 * 255.0) / mse) : 99.0;
    std::printf("    [Android 1920x1080->1080x1920 rot=270 mirror=1 pad=128] PSNR=%.2f dB\n", psnr);
    CHECK(psnr > 35.0);

    // 4. End-to-end C ABI lv_sender_push_yuv_planes_ex UDP loopback check
    std::atomic<int> rx_w{0}, rx_h{0}, rx_cnt{0};
    lanvid::Receiver rx;
    rx.on_video([&](const LV_VideoFrameView& f) {
        rx_w.store(f.width, std::memory_order_relaxed);
        rx_h.store(f.height, std::memory_order_relaxed);
        rx_cnt.fetch_add(1, std::memory_order_relaxed);
    });
    auto rcfg = lanvid::Receiver::default_config(0, LV_PIXEL_FORMAT_RGBA8888);
    std::snprintf(rcfg.bind_ip, sizeof(rcfg.bind_ip), "127.0.0.1");
    rcfg.enable_discovery = 0;
    REQUIRE(rx.start(rcfg).is_ok());

    LvSenderHandle* tx = lv_sender_create();
    REQUIRE(tx != nullptr);
    LV_SenderConfig scfg{};
    lv_sdk_sender_config_default(&scfg);
    std::snprintf(scfg.dest_ip, sizeof(scfg.dest_ip), "127.0.0.1");
    scfg.dest_port = rx.local_port();
    scfg.width = W;
    scfg.height = H;
    scfg.quality = 80;
    REQUIRE(lv_sdk_sender_start(tx, &scfg) == LV_OK);

    LV_YuvPlanes yp{y_plane.data(), uv_plane.data(), uv_plane.data() + 1, y_stride, uv_stride,
                    uv_stride, 2, LV_PIXEL_FORMAT_NV12};
    REQUIRE(lv_sender_push_yuv_planes_ex(tx, &yp, W, H, 270, 1, 0) == LV_OK);

    for (int i = 0; i < 80 && rx_cnt.load() < 1; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(rx_cnt.load() >= 1);
    CHECK(rx_w.load() == 1080);
    CHECK(rx_h.load() == 1920);

    lv_sender_stop(tx);
    lv_sender_destroy(tx);
    rx.stop();
}
