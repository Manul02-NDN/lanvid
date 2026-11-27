#include "mini_test.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <vector>

#include "sources/test_pattern.hpp"
#include "codec/frame_codec.hpp"
#include "net/adaptive_quality.hpp"
#include "net/assembler.hpp"
#include "net/packet.hpp"
#include "net/packetizer.hpp"

TEST(fec_single_loss_full_recovery_all_positions) {
    // Encode a real frame with small MTU so slices span > 1 FEC group (e.g. K > 6 with fec_group_size=6).
    constexpr int W = 640, H = 160;
    lv::Image src, ref;
    src.allocate(W, H, lv::PixelOrder::kRGBA);
    lv::generate_test_frame(src, 19);

    lv::CodecParams p;
    p.width = W;
    p.height = H;
    p.chroma = lv::ChromaFormat::k420;
    p.quality = 85;
    p.slice_height = 16;  // 10 slices

    lv::EncodedFrame enc;
    REQUIRE(lv::encode_frame(src, p, enc));
    REQUIRE(lv::decode_frame(enc, ref).slices_failed == 0);

    lv::PacketizerConfig pcfg;
    pcfg.mtu = 300;  // ~264 B max data payload -> multiple FEC groups per slice!
    pcfg.enable_crc = true;
    pcfg.fec_group_size = 6;
    std::atomic<uint16_t> seq{0};

    std::vector<std::vector<uint8_t>> all_pkts;
    for (int sid = 0; sid < enc.geom.total_slices; ++sid) {
        lv::packetize_slice(1, 1000, uint16_t(sid), uint16_t(enc.geom.total_slices),
                            enc.slices[size_t(sid)].data(), enc.slices[size_t(sid)].size(),
                            pcfg, seq, [&](const uint8_t* d, size_t n) {
                                CHECK(n <= pcfg.mtu);
                                all_pkts.emplace_back(d, d + n);
                            });
    }

    // Drop 1 data packet from EVERY FEC group in EVERY slice:
    // - group 0 of slice 0: drop first fragment (fi=0)
    // - last group of each slice: drop the shorter final fragment (fi=frag_count-1)
    // - middle groups: drop a middle fragment
    std::vector<std::vector<uint8_t>> filtered;
    uint64_t expected_drops = 0;
    bool saw_multi_group = false;

    for (const auto& pkt : all_pkts) {
        lv::PacketHeader h;
        REQUIRE(lv::parse_packet(pkt.data(), pkt.size(), h) == lv::PacketParseStatus::kOk);
        if ((h.flags & lv::kFlagFecParity) == 0) {
            const uint16_t grp = h.frag_index / pcfg.fec_group_size;
            const uint16_t grp_start = uint16_t(grp * pcfg.fec_group_size);
            const uint16_t grp_end = std::min<uint16_t>(h.frag_count, uint16_t(grp_start + pcfg.fec_group_size));
            if (grp > 0) saw_multi_group = true;

            uint16_t drop_fi = grp_start;
            if (grp_end == h.frag_count) {
                drop_fi = uint16_t(h.frag_count - 1);  // test recovering the shorter final fragment!
            } else if ((h.slice_id + grp) % 2 == 1) {
                drop_fi = uint16_t(grp_start + (grp_end - grp_start) / 2);
            }
            if (h.frag_index == drop_fi) {
                ++expected_drops;
                continue;
            }
        }
        filtered.push_back(pkt);
    }
    REQUIRE(saw_multi_group);
    REQUIRE(expected_drops >= uint64_t(enc.geom.total_slices));

    // Test BOTH normal arrival order AND reverse order (FEC parity arrives BEFORE data packets!).
    for (int rev = 0; rev < 2; ++rev) {
        auto stream = filtered;
        if (rev) std::reverse(stream.begin(), stream.end());

        lv::Image decoded;
        int got_frames = 0;
        lv::AssemblerConfig acfg;
        acfg.frame_timeout_us = 25000;
        lv::FrameAssembler asm_buf(acfg, [&](const lv::CompletedFrame& cf) {
            CHECK(cf.slices_ok == enc.geom.total_slices);
            CHECK(cf.slices_lost == 0);
            CHECK(cf.slices_corrupt == 0);
            CHECK(cf.quality == 85);
            decoded = *cf.image;
            ++got_frames;
        });

        uint64_t t = 5000;
        for (const auto& d : stream) {
            CHECK(asm_buf.ingest_datagram(d.data(), d.size(), t) == lv::PacketParseStatus::kOk);
            t += 5;
        }
        // Frame must finalize IMMEDIATELY via FEC without waiting for timeout!
        REQUIRE(got_frames == 1);
        CHECK(asm_buf.stats().frames_complete == 1);
        CHECK(asm_buf.stats().frames_concealed == 0);
        CHECK(asm_buf.stats().fec_recovered_packets == expected_drops);
        CHECK(decoded.data == ref.data);
    }
}

