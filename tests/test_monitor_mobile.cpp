// LanVid Monitor (Android / iOS app) protocol: everything the phone uses, through the same C ABI the apps
// call - connect, channel names + live VU, DSP with compressor gain-reduction telemetry, live stream
// quality / buffer changes (incl. Lossless), keepalive, stereo audio - plus the "_lanvid._udp" mDNS
// announcement LanVid Studio publishes (Bonjour browse + resolve on Apple platforms).
#include "mini_test.hpp"
#include "audio/personal_monitor_bus.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "common/clock.hpp"
#include "lanvid/lanvid_sdk.h"
#include "net/mdns_advertiser.hpp"
#include "audio/audio_reliability.hpp"
#include "audio/audio_rx.hpp"
#include "audio/audio_sync_matrix.hpp"
#include "audio/audio_tx.hpp"
#include "audio/intercom_engine.hpp"
#include "audio/master_clock.hpp"
#include "net/packet.hpp"
#include "net/udp_socket.hpp"
#include <algorithm>
#include <map>
#include <random>
#if defined(__APPLE__)
#include <arpa/inet.h>
#include <dns_sd.h>
#include <sys/select.h>
#endif

namespace {

#if defined(__APPLE__)
struct BrowseCtx {
    std::string want;
    bool found = false;
    uint16_t port = 0;
    std::string txt_role;
};

void DNSSD_API on_resolve(DNSServiceRef, DNSServiceFlags, uint32_t, DNSServiceErrorType err, const char*,
                          const char*, uint16_t port_be, uint16_t txt_len, const unsigned char* txt, void* ctx) {
    auto* c = static_cast<BrowseCtx*>(ctx);
    if (err != kDNSServiceErr_NoError) return;
    c->port = ntohs(port_be);
    uint8_t vlen = 0;
    const void* v = TXTRecordGetValuePtr(txt_len, txt, "role", &vlen);
    if (v) c->txt_role.assign(static_cast<const char*>(v), vlen);
}

void DNSSD_API on_browse(DNSServiceRef, DNSServiceFlags flags, uint32_t ifindex, DNSServiceErrorType err,
                         const char* name, const char* type, const char* domain, void* ctx) {
    auto* c = static_cast<BrowseCtx*>(ctx);
    if (err != kDNSServiceErr_NoError || !(flags & kDNSServiceFlagsAdd) || c->want != name) return;
    DNSServiceRef rr = nullptr;
    if (DNSServiceResolve(&rr, 0, ifindex, name, type, domain, &on_resolve, c) == kDNSServiceErr_NoError) {
        const int fd = DNSServiceRefSockFD(rr);
        for (int i = 0; i < 20 && c->port == 0; ++i) {
            fd_set rs;
            FD_ZERO(&rs);
            FD_SET(fd, &rs);
            timeval tv{0, 100000};
            if (select(fd + 1, &rs, nullptr, nullptr, &tv) > 0) DNSServiceProcessResult(rr);
        }
        DNSServiceRefDeallocate(rr);
    }
    c->found = true;
}
#endif

}  // namespace

