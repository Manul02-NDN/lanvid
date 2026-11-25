// dsp_server_main.cpp — Standalone Headless LanVid Grid DSP Server (Linux RT / Windows / macOS)
// Processes up to 256 Layer 2 audio channels in real time (< 1 ms) and supports Primary/Backup Redundancy.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "common/clock.hpp"
#include "grid/dsp_server.hpp"
#include "grid/l2_transport.hpp"

namespace {
std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop.store(true, std::memory_order_release); }
}  // namespace

int main(int argc, char** argv) {
    // Line-buffered: as a LaunchDaemon / service stdout is a log file and would otherwise only be written at exit.
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    std::string nic_filter;
    std::string name = "LanVid DSP Server";
    bool backup = false;
    bool list_only = false;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--list") == 0) {
            list_only = true;
        } else if (std::strcmp(argv[i], "--backup") == 0) {
            backup = true;
        } else if (std::strcmp(argv[i], "--nic") == 0 && i + 1 < argc) {
            nic_filter = argv[++i];
        } else if (std::strcmp(argv[i], "--name") == 0 && i + 1 < argc) {
            name = argv[++i];
        } else if (std::strcmp(argv[i], "--help") == 0) {
            std::printf("Usage: lanvid_dsp_server [--list] [--nic <name>] [--name <label>] [--backup]\n");
            return 0;
        }
    }

    const auto adapters = lv::grid::list_l2_adapters();
    if (list_only) {
        std::string status;
        lv::grid::l2_native_backend_available(&status);
        std::printf("L2 Backend: %s\n", status.c_str());
        for (const auto& a : adapters) {
            std::printf("  %s | %s | MAC=%s | %s | %u Mbps\n",
                        a.friendly_name.c_str(), a.device_name.c_str(), a.mac.to_string().c_str(),
                        a.link_up ? "UP" : "DOWN", a.speed_mbps);
        }
        return 0;
    }

    lv::grid::L2AdapterInfo chosen{};
    bool found = false;
    for (const auto& a : adapters) {
        if (!nic_filter.empty() && (a.device_name == nic_filter ||
                                    a.friendly_name.find(nic_filter) != std::string::npos ||
                                    a.device_name.find(nic_filter) != std::string::npos)) {
            chosen = a;
            found = true;
            break;
        }
    }
    if (!found) found = lv::grid::pick_default_l2_adapter(adapters, chosen);  // wired Ethernet with carrier
    if (!found) {
        std::fprintf(stderr, "[lanvid_dsp_server] No network adapter found.\n");
        return 1;
    }

    std::string err;
    auto transport = lv::grid::create_native_l2_transport(chosen, &err);
    if (!transport) {
        std::fprintf(stderr, "[lanvid_dsp_server] Failed to open L2 adapter '%s': %s\n",
                     chosen.friendly_name.c_str(), err.c_str());
        return 2;
    }

    lv::grid::GridDspServer srv(std::move(transport), name, backup, 48000);
    std::printf("[lanvid_dsp_server] Running '%s' (%s) on %s (MAC %s, 256ch LV-DSP, EtherType 0x%04X)\n",
                name.c_str(), backup ? "BACKUP" : "PRIMARY", chosen.friendly_name.c_str(),
                srv.mac().to_string().c_str(), unsigned(lv::grid::kGridEtherType));

    uint64_t last_hb_us = 0;
    while (!g_stop.load(std::memory_order_acquire)) {
        const int n = srv.service(256);
        const uint64_t now = lv::now_us();
        if (now - last_hb_us >= 250'000ull) {
            srv.emit_heartbeat();
            last_hb_us = now;
        }
        if (n == 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(150));
        }
    }
    std::printf("[lanvid_dsp_server] Stopped cleanly (frames processed: %llu).\n",
                static_cast<unsigned long long>(srv.frames_processed()));
    return 0;
}
