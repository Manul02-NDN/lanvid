// Unit tests for LanVid Grid Layer 2 Ethernet Audio & Control Protocol (EtherType 0x88B6),
// 256-channel 24-bit audio transport, IEEE 802.1Q/802.1p VLAN QoS, L2 Broadcast Discovery,
// Clock Master Sync, Decentralized Peer-to-Peer Patchbay, Distributed DSP Server Offload +
// Hot Standby Failover Redundancy (< 10 ms), and Classic/Grid/Hybrid AppConfig persistence.
#include "mini_test.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "common/clock.hpp"
#include "grid/dsp_server.hpp"
#include "grid/grid_node.hpp"
#include "grid/l2_protocol.hpp"
#include "grid/l2_transport.hpp"
#include "gui/app_config.hpp"

using namespace lv::grid;

namespace {

constexpr float kTwoPi = 6.28318530717958647692f;

MacAddress make_mac(uint8_t last_octet) {
    return MacAddress{{{0x02, 0x4C, 0x56, 0x47, 0x00, last_octet}}};  // locally-administered "LVG"
}

float compute_rms(const std::vector<float>& v) {
    if (v.empty()) return 0.0f;
    double sum = 0.0;
    for (float x : v) sum += double(x) * double(x);
    return float(std::sqrt(sum / double(v.size())));
}

}  // namespace