TEST(fec_double_loss_falls_back_to_concealment) {
    constexpr int W = 320, H = 128;
    lv::Image src, ref;
    src.allocate(W, H, lv::PixelOrder::kRGBA);
    lv::generate_test_frame(src, 33);

    lv::CodecParams p;
    p.width = W;
    p.height = H;
    p.quality = 85;
    p.slice_height = 16;  // 8 slices

    lv::EncodedFrame enc;
    REQUIRE(lv::encode_frame(src, p, enc));
    REQUIRE(lv::decode_frame(enc, ref).slices_failed == 0);

    lv::PacketizerConfig pcfg;
    pcfg.mtu = 250;
    pcfg.fec_group_size = 6;
    std::atomic<uint16_t> seq{0};

    std::vector<std::vector<uint8_t>> pkts;
    for (int sid = 0; sid < enc.geom.total_slices; ++sid) {
        lv::packetize_slice(1, 1000, uint16_t(sid), uint16_t(enc.geom.total_slices),
                            enc.slices[size_t(sid)].data(), enc.slices[size_t(sid)].size(),
                            pcfg, seq, [&](const uint8_t* d, size_t n) {
                                pkts.emplace_back(d, d + n);
                            });
    }

    // Slice 2: drop 2 packets in group 0 (unrecoverable -> must conceal ONLY slice 2).
    // Slice 5: drop 1 packet in group 0 (recoverable -> must decode bit-exact via FEC!).
    lv::Image out;
    int lost_slices = -1;
    lv::AssemblerConfig acfg;
    acfg.conceal = lv::ConcealMode::kGray;
    acfg.frame_timeout_us = 25000;
    lv::FrameAssembler asm_buf(acfg, [&](const lv::CompletedFrame& cf) {
        out = *cf.image;
        lost_slices = cf.slices_lost;
    });

    uint64_t t = 1000;
    for (const auto& d : pkts) {
        lv::PacketHeader h;
        REQUIRE(lv::parse_packet(d.data(), d.size(), h) == lv::PacketParseStatus::kOk);
        if ((h.flags & lv::kFlagFecParity) == 0) {
            if (h.slice_id == 2 && (h.frag_index == 0 || h.frag_index == 1)) continue;
            if (h.slice_id == 5 && h.frag_index == 0) continue;
        }
        asm_buf.ingest_datagram(d.data(), d.size(), t);
        t += 5;
    }
    asm_buf.poll(1000 + acfg.frame_timeout_us + 100);

    REQUIRE(lost_slices == 1);
    CHECK(asm_buf.stats().fec_recovered_packets == 1);
    // Slice 5 recovered cleanly; slice 2 concealed gray; all other slices bit-exact.
    for (int sid = 0; sid < enc.geom.total_slices; ++sid) {
        for (int y = sid * 16; y < (sid + 1) * 16; ++y) {
            if (sid == 2) {
                CHECK(out.row(y)[0] == 128 && out.row(y)[1] == 128 && out.row(y)[2] == 128);
            } else {
                CHECK(std::memcmp(out.row(y), ref.row(y), ref.stride) == 0);
            }
        }
    }
}

