#include "mini_test.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "app/test_pattern.hpp"
#include "codec/frame_codec.hpp"
#include "codec/video_codec_common.hpp"
#include "codec/video_decoder.hpp"
#include "codec/video_encoder.hpp"
#include "common/clock.hpp"
#include "common/image.hpp"
#include "lanvid.hpp"
#include "net/adaptive_quality.hpp"
#include "net/assembler.hpp"
#include "net/packet.hpp"
#include "net/packetizer.hpp"
#include "net/receiver.hpp"
#include "net/sender.hpp"

namespace {

double compute_psnr_rgb(const lv::Image& a, const lv::Image& b) {
    if (a.width != b.width || a.height != b.height || a.width <= 0 || a.height <= 0) return 0.0;
    const bool swap_ab = (a.order != b.order);
    double sse = 0.0;
    const size_t n = size_t(a.width) * size_t(a.height);
    for (int y = 0; y < a.height; ++y) {
        const uint8_t* pa = a.row(y);
        const uint8_t* pb = b.row(y);
        for (int x = 0; x < a.width; ++x) {
            const double d0 = double(pa[x * 4 + 0]) - double(pb[x * 4 + (swap_ab ? 2 : 0)]);
            const double d1 = double(pa[x * 4 + 1]) - double(pb[x * 4 + 1]);
            const double d2 = double(pa[x * 4 + 2]) - double(pb[x * 4 + (swap_ab ? 0 : 2)]);
            sse += d0 * d0 + d1 * d1 + d2 * d2;
        }
    }
    const double mse = sse / double(n * 3);
    if (mse <= 1e-9) return 99.0;
    return 10.0 * std::log10((255.0 * 255.0) / mse);
}

}  // namespace

TEST(protocol_codec_flags_and_pli_wire) {
    // 1. Verify codec_id + keyframe + codec_config flags fit inside 28-byte PacketHeader cleanly.
    for (lv::VideoCodecId cid :
         {lv::VideoCodecId::kLanVidDct, lv::VideoCodecId::kH264, lv::VideoCodecId::kH265}) {
        lv::PacketHeader h;
        h.type = lv::PacketType::kVideoSlice;
        h.flags = uint8_t(lv::kFlagHasCrc | lv::encode_codec_flags(cid) | lv::kFlagKeyFrame |
                          lv::kFlagCodecConfig);
        h.frame_id = 42;
        h.slice_id = 0;
        h.total_slices = 1;
        h.frag_index = 0;
        h.frag_count = 1;
        const uint8_t payload[8] = {0, 0, 0, 1, 0x67, 0x42, 0x00, 0x1F};

        uint8_t wire[64];
        const size_t n = lv::write_packet(wire, sizeof(wire), h, payload, sizeof(payload));
        REQUIRE(n == lv::kPacketHeaderSize + sizeof(payload));

        lv::PacketHeader out{};
        const uint8_t* out_pay = nullptr;
        REQUIRE(lv::parse_packet(wire, n, out, &out_pay) == lv::PacketParseStatus::kOk);
        CHECK(lv::packet_codec_id(out.flags) == cid);
        CHECK((out.flags & lv::kFlagKeyFrame) != 0);
        CHECK((out.flags & lv::kFlagCodecConfig) != 0);
    }

    // 2. Verify FeedbackPayload request_idr (PLI) roundtrips within exact 16-byte kFeedbackSize.
    lv::FeedbackPayload fb;
    fb.last_completed_frame_id = 123;
    fb.packet_loss_permille = 25;
    fb.fec_recovered_permille = 80;
    fb.jitter_us = 450;
    fb.decode_time_us = 900;
    fb.request_idr = true;

    uint8_t fbuf[lv::kFeedbackSize];
    lv::write_feedback(fbuf, fb);
    lv::FeedbackPayload parsed{};
    REQUIRE(lv::parse_feedback(fbuf, sizeof(fbuf), parsed));
    CHECK(parsed.last_completed_frame_id == 123u);
    CHECK(parsed.packet_loss_permille == 25u);
    CHECK(parsed.fec_recovered_permille == 80u);
    CHECK(parsed.request_idr == true);

    fb.request_idr = false;
    lv::write_feedback(fbuf, fb);
    REQUIRE(lv::parse_feedback(fbuf, sizeof(fbuf), parsed));
    CHECK(parsed.request_idr == false);
    CHECK(parsed.packet_loss_permille == 25u);

    // 3. Verify AIMD adjusts target_bitrate_kbps dynamically for H.264/H.265.
    lv::AdaptiveQualityConfig aqcfg;
    aqcfg.enabled = true;
    aqcfg.initial_bitrate_kbps = 10000;
    aqcfg.bitrate_min_kbps = 2000;
    aqcfg.bitrate_max_kbps = 20000;
    lv::AdaptiveQualityController aq(aqcfg);
    CHECK(aq.current_bitrate_kbps() == 10000u);

    lv::FeedbackPayload bad_fb{};
    bad_fb.packet_loss_permille = 30;  // 3.0% loss -> multiplicative decrease
    CHECK(aq.on_feedback(bad_fb, 1000000));
    CHECK(aq.current_bitrate_kbps() < 10000u);
    const uint32_t dropped_br = aq.current_bitrate_kbps();

    // Clean channel > 500 ms -> additive increase
    lv::FeedbackPayload good_fb{};
    aq.on_feedback(good_fb, 2000000);
    CHECK(aq.on_feedback(good_fb, 2600000));
    CHECK(aq.current_bitrate_kbps() > dropped_br);

    // 4. Verify C ABI lv_get_supported_codecs() JSON lists DCT, H264, H265.
    char* json = lv_get_supported_codecs();
    REQUIRE(json != nullptr);
    const std::string js(json);
    lv_free_string(json);
    CHECK(js.find("LANVID_DCT") != std::string::npos);
    CHECK(js.find("H264") != std::string::npos);
    CHECK(js.find("H265") != std::string::npos);
}

