// Regression: live restarting CrossLink sender/receiver handles under heavy concurrent AoIP + MIDI +
// ControlSync load must never crash or deadlock.
#include "mini_test.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

#include "crosslink/crosslink_sdk.h"
#if defined(CL_HAVE_SHM_BUS)
#include "audio/shm_audio_bridge.hpp"
#endif

namespace {

struct SyncSeen {
    std::mutex mu;
    uint64_t updates = 0;
    uint64_t last_rev = 0;
    uint8_t last_midi_only = 0xFF;
};

void on_sync(const CL_ControlSyncState* cs, void* ud) {
    auto* s = static_cast<SyncSeen*>(ud);
    if (!s || !cs) return;
    std::lock_guard<std::mutex> lk(s->mu);
    if (cs->revision <= s->last_rev) return;
    s->last_rev = cs->revision;
    s->last_midi_only = cs->midi_only;
    ++s->updates;
}

bool start_tx(ClSenderHandle* tx, uint16_t port, uint32_t latency_ms, int codec) {
    CL_SenderConfig sc{};
    cl_sender_config_default(&sc);
    std::snprintf(sc.dest_ip, sizeof(sc.dest_ip), "127.0.0.1");
    sc.dest_port = port;
    sc.latency_ms = latency_ms;
    sc.audio_codec = codec;
    return cl_sender_start(tx, &sc) == CL_OK;
}

bool start_rx(ClReceiverHandle* rx, uint16_t port, uint32_t latency_ms, int adaptive) {
    CL_ReceiverConfig rc{};
    cl_receiver_config_default(&rc);
    std::snprintf(rc.bind_ip, sizeof(rc.bind_ip), "127.0.0.1");
    rc.bind_port = port;
    rc.latency_ms = latency_ms;
    rc.num_channels = 8;
    rc.adaptive_jitter = adaptive;
    rc.enable_discovery = 0;
    return cl_receiver_start(rx, &rc) == CL_OK;
}

}  // namespace

