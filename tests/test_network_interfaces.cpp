// Cross-platform Network Interface Enumeration (Ethernet / Wi-Fi / USB-LAN) +
// Physical Dual-NIC 1+1 Failover (0 ms interruption when Primary NIC drops).
#include "mini_test.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "audio/audio_rx.hpp"
#include "audio/audio_sync_matrix.hpp"
#include "audio/audio_tx.hpp"
#include "audio/master_clock.hpp"
#include "audio/personal_monitor_bus.hpp"
#include "common/clock.hpp"
#include "common/rt_thread.hpp"
#include "lanvid/lanvid_sdk.h"
#include "net/network_interfaces.hpp"
#include "net/packet.hpp"
#include "net/sender.hpp"
#include "net/udp_socket.hpp"

TEST(network_interfaces_enumerate_ethernet_and_wifi) {
    const auto active = lv::enumerate_network_interfaces(false, false);
    const auto all = lv::enumerate_network_interfaces(true, true);

    std::printf("    [nic scan] active=%zu, all=%zu\n", active.size(), all.size());
    for (const auto& nic : active) {
        std::printf("      [%s] %s (%s) IP=%s mask=%s bcast=%s mac=%s %u Mbps idx=%u\n",
                    lv::network_interface_type_tag(nic.type),
                    nic.friendly_name.c_str(),
                    nic.system_name.c_str(),
                    nic.ipv4_address.c_str(),
                    nic.subnet_mask.c_str(),
                    nic.broadcast_address.c_str(),
                    nic.mac_address.c_str(),
                    nic.link_speed_mbps,
                    nic.if_index);
        CHECK(!nic.id.empty());
        CHECK(!nic.friendly_name.empty());
        CHECK(!nic.ipv4_address.empty());
        CHECK(nic.ipv4_address != "127.0.0.1");
        CHECK(nic.is_up);
        CHECK(nic.type == lv::NetworkInterfaceType::kEthernet ||
              nic.type == lv::NetworkInterfaceType::kWiFi ||
              nic.type == lv::NetworkInterfaceType::kUsbEthernet ||
              nic.type == lv::NetworkInterfaceType::kVirtual ||
              nic.type == lv::NetworkInterfaceType::kOther);
    }

    // C ABI parity check: lv_net_list_interfaces must match C++ enumerate_network_interfaces.
    LvNetInterfaceInfo c_buf[64]{};
    const int c_count = lv_net_list_interfaces(c_buf, 64);
    CHECK(c_count == int(std::min<size_t>(active.size(), 64)));
    for (int i = 0; i < c_count; ++i) {
        CHECK(std::strcmp(c_buf[i].ipv4_address, active[size_t(i)].ipv4_address.c_str()) == 0);
        CHECK(std::strcmp(c_buf[i].id, active[size_t(i)].id.c_str()) == 0);
        CHECK(c_buf[i].is_up == 1);
    }

    // Extended C ABI query (include_disconnected=1, show_all=1) must return >= active count.
    LvNetInterfaceInfo c_all[64]{};
    const int c_all_count = lv_net_list_interfaces_ex(c_all, 64, 1, 1);
    CHECK(c_all_count >= c_count);

    // At least one active interface on a normal workstation / CI runner (or loopback fallback check).
    CHECK(all.size() >= active.size());
    if (!active.empty()) {
        lv::NetworkInterfaceInfo found{};
        CHECK(lv::find_interface_by_ip(active[0].ipv4_address, found));
        CHECK(found.ipv4_address == active[0].ipv4_address);
        CHECK(lv::is_interface_ip_up(active[0].ipv4_address));

        // UdpSocket::bind_to_interface must bind cleanly to the discovered active NIC.
        lv::UdpSocket s;
        REQUIRE(s.open());
        CHECK(s.bind_to_interface(active[0].ipv4_address, 0, active[0].if_index, active[0].system_name));
        CHECK(s.local_port() != 0);
        s.close();
    }

    // Loopback bind_to_interface always works on every OS.
    {
        lv::UdpSocket lo;
        REQUIRE(lo.open());
        CHECK(lo.bind_to_interface("127.0.0.1", 0, 0, ""));
        CHECK(lo.local_port() != 0);
        lo.close();
    }

}