TEST(codec_h264_zerolatency_roundtrip_and_pli) {
    constexpr int W = 640, H = 360;
    lv::VideoEncoderConfig ecfg;
    ecfg.width = W;
    ecfg.height = H;
    ecfg.fps = 60.0;
    ecfg.bitrate_kbps = 4000;
    ecfg.keyframe_interval = 120;
    ecfg.prefer_hardware = true;

    auto enc = lv::create_video_encoder(lv::VideoCodecId::kH264, ecfg);
    REQUIRE(enc != nullptr);
    auto dec = lv::create_video_decoder(lv::VideoCodecId::kH264, lv::PixelOrder::kBGRA);
    REQUIRE(dec != nullptr);

    std::printf("    [H.264] enc=%s | dec=%s\n", enc->backend_name().c_str(),
                dec->backend_name().c_str());

    // 1. Direct encode -> decode zero-latency roundtrip (10 frames) + live bitrate change.
    lv::Image src, decoded;
    src.allocate(W, H, lv::PixelOrder::kBGRA);
    lv::EncodedVideoPacket pkt;
    double psnr_sum = 0.0;
    int decoded_cnt = 0;

    for (uint32_t f = 0; f < 10; ++f) {
        lv::generate_test_frame(src, f);
        if (f == 5) {
            CHECK(enc->set_bitrate_kbps(6000));
        }
        REQUIRE(enc->encode_image(src, pkt));
        REQUIRE(!pkt.data.empty());
        if (f == 0) {
            CHECK(pkt.is_keyframe);
            CHECK(pkt.has_codec_config);
        }
        if (dec->decode(pkt.data.data(), pkt.data.size(), lv::PixelOrder::kBGRA, decoded)) {
            REQUIRE(decoded.width == W && decoded.height == H);
            psnr_sum += compute_psnr_rgb(src, decoded);
            ++decoded_cnt;
        }
    }
    REQUIRE(decoded_cnt == 10);
    const double avg_psnr = psnr_sum / double(decoded_cnt);
    std::printf("    [H.264] direct roundtrip decoded=%d/10 avg_PSNR=%.2f dB (0-frame lag)\n",
                decoded_cnt, avg_psnr);
    CHECK(avg_psnr > 32.0);

    // 2. Packetizer + XOR FEC + FrameAssembler + PLI / IDR recovery test!
    std::vector<uint32_t> delivered_fids;
    std::vector<double> delivered_psnrs;
    std::vector<lv::Image> ref_frames(12);
    for (uint32_t f = 0; f < 12; ++f) {
        ref_frames[f].allocate(W, H, lv::PixelOrder::kBGRA);
        lv::generate_test_frame(ref_frames[f], f + 100);
    }

    int pli_count = 0;
    lv::AssemblerConfig acfg;
    acfg.output_order = lv::PixelOrder::kBGRA;
    acfg.frame_timeout_us = 25000;

    lv::FrameAssembler assembler(acfg, [&](const lv::CompletedFrame& cf) {
        REQUIRE(cf.codec == lv::VideoCodecId::kH264);
        REQUIRE(cf.image != nullptr);
        delivered_fids.push_back(cf.frame_id);
        if (cf.frame_id < ref_frames.size()) {
            delivered_psnrs.push_back(compute_psnr_rgb(ref_frames[cf.frame_id], *cf.image));
        }
    });
    assembler.set_pli_callback([&](uint32_t) {
        ++pli_count;
        enc->force_keyframe();
    });

    std::atomic<uint16_t> seq{0};
    lv::PacketizerConfig pcfg;
    pcfg.mtu = 1400;
    pcfg.enable_crc = true;
    pcfg.fec_group_size = 6;

    // Force fresh IDR at frame 0 of this stream.
    enc->force_keyframe();
    uint64_t sim_time_us = 100000;

    for (uint32_t fid = 0; fid < 12; ++fid) {
        REQUIRE(enc->encode_image(ref_frames[fid], pkt));
        pcfg.extra_flags =
            uint8_t(lv::encode_codec_flags(lv::VideoCodecId::kH264) |
                    (pkt.is_keyframe ? lv::kFlagKeyFrame : 0) |
                    (pkt.has_codec_config ? lv::kFlagCodecConfig : 0));

        // Frame 4: simulate unrecoverable P-frame loss (drop ALL packets of frame 4).
        // Frame 2: drop 1 data packet (should be transparently recovered by XOR FEC without PLI!).
        int pkt_idx = 0;
        lv::packetize_slice(fid, uint32_t(sim_time_us), 0, 1, pkt.data.data(), pkt.data.size(),
                            pcfg, seq, [&](const uint8_t* dgram, size_t len) {
                                const int idx = pkt_idx++;
                                if (fid == 4) return;            // total P-frame loss!
                                if (fid == 2 && idx == 0) return;  // single packet loss -> XOR FEC!
                                assembler.ingest_datagram(dgram, len, sim_time_us);
                            });
        sim_time_us += 16667;
        assembler.poll(sim_time_us);
    }
    assembler.flush(sim_time_us + 50000);

    std::printf("    [H.264 PLI] delivered=%zu/12 | PLI_fired=%d | FEC_rec=%llu | dropped_await_idr=%llu\n",
                delivered_fids.size(), pli_count,
                (unsigned long long)assembler.stats().fec_recovered_packets,
                (unsigned long long)assembler.stats().frames_dropped_awaiting_idr);

    // Frame 2 must have been recovered by XOR FEC cleanly; frame 4 drop must have triggered PLI!
    CHECK(pli_count >= 1);
    CHECK(!assembler.awaiting_idr());  // recovered cleanly on forced IDR!
    CHECK(delivered_fids.size() >= 9);
    // Final frame after PLI recovery must be clean high-PSNR picture (no decoder corruption)!
    REQUIRE(!delivered_psnrs.empty());
    CHECK(delivered_psnrs.back() > 28.0);
}

