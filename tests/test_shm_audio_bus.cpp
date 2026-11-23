// LanVidAudioBus_v1 shared-memory bus: multithreaded 64-channel lossless roundtrip + DAW detection.
#include "mini_test.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

#include "audio/shm_audio_bridge.hpp"
#include "audio/shm_audio_host.hpp"
#include "lanvid/lanvid_sdk.h"
#if !defined(_WIN32)
#include <sys/mman.h>
#endif

TEST(shm_audio_bus_64ch_lockfree_roundtrip) {
    char name[64];
    std::snprintf(name, sizeof(name), "LanVidAudioBus_test_%u", unsigned(lv::shm_bus_now_ms() & 0xFFFFFF));
    lv::ShmAudioBus driver, host;  // two independent mappings of the same segment
    REQUIRE(driver.open(name));
    REQUIRE(host.open(name));
    REQUIRE(driver.header()->magic.load() == lv::kShmBusMagic);

    // DAW detection: not connected until IO starts, connected with heartbeat, gone after stop.
    CHECK(!host.driver_connected());
    driver.driver_set_running(true, 48000, 128, "test");
    CHECK(host.driver_connected());
    CHECK(host.header()->buffer_size.load() == 128);

    constexpr uint32_t kCh = 64, kBlk = 128, kBlocks = 2000;  // 256k frames >> 4096 ring
    // value encodes (frame, channel) so any loss, duplication or channel swap is detected
    auto val = [](uint64_t f, uint32_t c) { return float((f * 64 + c) % 8000000); };

    std::atomic<bool> fail{false};
    std::atomic<uint64_t> tx_frames{0}, rx_frames{0};
    std::atomic<bool> daw_done{false};  // the DAW stops reading RX when it is done: don't wait on it
    std::thread daw([&] {  // driver RT thread: writes TX, reads RX
        std::vector<float> in(size_t(kCh) * kBlk), out(size_t(kCh) * kBlk);
        uint64_t f = 0, rf = 0;
        while (f < uint64_t(kBlocks) * kBlk) {
            for (uint32_t i = 0; i < kBlk; ++i)
                for (uint32_t c = 0; c < kCh; ++c) in[size_t(i) * kCh + c] = val(f + i, c);
            uint32_t done = 0;
            while (done < kBlk) {  // retry while host drains (test only; RT code would drop)
                done += driver.driver_write_tx_interleaved(in.data() + size_t(done) * kCh, kCh, kBlk - done);
                if (done < kBlk) std::this_thread::yield();
            }
            f += kBlk;
            if (driver.host_rx_fill() >= kBlk) {  // consume RX only when a full block is ready
                driver.driver_read_rx_interleaved(out.data(), kCh, kBlk);
                for (uint32_t i = 0; i < kBlk; ++i)
                    for (uint32_t c = 0; c < kCh; ++c)
                        if (out[size_t(i) * kCh + c] != val(rf + i, c) + 1.0f) fail = true;
                rf += kBlk;
            }
            driver.driver_heartbeat();
        }
        tx_frames = f;
        daw_done = true;
        rx_frames = rf;
    });
    std::thread net([&] {  // host: TX -> "network" -> RX loopback (+1.0 marks the RX direction)
        std::vector<std::vector<float>> planes(kCh, std::vector<float>(kBlk));
        std::vector<float*> pp(kCh);
        for (uint32_t c = 0; c < kCh; ++c) pp[c] = planes[c].data();
        uint64_t got = 0;
        while (got < uint64_t(kBlocks) * kBlk) {
            const uint32_t n = host.host_read_tx(pp.data(), kCh, kBlk);
            if (!n) { std::this_thread::yield(); continue; }
            for (uint32_t c = 0; c < kCh; ++c)
                for (uint32_t i = 0; i < n; ++i) {
                    if (planes[c][i] != val(got + i, c)) fail = true;
                    planes[c][i] += 1.0f;
                }
            uint32_t w = 0;
            while (w < n && !daw_done.load()) {
                std::vector<const float*> cp(kCh);
                for (uint32_t c = 0; c < kCh; ++c) cp[c] = pp[c] + w;
                w += host.host_write_rx(cp.data(), kCh, n - w);
                if (w < n) std::this_thread::yield();
            }
            got += n;
            host.host_heartbeat();
        }
    });
    daw.join();
    net.join();
    std::printf("    [shm bus] tx=%llu rx_checked=%llu frames x 64ch, bit-exact\n",
                (unsigned long long)tx_frames.load(), (unsigned long long)rx_frames.load());
    CHECK(!fail.load());
    CHECK(tx_frames == uint64_t(kBlocks) * kBlk);
    CHECK(rx_frames > uint64_t(kBlocks) * kBlk / 2);
    CHECK(driver.host_connected());

    driver.driver_set_running(false, 0, 0, nullptr);
    CHECK(!host.driver_connected());
    driver.close();
    host.close();
#if !defined(_WIN32)
    char full[80];
    std::snprintf(full, sizeof(full), "/%s", name);
    shm_unlink(full);
#endif
    // ---- End-to-end: DAW -> SHM TX -> lv_sender -> UDP -> lv_receiver -> SHM RX -> DAW ----
    {
        char ename[64];
        std::snprintf(ename, sizeof(ename), "LanVidAudioBus_e2e_%u", unsigned(lv::shm_bus_now_ms() & 0xFFFFFF));
        LvReceiverHandle* rx = lv_receiver_create();
        LvSenderHandle* tx = lv_sender_create();
        REQUIRE(rx && tx);
        LV_AudioBusConfig bus;
        lv_audio_bus_config_default(&bus);
        bus.latency_ms = 50;  // headroom for shared / virtualised CI runners
        REQUIRE(lv_receiver_configure_audio_bus(rx, &bus) == LV_OK);
        REQUIRE(lv_sender_configure_audio_bus(tx, &bus) == LV_OK);
        LV_ReceiverConfig rc;
        lv_sdk_receiver_config_default(&rc);
        std::snprintf(rc.bind_ip, sizeof(rc.bind_ip), "127.0.0.1");
        rc.bind_port = 0;
        rc.enable_discovery = 0;
        REQUIRE(lv_sdk_receiver_start(rx, &rc) == LV_OK);
        LV_SenderConfig sc;
        lv_sdk_sender_config_default(&sc);
        std::snprintf(sc.dest_ip, sizeof(sc.dest_ip), "127.0.0.1");
        sc.dest_port = lv_receiver_get_local_port(rx);
        REQUIRE(lv_sdk_sender_start(tx, &sc) == LV_OK);

        lv::ShmAudioHost::Config hc;
        hc.channels = 64;
        hc.bus_name = ename;
        hc.rx_target_frames = 1024;  // ~21 ms of RX headroom against host-thread scheduling stalls
        lv::ShmAudioHost tx_host, rx_host;
        REQUIRE(tx_host.start(hc, [tx](const float* const* p, uint32_t ch, uint32_t n, bool disc) {
            const uint64_t ts = disc ? lv_sender_get_master_clock_samples(tx) - n : LV_MEDIA_TS_AUTO;
            lv_sender_push_audio_multichannel(tx, p, uint16_t(ch), n, LV_AUDIO_FORMAT_F32,
                                              LV_AUDIO_LAYOUT_PLANAR, ts);
        }, nullptr));
        REQUIRE(rx_host.start(hc, nullptr, [rx](float* const* p, uint32_t ch, uint32_t n) {
            // planar pop into a contiguous scratch, then scatter (host-side, non-RT)
            static thread_local std::vector<float> tmp;
            tmp.resize(size_t(ch) * n);
            lv_receiver_pop_audio_multichannel(rx, tmp.data(), uint16_t(ch), n, LV_AUDIO_LAYOUT_PLANAR);
            for (uint32_t c = 0; c < ch; ++c) std::copy(tmp.begin() + c * n, tmp.begin() + (c + 1) * n, p[c]);
        }));

        lv::ShmAudioBus daw;  // plays the role of the ASIO/HAL/JACK driver inside a DAW
        REQUIRE(daw.open(ename));
        daw.driver_set_running(true, 48000, 128, "test-daw");
        constexpr uint32_t kB = 128;
        std::vector<std::vector<float>> o(64, std::vector<float>(kB)), in(64, std::vector<float>(kB));
        std::vector<const float*> op(64);
        std::vector<float*> ip(64);
        for (int c = 0; c < 64; ++c) { op[c] = o[c].data(); ip[c] = in[c].data(); }
        uint64_t pos = 0;
        int blocks = 0, live = 0;
        double worst = 0.0;
        int worst_block = -1;
        int gap_blocks = 0;
        auto next = std::chrono::steady_clock::now();
        const int total = int(3.0 * 48000 / kB);  // 3 s of real time
        for (int b = 0; b < total; ++b) {
            next += std::chrono::microseconds(1000000ull * kB / 48000);
            std::this_thread::sleep_until(next);
            // channel c = same sine scaled by (c+1)/64: detects channel swaps AND phase skew
            for (uint32_t i = 0; i < kB; ++i) {
                const float v = 0.5f * std::sin(0.031f * float(pos + i));
                for (int c = 0; c < 64; ++c) o[c][i] = v * float(c + 1) / 64.0f;
            }
            pos += kB;
            daw.driver_write_tx(op.data(), 64, kB);
            daw.driver_read_rx(ip.data(), 64, kB);
            daw.driver_heartbeat();
            if (b > total / 2) {
                ++blocks;
                // A sample where one channel is silent while the reference is not (or vice versa) is a
                // per-channel gap (a packet that missed its playout deadline on a loaded host), not a
                // phase error. Phase alignment is checked strictly on every gap-free block.
                bool any = false, gap = false;
                double blk_worst = 0.0;
                for (uint32_t i = 0; i < kB; ++i) {
                    const float ref = in[63][i];  // full-scale channel
                    if (std::fabs(ref) > 0.05f) any = true;
                    for (int c = 0; c < 63; ++c) {
                        const float scaled = in[c][i] * 64.0f / float(c + 1);
                        if ((std::fabs(ref) > 0.05f && in[c][i] == 0.0f) ||
                            (ref == 0.0f && std::fabs(scaled) > 0.05f))
                            gap = true;
                        blk_worst = std::max(blk_worst, double(std::fabs(scaled - ref)));
                    }
                }
                if (gap) ++gap_blocks;
                else if (blk_worst > worst) { worst = blk_worst; worst_block = b; }
                if (any) ++live;
            }
        }
        const auto ts = tx_host.status(), rs = rx_host.status();
        uint64_t late = 0, lost = 0, under = 0, fec = 0, nack = 0;
        for (uint16_t c = 0; c < 64; ++c) {
            LV_AudioChannelStats st{};
            lv_receiver_get_channel_stats(rx, c, &st);
            late += st.late_packets; lost += st.packets_lost; under += st.underruns;
            fec += st.fec_recovered; nack += st.nack_recovered;
        }
        std::printf("    [shm e2e] per-channel totals: late=%llu lost=%llu underruns=%llu fec=%llu nack=%llu | worst at block %d\n",
                    (unsigned long long)late, (unsigned long long)lost, (unsigned long long)under,
                    (unsigned long long)fec, (unsigned long long)nack, worst_block);
        std::printf("    [shm e2e] blocks=%d live=%d worst_ch_mismatch=%.2e | tx_sent=%llu rx_delivered=%llu "
                    "reanchors=%llu | DAW seen by tx=%d rx=%d\n",
                    blocks, live, worst, (unsigned long long)ts.tx_frames, (unsigned long long)rs.rx_frames,
                    (unsigned long long)ts.reanchors, int(ts.driver_connected), int(rs.driver_connected));
        CHECK(ts.driver_connected && rs.driver_connected);
        CHECK(ts.driver_name == "test-daw");
        CHECK(ts.tx_frames > 48000);
        CHECK(rs.rx_frames > 48000);
        std::printf("    [shm e2e] gap blocks (late single-channel packets) = %d / %d\n", gap_blocks, blocks);
        CHECK(live > blocks * 8 / 10);        // audio made the full round trip
        CHECK(gap_blocks <= blocks / 20);     // late per-channel packets stay rare (<= 5 %)
        // A one-sample skew between channels at this tone would show as ~1.5e-2; float rounding plus
        // resampler interpolation stay below ~1e-3 (seen on Windows runners), so 2e-3 still proves
        // sample-accurate alignment with a ~7x margin.
        CHECK(worst < 2e-3);                  // 64 channels phase-aligned, no swaps, on every gap-free block
        daw.driver_set_running(false, 0, 0, nullptr);
        tx_host.stop();
        rx_host.stop();
        daw.close();
        lv_sender_destroy(tx);
        lv_receiver_destroy(rx);
#if !defined(_WIN32)
        char full[80];
        std::snprintf(full, sizeof(full), "/%s", ename);
        shm_unlink(full);
#endif
    }
}
