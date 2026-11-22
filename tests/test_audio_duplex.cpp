// Full-duplex 64x64 AoIP + patchbay: two duplex nodes A <-> B on loopback, each with its own
// shared-memory bus and simulated DAW, exchanging 64 channels both ways simultaneously with channel
// re-routing, Mute and live Solo; plus deterministic checks of the 5 ms ramps / route crossfade and
// AoIP node discovery.
#include "mini_test.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

#include "audio/channel_patchbay.hpp"
#if defined(LV_HAVE_SHM_BUS)
#include "audio/shm_audio_bridge.hpp"
#include "lanvid/lanvid_sdk.h"
#if !defined(_WIN32)
#include <sys/mman.h>
#endif
#endif

namespace {

void patchbay_ramp_and_crossfade_checks() {
    lv::ChannelPatchbay pb(48000);
    constexpr uint32_t kN = 512;
    std::vector<std::vector<float>> in(64, std::vector<float>(kN, 0.0f)), out(64, std::vector<float>(kN));
    std::vector<const float*> ip(64);
    std::vector<float*> op(64);
    for (int c = 0; c < 64; ++c) {
        for (uint32_t i = 0; i < kN; ++i) in[c][i] = 0.01f * float(c + 1);
        ip[c] = in[c].data();
        op[c] = out[c].data();
    }
    // identity, unity: bit-exact pass-through
    pb.process(ip.data(), op.data(), 64, kN);
    int exact = 0;
    for (int c = 0; c < 64; ++c) exact += (out[c] == in[c]);
    CHECK(exact == 64);

    // Mute ch 3: 5 ms (240 samples) linear ramp, no step larger than one ramp increment.
    pb.set_mute(3, true);
    pb.process(ip.data(), op.data(), 64, kN);
    const float v = in[3][0];
    float max_step = 0.0f;
    for (uint32_t i = 1; i < kN; ++i) max_step = std::max(max_step, std::fabs(out[3][i] - out[3][i - 1]));
    CHECK(max_step <= v / 240.0f + 1e-6f);   // click-free
    CHECK(out[3][kN - 1] == 0.0f);           // fully muted after the ramp
    CHECK(std::fabs(out[3][120] - v * 0.5f) < v * 0.02f);  // half-way at 2.5 ms
    CHECK(out[4] == in[4]);                  // neighbours untouched

    // Solo ch 9: everything else ramps to 0, ch 9 stays at unity.
    pb.set_mute(3, false);
    pb.set_solo(9, true);
    pb.process(ip.data(), op.data(), 64, kN);
    pb.process(ip.data(), op.data(), 64, kN);
    int silent = 0;
    for (int c = 0; c < 64; ++c) silent += (c != 9 && out[c][kN - 1] == 0.0f);
    CHECK(silent == 63);
    CHECK(out[9] == in[9]);
    pb.clear_mute_solo();
    pb.process(ip.data(), op.data(), 64, kN);
    pb.process(ip.data(), op.data(), 64, kN);

    // Live re-route (swap 0 <-> 1): 5 ms crossfade, total energy on {0,1} preserved, no overshoot.
    int8_t map[64];
    for (int c = 0; c < 64; ++c) map[c] = int8_t(c);
    map[0] = 1;
    map[1] = 0;
    pb.set_routing(map);
    pb.process(ip.data(), op.data(), 64, kN);
    const float a = in[0][0], b = in[1][0];
    float worst_sum = 0.0f;
    bool monotonic = true;
    for (uint32_t i = 0; i < kN; ++i) {
        worst_sum = std::max(worst_sum, std::fabs(out[0][i] + out[1][i] - (a + b)));
        if (i && out[0][i] < out[0][i - 1] - 1e-7f) monotonic = false;  // a -> b rises (b > a)
    }
    CHECK(worst_sum < 1e-6f);
    CHECK(monotonic);
    CHECK(std::fabs(out[0][kN - 1] - b) < 1e-7f && std::fabs(out[1][kN - 1] - a) < 1e-7f);
    int8_t back[64];
    pb.get_routing(back);
    CHECK(back[0] == 1 && back[1] == 0 && back[2] == 2);
    std::printf("    [patchbay] unity exact, mute ramp max_step=%.2e (limit %.2e), solo isolates, re-route xfade ok\n",
                double(max_step), double(v / 240.0f));
}

}  // namespace

