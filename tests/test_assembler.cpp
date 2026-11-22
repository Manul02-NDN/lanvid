#include "mini_test.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <set>
#include <thread>
#include <vector>

#include "app/test_pattern.hpp"
#include "codec/frame_codec.hpp"
#include "common/clock.hpp"
#include "net/assembler.hpp"
#include "net/packetizer.hpp"
#include "net/receiver.hpp"
#include "net/sender.hpp"

namespace {

uint32_t xorshift32(uint32_t& s) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

}  // namespace

TEST(seq_loss_tracker_loss_dups_reorder_wrap) {
    lv::SeqLossTracker tr;
    std::vector<uint16_t> stream;
    constexpr uint16_t kStart = 65500;
    constexpr int kTotal = 1000;
    int dropped = 0;
    uint32_t rng = 0x13579BDFu;

    for (int i = 0; i < kTotal; ++i) {
        const uint16_t seq = uint16_t(kStart + i);
        if ((xorshift32(rng) % 20u) == 0 && i > 0 && i < kTotal - 1) {  // ~5% drop
            ++dropped;
            continue;
        }
        stream.push_back(seq);
    }

    // Local reordering within 8-packet blocks.
    for (size_t i = 0; i + 8 <= stream.size(); i += 8) {
        std::swap(stream[i + 1], stream[i + 5]);
        std::swap(stream[i + 2], stream[i + 7]);
    }

    // Inject 25 duplicates.
    for (int d = 0; d < 25; ++d) {
        stream.insert(stream.begin() + ptrdiff_t(d * 10 + 3), stream[size_t(d * 10)]);
    }

    for (uint16_t s : stream) tr.observe(s);

    CHECK(tr.received() == uint64_t(kTotal - dropped));
    CHECK(tr.duplicates() == 25);
    CHECK(tr.reordered() > 0);
    // With margin=0 (end of stream), exact lost count must equal `dropped`.
    CHECK(tr.lost(0) == uint64_t(dropped));
}

TEST(assembler_5pct_loss_dups_reorder_and_concealment) {
    // Stage 4 acceptance criterion:
    // 5% random packet loss + duplicates + reordering -> only affected slices concealed,
    // all remaining slices match reference decoded frame bit-for-bit!
    constexpr int W = 640, H = 320;
    lv::CodecParams p;
    p.width = W;
    p.height = H;
    p.chroma = lv::ChromaFormat::k420;
    p.quality = 85;
    p.slice_height = 16;  // 20 slices

    // Encode 2 frames: frame 0 arrives intact (sets prev_output), frame 1 experiences 5% loss/dups/reorder.
    lv::Image src0, src1, ref0, ref1;
    src0.allocate(W, H);
    src1.allocate(W, H);
    lv::generate_test_frame(src0, 0);
    lv::generate_test_frame(src1, 1);

    lv::EncodedFrame enc0, enc1;
    REQUIRE(lv::encode_frame(src0, p, enc0));
    REQUIRE(lv::encode_frame(src1, p, enc1));
    REQUIRE(lv::decode_frame(enc0, ref0).slices_failed == 0);
    REQUIRE(lv::decode_frame(enc1, ref1).slices_failed == 0);

    // Use small MTU (400 B) so every slice splits into multiple fragments!
    lv::PacketizerConfig pcfg;
    pcfg.mtu = 400;
    pcfg.enable_crc = true;
    std::atomic<uint16_t> seq{100};

    auto make_packets = [&](const lv::EncodedFrame& enc, uint32_t fid, uint32_t ts) {
        std::vector<std::vector<uint8_t>> pkts;
        for (int sid = 0; sid < enc.geom.total_slices; ++sid) {
            lv::packetize_slice(fid, ts, uint16_t(sid), uint16_t(enc.geom.total_slices),
                                enc.slices[size_t(sid)].data(), enc.slices[size_t(sid)].size(),
                                pcfg, seq, [&](const uint8_t* d, size_t n) {
                                    pkts.emplace_back(d, d + n);
                                });
        }
        return pkts;
    };

    auto pkts0 = make_packets(enc0, 0, 1000);
    auto pkts1 = make_packets(enc1, 1, 17667);

    // On frame 1: drop ~5% of packets (ensuring at least 1 drop), reverse fragments inside slices
    // (forces last-fragment-first arrival!), reorder across slices, and inject duplicates.
    std::set<uint16_t> damaged_slices;
    std::vector<std::vector<uint8_t>> noisy1;
    uint32_t rng = 0x55AA1234u;
    for (size_t i = 0; i < pkts1.size(); ++i) {
        lv::PacketHeader h;
        REQUIRE(lv::parse_packet(pkts1[i].data(), pkts1[i].size(), h) == lv::PacketParseStatus::kOk);
        if ((xorshift32(rng) % 20u) == 0 || i == pkts1.size() / 2) {
            damaged_slices.insert(h.slice_id);
            continue;  // dropped!
        }
        noisy1.push_back(pkts1[i]);
        if ((i % 13) == 0) noisy1.push_back(pkts1[i]);  // duplicate!
    }
    REQUIRE(!damaged_slices.empty());
    // Reverse entire packet order for frame 1 -> guarantees last fragments arrive BEFORE first fragments!
    std::reverse(noisy1.begin(), noisy1.end());

    // Also inject one CRC-corrupted packet.
    auto bad_crc = pkts1[0];
    bad_crc.back() ^= 0xFF;
    noisy1.push_back(bad_crc);

    // Run both ConcealMode::kPrev and ConcealMode::kGray.
    for (auto mode : {lv::ConcealMode::kPrev, lv::ConcealMode::kGray}) {
        std::vector<lv::Image> outputs;
        std::vector<uint16_t> lost_counts;

        lv::AssemblerConfig acfg;
        acfg.frame_timeout_us = 25000;
        acfg.conceal = mode;

        lv::FrameAssembler asm_buf(acfg, [&](const lv::CompletedFrame& cf) {
            outputs.push_back(*cf.image);
            lost_counts.push_back(cf.slices_lost);
        });

        uint64_t t = 1000;
        for (const auto& d : pkts0) {
            CHECK(asm_buf.ingest_datagram(d.data(), d.size(), t) == lv::PacketParseStatus::kOk);
            t += 10;
        }
        REQUIRE(outputs.size() == 1);
        CHECK(outputs[0].data == ref0.data);  // Frame 0 intact -> bit-exact!

        t = 18000;
        for (const auto& d : noisy1) {
            asm_buf.ingest_datagram(d.data(), d.size(), t);
            t += 10;
        }
        // Frame 1 has missing slices -> must NOT finalize before timeout, MUST finalize on timeout.
        CHECK(outputs.size() == 1);
        asm_buf.poll(18000 + acfg.frame_timeout_us + 100);
        REQUIRE(outputs.size() == 2);
        CHECK(lost_counts[1] == damaged_slices.size());
        CHECK(asm_buf.stats().crc_errors == 1);

        // Verify EVERY unaffected slice matches ref1 bit-for-bit, and EVERY damaged slice was concealed!
        for (int sid = 0; sid < enc1.geom.total_slices; ++sid) {
            const bool is_damaged = damaged_slices.count(uint16_t(sid)) > 0;
            for (int y = sid * p.slice_height; y < (sid + 1) * p.slice_height; ++y) {
                if (!is_damaged) {
                    CHECK(std::memcmp(outputs[1].row(y), ref1.row(y), ref1.stride) == 0);
                } else if (mode == lv::ConcealMode::kPrev) {
                    CHECK(std::memcmp(outputs[1].row(y), ref0.row(y), ref0.stride) == 0);
                } else {
                    const uint8_t* r = outputs[1].row(y);
                    bool all_gray = true;
                    for (int x = 0; x < W; ++x) {
                        if (r[x * 4 + 0] != 128 || r[x * 4 + 1] != 128 || r[x * 4 + 2] != 128)
                            all_gray = false;
                    }
                    CHECK(all_gray);
                }
            }
        }
    }
}

