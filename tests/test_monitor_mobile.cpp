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