TEST(audio_64ch_fullduplex_and_patchbay_routing) {
    patchbay_ramp_and_crossfade_checks();
#if defined(LV_HAVE_SHM_BUS)
    const unsigned tag = unsigned(lv::shm_bus_now_ms() & 0xFFFF);
    char bus_a[64], bus_b[64];
    std::snprintf(bus_a, sizeof(bus_a), "LanVidAudioBus_dpxA_%u", tag);
    std::snprintf(bus_b, sizeof(bus_b), "LanVidAudioBus_dpxB_%u", tag);
    const uint16_t port_a = uint16_t(41000 + (tag % 4000) * 2), port_b = uint16_t(port_a + 1);
    const uint16_t disc_port = uint16_t(port_a + 9000);

    LV_DuplexConfig ca, cb;
    lv_duplex_config_default(&ca);
    lv_duplex_config_default(&cb);
    std::snprintf(ca.peer_host, sizeof(ca.peer_host), "127.0.0.1");
    std::snprintf(cb.peer_host, sizeof(cb.peer_host), "127.0.0.1");
    std::snprintf(ca.bind_ip, sizeof(ca.bind_ip), "127.0.0.1");
    std::snprintf(cb.bind_ip, sizeof(cb.bind_ip), "127.0.0.1");
    ca.listen_port = port_a; ca.peer_port = port_b;
    cb.listen_port = port_b; cb.peer_port = port_a;
    ca.latency_ms = cb.latency_ms = 50;
    std::snprintf(ca.bus_name, sizeof(ca.bus_name), "%s", bus_a);
    std::snprintf(cb.bus_name, sizeof(cb.bus_name), "%s", bus_b);
    std::snprintf(ca.device_name, sizeof(ca.device_name), "Duplex-Node-A");
    ca.discovery_port = disc_port;
    cb.enable_discovery = 0;

    LvDuplexHandle* A = lv_duplex_create();
    LvDuplexHandle* B = lv_duplex_create();
    REQUIRE(A && B);
    REQUIRE(lv_duplex_start(A, &ca) == LV_OK);
    REQUIRE(lv_duplex_start(B, &cb) == LV_OK);

    // A -> B: reverse the 64 channels (src s -> dst 63-s) and mute A's source 10 (arrives at B 53).
    int8_t rev[64];
    for (int c = 0; c < 64; ++c) rev[c] = int8_t(63 - c);
    REQUIRE(lv_audio_set_tx_routing(A, rev) == LV_OK);
    REQUIRE(lv_audio_set_channel_mute_solo(A, LV_AUDIO_DIR_TX, 10, 1, -1) == LV_OK);
    CHECK(lv_audio_set_channel_mute_solo(A, LV_AUDIO_DIR_TX, 64, 1, -1) == LV_ERR_INVALID_ARG);

    // Simulated DAWs (driver side of each node's bus).
    lv::ShmAudioBus daw_a, daw_b;
    REQUIRE(daw_a.open(bus_a));
    REQUIRE(daw_b.open(bus_b));
    daw_a.driver_set_running(true, 48000, 128, "test-daw-A");
    daw_b.driver_set_running(true, 48000, 128, "test-daw-B");

    constexpr uint32_t kB = 128;
    std::vector<std::vector<float>> oa(64, std::vector<float>(kB)), ob(64, std::vector<float>(kB));
    std::vector<std::vector<float>> ia(64, std::vector<float>(kB)), ib(64, std::vector<float>(kB));
    std::vector<const float*> oap(64), obp(64);
    std::vector<float*> iap(64), ibp(64);
    for (int c = 0; c < 64; ++c) { oap[c] = oa[c].data(); obp[c] = ob[c].data(); iap[c] = ia[c].data(); ibp[c] = ib[c].data(); }

    const int total = int(4.0 * 48000 / kB);          // 4 s real time
    const int solo_at = int(2.5 * 48000 / kB);        // live Solo on A's RX channel 7 at 2.5 s
    const int check_from = int(1.2 * 48000 / kB);
    int blocks = 0, live_b = 0, live_a = 0, gaps = 0, solo_blocks = 0, solo_leak = 0, mute_leak = 0;
    double worst_b = 0.0, worst_a = 0.0;
    uint64_t pos = 0;
    auto next = std::chrono::steady_clock::now();
    for (int b = 0; b < total; ++b) {
        next += std::chrono::microseconds(1000000ull * kB / 48000);
        std::this_thread::sleep_until(next);
        for (uint32_t i = 0; i < kB; ++i) {
            const float va = 0.5f * std::sin(0.023f * float(pos + i));
            const float vb = 0.5f * std::sin(0.041f * float(pos + i));
            for (int c = 0; c < 64; ++c) {
                oa[c][i] = va * float(c + 1) / 64.0f;
                ob[c][i] = vb * float(c + 1) / 64.0f;
            }
        }
        pos += kB;
        daw_a.driver_write_tx(oap.data(), 64, kB);
        daw_b.driver_write_tx(obp.data(), 64, kB);
        daw_a.driver_read_rx(iap.data(), 64, kB);
        daw_b.driver_read_rx(ibp.data(), 64, kB);
        daw_a.driver_heartbeat();
        daw_b.driver_heartbeat();
        if (b == solo_at) lv_audio_set_channel_mute_solo(A, LV_AUDIO_DIR_RX, 7, -1, 1);
        if (b < check_from) continue;
        // Skip the Solo transition itself (5 ms ramp + RX ring/pump latency): channels legitimately fade.
        if (b >= solo_at && b <= solo_at + 30) continue;
        ++blocks;

        // ---- B's inputs: A's channels reversed; B[k] = A_out[63-k], amplitude (64-k)/64; B[53] muted.
        bool gap = false, any_b = false;
        double wb = 0.0;
        for (uint32_t i = 0; i < kB; ++i) {
            const float ref = ib[0][i];  // = A_out[63], full scale
            if (std::fabs(ref) > 0.05f) any_b = true;
            for (int k = 1; k < 64; ++k) {
                if (k == 53) { if (ib[53][i] != 0.0f && b > check_from + 4) ++mute_leak; continue; }
                const float scaled = ib[k][i] * 64.0f / float(64 - k);
                if ((std::fabs(ref) > 0.05f && ib[k][i] == 0.0f) || (ref == 0.0f && std::fabs(scaled) > 0.05f)) gap = true;
                wb = std::max(wb, double(std::fabs(scaled - ref)));
            }
        }
        if (any_b) ++live_b;

        // ---- A's inputs: B's channels 1:1 until the Solo, then only channel 7 (after the 5 ms ramp).
        bool any_a = false;
        double wa = 0.0;
        const bool soloed = b > solo_at + 30;  // > ramp + bus/pump latency
        for (uint32_t i = 0; i < kB; ++i) {
            const float ref = ia[63][i];
            if (!soloed) {
                if (std::fabs(ref) > 0.05f) any_a = true;
                for (int k = 0; k < 63; ++k) {
                    const float scaled = ia[k][i] * 64.0f / float(k + 1);
                    if ((std::fabs(ref) > 0.05f && ia[k][i] == 0.0f) || (ref == 0.0f && std::fabs(scaled) > 0.05f)) gap = true;
                    wa = std::max(wa, double(std::fabs(scaled - ref)));
                }
            } else {
                if (std::fabs(ia[7][i]) > 0.01f) any_a = true;
                for (int k = 0; k < 64; ++k) if (k != 7 && ia[k][i] != 0.0f) ++solo_leak;
            }
        }
        if (soloed) ++solo_blocks;
        if (any_a) ++live_a;
        if (gap) ++gaps;
        else { worst_a = std::max(worst_a, wa); worst_b = std::max(worst_b, wb); }
    }

    LV_DuplexStats sa{}, sb{};
    CHECK(lv_duplex_get_stats(A, &sa) == LV_OK);
    CHECK(lv_duplex_get_stats(B, &sb) == LV_OK);
    LV_AudioNode nodes[8];
    const int found = lv_discovery_scan_audio_nodes(400, disc_port, nodes, 8);
    bool found_a = false;
    for (int i = 0; i < found; ++i)
        if (nodes[i].rx_port == port_a && nodes[i].duplex && nodes[i].channels == 64) {
            found_a = true;
            CHECK(std::string(nodes[i].name) == "Duplex-Node-A");
            CHECK(std::string(nodes[i].audio_driver) == "test-daw-A");
            CHECK(nodes[i].driver_connected == 1);
            CHECK(nodes[i].tx_port == port_b);
        }

    std::printf("    [duplex] blocks=%d live A<-B=%d B<-A=%d gaps=%d | worst mismatch A=%.2e B=%.2e | mute leak=%d | "
                "solo blocks=%d leak=%d\n", blocks, live_a, live_b, gaps, worst_a, worst_b, mute_leak, solo_blocks, solo_leak);
    std::printf("    [duplex] A: rx_clock=%d rtt=%u us pkts rx=%llu tx=%llu lost=%llu pongs=%llu | "
                "B: rx_clock=%d rtt=%u us pkts rx=%llu tx=%llu lost=%llu | discovery found=%d nodeA=%d\n",
                sa.rx_clock_synced, sa.rx_rtt_us, (unsigned long long)sa.rx_packets, (unsigned long long)sa.tx_packets,
                (unsigned long long)sa.rx_lost, (unsigned long long)sa.tx_pongs_sent, sb.rx_clock_synced, sb.rx_rtt_us,
                (unsigned long long)sb.rx_packets, (unsigned long long)sb.tx_packets, (unsigned long long)sb.rx_lost,
                found, int(found_a));

    CHECK(sa.driver_connected && sb.driver_connected);
    CHECK(sa.rx_clock_synced && sb.rx_clock_synced);  // independent MasterClock per direction
    CHECK(sa.rx_packets > 1000 && sb.rx_packets > 1000 && sa.tx_packets > 1000 && sb.tx_packets > 1000);
    CHECK(sa.rx_lost == 0 && sb.rx_lost == 0);
    CHECK(live_b > blocks * 9 / 10);
    CHECK(live_a > blocks * 9 / 10);
    CHECK(gaps <= blocks / 20);
    CHECK(worst_a < 1e-4 && worst_b < 1e-4);          // routed channels arrive intact and phase-aligned
    CHECK(mute_leak == 0);
    CHECK(solo_blocks > 100 && solo_leak == 0);
    CHECK(found_a);

    daw_a.driver_set_running(false, 0, 0, nullptr);
    daw_b.driver_set_running(false, 0, 0, nullptr);
    lv_duplex_stop(A);
    lv_duplex_stop(B);
    lv_duplex_destroy(A);
    lv_duplex_destroy(B);
    daw_a.close();
    daw_b.close();
#if !defined(_WIN32)
    char full[96];
    std::snprintf(full, sizeof(full), "/%s", bus_a);
    shm_unlink(full);
    std::snprintf(full, sizeof(full), "/%s", bus_b);
    shm_unlink(full);
#endif
#endif
}