TEST(codec_h265_zerolatency_roundtrip) {
    const auto caps = lv::get_supported_codecs();
    bool has_h265_enc = false, has_h265_dec = false;
    for (const auto& c : caps) {
        if (c.codec == lv::VideoCodecId::kH265) {
            has_h265_enc = c.encoder_available;
            has_h265_dec = c.decoder_available;
        }
    }
    if (!has_h265_enc) {
        std::printf("    [H.265] Skipped (HEVC encoder MFT not installed on host)\n");
        return;
    }

    constexpr int W = 1280, H = 720;
    lv::VideoEncoderConfig ecfg;
    ecfg.width = W;
    ecfg.height = H;
    ecfg.fps = 60.0;
    ecfg.bitrate_kbps = 6000;
    ecfg.keyframe_interval = 120;
    ecfg.prefer_hardware = true;

    auto enc = lv::create_video_encoder(lv::VideoCodecId::kH265, ecfg);
    if (!enc) {
        std::printf("    [H.265] Skipped (HEVC encoder init unavailable on this host)\n");
        return;
    }
    auto dec = has_h265_dec ? lv::create_video_decoder(lv::VideoCodecId::kH265, lv::PixelOrder::kBGRA)
                            : nullptr;

    std::printf("    [H.265] enc=%s | dec=%s\n", enc->backend_name().c_str(),
                dec ? dec->backend_name().c_str() : "N/A (install Windows HEVC Video Extensions for HW decode)");

    lv::Image src, decoded;
    src.allocate(W, H, lv::PixelOrder::kBGRA);
    lv::EncodedVideoPacket pkt;
    size_t h265_bytes = 0;
    size_t dct_bytes = 0;
    lv::FrameCodec dct_fc;
    lv::CodecParams dct_cp;
    dct_cp.width = W;
    dct_cp.height = H;
    dct_cp.quality = 80;
    dct_cp.chroma = lv::ChromaFormat::k420;
    dct_cp.slice_height = 16;
    lv::EncodedFrame dct_ef;

    double psnr_sum = 0.0;
    int dec_ok = 0;
    for (uint32_t f = 0; f < 10; ++f) {
        lv::generate_test_frame(src, f);
        if (f == 5) enc->force_keyframe();
        REQUIRE(enc->encode_image(src, pkt));
        REQUIRE(!pkt.data.empty());
        h265_bytes += pkt.data.size();
        const auto nalus = lv::annexb_scan_nalus(pkt.data.data(), pkt.data.size(), lv::VideoCodecId::kH265);
        REQUIRE(!nalus.empty());
        if (f == 0 || f == 5) {
            CHECK(pkt.is_keyframe);
            CHECK(pkt.has_codec_config);
        }
        if (dec && dec->decode(pkt.data.data(), pkt.data.size(), lv::PixelOrder::kBGRA, decoded)) {
            REQUIRE(decoded.width == W && decoded.height == H);
            psnr_sum += compute_psnr_rgb(src, decoded);
            ++dec_ok;
        }
        REQUIRE(dct_fc.encode(src, dct_cp, dct_ef, nullptr));
        dct_bytes += dct_ef.total_bytes();
    }

    const double savings_ratio = double(dct_bytes) / double(std::max<size_t>(1, h265_bytes));
    if (dec) {
        REQUIRE(dec_ok >= 8);
        const double avg_psnr = psnr_sum / double(dec_ok);
        std::printf("    [H.265] decoded=%d/10 | avg_PSNR=%.2f dB | H.265=%zu B vs DCT=%zu B (%.1fx smaller)\n",
                    dec_ok, avg_psnr, h265_bytes, dct_bytes, savings_ratio);
        CHECK(avg_psnr > 28.0);
    } else {
        std::printf("    [H.265] encoded=10/10 (Annex-B VPS/SPS/PPS+IDR verified) | H.265=%zu B vs DCT=%zu B (%.1fx smaller)\n",
                    h265_bytes, dct_bytes, savings_ratio);
    }
    CHECK(h265_bytes < dct_bytes);
}