TEST(crosslink_runtime_restart_under_load_no_crash) {
    const uint16_t port = uint16_t(52100 + (std::chrono::steady_clock::now().time_since_epoch().count() / 1000) % 3000);
    constexpr uint16_t kCh = 8;
    constexpr uint32_t kBlk = 128;

    ClSenderHandle* tx = cl_sender_create();
    ClReceiverHandle* rx = cl_receiver_create();
    REQUIRE(tx && rx);
    CL_AudioBusConfig bus{};
    cl_audio_bus_config_default(&bus);
    bus.num_channels = kCh;
    bus.latency_ms = 30;
    cl_sender_configure_audio_bus(tx, &bus);
    cl_receiver_configure_audio_bus(rx, &bus);
    SyncSeen rx_seen;
    cl_receiver_set_control_sync_callback(rx, &on_sync, &rx_seen);

    REQUIRE(start_rx(rx, port, 30, 1));
    REQUIRE(start_tx(tx, port, 30, 0));

#if defined(CL_HAVE_SHM_BUS)
    const unsigned tag = unsigned(cl::shm_bus_now_ms() & 0xFFFF);
    CL_DuplexConfig ca{}, cb{};
    cl_duplex_config_default(&ca);
    cl_duplex_config_default(&cb);
    for (CL_DuplexConfig* c : {&ca, &cb}) {
        std::snprintf(c->peer_host, sizeof(c->peer_host), "127.0.0.1");
        std::snprintf(c->bind_ip, sizeof(c->bind_ip), "127.0.0.1");
        c->num_channels = kCh;
        c->latency_ms = 30;
        c->enable_discovery = 0;
    }
    ca.listen_port = uint16_t(port + 10);
    ca.peer_port = uint16_t(port + 11);
    cb.listen_port = uint16_t(port + 11);
    cb.peer_port = uint16_t(port + 10);
    std::snprintf(ca.bus_name, sizeof(ca.bus_name), "CrossLinkBus_rstA_%u", tag);
    std::snprintf(cb.bus_name, sizeof(cb.bus_name), "CrossLinkBus_rstB_%u", tag);
    ClDuplexHandle* A = cl_duplex_create();
    ClDuplexHandle* B = cl_duplex_create();
    REQUIRE(A && B);
    SyncSeen b_seen;
    cl_duplex_set_control_sync_callback(B, &on_sync, &b_seen);
    REQUIRE(cl_duplex_start(A, &ca) == CL_OK);
    REQUIRE(cl_duplex_start(B, &cb) == CL_OK);
#endif

    std::atomic<bool> stop{false};
    std::atomic<uint64_t> pushes_ok{0}, pops_ok{0}, midi_ok{0};
    std::thread pusher([&] {
        std::vector<std::vector<float>> pl(kCh, std::vector<float>(kBlk, 0.1f));
        std::vector<const float*> pp(kCh);
        for (int c = 0; c < kCh; ++c) pp[c] = pl[c].data();
        while (!stop.load()) {
            if (cl_sender_push_audio_multichannel(tx, pp.data(), kCh, kBlk, CL_AUDIO_FORMAT_F32, CL_AUDIO_LAYOUT_PLANAR,
                                                  CL_MEDIA_TS_AUTO) == CL_OK)
                pushes_ok.fetch_add(1);
            (void)cl_sender_get_master_clock_samples(tx);
            std::this_thread::sleep_for(std::chrono::microseconds(2667));
        }
    });
    std::thread popper([&] {
        std::vector<float> buf(size_t(kCh) * kBlk);
        while (!stop.load()) {
            if (cl_receiver_pop_audio_multichannel(rx, buf.data(), kCh, kBlk, CL_AUDIO_LAYOUT_PLANAR) > 0)
                pops_ok.fetch_add(1);
            CL_AudioChannelStats cs{};
            (void)cl_receiver_get_channel_stats(rx, 0, &cs);
            std::this_thread::sleep_for(std::chrono::microseconds(2667));
        }
    });
    std::thread midi([&] {
        uint8_t n = 60;
        while (!stop.load()) {
            if (cl_sender_push_midi(tx, 0x90, n, 100, 0, 0) == CL_OK) midi_ok.fetch_add(1);
            cl_sender_push_midi(tx, 0x80, n, 0, 0, 0);
            n = uint8_t(60 + (n - 59) % 12);
            std::this_thread::sleep_for(std::chrono::milliseconds(7));
        }
    });

    uint64_t rev = 1;
    int restarts = 0;
    for (int i = 0; i < 16; ++i) {
        const uint32_t lat = (i % 2 == 0) ? 20 : 35;
        const int adaptive = (i % 2 == 0) ? 1 : 0;
        REQUIRE(start_rx(rx, port, lat, adaptive));
        REQUIRE(start_tx(tx, port, lat, i % 3));
        ++restarts;
        CL_ControlSyncState cs{};
        cs.revision = ++rev;
        cs.origin_id = 0xC1000001u;
        cs.audio_channels = kCh;
        cs.target_buffer_ms = uint16_t(lat);
        cs.latency_mode = uint8_t(adaptive);
        cs.midi_only = uint8_t(i % 2);
        cs.active_preset_id = -1;
        cl_sender_send_control_sync(tx, &cs);
#if defined(CL_HAVE_SHM_BUS)
        CHECK(cl_duplex_send_control_sync(A, &cs) == CL_OK);
        cl_duplex_set_latency_ms(A, lat);
#endif
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
    }

    const uint64_t pushes_before = pushes_ok.load();
    CL_AudioChannelStats r0{}, r1{};
    cl_receiver_get_channel_stats(rx, 0, &r0);
    const auto settle_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (pushes_ok.load() - pushes_before <= 50 && std::chrono::steady_clock::now() < settle_deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    cl_receiver_get_channel_stats(rx, 0, &r1);
    stop.store(true);
    pusher.join();
    popper.join();
    midi.join();

    CL_StreamStats st{};
    CHECK(cl_sender_get_stream_stats(tx, &st) == CL_OK);
    uint64_t b_updates = 0;
    uint8_t b_midi_only = 0xFF;
#if defined(CL_HAVE_SHM_BUS)
    {
        std::lock_guard<std::mutex> lk(b_seen.mu);
        b_updates = b_seen.updates;
        b_midi_only = b_seen.last_midi_only;
    }
    CL_DuplexStats ds{};
    CHECK(cl_duplex_get_stats(B, &ds) == CL_OK);
    cl_duplex_destroy(A);
    cl_duplex_destroy(B);
#endif
    std::printf("    [crosslink restart] %d restarts under load | pushes +%llu pops %llu midi %llu | rx pkts %llu -> %llu | sync=%llu\n",
                restarts, (unsigned long long)(pushes_ok.load() - pushes_before),
                (unsigned long long)pops_ok.load(), (unsigned long long)midi_ok.load(),
                (unsigned long long)r0.packets_rx, (unsigned long long)r1.packets_rx,
                (unsigned long long)b_updates);

    CHECK(st.state >= CL_STATE_RUNNING);
    CHECK(pushes_ok.load() - pushes_before > 50);
    CHECK(r1.packets_rx > r0.packets_rx);
    CHECK(pops_ok.load() > 50);
    CHECK(midi_ok.load() > 10);
#if defined(CL_HAVE_SHM_BUS)
    CHECK(b_updates >= 1);
    CHECK(b_midi_only == 1);
#endif
    cl_sender_destroy(tx);
    cl_receiver_destroy(rx);
}