TEST(feedback_packet_roundtrip_and_aimd_controller) {
    // 1. FeedbackPayload wire serialization + CRC32C roundtrip.
    lv::FeedbackPayload fb{};
    fb.last_completed_frame_id = 4242;
    fb.packet_loss_permille = 52;
    fb.fec_recovered_permille = 80;
    fb.jitter_us = 310;
    fb.decode_time_us = 950;

    uint8_t pl[lv::kFeedbackSize];
    lv::write_feedback(pl, fb);

    lv::PacketHeader h{};
    h.type = lv::PacketType::kFeedback;
    h.flags = lv::kFlagHasCrc;
    h.frame_id = fb.last_completed_frame_id;
    uint8_t dgram[64];
    const size_t n = lv::write_packet(dgram, sizeof(dgram), h, pl, sizeof(pl));
    REQUIRE(n == lv::kPacketHeaderSize + lv::kFeedbackSize);

    lv::PacketHeader oh{};
    const uint8_t* opl = nullptr;
    REQUIRE(lv::parse_packet(dgram, n, oh, &opl) == lv::PacketParseStatus::kOk);
    CHECK(oh.type == lv::PacketType::kFeedback);
    lv::FeedbackPayload out{};
    REQUIRE(lv::parse_feedback(opl, oh.payload_size, out));
    CHECK(out.last_completed_frame_id == fb.last_completed_frame_id);
    CHECK(out.packet_loss_permille == fb.packet_loss_permille);
    CHECK(out.fec_recovered_permille == fb.fec_recovered_permille);
    CHECK(out.jitter_us == fb.jitter_us);
    CHECK(out.decode_time_us == fb.decode_time_us);

    // 2. AIMD controller behavior: drop on loss (>0.5%) or high FEC (>5%), ramp up after 500ms clean.
    lv::AdaptiveQualityConfig cfg;
    cfg.enabled = true;
    cfg.q_min = 35;
    cfg.q_max = 85;
    cfg.initial_q = 85;
    lv::AdaptiveQualityController aq(cfg);
    CHECK(aq.current_quality() == 85);

    uint64_t t = 1000000;
    lv::FeedbackPayload loss_fb{};
    loss_fb.packet_loss_permille = 50;  // 5.0% loss

    // First drop: 85 * 0.82 = 69.7 -> 69
    CHECK(aq.on_feedback(loss_fb, t));
    CHECK(aq.current_quality() == 69);

    // Cooldown (<200 ms): must not double-drop on next 100 ms tick.
    CHECK(!aq.on_feedback(loss_fb, t + 100000));
    CHECK(aq.current_quality() == 69);

    // Next drops every 200 ms down to q_min = 35.
    for (int step = 1; step <= 8; ++step) {
        aq.on_feedback(loss_fb, t + uint64_t(step) * 200000);
    }
    CHECK(aq.current_quality() == 35);

    // Clean channel: must hold steady for first 400 ms (< 500 ms hold).
    lv::FeedbackPayload clean_fb{};
    uint64_t t_clean = t + 2000000;
    for (int i = 0; i < 5; ++i) {
        aq.on_feedback(clean_fb, t_clean + uint64_t(i) * 100000);  // 0..400 ms
    }
    CHECK(aq.current_quality() == 35);

    // At +500 ms clean -> first +2 step (35 -> 37), then +2 every 200 ms back up to 85!
    CHECK(aq.on_feedback(clean_fb, t_clean + 500000));
    CHECK(aq.current_quality() == 37);
    for (int i = 1; i <= 30; ++i) {
        aq.on_feedback(clean_fb, t_clean + 500000 + uint64_t(i) * 200000);
    }
    CHECK(aq.current_quality() == 85);

    // High FEC recovery (>5%) even with 0% loss -> triggers multiplicative decrease!
    lv::FeedbackPayload fec_stress{};
    fec_stress.packet_loss_permille = 0;
    fec_stress.fec_recovered_permille = 80;  // 8%
    CHECK(aq.on_feedback(fec_stress, t_clean + 10000000));
    CHECK(aq.current_quality() == 69);
}