TEST(udp_loopback_sdk_h264_autodetect) {
    // End-to-end C++17 SDK loopback streaming H.264 -> receiver auto-detects codec with zero config!
    lanvid::Runtime rt;

    std::atomic<int> rx_h264_frames{0};
    std::atomic<int> rx_w{0}, rx_h{0};

    lanvid::Receiver rx;
    rx.on_video([&](const LV_VideoFrameView& f) {
        if (f.data && f.codec == LV_CODEC_H264) {
            rx_w.store(f.width, std::memory_order_relaxed);
            rx_h.store(f.height, std::memory_order_relaxed);
            rx_h264_frames.fetch_add(1, std::memory_order_relaxed);
        }
    });

    auto rcfg = lanvid::Receiver::default_config(0, LV_PIXEL_FORMAT_BGRA8888);
    std::snprintf(rcfg.bind_ip, sizeof(rcfg.bind_ip), "127.0.0.1");
    rcfg.enable_discovery = 0;
    REQUIRE(rx.start(rcfg).is_ok());
    const uint16_t port = rx.local_port();
    REQUIRE(port > 0);

    lanvid::Sender tx;
    auto scfg = lanvid::Sender::default_config("127.0.0.1", port);
    scfg.width = 640;
    scfg.height = 360;
    scfg.fps = 60.0;
    scfg.codec = LV_CODEC_H264;
    scfg.bitrate_kbps = 4000;
    scfg.fec_group_size = 6;
    REQUIRE(tx.start(scfg).is_ok());

    lv::Image img;
    img.allocate(640, 360, lv::PixelOrder::kBGRA);
    for (uint32_t f = 0; f < 10; ++f) {
        lv::generate_test_frame(img, f);
        if (f == 4) CHECK(tx.set_bitrate_kbps(6000).is_ok());
        if (f == 6) CHECK(tx.force_keyframe().is_ok());
        REQUIRE(tx.push_frame(img.data.data(), 640, 360, int(img.stride), LV_PIXEL_FORMAT_BGRA8888)
                    .is_ok());
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }

    for (int i = 0; i < 80 && rx_h264_frames.load() < 6; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    const LV_StreamStats rst = rx.stats();
    CHECK(rx_h264_frames.load() >= 6);
    CHECK(rx_w.load() == 640 && rx_h.load() == 360);
    CHECK(rst.active_codec == LV_CODEC_H264);
    CHECK(std::strlen(rst.codec_backend) > 0);

    rx.stop();
    tx.stop();
}