TEST(network_dual_nic_1plus1_failover_zero_dropouts) {
    // ------------------------------------------------------------------------
    // Part A: Deterministic AudioTxEngine + AudioRxEngine Dual-NIC cable pull
    // ------------------------------------------------------------------------
    {
        lv::MasterClock clk;
        lv::AudioSyncMatrix mat{48000};
        std::multimap<uint64_t, std::vector<uint8_t>> wire_q;
        uint64_t now = 1'000'000;
        bool primary_up = true;
        uint64_t pri_delivered = 0;
        uint64_t sec_delivered = 0;

        lv::AudioTxEngine tx(48000, lv::kDefaultMtu, [&](const uint8_t* d, size_t n) {
            if (!primary_up) return false;  // simulated Primary Ethernet cable disconnect
            ++pri_delivered;
            wire_q.insert({now + 400, std::vector<uint8_t>(d, d + n)});
            return true;
        });
        tx.set_secondary_send([&](const uint8_t* d, size_t n) {
            ++sec_delivered;
            // Secondary NIC (e.g. Wi-Fi 5/6 GHz) arrives slightly later (+1.2 ms)
            wire_q.insert({now + 1600, std::vector<uint8_t>(d, d + n)});
            return true;
        });
        tx.set_network_interfaces("192.168.50.10", "192.168.51.20");
        tx.set_wifi_redundancy(true, 0);  // Dual-NIC immediate 1+1 copy
        tx.set_latency_ms(10);
        tx.set_codec_profile(lv::AudioCodecProfile::kPcm24);

        lv::AudioRxEngine rx(mat, clk, [](const uint8_t*, size_t) { return true; });
        rx.set_latency_ms(10);
        rx.set_adaptive_jitter_enabled(false);
        rx.set_one_way_clock_sync(true);
        mat.set_underrun_prefill(true, 75);

        constexpr uint32_t kStep = 120, kPop = 240;
        uint64_t next_send = now, next_pop = now + 20'000, media = 0;
        const uint64_t warm = now + 500'000;
        const uint64_t pull_cable_at = now + 1'500'000;
        const uint64_t end = now + 3'500'000;
        std::vector<float> blk(kStep), out(kPop * 2);

        bool measuring = false;
        uint64_t und0 = 0;
        float prev = 0.0f, max_step = 0.0f;
        uint64_t big_steps = 0, zero_run = 0, max_zero_run = 0;

        while (now < end) {
            now = std::min(next_send, next_pop);
            if (primary_up && now >= pull_cable_at) {
                primary_up = false;  // pull Primary NIC cable mid-stream!
            }
            if (now >= next_send) {
                for (uint32_t i = 0; i < kStep; ++i) {
                    blk[i] = 0.5f * float(std::sin(2.0 * 3.14159265358979 * 440.0 * double(media + i) / 48000.0));
                }
                tx.push_channel(0, blk.data(), kStep, lv::AudioFormat2::kFloat32, media, uint32_t(now));
                tx.push_channel(1, blk.data(), kStep, lv::AudioFormat2::kFloat32, media, uint32_t(now));
                tx.service_redundancy(uint32_t(now));
                media += kStep;
                next_send += 2500;
            }
            while (!wire_q.empty() && wire_q.begin()->first <= now) {
                auto dg = std::move(wire_q.begin()->second);
                wire_q.erase(wire_q.begin());
                lv::PacketHeader h{};
                const uint8_t* pl = nullptr;
                if (lv::parse_packet(dg.data(), dg.size(), h, &pl) == lv::PacketParseStatus::kOk &&
                    h.type == lv::PacketType::kAudio2) {
                    rx.on_packet(h, pl, h.payload_size, now);
                }
            }
            rx.service(now);
            if (now >= next_pop) {
                mat.pop_interleaved(clk, now, out.data(), 2, kPop);
                next_pop += 5000;
                if (!measuring && now >= warm) {
                    measuring = true;
                    und0 = mat.total_underruns();
                }
                if (measuring) {
                    for (uint32_t i = 0; i < kPop; ++i) {
                        const float v = out[2 * i];
                        const float d = std::fabs(v - prev);
                        max_step = std::max(max_step, d);
                        if (d > 0.15f) ++big_steps;
                        zero_run = (v == 0.0f) ? zero_run + 1 : 0;
                        max_zero_run = std::max(max_zero_run, zero_run);
                        prev = v;
                    }
                } else {
                    prev = out[2 * (kPop - 1)];
                }
            }
        }

        const auto st = tx.stats();
        const uint64_t und = mat.total_underruns() - und0;
        std::printf("    [dual-nic audio failover] pri=%llu sec=%llu failover=%llu underruns=%llu "
                    "big_steps=%llu max_step=%.3f zero_run=%llu nacks=%llu\n",
                    (unsigned long long)pri_delivered, (unsigned long long)sec_delivered,
                    (unsigned long long)st.failover_packets, (unsigned long long)und,
                    (unsigned long long)big_steps, max_step, (unsigned long long)max_zero_run,
                    (unsigned long long)rx.total_nacks_sent());
        CHECK(pri_delivered > 200);
        CHECK(sec_delivered > 500);
        CHECK(st.failover_packets > 200);
        CHECK(und == 0);
        CHECK(big_steps == 0);
        CHECK(max_zero_run < 8);
        CHECK(rx.total_nacks_sent() == 0);
    }

    // ------------------------------------------------------------------------
    // Part B: Real UDP Sockets PersonalMonitorServer + Client Dual-NIC Failover
    // ------------------------------------------------------------------------
    {
        lv::PersonalMonitorServer srv(48000);
        REQUIRE(srv.start(0, "127.0.0.1", 0));
        REQUIRE(srv.set_network_interfaces("127.0.0.1", "127.0.0.1"));
        REQUIRE(srv.dual_nic_active());
        srv.set_wifi_redundancy(true, 1500);
        srv.set_multichannel_flow(true, 2);

        lv::PersonalMonitorClient cli(48000);
        cli.set_wifi_redundancy(true);
        REQUIRE(cli.connect("127.0.0.1", srv.bound_port(), "DualNicPhone", 256, 15));

        std::atomic<bool> stop{false};
        std::thread mixer([&] {
            lv::RealtimeThreadScope rt{5000u};  // like Studio's mixer thread (real-time scheduling)
            constexpr uint32_t kBlk = 240;  // 5 ms @ 48 kHz
            std::vector<float> ch(kBlk);
            uint64_t sample_pos = 0;
            uint64_t next = lv::now_us();
            while (!stop.load(std::memory_order_acquire)) {
                for (uint32_t i = 0; i < kBlk; ++i) {
                    ch[i] = 0.45f * float(std::sin(2.0 * 3.14159265358979 * 440.0 * double(sample_pos + i) / 48000.0));
                }
                const float* planes[2] = {ch.data(), ch.data()};
                srv.process_and_send(planes, 2, kBlk);
                sample_pos += kBlk;
                next += 5000;
                lv::sleep_until_us(next, [] {});
            }
        });

        constexpr uint32_t kPop = 240;
        std::vector<float> stereo(kPop * 2, 0.0f);
        lv::RealtimeThreadScope phone_rt{5000u};  // the phone's audio IO thread is real-time too
        const uint64_t t_start = lv::now_us();
        const uint64_t t_warm = t_start + 350'000;
        const uint64_t t_pull = t_start + 750'000;
        const uint64_t t_end = t_start + 1'550'000;
        uint64_t next_pop = t_start + 5000;
        bool cable_pulled = false;
        uint64_t non_silent_before = 0, non_silent_after = 0;
        uint64_t silent_after_pull = 0;

        while (lv::now_us() < t_end) {
            cli.poll();
            const uint64_t now = lv::now_us();
            if (!cable_pulled && now >= t_pull) {
                srv.set_primary_link_down(true);  // simulate Primary NIC cable pull mid-stream!
                cable_pulled = true;
            }
            if (now >= next_pop) {
                cli.pop_stereo(stereo.data(), kPop);
                next_pop += 5000;
                if (now >= t_warm) {
                    float peak = 0.0f;
                    for (uint32_t i = 0; i < kPop; ++i) {
                        peak = std::max(peak, std::fabs(stereo[2 * i]));
                    }
                    if (!cable_pulled) {
                        if (peak > 0.05f) ++non_silent_before;
                    } else {
                        if (peak > 0.05f) ++non_silent_after;
                        else ++silent_after_pull;
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::microseconds(400));
        }

        stop.store(true, std::memory_order_release);
        mixer.join();

        const auto dt = cli.dante_telemetry();
        std::printf("    [dual-nic UDP monitor failover] pri_bytes=%llu sec_bytes=%llu failover_pkts=%llu "
                    "non_silent=%llu/%llu silent_after=%llu dup_rec=%llu nacks=%llu\n",
                    (unsigned long long)srv.primary_tx_bytes(),
                    (unsigned long long)srv.secondary_tx_bytes(),
                    (unsigned long long)srv.failover_packets(),
                    (unsigned long long)non_silent_before,
                    (unsigned long long)non_silent_after,
                    (unsigned long long)silent_after_pull,
                    (unsigned long long)dt.duplicates_recovered,
                    (unsigned long long)dt.nacks_sent);

        CHECK(srv.primary_tx_bytes() > 0);
        CHECK(srv.secondary_tx_bytes() > 0);
        CHECK(srv.failover_packets() >= 40);
        CHECK(non_silent_before >= 40);
        CHECK(non_silent_after >= 80);
        CHECK(silent_after_pull == 0);
        CHECK(dt.nacks_sent == 0);

        cli.disconnect();
        srv.stop();
    }

    // ------------------------------------------------------------------------
    // Part C: VideoTxEngine (UdpSender) Dual-NIC failover API check
    // ------------------------------------------------------------------------
    {
        lv::UdpSocket rx_sock;
        REQUIRE(rx_sock.open());
        REQUIRE(rx_sock.bind("127.0.0.1", 0));
        rx_sock.set_nonblocking(true);

        lv::SenderConfig cfg{};
        cfg.dest_ip = "127.0.0.1";
        cfg.dest_port = rx_sock.local_port();
        cfg.bind_ip = "127.0.0.1";
        cfg.secondary_bind_ip = "127.0.0.1";

        lv::VideoTxEngine sender(cfg);
        REQUIRE(sender.init());
        CHECK(sender.dual_nic_active());

        // Cut primary link and send MIDI packets -> must fail over to secondary NIC seamlessly.
        sender.set_primary_link_down(true);
        for (int i = 0; i < 8; ++i) {
            CHECK(sender.push_midi(0x90, uint8_t(60 + i), 100, 0, uint64_t((i + 1) * 1000)));
        }
        CHECK(sender.failover_packets() >= 8);
        CHECK(sender.secondary_bytes_sent() > 0);

        uint8_t buf[256];
        lv::Endpoint from{};
        int received = 0;
        for (int i = 0; i < 32; ++i) {
            if (rx_sock.recv_from(buf, sizeof(buf), from) > 0) ++received;
        }
        CHECK(received >= 8);
        rx_sock.close();
    }
}
