// Regression: toggling "Audio Only" at runtime must never crash LanVid Studio.
//
// The crash (macOS reports 2026-11-23, EXC_BAD_ACCESS in UdpSender::push_audio_channel): Studio restarts
// the sender/receiver handles when Audio Only flips, while the Audio Matrix mixer thread is inside
// lv_sender_push_audio_multichannel / lv_receiver_pop_audio_multichannel on the same handles - the engine
// object was freed under it. Also covered: video sources that cannot open (no Screen Recording permission
// on macOS, no capture backend, missing camera) must not stop the audio stream.
#include "mini_test.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

#include "lanvid/lanvid_sdk.h"
#include "lanvid_c_api.h"
#if defined(LV_HAVE_SHM_BUS)
#include "audio/shm_audio_bridge.hpp"
#endif

namespace {

struct SyncSeen {
    std::mutex mu;
    uint64_t updates = 0;
    uint32_t last_rev = 0;
    uint8_t last_audio_only = 0xFF;
};

void on_sync(const LV_ControlSyncState* cs, void* ud) {
    auto* s = static_cast<SyncSeen*>(ud);
    if (!s || !cs) return;
    std::lock_guard<std::mutex> lk(s->mu);
    if (cs->revision <= s->last_rev) return;
    s->last_rev = uint32_t(cs->revision);
    s->last_audio_only = cs->audio_only;
    ++s->updates;
}

bool start_tx(LvSenderHandle* tx, uint16_t port, int audio_only, int source) {
    LvSenderConfig sc;
    lv_sender_config_default(&sc);
    std::snprintf(sc.dest_ip, sizeof(sc.dest_ip), "127.0.0.1");
    sc.dest_port = port;
    sc.audio_only = audio_only;
    sc.source_type = source;
    sc.width = 320;
    sc.height = 180;
    sc.fps = 30.0;
    return lv_sender_start(tx, &sc) == 1;
}

bool start_rx(LvReceiverHandle* rx, uint16_t port, int audio_only) {
    LvReceiverConfig rc;
    lv_receiver_config_default(&rc);
    std::snprintf(rc.bind_ip, sizeof(rc.bind_ip), "127.0.0.1");
    rc.bind_port = port;
    rc.enable_audio = 0;
    rc.enable_discovery = 0;
    rc.audio_only = audio_only;
    return lv_receiver_start(rx, &rc) == 1;
}

}  // namespace