TEST(monitor_mobile_client_protocol_and_mdns) {
    constexpr int kCh = 4;
    constexpr uint32_t kBlk = 240;  // 5 ms

    // ---- PC side: monitor server fed by a 4-channel "matrix" ----
    LvMonitorServerHandle* srv = lv_monitor_server_create(48000);
    REQUIRE(srv);
    REQUIRE(lv_monitor_server_start(srv, 0) == LV_OK);
    const uint16_t port = lv_monitor_server_port(srv);
    REQUIRE(port != 0);
    const char* names[kCh] = {"Kick", "Bass", "Vocal", "Click"};
    for (int c = 0; c < kCh; ++c) lv_monitor_server_set_channel_name(srv, c, names[c]);

    std::atomic<bool> stop{false};
    std::thread pc([&] {
        std::vector<std::vector<float>> pl(kCh, std::vector<float>(kBlk));
        const float* pp[kCh];
        for (int c = 0; c < kCh; ++c) pp[c] = pl[c].data();
        uint64_t n = 0;
        uint64_t next = lv::now_us();
        while (!stop.load()) {
            for (int c = 0; c < kCh; ++c)
                for (uint32_t i = 0; i < kBlk; ++i)  // loud tones (-3 dBFS) so the compressor has work
                    pl[c][i] = 0.7f * float(std::sin(2.0 * 3.14159265358979 * (220.0 * (c + 1)) * double(n + i) / 48000.0));
            lv_monitor_server_push_audio_planar(srv, pp, kCh, kBlk);
            n += kBlk;
            next += 5000;
            lv::sleep_until_us(next, [] {});
        }
    });

    // ---- Phone side (exactly what the apps do) ----
    LvMonitorClientHandle* cli = lv_monitor_client_create(48000);
    REQUIRE(cli);
    REQUIRE(lv_monitor_client_connect(cli, "127.0.0.1", port, "Pixel-Test", LV_MONITOR_BITRATE_LOSSLESS, 20) == LV_OK);
    std::atomic<uint64_t> nonsilent{0};
    std::thread phone([&] {  // network poll + meta at 20 Hz + keepalive 1 s + "audio callback" pops
        // The audio callback pulls exactly the frames that elapsed on a precise clock (a device clock does
        // not care how coarse the OS sleep is; Windows sleeps in ~15 ms steps without a high-res timer).
        std::vector<float> st(2 * 1024);
        const uint64_t t0 = lv::now_us();
        uint64_t t_meta = t0, t_keep = t0, popped = 0;
        while (!stop.load()) {
            lv_monitor_client_poll(cli);
            const uint64_t now = lv::now_us();
            if (now - t_meta > 50'000) { lv_monitor_client_request_meta(cli); t_meta = now; }
            if (now - t_keep > 1'000'000) { lv_monitor_client_keepalive(cli); t_keep = now; }
            const uint64_t due = (now - t0) * 48000ull / 1000000ull;
            while (popped + 96 <= due) {
                lv_monitor_client_pop_stereo(cli, st.data(), 96);
                popped += 96;
                for (int i = 0; i < 192; ++i)
                    if (std::fabs(st[size_t(i)]) > 0.01f) { nonsilent.fetch_add(1); break; }
            }
            lv::sleep_until_us(lv::now_us() + 1000, [] {});
        }
    });

    // Mixer + DSP from the phone: kick only, hard compressor.
    for (int c = 0; c < kCh; ++c) lv_monitor_client_set_channel_mix(cli, c, c == 0 ? 0.0f : -60.0f, 0.0f, c != 0, 0);
    LV_MonitorDspParams dsp{};
    dsp.hpf_enabled = 1;
    dsp.hpf_hz = 40.0f;
    dsp.eq_mid_hz = 1200.0f;
    dsp.comp_enabled = 1;
    dsp.comp_thresh_db = -30.0f;
    dsp.comp_ratio = 8.0f;
    dsp.comp_attack_ms = 2.0f;
    dsp.comp_release_ms = 80.0f;
    dsp.comp_makeup_db = 0.0f;
    dsp.master_volume_db = 0.0f;
    REQUIRE(lv_monitor_client_set_dsp(cli, &dsp) == LV_OK);
    std::this_thread::sleep_for(std::chrono::milliseconds(800));

    LV_MonitorMetaChannel meta[64]{};
    LV_MonitorTelemetry tel{};
    const int n = lv_monitor_client_get_meta(cli, meta, 64, &tel);
    LV_MonitorClientInfo info[8]{};
    int nc = lv_monitor_server_get_clients(srv, info, 8);
    const uint16_t br_lossless = nc > 0 ? info[0].bitrate_kbps : 0;

    // Live stream change: 256 kbps stereo, 15 ms buffer - no reconnect.
    REQUIRE(lv_monitor_client_set_stream(cli, 256, 15) == LV_OK);
    CHECK(lv_monitor_client_set_stream(cli, 999, 15) == LV_ERR_INVALID_ARG);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    nc = lv_monitor_server_get_clients(srv, info, 8);
    const uint64_t audio_before = tel.audio_packets_rx;
    LV_MonitorTelemetry tel2{};
    lv_monitor_client_get_meta(cli, nullptr, 0, &tel2);

    // mDNS announcement (what Studio publishes when the monitor server is on).
    lv::MdnsAdvertiser adv;
    const std::string inst = "LanVid Studio (unit-" + std::to_string(port) + ")";
    const bool adv_ok = adv.start(inst, port, {{"role", "monitor"}, {"ver", "2.6.0"}});
    bool mdns_found = false;
    uint16_t mdns_port = 0;
    std::string mdns_role;
#if defined(__APPLE__)
    BrowseCtx bc;
    bc.want = inst;
    DNSServiceRef br = nullptr;
    if (adv_ok && DNSServiceBrowse(&br, 0, 0, lv::kLanVidMdnsService, nullptr, &on_browse, &bc) == kDNSServiceErr_NoError) {
        const int fd = DNSServiceRefSockFD(br);
        for (int i = 0; i < 50 && !bc.found; ++i) {  // up to 5 s
            fd_set rs;
            FD_ZERO(&rs);
            FD_SET(fd, &rs);
            timeval tv{0, 100000};
            if (select(fd + 1, &rs, nullptr, nullptr, &tv) > 0) DNSServiceProcessResult(br);
        }
        DNSServiceRefDeallocate(br);
    }
    mdns_found = bc.found;
    mdns_port = bc.port;
    mdns_role = bc.txt_role;
#endif
    adv.stop();

    stop.store(true);
    phone.join();
    pc.join();
    lv_monitor_client_disconnect(cli);
    lv_monitor_client_destroy(cli);
    lv_monitor_server_stop(srv);
    lv_monitor_server_destroy(srv);

    std::printf("    [monitor app] channels %d: %s %d dBFS / %s %d dBFS ... | GR %.1f dB, mix peak %.0f dBFS, buffer %.1f ms, "
                "audio pkts %llu -> %llu, meta %llu | server sees: lossless=%s then %u kbps / %u ms | non-silent blocks %llu | "
                "mDNS adv=%d found=%d port=%u role=%s\n",
                n, meta[0].name, meta[0].peak_dbfs, meta[2].name, meta[2].peak_dbfs, tel.bus_gain_reduction_db,
                tel.bus_peak_dbfs, tel2.buffer_ms, (unsigned long long)audio_before,
                (unsigned long long)tel2.audio_packets_rx, (unsigned long long)tel2.meta_packets_rx,
                br_lossless == LV_MONITOR_BITRATE_LOSSLESS ? "yes" : "no", nc > 0 ? unsigned(info[0].bitrate_kbps) : 0u,
                nc > 0 ? unsigned(info[0].latency_ms) : 0u, (unsigned long long)nonsilent.load(), int(adv_ok),
                int(mdns_found), unsigned(mdns_port), mdns_role.c_str());

    CHECK(n == kCh);
    CHECK(std::strcmp(meta[0].name, "Kick") == 0 && std::strcmp(meta[2].name, "Vocal") == 0);
    CHECK(meta[0].peak_dbfs > -10 && meta[0].peak_dbfs <= 0);  // live input VU
    CHECK(tel.connected == 1);
    CHECK(tel.bus_gain_reduction_db < -3.0f);                // compressor working on the PC, reported to the phone
    CHECK(tel.bus_peak_dbfs > -30.0f);
    CHECK(br_lossless == LV_MONITOR_BITRATE_LOSSLESS);
    CHECK(nc == 1 && info[0].bitrate_kbps == 256 && info[0].latency_ms == 15);
    CHECK(std::string(info[0].name) == "Pixel-Test");
    CHECK(tel2.audio_packets_rx > audio_before && audio_before > 50);  // stream continued across the change
    CHECK(tel2.meta_packets_rx > 5);
    // The 15 ms the musician chose (Fixed, no growth to 50..200 ms). The reading also includes how much of the
    // current pop cycle has elapsed (up to ~4 ms), hence the lower bound.
    CHECK(tel2.buffer_ms > 8.0 && tel2.buffer_ms < 25.0);
    CHECK(nonsilent.load() > 100);
#if defined(__APPLE__)
    CHECK(lv::MdnsAdvertiser::supported());
    CHECK(adv_ok);
    CHECK(mdns_found);
    CHECK(mdns_port == port);
    CHECK(mdns_role == "monitor");
#endif
}

TEST(monitor_discovery_and_handshake_across_interfaces) {
    using namespace lv;

    // 1. Start PersonalMonitorServer bound to 0.0.0.0 with ephemeral monitor + discovery ports
    PersonalMonitorServer srv(48000);
    srv.set_studio_name("LanVid Studio (Diag-PC)");
    srv.set_channel_name(0, "Lead Vox");
    srv.set_channel_name(1, "Guitar");
    REQUIRE(srv.start(0, "0.0.0.0", 0));
    const uint16_t mon_port = srv.bound_port();
    const uint16_t disc_port = srv.discovery_port();
    REQUIRE(mon_port != 0);
    REQUIRE(disc_port != 0);

    // Poll server on a background thread (just like LanVidStudioApp::render_frame())
    std::atomic<bool> stop_srv{false};
    std::thread srv_thread([&]() {
        std::vector<float> ch0(240, 0.25f), ch1(240, -0.20f);
        const float* planes[2] = {ch0.data(), ch1.data()};
        while (!stop_srv.load(std::memory_order_relaxed)) {
            srv.poll_control();
            srv.process_and_send(planes, 2, 240);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });

    // 2. Discover studio via UDP LAN Broadcast Discovery (works even when mDNS/Bonjour is disabled)
    const auto found = discover_monitor_servers(400, disc_port, /*include_loopback=*/true);
    CHECK(!found.empty());
    bool matched = false;
    for (const auto& f : found) {
        if (f.port == mon_port && f.name == "LanVid Studio (Diag-PC)") matched = true;
    }
    CHECK(matched);
    CHECK(srv.discovery_replies() >= 1);

    // 3. Direct kHello -> kSyncMetaResp handshake + audio streaming
    PersonalMonitorClient cli(48000);
    REQUIRE(cli.connect("127.0.0.1", mon_port, "iPhone-Diag", 256, 15));
    for (int i = 0; i < 40 && (cli.meta_packets_rx() == 0 || cli.audio_packets_rx() < 5); ++i) {
        cli.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(cli.meta_packets_rx() >= 1);
    CHECK(cli.audio_packets_rx() >= 5);
    CHECK(srv.total_control_rx() >= 1);

    // 4. Reconnect regression test: client reconnects on a brand-new ephemeral UDP port!
    cli.disconnect();
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    const uint64_t prev_audio = cli.audio_packets_rx();
    REQUIRE(cli.connect("127.0.0.1", mon_port, "iPhone-Diag", 256, 15));
    for (int i = 0; i < 40 && cli.audio_packets_rx() <= prev_audio + 5; ++i) {
        cli.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(cli.audio_packets_rx() > prev_audio + 5);

    std::printf("    [monitor discovery+handshake] found=%zu disc_replies=%llu ctrl_rx=%llu audio_rx=%llu (reconnect OK)\n",
                found.size(), static_cast<unsigned long long>(srv.discovery_replies()),
                static_cast<unsigned long long>(srv.total_control_rx()),
                static_cast<unsigned long long>(cli.audio_packets_rx()));

    cli.disconnect();
    stop_srv.store(true);
    srv_thread.join();
    srv.stop();
}

// ---------------------------------------------------------------------------------------------
// Monitor audio quality: no clipping of a hot 64->2 sum, and a long Wi-Fi-like playout (consumer clock
// +120 ppm fast, 2 % packet loss, 0..2 ms jitter) without discontinuities, zero-gaps or latency creep.
// Virtual time: deterministic, no sockets, no sleeps.
// ---------------------------------------------------------------------------------------------
TEST(monitor_bus_hot_sum_never_clips) {
    lv::PersonalMonitorBus bus(48000);
    constexpr int kCh = 16;
    constexpr uint32_t kFrames = 480;
    std::vector<std::vector<float>> in(kCh, std::vector<float>(kFrames));
    std::vector<const float*> planes(kCh);
    for (int c = 0; c < kCh; ++c) planes[c] = in[c].data();
    std::vector<float> l(kFrames), r(kFrames);
    float peak = 0.0f, max_step = 0.0f, prev = 0.0f;
    for (int blk = 0; blk < 200; ++blk) {
        for (int c = 0; c < kCh; ++c)
            for (uint32_t i = 0; i < kFrames; ++i) {
                const double t = double(blk * kFrames + i) / 48000.0;
                in[c][i] = float(std::sin(2.0 * 3.14159265358979 * (220.0 + 37.0 * c) * t));  // 0 dBFS each
            }
        bus.process_block(planes.data(), kCh, l.data(), r.data(), kFrames);
        for (uint32_t i = 0; i < kFrames; ++i) {
            peak = std::max({peak, std::fabs(l[i]), std::fabs(r[i])});
            if (blk > 2) max_step = std::max(max_step, std::fabs(l[i] - prev));
            prev = l[i];
        }
    }
    std::printf("    [monitor bus] 16 x 0 dBFS sum: peak=%.4f max_step=%.4f\n", peak, max_step);
    CHECK(peak <= 0.9441f);        // brickwall ceiling, never above full scale
    CHECK(max_step < 0.5f);        // no hard-clip steps
}

namespace {
struct DriftSim {
    static constexpr uint32_t kRate = 48000;
    static constexpr int64_t kOff = 5'000'000;
    lv::MasterClock clk;
    lv::AudioSyncMatrix mat{kRate};
    lv::AudioTxEngine tx;
    lv::AudioRxEngine rx;
    std::multimap<double, std::vector<uint8_t>> q;  // arrival (true us) -> datagram
    std::mt19937 rng{2024};
    double true_us = 1'000'000.0;
    double drift = 120e-6;
    uint64_t lt() const { return uint64_t(true_us * (1.0 + drift)); }

    DriftSim()
        : tx(kRate, lv::kDefaultMtu, [this](const uint8_t* d, size_t n) {
              if (std::uniform_real_distribution<double>(0, 1)(rng) < 0.02) return true;  // 2 % loss
              q.insert({true_us + 1000.0 + double(rng() % 2000), std::vector<uint8_t>(d, d + n)});
              return true;
          }),
          rx(mat, clk, [](const uint8_t*, size_t) { return true; }) {
        clk.set_fixed_offset(kOff);
    }
    void deliver() {
        while (!q.empty() && q.begin()->first <= true_us) {
            auto dg = std::move(q.begin()->second);
            q.erase(q.begin());
            lv::PacketHeader h;
            const uint8_t* pl = nullptr;
            if (lv::parse_packet(dg.data(), dg.size(), h, &pl) == lv::PacketParseStatus::kOk &&
                h.type == lv::PacketType::kAudio2)
                rx.on_packet(h, pl, h.payload_size, lt());
        }
    }
};
}  // namespace

TEST(monitor_playout_drift_120ppm_loss_2pct_is_seamless) {
    DriftSim s;
    s.mat.set_target_latency_ms(15);
    s.rx.set_latency_ms(15);
    s.rx.set_adaptive_jitter_enabled(false);
    s.mat.set_max_resample_ppm(2000.0);  // as PersonalMonitorClient
    s.tx.set_latency_ms(15);
    s.tx.set_codec_profile(lv::audio_codec_profile_from_bitrate(128));  // Opus CELT

    constexpr uint32_t kSend = 120;   // 2.5 ms
    constexpr uint32_t kPop = 128;    // phone callback
    const double send_period = 1e6 * kSend / 48000.0;
    const double pop_period = 1e6 * kPop / (48000.0 * (1.0 + s.drift));  // consumer crystal runs fast
    double next_send = s.true_us, next_pop = s.true_us + 40'000.0;
    const double end_us = s.true_us + 60'000'000.0;  // 60 s
    std::vector<float> out(kPop);
    float prev = 0.0f, peak = 0.0f, max_step = 0.0f;
    uint64_t pops = 0, zero_run = 0, max_zero_run = 0, big_steps = 0;
    double lat_min = 1e9, lat_max = 0.0;
    std::vector<float> blk(kSend);
    while (s.true_us < end_us) {
        s.true_us = std::min(next_send, next_pop);
        if (s.true_us >= next_send) {
            const uint64_t ts0 = uint64_t((int64_t(s.true_us) + DriftSim::kOff) * 48000 / 1'000'000) - kSend;
            for (uint32_t i = 0; i < kSend; ++i)
                blk[i] = 0.5f * float(std::sin(2.0 * 3.14159265358979 * 1000.0 * double(ts0 + i) / 48000.0));
            s.tx.push_channel(0, blk.data(), kSend, lv::AudioFormat2::kFloat32, ts0, uint32_t(s.true_us));
            next_send += send_period;
        }
        s.deliver();
        s.rx.service(s.lt());
        if (s.true_us >= next_pop) {
            s.mat.pop_interleaved(s.clk, s.lt(), out.data(), 1, kPop);
            next_pop += pop_period;
            ++pops;
            if (pops > 1500) {  // after start-up + settling (~4 s)
                for (uint32_t i = 0; i < kPop; ++i) {
                    peak = std::max(peak, std::fabs(out[i]));
                    const float st = std::fabs(out[i] - prev);
                    max_step = std::max(max_step, st);
                    if (st > 0.25f) ++big_steps;
                    zero_run = (out[i] == 0.0f) ? zero_run + 1 : 0;
                    max_zero_run = std::max(max_zero_run, zero_run);
                    prev = out[i];
                }
                const double lat = s.mat.current_effective_latency_ms();
                lat_min = std::min(lat_min, lat);
                lat_max = std::max(lat_max, lat);
            } else {
                prev = out[kPop - 1];
            }
        }
    }
    std::printf("    [monitor drift] 60 s, +120 ppm, 2%% loss: peak=%.3f max_step=%.3f big_steps=%llu zero_run=%llu "
                "latency=%.1f..%.1f ms resyncs=%llu ppm=%.0f\n",
                peak, max_step, (unsigned long long)big_steps, (unsigned long long)max_zero_run, lat_min, lat_max,
                (unsigned long long)s.mat.resync_count(), s.mat.resample_ppm());
    CHECK(peak <= 1.0f);
    CHECK(big_steps == 0);          // no click-sized discontinuities (1 kHz @ 0.5 FS has steps <= 0.07)
    CHECK(max_zero_run < 8);        // no zero-filled holes
    CHECK(lat_min >= 3.0 && lat_max <= 40.0);  // latency stays bounded (no creep)
    CHECK(s.mat.resync_count() <= 3);
}

// Wi-Fi burst delivery (power-save / airtime stalls): every 300 ms the link holds packets for 25 ms and then
// delivers them in one clump. Target buffer 20 ms, Fixed mode, exactly the phone's receive path (one-way
// clock from the packet timestamps + 75 % prefill hysteresis). After the first stall teaches the cushion,
// the buffer must never fall to 0 ("Waiting for audio"), never underrun again and never click.
TEST(monitor_wifi_burst_jitter_25ms_no_dropouts) {
    for (int profile = 0; profile < 2; ++profile) {  // 0: Opus 256 kbps stereo, 1: Lossless PCM24
        lv::MasterClock clk;
        lv::AudioSyncMatrix mat{48000};
        std::multimap<uint64_t, std::vector<uint8_t>> q;
        std::mt19937 rng(77 + profile);
        uint64_t now = 1'000'000;
        lv::AudioTxEngine tx(48000, lv::kDefaultMtu, [&](const uint8_t* d, size_t n) {
            uint64_t arrive = now + 1000 + rng() % 1000;  // 1..2 ms air time
            const uint64_t phase = now % 300'000;           // 25 ms stall every 300 ms
            // Held frames are released in order (the AP queue is FIFO), 20 us apart.
            if (phase < 25'000) arrive = now - phase + 25'000 + 1000 + phase / 125;
            q.insert({arrive, std::vector<uint8_t>(d, d + n)});
            return true;
        });
        lv::AudioRxEngine rx(mat, clk, [](const uint8_t*, size_t) { return true; });
        rx.set_latency_ms(20);
        rx.set_adaptive_jitter_enabled(false);
        rx.set_one_way_clock_sync(true);
        mat.set_underrun_prefill(true, 75);
        mat.set_max_resample_ppm(2000.0);
        tx.set_latency_ms(20);
        tx.set_codec_profile(profile == 0 ? lv::audio_codec_profile_from_bitrate(128) : lv::AudioCodecProfile::kPcm24);

        constexpr uint32_t kStep = 120, kPop = 240;  // server pacer step / iOS 5 ms IO buffer
        uint64_t next_send = now, next_pop = now + 30'000, media = 0;
        std::vector<float> blk(kStep), out(kPop * 2);
        const uint64_t warm = now + 3'000'000, end = now + 33'000'000;
        double lat_min = 1e9, lat_max = 0.0;
        float prev = 0.0f, max_step = 0.0f;
        uint64_t zero_run = 0, max_zero_run = 0, und0 = 0, pref0 = 0;
        bool measuring = false;
        while (now < end) {
            now = std::min(next_send, next_pop);
            if (now >= next_send) {
                for (uint32_t i = 0; i < kStep; ++i)
                    blk[i] = 0.5f * float(std::sin(2.0 * 3.14159265358979 * 440.0 * double(media + i) / 48000.0));
                tx.push_channel(0, blk.data(), kStep, lv::AudioFormat2::kFloat32, media, uint32_t(now));
                tx.push_channel(1, blk.data(), kStep, lv::AudioFormat2::kFloat32, media, uint32_t(now));
                media += kStep;
                next_send += 2500;
            }
            while (!q.empty() && q.begin()->first <= now) {
                auto dg = std::move(q.begin()->second);
                q.erase(q.begin());
                lv::PacketHeader h;
                const uint8_t* pl = nullptr;
                if (lv::parse_packet(dg.data(), dg.size(), h, &pl) == lv::PacketParseStatus::kOk &&
                    h.type == lv::PacketType::kAudio2)
                    rx.on_packet(h, pl, h.payload_size, now);
            }
            rx.service(now);
            if (now >= next_pop) {
                mat.pop_interleaved(clk, now, out.data(), 2, kPop);
                next_pop += 5000;
                if (!measuring && now >= warm) {
                    measuring = true;
                    und0 = mat.total_underruns();
                    pref0 = mat.prefill_events();
                }
                if (measuring) {
                    for (uint32_t i = 0; i < kPop; ++i) {
                        const float v = out[2 * i];
                        max_step = std::max(max_step, std::fabs(v - prev));
                        zero_run = (v == 0.0f) ? zero_run + 1 : 0;
                        max_zero_run = std::max(max_zero_run, zero_run);
                        prev = v;
                    }
                    const double lat = mat.current_effective_latency_ms();
                    lat_min = std::min(lat_min, lat);
                    lat_max = std::max(lat_max, lat);
                } else {
                    prev = out[2 * (kPop - 1)];
                }
            }
        }
        const uint64_t und = mat.total_underruns() - und0;
        std::printf("    [monitor wifi bursts 25 ms / %s] buffer=%.1f..%.1f ms cushion=%.1f ms underruns=%llu "
                    "prefills=%llu (warm-up %llu) max_step=%.3f zero_run=%llu\n",
                    profile == 0 ? "Opus 256" : "Lossless", lat_min, lat_max, mat.fixed_cushion_ms(),
                    (unsigned long long)und, (unsigned long long)(mat.prefill_events() - pref0),
                    (unsigned long long)pref0, max_step, (unsigned long long)max_zero_run);
        CHECK(lat_min >= 15.0);   // never drains towards 0 ms / "Waiting for audio"
        CHECK(lat_max <= 50.0);   // bounded cushion
        CHECK(und == 0);          // no dropouts once the cushion is learned
        CHECK(max_step < 0.1f);   // 440 Hz @ 0.5 FS moves <= 0.03 per sample: no clicks
        CHECK(max_zero_run < 8);
    }
}

// Server pacing: a mixer running 1024-frame blocks (21.3 ms) must still put packets on the wire evenly
// (dedicated real-time send thread, 2.5 ms steps -> one packet per channel every 5 ms), not in clumps.
TEST(monitor_server_paces_large_mixer_blocks) {
    lv::PersonalMonitorServer srv(48000);
    REQUIRE(srv.start(0, "127.0.0.1", 0));
    lv::PersonalMonitorClient cli(48000);
    REQUIRE(cli.connect("127.0.0.1", srv.bound_port(), "Pacing", 256, 20));
    std::atomic<bool> stop{false};
    std::thread mixer([&] {
        constexpr uint32_t kBlk = 1024;
        std::vector<float> ch(kBlk, 0.1f);
        const float* planes[2] = {ch.data(), ch.data()};
        uint64_t next = lv::now_us();
        while (!stop.load()) {
            srv.process_and_send(planes, 2, kBlk);
            next += kBlk * 1'000'000ull / 48000;
            lv::sleep_until_us(next, [] {});
        }
    });
    // Measure arrival spacing of audio packets at the phone.
    std::vector<uint64_t> arrivals;
    uint64_t last_pkts = 0;
    const uint64_t t_end = lv::now_us() + 1'500'000;
    while (lv::now_us() < t_end) {
        cli.poll();
        const uint64_t n = cli.audio_packets_rx();
        if (n != last_pkts) {
            if (lv::now_us() > t_end - 1'000'000) arrivals.push_back(lv::now_us());
            last_pkts = n;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    stop.store(true);
    mixer.join();
    std::vector<double> gaps;
    for (size_t i = 1; i < arrivals.size(); ++i) gaps.push_back(double(arrivals[i] - arrivals[i - 1]) / 1000.0);
    std::sort(gaps.begin(), gaps.end());
    const double p50 = gaps.empty() ? 0 : gaps[gaps.size() / 2];
    const double p95 = gaps.empty() ? 0 : gaps[gaps.size() * 95 / 100];
    std::printf("    [monitor pacing] 1024-frame mixer blocks: %zu arrival events/s, gap p50=%.2f p95=%.2f max=%.2f ms, "
                "paced steps=%llu, pings=%llu\n",
                arrivals.size(), p50, p95, gaps.empty() ? 0 : gaps.back(),
                (unsigned long long)srv.paced_chunks(), (unsigned long long)cli.wake_pings_sent());
    CHECK(arrivals.size() >= 120);  // ~200 packet events/s; a 21 ms burst pattern would give ~47
    CHECK(p95 < 15.0);              // evenly spaced (bursty mixer would show ~21 ms gaps)
    CHECK(cli.wake_pings_sent() >= 50);
    cli.disconnect();
    srv.stop();
}

// Regression for real-world iOS Wi-Fi stuttering (VID_20261124):
// Isolated mid-stream packet loss must NOT trigger prefill playback halt, and a 75 ms Wi-Fi burst
// must NOT blow up the buffer to 200-300 ms; NACK + Opus PLC recover seamlessly.
TEST(monitor_wifi_loss_and_burst_no_prefill_freeze_and_nack_plc) {
    constexpr uint32_t kRate = 48000;
    constexpr uint32_t kChunk = 240;  // 5 ms
    lv::AudioSyncMatrix mat(kRate);
    lv::MasterClock clk;
    std::vector<std::vector<uint8_t>> nack_q;
    lv::AudioRxEngine arx(mat, clk, [&](const uint8_t* d, size_t n) {
        nack_q.emplace_back(d, d + n);
        return true;
    });
    arx.set_latency_ms(20);
    arx.set_adaptive_jitter_enabled(false);
    arx.set_one_way_clock_sync(true);
    mat.set_underrun_prefill(true, 75);
    mat.set_max_resample_ppm(2000.0);

    struct NetPkt { uint64_t deliver_us; std::vector<uint8_t> bytes; };
    std::vector<NetPkt> wire;
    uint64_t sim_now = 1'000'000;
    uint32_t pkt_idx = 0;
    lv::AudioTxEngine atx(kRate, lv::kDefaultMtu, [&](const uint8_t* d, size_t n) {
        ++pkt_idx;
        // Drop 4% of packets (every 25th packet) to verify mid-stream loss never freezes prefill!
        if (pkt_idx > 20 && (pkt_idx % 25 == 0)) return true;
        // Inject a 75 ms Wi-Fi burst stall around packet 140..155
        const uint64_t burst = (pkt_idx >= 140 && pkt_idx <= 155) ? 75'000ull : 2'000ull;
        wire.push_back({sim_now + burst, std::vector<uint8_t>(d, d + n)});
        return true;
    });
    atx.set_latency_ms(20);
    atx.set_codec_profile(lv::audio_codec_profile_from_bitrate(128));  // 256 kbps stereo Opus

    std::vector<float> ch0(kChunk), ch1(kChunk);
    std::vector<float> out(kChunk * 2);
    uint64_t media_ts = 0;
    double max_lat_ms = 0.0;
    uint64_t prefills_during_loss = 0;

    for (int step = 0; step < 300; ++step) {
        for (uint32_t i = 0; i < kChunk; ++i) {
            const double ph = 2.0 * 3.141592653589793 * 440.0 * double(media_ts + i) / double(kRate);
            ch0[i] = ch1[i] = float(0.4 * std::sin(ph));
        }
        atx.push_channel(0, ch0.data(), kChunk, lv::AudioFormat2::kFloat32, media_ts, uint32_t(sim_now));
        atx.push_channel(1, ch1.data(), kChunk, lv::AudioFormat2::kFloat32, media_ts, uint32_t(sim_now));
        media_ts += kChunk;

        // Deliver any wire packets due by sim_now
        auto it = wire.begin();
        while (it != wire.end()) {
            if (it->deliver_us <= sim_now) {
                lv::PacketHeader h{};
                const uint8_t* p = nullptr;
                if (lv::parse_packet(it->bytes.data(), it->bytes.size(), h, &p) == lv::PacketParseStatus::kOk &&
                    h.type == lv::PacketType::kAudio2) {
                    arx.on_packet(h, p, h.payload_size, sim_now);
                }
                it = wire.erase(it);
            } else {
                ++it;
            }
        }
        arx.service(sim_now);
        // Feed NACKs back to sender
        for (const auto& nk : nack_q) {
            lv::PacketHeader h{};
            const uint8_t* p = nullptr;
            if (lv::parse_packet(nk.data(), nk.size(), h, &p) == lv::PacketParseStatus::kOk &&
                h.type == lv::PacketType::kAudioNack) {
                atx.on_nack(p, h.payload_size);
            }
        }
        nack_q.clear();

        mat.pop_interleaved(clk, sim_now, out.data(), 2, kChunk);
        const double lat = mat.current_effective_latency_ms();
        if (step > 20) max_lat_ms = std::max(max_lat_ms, lat);
        if (step == 135) prefills_during_loss = mat.prefill_events();
        sim_now += 5000;
    }

    std::printf("    [monitor loss+75ms burst] max_buffer=%.1f ms | prefills_before_burst=%llu | total_prefills=%llu\n",
                max_lat_ms, static_cast<unsigned long long>(prefills_during_loss),
                static_cast<unsigned long long>(mat.prefill_events()));
    CHECK(max_lat_ms < 120.0);         // NEVER blows up to 294.8 ms!
    CHECK(prefills_during_loss <= 1);  // Mid-stream 4% loss does NOT trigger false prefill halts!
}

// Dante-inspired Wi-Fi Stability & Low Latency:
// 5% random Wi-Fi packet loss with 1+1 Duplicate (+1.5 ms stagger) + Deterministic Timestamp-Driven
// Playout (8 ms target, +/-0.1% micro-rate trim, +80 ppm consumer clock drift):
// Must achieve 0 NACK retransmissions, 0 dropouts/discontinuities, and rock-steady 5..10 ms latency!
TEST(monitor_wifi_redundancy_1plus1_5pct_loss_zero_nack_zero_glitches) {
    constexpr uint32_t kRate = 48000;
    constexpr uint32_t kStep = 120;  // 2.5 ms push -> 5 ms Opus CELT packets
    constexpr uint32_t kPop = 128;   // audio callback block
    constexpr double kDrift = 80e-6; // +80 ppm receiver crystal drift

    lv::MasterClock clk;
    constexpr int64_t kOff = 4'000'000;
    clk.set_fixed_offset(kOff);

    lv::AudioSyncMatrix mat(kRate);
    mat.set_target_latency_ms(9);                     // 9 ms deterministic target (within 5..10 ms)
    mat.set_deterministic_playout(true, 1000.0);      // +/-0.1% (+/-1000 ppm) micro-rate trim, no cushion growth

    lv::AudioRxEngine rx(mat, clk, [](const uint8_t*, size_t) { return true; });
    rx.set_latency_ms(9);
    rx.set_adaptive_jitter_enabled(false);

    std::multimap<double, std::vector<uint8_t>> wire;
    std::mt19937 rng(20261125);
    std::uniform_real_distribution<double> u01(0.0, 1.0);
    double true_us = 1'000'000.0;
    // MasterClock is PTP-locked to the sender timeline (`true_us`), while the receiver audio hardware
    // playback crystal runs +80 ppm fast (`pop_period`), exercising +/-0.1% micro-rate trim!
    auto clk_now_us = [&]() -> uint64_t { return uint64_t(true_us); };

    // Track primary drops per (stream_id, seq) to model 5% random isolated Wi-Fi interference (< 1.5 ms)
    std::map<uint64_t, bool> primary_dropped;
    uint64_t total_sent = 0, dropped_primary = 0, dropped_dup = 0;

    lv::AudioTxEngine tx(kRate, lv::kDefaultMtu, [&](const uint8_t* d, size_t n) {
        ++total_sent;
        lv::PacketHeader h{};
        const uint8_t* pl = nullptr;
        lv::AudioHeader2 ah{};
        bool is_dup = false;
        uint64_t key = 0;
        if (lv::parse_packet(d, n, h, &pl) == lv::PacketParseStatus::kOk &&
            h.type == lv::PacketType::kAudio2 &&
            lv::parse_audio_header2(pl, h.payload_size, ah)) {
            is_dup = (ah.flags & lv::kAudioFlagDuplicate) != 0;
            key = (uint64_t(ah.stream_id) << 32) | uint64_t(h.frame_id);
        }
        // 5% random packet loss across the Wi-Fi link (isolated interference shorter than 1.5 ms stagger)
        if (!is_dup) {
            if (u01(rng) < 0.05) {
                primary_dropped[key] = true;
                ++dropped_primary;
                return true;
            }
        } else {
            auto it = primary_dropped.find(key);
            const bool need_backup = (it != primary_dropped.end());
            if (need_backup) {
                primary_dropped.erase(it);
            } else if (u01(rng) < 0.05) {
                ++dropped_dup;
                return true;
            }
        }
        const double air_us = 800.0 + double(rng() % 1400);  // 0.8..2.2 ms Wi-Fi transit jitter
        wire.insert({true_us + air_us, std::vector<uint8_t>(d, d + n)});
        return true;
    });
    tx.set_latency_ms(9);
    tx.set_codec_profile(lv::audio_codec_profile_from_bitrate(128));  // 256 kbps stereo Opus (2.5 ms CELT frames)
    tx.set_wifi_redundancy(true, 1500);                               // 1+1 Duplicate (+1.5 ms stagger)

    const double send_period = 1e6 * double(kStep) / double(kRate);
    const double pop_period = 1e6 * double(kPop) / (double(kRate) * (1.0 + kDrift));
    double next_send = true_us;
    double next_pop = true_us + 25'000.0;
    const double end_us = true_us + 10'000'000.0;  // 10 s virtual-time test
    const double warm_us = true_us + 1'000'000.0;

    std::vector<float> blk(kStep);
    std::vector<float> out(kPop * 2);
    float prev = 0.0f, max_step = 0.0f;
    uint64_t big_steps = 0, zero_run = 0, max_zero_run = 0, und0 = 0, resync0 = 0;
    double lat_min = 1e9, lat_max = 0.0;
    bool measuring = false;

    while (true_us < end_us) {
        const double next_tick = std::min({next_send, next_pop, true_us + 200.0});
        true_us = next_tick;
        if (true_us >= next_send) {
            const uint64_t ts0 = uint64_t((int64_t(true_us) + kOff) * int64_t(kRate) / 1'000'000) - kStep;
            for (uint32_t i = 0; i < kStep; ++i) {
                blk[i] = 0.5f * float(std::sin(2.0 * 3.141592653589793 * 440.0 * double(ts0 + i) / double(kRate)));
            }
            tx.push_channel(0, blk.data(), kStep, lv::AudioFormat2::kFloat32, ts0, uint32_t(true_us));
            tx.push_channel(1, blk.data(), kStep, lv::AudioFormat2::kFloat32, ts0, uint32_t(true_us));
            next_send += send_period;
        }
        tx.service_redundancy(uint32_t(true_us));

        while (!wire.empty() && wire.begin()->first <= true_us) {
            auto dg = std::move(wire.begin()->second);
            wire.erase(wire.begin());
            lv::PacketHeader h{};
            const uint8_t* pl = nullptr;
            if (lv::parse_packet(dg.data(), dg.size(), h, &pl) == lv::PacketParseStatus::kOk &&
                h.type == lv::PacketType::kAudio2) {
                rx.on_packet(h, pl, h.payload_size, clk_now_us());
            }
        }
        rx.service(clk_now_us());

        if (true_us >= next_pop) {
            mat.pop_interleaved(clk, clk_now_us(), out.data(), 2, kPop);
            next_pop += pop_period;
            if (!measuring && true_us >= warm_us) {
                measuring = true;
                und0 = mat.total_underruns();
                resync0 = mat.resync_count();
                prev = out[2 * (kPop - 1)];
            } else if (measuring) {
                for (uint32_t i = 0; i < kPop; ++i) {
                    const float v = out[2 * i];
                    const float st = std::fabs(v - prev);
                    max_step = std::max(max_step, st);
                    if (st > 0.20f) ++big_steps;
                    zero_run = (v == 0.0f) ? (zero_run + 1) : 0;
                    max_zero_run = std::max(max_zero_run, zero_run);
                    prev = v;
                }
                const double lat = mat.current_effective_latency_ms();
                lat_min = std::min(lat_min, lat);
                lat_max = std::max(lat_max, lat);
            } else {
                prev = out[2 * (kPop - 1)];
            }
        }
    }

    const uint64_t und = mat.total_underruns() - und0;
    const uint64_t resyncs = mat.resync_count() - resync0;
    const double loss_pct = 100.0 * double(dropped_primary + dropped_dup) / double(std::max<uint64_t>(1, total_sent));
    std::printf("    [dante 1+1 wifi 5%% loss] sent=%llu drop_pri=%llu drop_dup=%llu (%.2f%%) | "
                "nacks=%llu dedup=%llu underruns=%llu big_steps=%llu resyncs=%llu | "
                "lat=%.2f..%.2f ms cushion=%.1f ms ppm=%.0f\n",
                (unsigned long long)total_sent, (unsigned long long)dropped_primary, (unsigned long long)dropped_dup,
                loss_pct, (unsigned long long)rx.total_nacks_sent(), (unsigned long long)rx.total_duplicates(),
                (unsigned long long)und, (unsigned long long)big_steps, (unsigned long long)resyncs,
                lat_min, lat_max, mat.fixed_cushion_ms(), mat.resample_ppm());

    CHECK(dropped_primary >= 250);               // genuine ~5% primary loss actually occurred
    CHECK(rx.total_nacks_sent() == 0);           // 0 NACK retransmissions needed!
    CHECK(rx.total_duplicates() >= 3000);        // receiver deduplicated +1.5 ms backup copies instantly
    CHECK(und == 0);                             // 0 audio dropouts
    CHECK(big_steps == 0);                       // 0 waveform glitches/clicks
    CHECK(max_zero_run < 8);                     // 0 silence holes
    CHECK(resyncs == 0);                         // deterministic playout stayed locked without jumps
    CHECK(mat.fixed_cushion_ms() == 0.0);        // never expanded buffer cushion
    CHECK(lat_min >= 5.0 && lat_max <= 10.5);    // locked steadily around 9 ms target (5..10 ms)
    CHECK(std::fabs(mat.resample_ppm()) >= 30.0 && std::fabs(mat.resample_ppm()) <= 1000.1);  // active +/-0.1% trim!
}

// Dante-inspired Multi-Channel UDP Flows (kMonitorAudioFlow), Receiver-Driven Subscriptions
// (active_channel_mask in 15 ms LVKA), Differentiated DSCP QoS (CS7/EF/AF41), and Tally/Intercom 1+1 Dedup.
TEST(monitor_dante_multichannel_flow_subscriptions_and_dscp_qos) {
    // 1. Differentiated DSCP QoS verification on UdpSocket
    {
        lv::UdpSocket s;
        REQUIRE(s.open());
        REQUIRE(s.bind("127.0.0.1", 0));
        s.set_qos_class(lv::QosTrafficClass::kVideo);
        CHECK(s.tos() == lv::UdpSocket::kDscpAf41Tos);
        s.set_qos_class(lv::QosTrafficClass::kSyncControl);
        // Elevated/POSIX: CS7 (0xE0) or CS6 (0xC0); non-admin Windows falls back cleanly to EF (0xB8, WMM AC_VO).
        CHECK(s.tos() == lv::UdpSocket::kDscpCs7Tos || s.tos() == lv::UdpSocket::kDscpCs6Tos ||
              s.tos() == lv::UdpSocket::kDscpEfTos);
        s.set_qos_class(lv::QosTrafficClass::kAudio);
        CHECK(s.tos() == lv::UdpSocket::kDscpEfTos);
        s.set_qos_class(lv::QosTrafficClass::kVideo);
        CHECK(s.tos() == lv::UdpSocket::kDscpAf41Tos);
        s.close();
    }

    // 2. Multi-Channel UDP Flow (8ch in ONE compact packet) + 1+1 Wi-Fi Redundancy + Rx Subscriptions
    constexpr int kCh = 8;
    constexpr uint32_t kBlk = 240;
    lv::PersonalMonitorServer srv(48000);
    srv.set_multichannel_flow(true, kCh);
    srv.set_wifi_redundancy(true, 1500);
    REQUIRE(srv.start(0, "127.0.0.1", 0));

    std::atomic<bool> stop{false};
    std::thread mixer([&] {
        std::vector<std::vector<float>> pl(kCh, std::vector<float>(kBlk));
        const float* pp[kCh];
        for (int c = 0; c < kCh; ++c) pp[c] = pl[c].data();
        uint64_t pos = 0;
        uint64_t next = lv::now_us();
        while (!stop.load(std::memory_order_relaxed)) {
            srv.poll_control();
            for (int c = 0; c < kCh; ++c) {
                for (uint32_t i = 0; i < kBlk; ++i) {
                    pl[c][i] = 0.4f * float(std::sin(2.0 * 3.141592653589793 * (220.0 + 55.0 * c) *
                                                     double(pos + i) / 48000.0));
                }
            }
            srv.process_and_send(pp, kCh, kBlk);
            pos += kBlk;
            next += 5000;
            lv::sleep_until_us(next, [] {});
        }
    });

    lv::PersonalMonitorClient cli(48000);
    REQUIRE(cli.connect("127.0.0.1", srv.bound_port(), "Dante-Flow-Phone", 256, 8));

    // Phase A: receive all 8 channels packed into single kMonitorAudioFlow UDP datagrams + 1+1 duplicates
    auto poll_for = [&](int ms) {
        const uint64_t until = lv::now_us() + uint64_t(ms) * 1000ull;
        while (lv::now_us() < until) {
            cli.poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };
    poll_for(280);
    const uint16_t mask_all = cli.last_rx_channel_mask();
    const uint64_t flow_rx_a = cli.flow_packets_rx();
    const uint64_t dup_drops_a = cli.flow_duplicates_dropped();
    const uint64_t dup_tx_a = srv.dup_packets_tx();

    // Verify planar multi-channel audio pop receives non-silent data on channel 5
    std::vector<std::vector<float>> out_pl(kCh, std::vector<float>(240, 0.0f));
    float* out_ptrs[kCh];
    for (int c = 0; c < kCh; ++c) out_ptrs[c] = out_pl[c].data();
    cli.pop_multichannel(out_ptrs, kCh, 240);
    float peak_ch5 = 0.0f;
    for (float s : out_pl[5]) peak_ch5 = std::max(peak_ch5, std::fabs(s));

    // Phase B: Receiver-Driven Subscription — Solo channel 2 only (bit 2 = 0x0004)
    REQUIRE(cli.set_channel_mix(2, lv::PersonalMonitorChannelMix{0.0f, 0.0f, /*mute=*/false, /*solo=*/true}));
    poll_for(140);
    const uint16_t mask_solo = cli.last_rx_channel_mask();
    const auto clist = srv.clients();
    const uint64_t srv_mask_solo = clist.empty() ? ~0ull : clist[0].active_channel_mask;

    // Phase C: Mute all channels (active_channel_mask == 0) -> server suppresses Wi-Fi TX completely!
    cli.set_active_channel_mask(0);
    poll_for(140);
    const uint64_t suppressed = srv.muted_packets_suppressed();

    stop.store(true, std::memory_order_relaxed);
    mixer.join();
    cli.disconnect();
    srv.stop();

    // 3. Tally & Intercom 1+1 Duplicate (+1.5 ms) + instant seq dedup verification
    lv::TallyIntercomEngine studio_ti, cam_ti;
    studio_ti.set_wifi_redundancy(true, 1500);
    cam_ti.set_wifi_redundancy(true, 1500);
    REQUIRE(studio_ti.start_studio(0, "127.0.0.1"));
    REQUIRE(cam_ti.start_camera("127.0.0.1", studio_ti.bound_port(), 1, "CAM-1"));
    for (int i = 0; i < 15; ++i) {
        cam_ti.poll();
        studio_ti.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    studio_ti.studio_set_camera_tally(1, lv::kTallyProgram, "CAM-1", true);
    studio_ti.studio_set_camera_intercom(1, true, true, true);
    studio_ti.set_intercom_state(true, false, 1.0f, 0.0f);
    std::vector<float> voice(lv::kIntercomFrameSamples, 0.25f);
    for (int i = 0; i < 12; ++i) {
        studio_ti.push_mic_pcm(voice.data(), lv::kIntercomFrameSamples);
        for (int k = 0; k < 5; ++k) {
            studio_ti.poll();
            cam_ti.poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    const uint64_t ti_tx = studio_ti.intercom_tx_packets();
    const uint64_t ti_dup_drop = cam_ti.intercom_duplicates_dropped();
    cam_ti.stop();
    studio_ti.stop();

    std::printf("    [dante flow+sub+qos] 8ch_mask=0x%04X solo_mask=0x%04X (srv=0x%llX) | "
                "flow_rx=%llu dup_tx=%llu dup_drop=%llu suppressed=%llu ch5_pk=%.3f | "
                "intercom tx=%llu dup_drop=%llu\n",
                unsigned(mask_all), unsigned(mask_solo), (unsigned long long)srv_mask_solo,
                (unsigned long long)flow_rx_a, (unsigned long long)dup_tx_a, (unsigned long long)dup_drops_a,
                (unsigned long long)suppressed, peak_ch5,
                (unsigned long long)ti_tx, (unsigned long long)ti_dup_drop);

    CHECK(flow_rx_a >= 15);
    CHECK(mask_all == 0x00FFu);              // all 8 channels packed into ONE UDP packet
    CHECK(dup_tx_a >= 15);                   // 1+1 duplicates transmitted
    CHECK(dup_drops_a >= 10);                // instant seq deduplication on client
    CHECK(cli.nacks_sent() == 0);            // zero NACKs
    CHECK(peak_ch5 > 0.05f);                 // decoded multi-channel audio is live
    CHECK(mask_solo == 0x0004u);             // Solo ch2 -> ONLY ch2 transmitted over Wi-Fi!
    CHECK(srv_mask_solo == 0x0004ull);       // server received updated mask via 15 ms LVKA
    CHECK(suppressed >= 5);                  // active_channel_mask == 0 suppresses TX
    CHECK(ti_tx >= 10);                      // Tally/Intercom frames transmitted
    CHECK(ti_dup_drop >= 5);                 // Intercom +1.5 ms duplicate instant seq dedup active
}

// Explicit Dante Multi-Channel Flow & C ABI Rx Subscription test:
// Packs 8 active channels into ONE kMonitorAudioFlow UDP frame and verifies immediate exclusion
// of muted channels via lv_monitor_client_set_subscribed_mask / Mute & Solo.
TEST(monitor_dante_multichannel_flow_and_subscriptions) {
    constexpr int kCh = 8;
    constexpr uint32_t kBlk = 240;
    LvMonitorServerHandle* srv = lv_monitor_server_create(48000);
    REQUIRE(srv);
    REQUIRE(lv_monitor_server_set_multichannel_flow(srv, 1, kCh) == LV_OK);
    REQUIRE(lv_monitor_server_start(srv, 0) == LV_OK);
    const uint16_t port = lv_monitor_server_port(srv);
    REQUIRE(port != 0);

    std::atomic<bool> stop{false};
    std::thread pc([&] {
        std::vector<std::vector<float>> pl(kCh, std::vector<float>(kBlk));
        const float* pp[kCh];
        for (int c = 0; c < kCh; ++c) pp[c] = pl[c].data();
        uint64_t pos = 0;
        uint64_t next = lv::now_us();
        while (!stop.load(std::memory_order_relaxed)) {
            for (int c = 0; c < kCh; ++c) {
                for (uint32_t i = 0; i < kBlk; ++i) {
                    pl[c][i] = 0.35f * float(std::sin(2.0 * 3.141592653589793 * (200.0 + 40.0 * c) *
                                                      double(pos + i) / 48000.0));
                }
            }
            lv_monitor_server_push_audio_planar(srv, pp, kCh, kBlk);
            pos += kBlk;
            next += 5000;
            lv::sleep_until_us(next, [] {});
        }
    });

    LvMonitorClientHandle* cli = lv_monitor_client_create(48000);
    REQUIRE(cli);
    REQUIRE(lv_monitor_client_connect(cli, "127.0.0.1", port, "DanteSub8", 256, 10) == LV_OK);

    auto poll_ms = [&](int ms) {
        const uint64_t until = lv::now_us() + uint64_t(ms) * 1000ull;
        while (lv::now_us() < until) {
            lv_monitor_client_poll(cli);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };

    // 1. All 8 channels active -> packed into ONE kMonitorAudioFlow packet
    poll_ms(220);
    LV_MonitorDanteTelemetry dt_all{};
    REQUIRE(lv_monitor_client_get_dante_telemetry(cli, &dt_all) == LV_OK);

    // 2. Subscribe ONLY to 3 channels (Ch0, Ch3, Ch7 -> mask 0x89) via C ABI
    REQUIRE(lv_monitor_client_set_subscribed_mask(cli, 0x0089ull) == LV_OK);
    poll_ms(140);
    LV_MonitorDanteTelemetry dt_sub3{};
    REQUIRE(lv_monitor_client_get_dante_telemetry(cli, &dt_sub3) == LV_OK);

    // 3. Mute all channels (mask = 0) -> flow packet RX pauses completely
    REQUIRE(lv_monitor_client_set_subscribed_mask(cli, 0ull) == LV_OK);
    poll_ms(60);
    LV_MonitorDanteTelemetry dt_m0{};
    REQUIRE(lv_monitor_client_get_dante_telemetry(cli, &dt_m0) == LV_OK);
    poll_ms(140);
    LV_MonitorDanteTelemetry dt_m1{};
    REQUIRE(lv_monitor_client_get_dante_telemetry(cli, &dt_m1) == LV_OK);

    stop.store(true, std::memory_order_relaxed);
    pc.join();
    lv_monitor_client_disconnect(cli);
    lv_monitor_client_destroy(cli);
    lv_monitor_server_stop(srv);
    lv_monitor_server_destroy(srv);

    std::printf("    [dante 8ch flow+sub C ABI] all_ch=%u (pkts=%llu lock=%d) -> sub3_ch=%u -> mute_delta=%llu\n",
                unsigned(dt_all.active_flow_channels), (unsigned long long)dt_all.flow_packets_rx,
                int(dt_all.clock_locked), unsigned(dt_sub3.active_flow_channels),
                (unsigned long long)(dt_m1.flow_packets_rx - dt_m0.flow_packets_rx));

    CHECK(dt_all.flow_packets_rx >= 15);
    CHECK(dt_all.active_flow_channels == 8u);
    CHECK(dt_all.clock_locked == 1);
    CHECK(dt_sub3.active_flow_channels == 3u);
    CHECK((dt_m1.flow_packets_rx - dt_m0.flow_packets_rx) == 0u);  // 0 Wi-Fi packets when all muted
}

// End-to-end Dante 1+1 Wi-Fi Redundancy test through real UDP sockets + C ABI:
// Client negotiates 1+1 Redundancy via lv_monitor_client_set_wifi_redundancy(cli, 1) at 10 ms target;
// a localhost UDP proxy randomly drops 5% of primary kMonitorAudioFlow packets while passing +1.5 ms
// redundant copies. Verifies duplicates_recovered > 0, nacks_sent == 0, playout within 10 +/- 1 ms, 0 dropouts!
TEST(monitor_dante_1plus1_redundancy_zero_nack_recovery) {
    constexpr int kCh = 2;
    constexpr uint32_t kBlk = 240;
    constexpr uint16_t kTargetMs = 10;

    lv::PersonalMonitorServer srv(48000);
    // Note: server global redundancy stays OFF to prove client-initiated kMonitorCapRedundancy1Plus1 works!
    REQUIRE(srv.start(0, "127.0.0.1", 0));
    const uint16_t srv_port = srv.bound_port();
    lv::Endpoint srv_ep{};
    REQUIRE(lv::Endpoint::parse("127.0.0.1", srv_port, srv_ep));

    // Localhost UDP proxy that models 5% random loss of PRIMARY kMonitorAudioFlow packets
    lv::UdpSocket proxy;
    REQUIRE(proxy.open());
    REQUIRE(proxy.bind("127.0.0.1", 0));
    proxy.set_nonblocking(true);
    const uint16_t proxy_port = proxy.local_port();
    REQUIRE(proxy_port != 0);

    std::atomic<bool> stop{false};
    std::atomic<uint64_t> proxy_dropped_primary{0};
    std::thread proxy_thread([&] {
        lv::Endpoint client_ep{};
        bool have_client = false;
        uint8_t buf[4200];
        std::mt19937 rng(20261125);
        std::uniform_real_distribution<double> u01(0.0, 1.0);
        uint32_t seen_primary = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            bool any = false;
            for (int i = 0; i < 64; ++i) {
                lv::Endpoint src{};
                const int n = proxy.recv_from(buf, sizeof(buf), src);
                if (n <= 0) break;
                any = true;
                if (src == srv_ep) {
                    if (!have_client) continue;
                    // Inspect kMonitorAudioFlow primary vs redundant copy
                    lv::PacketHeader ph{};
                    const uint8_t* pl = nullptr;
                    bool drop = false;
                    if (lv::parse_packet(buf, size_t(n), ph, &pl) == lv::PacketParseStatus::kOk &&
                        ph.type == lv::PacketType::kMonitorAudioFlow &&
                        ph.payload_size >= lv::kMonitorAudioFlowHeaderSize) {
                        const bool is_dup = (pl[21] & lv::kFlagRedundantCopy) != 0;
                        if (!is_dup) {
                            ++seen_primary;
                            // Drop ~5% of primary packets after initial 20-packet lock-in
                            if (seen_primary > 20 && u01(rng) < 0.05) {
                                drop = true;
                                proxy_dropped_primary.fetch_add(1, std::memory_order_relaxed);
                            }
                        }
                    }
                    if (!drop) proxy.send_to(buf, size_t(n), client_ep);
                } else {
                    client_ep = src;
                    have_client = true;
                    proxy.send_to(buf, size_t(n), srv_ep);
                }
            }
            if (!any) std::this_thread::sleep_for(std::chrono::microseconds(250));
        }
    });

    std::thread pc([&] {
        std::vector<float> l(kBlk), r(kBlk);
        const float* pp[kCh] = {l.data(), r.data()};
        uint64_t pos = 0;
        uint64_t next = lv::now_us();
        while (!stop.load(std::memory_order_relaxed)) {
            for (uint32_t i = 0; i < kBlk; ++i) {
                const float v = 0.45f * float(std::sin(2.0 * 3.141592653589793 * 440.0 * double(pos + i) / 48000.0));
                l[i] = v;
                r[i] = v;
            }
            srv.process_and_send(pp, kCh, kBlk);
            pos += kBlk;
            next += 5000;
            lv::sleep_until_us(next, [] {});
        }
    });

    // Phone side through C ABI: enable Wi-Fi 1+1 Redundancy + connect at 10 ms target
    LvMonitorClientHandle* cli = lv_monitor_client_create(48000);
    REQUIRE(cli);
    REQUIRE(lv_monitor_client_set_wifi_redundancy(cli, 1) == LV_OK);
    REQUIRE(lv_monitor_client_connect(cli, "127.0.0.1", proxy_port, "Dante1Plus1", 512, kTargetMs) == LV_OK);

    // Run real-time poll + steady 48 kHz playback clock for 1.8 s
    std::vector<float> st(2 * 120);
    const uint64_t t0 = lv::now_us();
    const uint64_t t_warm = t0 + 400'000ull;
    const uint64_t t_end = t0 + 1'800'000ull;
    uint64_t popped = 0, nonsilent = 0, big_steps = 0;
    float prev = 0.0f;
    bool measuring = false;
    double play_min = 1e9, play_max = 0.0;
    uint64_t und_before = 0;

    while (lv::now_us() < t_end) {
        lv_monitor_client_poll(cli);
        const uint64_t now = lv::now_us();
        const uint64_t due = (now - t0) * 48000ull / 1'000'000ull;
        while (popped + 120 <= due) {
            lv_monitor_client_pop_stereo(cli, st.data(), 120);
            popped += 120;
            if (!measuring && now >= t_warm) {
                measuring = true;
                prev = st[2 * 119];
                LV_MonitorDanteTelemetry dt{};
                lv_monitor_client_get_dante_telemetry(cli, &dt);
                (void)dt;
            } else if (measuring) {
                bool has_sig = false;
                for (int i = 0; i < 120; ++i) {
                    const float v = st[size_t(2 * i)];
                    if (std::fabs(v) > 0.01f) has_sig = true;
                    if (std::fabs(v - prev) > 0.25f) ++big_steps;
                    prev = v;
                }
                if (has_sig) ++nonsilent;
                LV_MonitorDanteTelemetry dt{};
                if (lv_monitor_client_get_dante_telemetry(cli, &dt) == LV_OK && dt.effective_playout_ms > 0.0) {
                    play_min = std::min(play_min, dt.effective_playout_ms);
                    play_max = std::max(play_max, dt.effective_playout_ms);
                }
            }
        }
        lv::sleep_until_us(lv::now_us() + 500, [] {});
    }

    LV_MonitorDanteTelemetry dt{};
    REQUIRE(lv_monitor_client_get_dante_telemetry(cli, &dt) == LV_OK);
    (void)und_before;

    stop.store(true, std::memory_order_relaxed);
    pc.join();
    proxy_thread.join();
    proxy.close();
    lv_monitor_client_disconnect(cli);
    lv_monitor_client_destroy(cli);
    srv.stop();

    std::printf("    [dante 1+1 e2e C ABI] dropped_pri=%llu recovered=%llu nacks=%llu | "
                "lock=%d jitter=%u us | playout=%.2f..%.2f ms (target=%u ms) | nonsilent=%llu glitches=%llu\n",
                (unsigned long long)proxy_dropped_primary.load(), (unsigned long long)dt.duplicates_recovered,
                (unsigned long long)dt.nacks_sent, int(dt.clock_locked), unsigned(dt.clock_offset_jitter_us),
                play_min, play_max, unsigned(kTargetMs), (unsigned long long)nonsilent,
                (unsigned long long)big_steps);

    CHECK(dt.wifi_redundancy_enabled == 1);
    CHECK(dt.clock_locked == 1);
    CHECK(proxy_dropped_primary.load() >= 10);
    CHECK(dt.duplicates_recovered >= proxy_dropped_primary.load() - 2);  // every dropped primary rescued by +1.5 ms copy!
    CHECK(dt.nacks_sent == 0);                                           // ZERO NACK retransmissions!
    CHECK(big_steps == 0);                                               // ZERO dropouts / glitches!
    CHECK(nonsilent > 200);
    CHECK(play_min >= double(kTargetMs) - 1.5 && play_max <= double(kTargetMs) + 1.5);
}