// =============================================================================
// 1. IEEE 802.1Q/802.1p VLAN QoS Frame Codec + Full 256-Channel 24-bit Roundtrip
// =============================================================================
TEST(grid_l2_frame_vlan_qos_and_256ch_audio_roundtrip) {
    // 1a. Verify MacAddress parse/format & VLAN QoS (802.1Q / 802.1p PCP=6) serialization
    MacAddress parsed_mac{};
    REQUIRE(MacAddress::parse("02:4C:56:47:00:01", parsed_mac));
    CHECK(parsed_mac == make_mac(0x01));
    CHECK(parsed_mac.to_string() == "02:4C:56:47:00:01");

    const MacAddress mac_a = make_mac(0x01);
    const MacAddress mac_b = make_mac(0x02);
    VlanQosTag vlan{true, 10, 6};  // VLAN 10, PCP=6 (Voice/Audio)

    LvL2Header hdr{};
    hdr.type = L2PacketType::kAudio;
    hdr.stream_id = 7;
    hdr.seq = 42;
    hdr.channel_start = 128;
    hdr.channel_count = 16;
    hdr.samples_per_ch = 16;
    hdr.audio_fmt = L2AudioFormat::kPcm24Packed;
    hdr.flags = 0x01;
    hdr.media_ts_lo = 0x12345678u;

    const uint8_t sample_payload[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t wire[256] = {};
    const size_t wire_len = write_l2_frame(wire, sizeof(wire), mac_b, mac_a, vlan, hdr, sample_payload, sizeof(sample_payload));
    REQUIRE(wire_len >= kEthHeaderSize + kVlanTagSize + kL2HeaderSize + sizeof(sample_payload));

    ParsedL2Frame pf{};
    REQUIRE(parse_l2_frame(wire, wire_len, pf));
    CHECK(pf.dst == mac_b);
    CHECK(pf.src == mac_a);
    CHECK(pf.vlan.enabled == true);
    CHECK(pf.vlan.vlan_id == 10);
    CHECK(pf.vlan.pcp == 6);
    CHECK(pf.hdr.type == L2PacketType::kAudio);
    CHECK(pf.hdr.stream_id == 7);
    CHECK(pf.hdr.seq == 42);
    CHECK(pf.hdr.channel_start == 128);
    CHECK(pf.hdr.channel_count == 16);
    CHECK(pf.hdr.samples_per_ch == 16);
    CHECK(pf.hdr.media_ts_lo == 0x12345678u);
    REQUIRE(pf.payload_len == sizeof(sample_payload));
    CHECK(std::memcmp(pf.payload, sample_payload, sizeof(sample_payload)) == 0);

    // 1b. Stream full 256 channels x 16 samples (0.33 ms @ 48 kHz) of 24-bit PCM over L2LoopbackSwitch
    constexpr uint16_t kChannels = kGridMaxChannels;  // 256
    constexpr uint16_t kSamples = 16;                 // 0.333 ms block

    auto sw = std::make_shared<L2LoopbackSwitch>();
    GridNodeConfig cfg_a{};
    cfg_a.node_name = "Stagebox-256";
    cfg_a.role = GridNodeRole::kStageboxIo;
    cfg_a.max_channels = kChannels;
    cfg_a.audio_fmt = L2AudioFormat::kPcm24Packed;
    cfg_a.vlan = vlan;

    GridNodeConfig cfg_b{};
    cfg_b.node_name = "Studio-DAW-256";
    cfg_b.role = GridNodeRole::kDawDriver;
    cfg_b.max_channels = kChannels;
    cfg_b.audio_fmt = L2AudioFormat::kPcm24Packed;
    cfg_b.vlan = vlan;

    GridNode node_a(create_loopback_l2_transport(sw, mac_a), cfg_a);
    GridNode node_b(create_loopback_l2_transport(sw, mac_b), cfg_b);
    REQUIRE(node_a.is_open() && node_b.is_open());

    std::vector<std::vector<float>> tx_planes(kChannels, std::vector<float>(kSamples, 0.0f));
    std::vector<const float*> tx_ptrs(kChannels, nullptr);
    for (uint16_t c = 0; c < kChannels; ++c) {
        const float freq_hz = 220.0f + float(c) * 17.0f;
        const float amp = 0.25f + 0.5f * (float(c % 16) / 15.0f);
        for (uint16_t s = 0; s < kSamples; ++s) {
            tx_planes[c][s] = amp * std::sin(kTwoPi * freq_hz * float(s) / 48000.0f + float(c) * 0.13f);
        }
        tx_ptrs[c] = tx_planes[c].data();
    }

    std::vector<std::vector<float>> rx_planes(kChannels, std::vector<float>(kSamples, 0.0f));
    std::vector<bool> ch_received(kChannels, false);
    uint32_t rx_ts = 0;
    node_b.set_audio_rx_callback([&](const MacAddress& src, uint16_t stream_id, uint16_t ch_start,
                                     uint16_t ch_count, uint16_t samples_per_ch, uint32_t media_ts_lo,
                                     const float* const* planes, uint8_t) {
        CHECK(src == mac_a);
        CHECK(stream_id == 101);
        CHECK(samples_per_ch == kSamples);
        rx_ts = media_ts_lo;
        for (uint16_t i = 0; i < ch_count; ++i) {
            const uint16_t ch = uint16_t(ch_start + i);
            if (ch < kChannels && planes[ch]) {
                std::memcpy(rx_planes[ch].data(), planes[ch], size_t(samples_per_ch) * sizeof(float));
                ch_received[ch] = true;
            }
        }
    });

    const size_t frames_sent =
        node_a.send_audio_direct(mac_b, 101, tx_ptrs.data(), 0, 0, kChannels, kSamples, 96000u);
    CHECK(frames_sent >= 9);  // 256ch * 16s * 3B = 12288B -> MTU-fragmented into ~9-10 IEEE 802.3 frames

    const int polled = node_b.poll(256);
    CHECK(polled == int(frames_sent));
    CHECK(rx_ts == 96000u);

    int ok_channels = 0;
    float max_err = 0.0f;
    for (uint16_t c = 0; c < kChannels; ++c) {
        if (!ch_received[c]) continue;
        ++ok_channels;
        for (uint16_t s = 0; s < kSamples; ++s) {
            const float err = std::fabs(tx_planes[c][s] - rx_planes[c][s]);
            if (err > max_err) max_err = err;
        }
    }
    std::printf("    [grid L2 256ch] sent %zu Ethernet frames (VLAN 10, PCP=6) -> %d/256 channels OK | max_err=%.2e\n",
                frames_sent, ok_channels, double(max_err));
    CHECK(ok_channels == kChannels);
    CHECK(max_err < 1e-4f);
}

// =============================================================================
// 2. Zero-IP L2 Broadcast Discovery, Clock Master Sync & Decentralized P2P Routing
// =============================================================================
TEST(grid_l2_discovery_clock_master_and_p2p_routing) {
    auto sw = std::make_shared<L2LoopbackSwitch>();

    const MacAddress mac_hub = make_mac(0x10);
    const MacAddress mac_stage = make_mac(0x11);
    const MacAddress mac_daw = make_mac(0x12);

    GridNodeConfig hub_cfg{};
    hub_cfg.node_name = "LanVid Studio Hub";
    hub_cfg.role = GridNodeRole::kStudioHost;
    hub_cfg.clock_master = true;
    hub_cfg.clock_priority = 10;

    GridNodeConfig stage_cfg{};
    stage_cfg.node_name = "Stagebox-A";
    stage_cfg.role = GridNodeRole::kStageboxIo;
    stage_cfg.clock_master = false;

    GridNodeConfig daw_cfg{};
    daw_cfg.node_name = "DAW-Workstation-B";
    daw_cfg.role = GridNodeRole::kDawDriver;
    daw_cfg.clock_master = false;

    GridNode hub(create_loopback_l2_transport(sw, mac_hub), hub_cfg);
    GridNode stagebox(create_loopback_l2_transport(sw, mac_stage), stage_cfg);
    GridNode daw(create_loopback_l2_transport(sw, mac_daw), daw_cfg);

    // 2a. Zero-IP Layer 2 Broadcast Discovery
    REQUIRE(hub.broadcast_discovery_probe());
    stagebox.poll(32);
    daw.poll(32);
    hub.poll(32);

    const auto peers = hub.discovered_nodes();
    bool saw_stage = false, saw_daw = false;
    for (const auto& p : peers) {
        if (p.mac == mac_stage && p.name == "Stagebox-A") saw_stage = true;
        if (p.mac == mac_daw && p.name == "DAW-Workstation-B") saw_daw = true;
    }
    CHECK(saw_stage);
    CHECK(saw_daw);

    // 2b. Clock Master deterministic sync pulses
    REQUIRE(hub.emit_clock_sync(48000ull));
    stagebox.poll(32);
    daw.poll(32);
    REQUIRE(hub.emit_clock_sync(48048ull));
    stagebox.poll(32);
    daw.poll(32);

    const auto st_clk = stagebox.clock_status();
    const auto daw_clk = daw.clock_status();
    CHECK(st_clk.locked == true);
    CHECK(st_clk.master_mac == mac_hub);
    CHECK(st_clk.master_sample_pos == 48048ull);
    CHECK(daw_clk.locked == true);
    CHECK(daw_clk.master_mac == mac_hub);
    CHECK(daw_clk.sync_rx_count == 2);

    // 2c. Decentralized Peer-to-Peer Patchbay:
    // Hub instructs Stagebox-A to send 32 channels directly to DAW-B's MAC address.
    // Audio traffic MUST bypass Hub completely (hub.audio_rx_frames() == 0)!
    L2RouteRulePayload rule{};
    rule.op = 1;  // add/update
    rule.route_id = 1;
    rule.dst_mac = mac_daw;
    rule.src_ch_start = 0;
    rule.dst_ch_start = 16;  // patch Stagebox ch 0..31 -> DAW ch 16..47
    rule.channel_count = 32;
    rule.stream_id = 5;
    rule.gain_cb = 0;  // 0.00 dB unity

    REQUIRE(hub.send_route_rule(mac_stage, rule));
    stagebox.poll(32);
    REQUIRE(stagebox.active_tx_routes().size() == 1);

    constexpr uint16_t kBlock = 32;
    std::vector<std::vector<float>> stage_buf(32, std::vector<float>(kBlock, 0.0f));
    std::vector<const float*> stage_ptrs(32, nullptr);
    for (uint16_t c = 0; c < 32; ++c) {
        for (uint16_t s = 0; s < kBlock; ++s) {
            stage_buf[c][s] = 0.5f * std::sin(kTwoPi * (300.0f + float(c) * 40.0f) * float(s) / 48000.0f);
        }
        stage_ptrs[c] = stage_buf[c].data();
    }

    int daw_channels_rx = 0;
    float daw_max_err = 0.0f;
    daw.set_audio_rx_callback([&](const MacAddress& src, uint16_t stream_id, uint16_t ch_start,
                                  uint16_t ch_count, uint16_t samples_per_ch, uint32_t,
                                  const float* const* planes, uint8_t) {
        CHECK(src == mac_stage);  // came directly from Stagebox, NOT Hub!
        CHECK(stream_id == 5);
        CHECK(samples_per_ch == kBlock);
        for (uint16_t i = 0; i < ch_count; ++i) {
            const uint16_t dst_ch = uint16_t(ch_start + i);
            REQUIRE(dst_ch >= 16 && dst_ch < 48);
            const uint16_t src_ch = uint16_t(dst_ch - 16);
            ++daw_channels_rx;
            for (uint16_t s = 0; s < kBlock; ++s) {
                daw_max_err = std::max(daw_max_err, std::fabs(planes[dst_ch][s] - stage_buf[src_ch][s]));
            }
        }
    });

    const size_t p2p_frames = stagebox.push_routed_audio(stage_ptrs.data(), 32, kBlock, 48048u);
    CHECK(p2p_frames > 0);
    daw.poll(64);
    hub.poll(64);

    std::printf("    [grid P2P] peers=%zu | clock locked=%d | DAW rx=%d/32 ch (err=%.2e) | Hub audio_rx=%llu\n",
                peers.size(), int(daw_clk.locked), daw_channels_rx, double(daw_max_err),
                (unsigned long long)hub.audio_rx_frames());
    CHECK(daw_channels_rx == 32);
    CHECK(daw_max_err < 1e-4f);
    CHECK(hub.audio_rx_frames() == 0);  // Decentralized P2P: zero audio passed through Hub!
}

// =============================================================================
// 3. Distributed DSP Server Offload + Hot Standby Failover Redundancy (< 10 ms)
// =============================================================================
TEST(grid_dsp_server_offload_and_hot_failover_redundancy) {
    auto sw = std::make_shared<L2LoopbackSwitch>();

    const MacAddress mac_host = make_mac(0x20);
    const MacAddress mac_dsp_pri = make_mac(0x21);
    const MacAddress mac_dsp_bak = make_mac(0x22);

    GridNodeConfig host_cfg{};
    host_cfg.node_name = "Studio-Host";
    host_cfg.role = GridNodeRole::kStudioHost;
    host_cfg.clock_master = true;

    GridNode host(create_loopback_l2_transport(sw, mac_host), host_cfg);
    GridDspServer dsp_primary(create_loopback_l2_transport(sw, mac_dsp_pri), "DSP-Primary", false, 48000);
    GridDspServer dsp_backup(create_loopback_l2_transport(sw, mac_dsp_bak), "DSP-Backup", true, 48000);

    // 6 ms failover timeout (< 10 ms live redundancy budget)
    GridFailoverController failover(host, mac_dsp_pri, mac_dsp_bak, 6000);

    uint64_t sim_now_us = lv::now_us();
    int accepted_from_primary = 0;
    int accepted_from_backup = 0;
    std::vector<float> last_wet_ch0(32, 0.0f);

    host.set_audio_rx_callback([&](const MacAddress& src, uint16_t, uint16_t ch_start, uint16_t ch_count,
                                   uint16_t samples_per_ch, uint32_t, const float* const* planes, uint8_t flags) {
        if ((flags & 0x02u) == 0) return;  // only DSP returns
        if (failover.on_processed_return(src, sim_now_us)) {
            if (src == mac_dsp_pri) ++accepted_from_primary;
            if (src == mac_dsp_bak) ++accepted_from_backup;
            if (ch_start == 0 && ch_count >= 1 && planes[0]) {
                last_wet_ch0.assign(planes[0], planes[0] + samples_per_ch);
            }
        }
    });
    host.set_heartbeat_callback([&](const MacAddress& src, const L2HeartbeatPayload& hb) {
        failover.on_heartbeat(src, hb, sim_now_us);
    });

    // Mirror DSP plugin params to BOTH Primary and Backup servers: -6.02 dB output gain (half amplitude) + HPF
    L2DspParamPayload p{};
    p.channel = 0xFFFFu;        // apply to all channels
    p.flags = 0x02u | 0x04u;    // HPF on + EQ flat on
    p.hpf_hz = 40.0f;
    p.output_gain_db = -6.0206f;  // ~0.5x amplitude
    p.revision = 1;
    failover.send_param_mirrored(p);
    dsp_primary.service(32);
    dsp_backup.service(32);

    // Dry 1 kHz sine wave (0.6 peak -> ~0.3 peak after -6 dB DSP return)
    constexpr uint16_t kCh = 8;
    constexpr uint16_t kBlk = 32;
    std::vector<std::vector<float>> dry(kCh, std::vector<float>(kBlk, 0.0f));
    std::vector<const float*> dry_ptrs(kCh, nullptr);
    for (uint16_t c = 0; c < kCh; ++c) dry_ptrs[c] = dry[c].data();

    auto fill_sine = [&](uint32_t sample_offset) {
        for (uint16_t s = 0; s < kBlk; ++s) {
            const float v = 0.6f * std::sin(kTwoPi * 1000.0f * float(sample_offset + s) / 48000.0f);
            for (uint16_t c = 0; c < kCh; ++c) dry[c][s] = v;
        }
    };

    // Phase 1: Normal operation — both servers process, Host uses Primary exclusively
    for (int blk = 0; blk < 6; ++blk) {
        fill_sine(uint32_t(blk * kBlk));
        failover.send_dry_mirrored(1, dry_ptrs.data(), 0, kCh, kBlk, uint32_t(blk * kBlk));
        dsp_primary.service(64);
        dsp_backup.service(64);
        dsp_primary.emit_heartbeat();
        dsp_backup.emit_heartbeat();
        host.poll(64);
        sim_now_us += 667;  // 0.667 ms per 32-sample block @ 48 kHz
    }

    const float dry_rms = compute_rms(dry[0]);
    const float pri_wet_rms = compute_rms(last_wet_ch0);
    const auto t_before = failover.telemetry();
    CHECK(!t_before.using_backup);
    CHECK(t_before.failover_count == 0);
    CHECK(accepted_from_primary == 6);
    CHECK(accepted_from_backup == 0);
    CHECK(pri_wet_rms > dry_rms * 0.42f && pri_wet_rms < dry_rms * 0.58f);  // -6 dB DSP applied

    // Phase 2: Simulate sudden Primary server cable pull / crash!
    sw->set_port_link(mac_dsp_pri, false);
    sim_now_us += 8000;  // 8 ms elapsed (> 6 ms failover timeout)

    for (int blk = 6; blk < 10; ++blk) {
        fill_sine(uint32_t(blk * kBlk));
        failover.send_dry_mirrored(1, dry_ptrs.data(), 0, kCh, kBlk, uint32_t(blk * kBlk));
        dsp_primary.service(64);  // link down -> drops everything
        dsp_backup.service(64);   // hot standby continues seamlessly!
        host.poll(64);
        sim_now_us += 667;
    }

    const float bak_wet_rms = compute_rms(last_wet_ch0);
    const auto t_after = failover.telemetry();
    std::printf("    [grid DSP failover] dry_rms=%.3f | primary_wet=%.3f (%d blk) -> LINK DOWN -> backup_wet=%.3f (%d blk) | failovers=%u\n",
                double(dry_rms), double(pri_wet_rms), accepted_from_primary, double(bak_wet_rms),
                accepted_from_backup, t_after.failover_count);

    CHECK(t_after.using_backup == true);
    CHECK(t_after.failover_count == 1);
    CHECK(accepted_from_backup == 4);
    CHECK(bak_wet_rms > dry_rms * 0.42f && bak_wet_rms < dry_rms * 0.58f);  // identical mirrored DSP state!
}

// =============================================================================
// 4. Dual-Engine Mode Switch (Classic UDP <-> Grid L2 <-> Hybrid) Persistence
// =============================================================================
TEST(grid_engine_mode_switch_preserves_classic_config) {
    namespace fs = std::filesystem;
    const std::string cfg_path = lv::gui::config_file_path();

    // Backup existing user config.json so this test never disturbs real settings
    bool had_existing = false;
    std::string backup_contents;
    if (fs::exists(cfg_path)) {
        had_existing = true;
        std::ifstream in(cfg_path, std::ios::binary);
        backup_contents.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }

    auto restore_user_config = [&]() {
        if (had_existing) {
            std::ofstream out(cfg_path, std::ios::binary | std::ios::trunc);
            out.write(backup_contents.data(), std::streamsize(backup_contents.size()));
        } else if (fs::exists(cfg_path)) {
            std::error_code ec;
            fs::remove(cfg_path, ec);
        }
    };

    // 4a. Default config starts in Classic UDP mode (0) for 100% backward compatibility
    lv::gui::AppConfig def{};
    CHECK(def.network_engine_mode == 0);
    CHECK(def.grid_channels == 256);
    CHECK(def.grid_vlan_pcp == 6);

    // 4b. Save Classic + Grid L2 settings (mode = 1) and reload
    lv::gui::AppConfig saved{};
    saved.dest_ip = "192.168.50.109";
    saved.dest_port = 5004;
    saved.aoip_channels = 64;
    saved.aoip_latency_ms = 20;
    saved.network_engine_mode = 1;  // Grid L2
    saved.grid_nic_name = "Ethernet-Test-NIC";
    saved.grid_vlan_enabled = true;
    saved.grid_vlan_id = 42;
    saved.grid_vlan_pcp = 6;
    saved.grid_channels = 256;
    saved.grid_clock_master = true;
    REQUIRE(lv::gui::save_app_config(saved));

    lv::gui::AppConfig loaded{};
    REQUIRE(lv::gui::load_app_config(loaded));
    CHECK(loaded.network_engine_mode == 1);
    CHECK(loaded.grid_nic_name == "Ethernet-Test-NIC");
    CHECK(loaded.grid_vlan_enabled == true);
    CHECK(loaded.grid_vlan_id == 42);
    CHECK(loaded.grid_vlan_pcp == 6);
    CHECK(loaded.grid_channels == 256);
    CHECK(loaded.grid_clock_master == true);
    // Classic UDP fields must be 100% preserved!
    CHECK(loaded.dest_ip == "192.168.50.109");
    CHECK(loaded.dest_port == 5004);
    CHECK(loaded.aoip_channels == 64);
    CHECK(loaded.aoip_latency_ms == 20);

    // 4c. Switch to Hybrid (mode = 2) and then back to Classic (mode = 0)
    loaded.network_engine_mode = 2;
    REQUIRE(lv::gui::save_app_config(loaded));
    lv::gui::AppConfig hybrid{};
    REQUIRE(lv::gui::load_app_config(hybrid));
    CHECK(hybrid.network_engine_mode == 2);
    CHECK(hybrid.grid_channels == 256);
    CHECK(hybrid.dest_ip == "192.168.50.109");

    hybrid.network_engine_mode = 0;
    REQUIRE(lv::gui::save_app_config(hybrid));
    lv::gui::AppConfig back_to_classic{};
    REQUIRE(lv::gui::load_app_config(back_to_classic));
    CHECK(back_to_classic.network_engine_mode == 0);
    CHECK(back_to_classic.grid_nic_name == "Ethernet-Test-NIC");
    CHECK(back_to_classic.aoip_channels == 64);

    restore_user_config();
}