TEST(toggle_audio_only_runtime_no_crash) {
    const uint16_t port = uint16_t(52000 + (std::chrono::steady_clock::now().time_since_epoch().count() / 1000) % 3000);
    constexpr uint16_t kCh = 8;
    constexpr uint32_t kBlk = 128;

    LvSenderHandle* tx = lv_sender_create();
    LvReceiverHandle* rx = lv_receiver_create();
    REQUIRE(tx && rx);
    LV_AudioBusConfig bus;
    lv_audio_bus_config_default(&bus);
    bus.num_channels = kCh;
    bus.latency_ms = 30;
    lv_sender_configure_audio_bus(tx, &bus);
    lv_receiver_configure_audio_bus(rx, &bus);
    SyncSeen rx_seen;
    lv_receiver_set_control_sync_callback(rx, &on_sync, &rx_seen);

    // ---- 1. Idle: true -> false -> true (screen source: unavailable on macOS without a backend / TCC
    //         permission - the sender must keep running audio-only and say why). ----
    for (int audio_only : {1, 0, 1}) {
        REQUIRE(start_rx(rx, port, audio_only));
        REQUIRE(start_tx(tx, port, audio_only, LV_SOURCE_SCREEN));
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        LvSenderStats st{};
        lv_sender_get_stats(tx, &st);
        CHECK(st.running == 1);  // a failed video source never takes the stream down
        if (audio_only) CHECK(st.video_source_error == LV_VIDEO_SOURCE_OK);
        else CHECK(st.video_source_error == LV_VIDEO_SOURCE_OK || st.video_source_error == LV_VIDEO_SOURCE_SCREEN_UNAVAILABLE);
    }

#if defined(LV_HAVE_SHM_BUS)
    // ---- 2. Active duplex stream A <-> B (control sync carries the audio_only flag) ----
    const unsigned tag = unsigned(lv::shm_bus_now_ms() & 0xFFFF);
    LV_DuplexConfig ca, cb;
    lv_duplex_config_default(&ca);
    lv_duplex_config_default(&cb);
    for (LV_DuplexConfig* c : {&ca, &cb}) {
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
    std::snprintf(ca.bus_name, sizeof(ca.bus_name), "LanVidAudioBus_tglA_%u", tag);
    std::snprintf(cb.bus_name, sizeof(cb.bus_name), "LanVidAudioBus_tglB_%u", tag);
    LvDuplexHandle* A = lv_duplex_create();
    LvDuplexHandle* B = lv_duplex_create();
    REQUIRE(A && B);
    SyncSeen b_seen;
    lv_duplex_set_control_sync_callback(B, &on_sync, &b_seen);
    REQUIRE(lv_duplex_start(A, &ca) == LV_OK);
    REQUIRE(lv_duplex_start(B, &cb) == LV_OK);
#endif

    // ---- 3. Under load: the Audio Matrix's threads hammer the handles while the GUI thread toggles ----
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> pushes_ok{0}, pops_ok{0}, midi_ok{0};
    std::thread pusher([&] {  // = MultiDeviceAudioMatrix network slot (npush), every 2.67 ms
        std::vector<std::vector<float>> pl(kCh, std::vector<float>(kBlk, 0.1f));
        std::vector<const float*> pp(kCh);
        for (int c = 0; c < kCh; ++c) pp[c] = pl[c].data();
        while (!stop.load()) {
            if (lv_sender_push_audio_multichannel(tx, pp.data(), kCh, kBlk, LV_AUDIO_FORMAT_F32, LV_AUDIO_LAYOUT_PLANAR,
                                                  LV_MEDIA_TS_AUTO) == LV_OK)
                pushes_ok.fetch_add(1);
            (void)lv_sender_get_master_clock_samples(tx);
            std::this_thread::sleep_for(std::chrono::microseconds(2667));
        }
    });
    std::thread popper([&] {  // = network slot npop
        std::vector<float> buf(size_t(kCh) * kBlk);
        while (!stop.load()) {
            if (lv_receiver_pop_audio_multichannel(rx, buf.data(), kCh, kBlk, LV_AUDIO_LAYOUT_PLANAR) > 0) pops_ok.fetch_add(1);
            LV_AudioChannelStats cs{};
            (void)lv_receiver_get_channel_stats(rx, 0, &cs);
            std::this_thread::sleep_for(std::chrono::microseconds(2667));
        }
    });
    std::thread midi([&] {  // = CoreMIDI -> network MIDI sink
        uint8_t n = 60;
        while (!stop.load()) {
            if (lv_sender_push_midi(tx, 0x90, n, 100, 0, 0) == LV_OK) midi_ok.fetch_add(1);
            lv_sender_push_midi(tx, 0x80, n, 0, 0, 0);
            n = uint8_t(60 + (n - 59) % 12);
            std::this_thread::sleep_for(std::chrono::milliseconds(7));
        }
    });

    uint32_t rev = 1;
    int toggles = 0;
    for (int i = 0; i < 24; ++i) {
        const int audio_only = (i % 2 == 0) ? 0 : 1;  // false -> true -> false ...
        const int source = (i % 4 == 0) ? LV_SOURCE_SCREEN : LV_SOURCE_PATTERN;
        // Exactly what LanVid Studio does on the toggle: restart both handles with the new flag.
        REQUIRE(start_rx(rx, port, audio_only));
        REQUIRE(start_tx(tx, port, audio_only, source));
        ++toggles;
        LV_ControlSyncState cs{};
        cs.revision = ++rev;
        cs.origin_id = 0xA11D0001u;
        cs.audio_channels = kCh;
        cs.target_buffer_ms = 30;
        cs.latency_mode = 1;
        cs.audio_only = uint8_t(audio_only);
        cs.active_preset_id = -1;
        lv_sender_send_control_sync(tx, &cs);
#if defined(LV_HAVE_SHM_BUS)
        CHECK(lv_duplex_send_control_sync(A, &cs) == LV_OK);
        lv_duplex_set_latency_ms(A, (i % 2) ? 30 : 40);
#endif
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
    }

    // Audio keeps flowing after the last toggle.
    const uint64_t pushes_before = pushes_ok.load();
    LV_AudioChannelStats r0{}, r1{};
    lv_receiver_get_channel_stats(rx, 0, &r0);
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    // Slow / virtualized CI runners coarsen the pusher's 2.67 ms sleep (seen: ~16 ms per iteration on
    // GitHub macOS), so wait for the same >50 pushes against a deadline instead of a fixed 400 ms window.
    const auto settle_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (pushes_ok.load() - pushes_before <= 50 && std::chrono::steady_clock::now() < settle_deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    lv_receiver_get_channel_stats(rx, 0, &r1);
    stop.store(true);
    pusher.join();
    popper.join();
    midi.join();

    LvSenderStats st{};
    lv_sender_get_stats(tx, &st);
    uint64_t b_updates = 0;
    uint8_t b_audio_only = 0xFF;
#if defined(LV_HAVE_SHM_BUS)
    {
        std::lock_guard<std::mutex> lk(b_seen.mu);
        b_updates = b_seen.updates;
        b_audio_only = b_seen.last_audio_only;
    }
    LV_DuplexStats ds{};
    CHECK(lv_duplex_get_stats(B, &ds) == LV_OK);
    lv_duplex_destroy(A);
    lv_duplex_destroy(B);
#endif
    std::printf("    [audio-only toggle] %d restarts under load | pushes ok %llu (+%llu after last toggle) pops %llu "
                "midi %llu | rx audio pkts %llu -> %llu | duplex sync updates %llu (last audio_only=%u) | "
                "video_source_error=%d\n",
                toggles, (unsigned long long)pushes_ok.load(), (unsigned long long)(pushes_ok.load() - pushes_before),
                (unsigned long long)pops_ok.load(), (unsigned long long)midi_ok.load(),
                (unsigned long long)r0.packets_rx, (unsigned long long)r1.packets_rx,
                (unsigned long long)b_updates, unsigned(b_audio_only), st.video_source_error);

    CHECK(st.running == 1);
    CHECK(pushes_ok.load() - pushes_before > 50);         // sender accepts audio after the final toggle
    CHECK(r1.packets_rx > r0.packets_rx);     // ... and it arrives
    CHECK(pops_ok.load() > 100);
    CHECK(midi_ok.load() > 10);
#if defined(LV_HAVE_SHM_BUS)
    CHECK(b_updates >= 1);                                // kControlSync crossed the duplex link
    CHECK(b_audio_only == 1);                             // last state sent: audio_only = true
#endif
    lv_sender_destroy(tx);
    lv_receiver_destroy(rx);
}