TEST(udp_loopback_sender_receiver_end_to_end) {
    // Real UDP sockets on 127.0.0.1 with ephemeral port, ThreadPool on both ends.
    lv::ReceiverConfig rcfg;
    rcfg.bind_ip = "127.0.0.1";
    rcfg.bind_port = 0;  // ephemeral
    rcfg.recv_timeout_ms = 5;
    rcfg.decode_threads = 4;

    std::atomic<int> got_frames{0};
    lv::Image last_img;
    lv::UdpReceiver rx(rcfg, [&](const lv::CompletedFrame& cf) {
        last_img = *cf.image;
        got_frames.fetch_add(1, std::memory_order_release);
    });
    REQUIRE(rx.init());
    const uint16_t port = rx.local_port();
    REQUIRE(port != 0);

    std::atomic<bool> stop{false};
    std::thread rx_thread([&] { rx.run(stop); });

    lv::SenderConfig scfg;
    scfg.dest_ip = "127.0.0.1";
    scfg.dest_port = port;
    scfg.threads = 4;
    lv::UdpSender tx(scfg);
    REQUIRE(tx.init());

    lv::CodecParams cp;
    cp.width = 640;
    cp.height = 360;
    cp.quality = 80;
    lv::Image src;
    src.allocate(cp.width, cp.height);

    constexpr int kFrames = 10;
    for (int f = 0; f < kFrames; ++f) {
        lv::generate_test_frame(src, uint32_t(f));
        REQUIRE(tx.send_frame(src, cp, uint32_t(f), lv::now_us32()));
        tx.service_control();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    // Wait up to 500 ms for all frames to arrive & decode.
    for (int i = 0; i < 100 && got_frames.load(std::memory_order_acquire) < kFrames; ++i) {
        tx.service_control();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    stop.store(true, std::memory_order_release);
    rx_thread.join();

    const auto ts = tx.stats();
    const auto& rs = rx.assembler_stats();
    std::printf("  loopback: tx_pkts=%llu tx_err=%llu rx_dgrams=%llu rx_bad=%llu rx_frames=%d\n",
                (unsigned long long)ts.packets_sent, (unsigned long long)ts.send_errors,
                (unsigned long long)rs.datagrams_total, (unsigned long long)rs.bad_packets,
                got_frames.load());

    CHECK(got_frames.load() == kFrames);
    CHECK(rx.assembler_stats().frames_complete == kFrames);
    CHECK(rx.assembler_stats().crc_errors == 0);
    CHECK(lv::psnr_luma(src, last_img) >= 38.0);
}
