// `sender --duplex`: headless full-duplex 64x64 AoIP node.
//   DAW outputs (virtual device TX ring) -> --host:--port      and      --listen-port -> DAW inputs
// with an optional 64x64 patchbay and Mute/Solo on either direction.
#include "app/duplex_cli.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "lanvid/lanvid_sdk.h"

namespace {

std::atomic<bool> g_dpx_stop{false};
void dpx_on_signal(int) { g_dpx_stop.store(true); }

void usage() {
    std::printf(
        "Full-duplex 64x64 AoIP node (virtual device <-> peer):\n"
        "  sender --duplex --host <PEER_IP> [--port 5004] [--listen-port 5005] [options]\n"
        "  --audio-latency <2..2000>   playout latency of the incoming bus (default 20)\n"
        "  --fixed-latency             Fixed jitter buffer held at --audio-latency (wired Ethernet / live\n"
        "                              monitoring); default Adaptive Wi-Fi (grows up to 200 ms on bursts)\n"
        "  --audio-rate <48000|96000>  bus sample rate (default 48000)\n"
        "  --audio-channels <1..64>    channels in each direction (default 64)\n"
        "  --bind <ip>                 local bind address (default 0.0.0.0)\n"
        "  --name <text>               name announced in LAN discovery (default: host name)\n"
        "  --no-discovery              do not announce this node on the LAN\n"
        "  --rx-headroom <frames>      frames queued for the DAW inputs (default auto: max(3 x DAW buffer, 512))\n"
        "  --tx-route <s:d,...>        TX patch, 1-based (e.g. 1:2,2:1 swaps; 5:0 unroutes 5)\n"
        "  --rx-route <s:d,...>        RX patch, 1-based\n"
        "  --tx-mute <c,...> --rx-mute <c,...> --tx-solo <c,...> --rx-solo <c,...>   1-based channels\n"
        "  --duration <sec>            stop after S seconds (default: until Ctrl+C)\n"
        "  --list-nodes                scan the LAN for AoIP nodes and exit\n"
        "Peer runs the mirror command: --host <THIS_IP> --port <our listen-port> --listen-port <our port>\n");
}

bool parse_list(const char* s, std::vector<int>& out) {
    out.clear();
    std::string str(s);
    size_t pos = 0;
    while (pos <= str.size()) {
        size_t end = str.find(',', pos);
        if (end == std::string::npos) end = str.size();
        const int v = std::atoi(str.substr(pos, end - pos).c_str());
        if (v < 1 || v > 64) return false;
        out.push_back(v - 1);
        pos = end + 1;
    }
    return true;
}

bool parse_route(const char* s, int8_t* map) {
    for (int c = 0; c < 64; ++c) map[c] = int8_t(c);
    std::string str(s);
    size_t pos = 0;
    while (pos <= str.size()) {
        size_t end = str.find(',', pos);
        if (end == std::string::npos) end = str.size();
        const std::string item = str.substr(pos, end - pos);
        const size_t colon = item.find(':');
        if (colon == std::string::npos) return false;
        const int src = std::atoi(item.substr(0, colon).c_str());
        const int dst = std::atoi(item.substr(colon + 1).c_str());
        if (src < 1 || src > 64 || dst < 0 || dst > 64) return false;
        map[src - 1] = int8_t(dst - 1);  // dst 0 -> -1 (unrouted)
        pos = end + 1;
    }
    return true;
}

}  // namespace

bool lv_cli_wants_duplex(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) if (std::strcmp(argv[i], "--duplex") == 0) return true;
    return false;
}

int lv_cli_run_duplex(int argc, char** argv) {
    LV_DuplexConfig cfg;
    lv_duplex_config_default(&cfg);
    cfg.peer_host[0] = 0;
    double duration = 0.0;
    bool list_nodes = false;
    bool tx_route_set = false, rx_route_set = false;
    int8_t tx_route[64], rx_route[64];
    std::vector<int> tx_mute, rx_mute, tx_solo, rx_solo;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto val = [&](void) -> const char* {
            if (i + 1 >= argc) { std::fprintf(stderr, "Missing value for %s\n", a.c_str()); std::exit(2); }
            return argv[++i];
        };
        if (a == "--duplex") continue;
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else if (a == "--host") std::snprintf(cfg.peer_host, sizeof(cfg.peer_host), "%s", val());
        else if (a == "--port") cfg.peer_port = uint16_t(std::atoi(val()));
        else if (a == "--listen-port") cfg.listen_port = uint16_t(std::atoi(val()));
        else if (a == "--audio-latency") cfg.latency_ms = uint32_t(std::atoi(val()));
        else if (a == "--fixed-latency") cfg.adaptive_jitter = 0;
        else if (a == "--audio-rate") cfg.sample_rate = std::atoi(val());
        else if (a == "--audio-channels") cfg.num_channels = std::atoi(val());
        else if (a == "--bind") std::snprintf(cfg.bind_ip, sizeof(cfg.bind_ip), "%s", val());
        else if (a == "--name") std::snprintf(cfg.device_name, sizeof(cfg.device_name), "%s", val());
        else if (a == "--no-discovery") cfg.enable_discovery = 0;
        else if (a == "--rx-headroom") cfg.rx_headroom_frames = uint32_t(std::max(0, std::atoi(val())));
        else if (a == "--duration") duration = std::atof(val());
        else if (a == "--list-nodes") list_nodes = true;
        else if (a == "--tx-route") { if (!parse_route(val(), tx_route)) { std::fprintf(stderr, "Bad --tx-route\n"); return 2; } tx_route_set = true; }
        else if (a == "--rx-route") { if (!parse_route(val(), rx_route)) { std::fprintf(stderr, "Bad --rx-route\n"); return 2; } rx_route_set = true; }
        else if (a == "--tx-mute") { if (!parse_list(val(), tx_mute)) { std::fprintf(stderr, "Bad --tx-mute\n"); return 2; } }
        else if (a == "--rx-mute") { if (!parse_list(val(), rx_mute)) { std::fprintf(stderr, "Bad --rx-mute\n"); return 2; } }
        else if (a == "--tx-solo") { if (!parse_list(val(), tx_solo)) { std::fprintf(stderr, "Bad --tx-solo\n"); return 2; } }
        else if (a == "--rx-solo") { if (!parse_list(val(), rx_solo)) { std::fprintf(stderr, "Bad --rx-solo\n"); return 2; } }
        else { std::fprintf(stderr, "Unknown duplex option: %s\n", a.c_str()); usage(); return 2; }
    }

    if (list_nodes) {
        LV_AudioNode nodes[32];
        const int n = lv_discovery_scan_audio_nodes(600, 0, nodes, 32);
        std::printf("Discovered AoIP nodes: %d\n", n);
        for (int i = 0; i < n; ++i)
            std::printf("  %-15s rx:%-5u tx:%-5u %2u ch %s | %s (%s) | driver: %s%s | %u ms\n", nodes[i].ip,
                        nodes[i].rx_port, nodes[i].tx_port, nodes[i].channels, nodes[i].duplex ? "duplex" : "rx",
                        nodes[i].name, nodes[i].hostname, nodes[i].audio_driver[0] ? nodes[i].audio_driver : "-",
                        nodes[i].driver_connected ? " (DAW connected)" : "", nodes[i].rtt_ms);
        return 0;
    }
    if (!cfg.peer_host[0]) {
        std::fprintf(stderr, "--duplex requires --host <PEER_IP> (try --list-nodes)\n");
        usage();
        return 2;
    }

    LvDuplexHandle* h = lv_duplex_create();
    const int rc = lv_duplex_start(h, &cfg);
    if (rc != LV_OK) {
        std::fprintf(stderr, "Failed to start duplex node: %s\n", lv_error_string(rc));
        lv_duplex_destroy(h);
        return 1;
    }
    if (tx_route_set) lv_audio_set_tx_routing(h, tx_route);
    if (rx_route_set) lv_audio_set_rx_routing(h, rx_route);
    for (int c : tx_mute) lv_audio_set_channel_mute_solo(h, LV_AUDIO_DIR_TX, uint16_t(c), 1, -1);
    for (int c : rx_mute) lv_audio_set_channel_mute_solo(h, LV_AUDIO_DIR_RX, uint16_t(c), 1, -1);
    for (int c : tx_solo) lv_audio_set_channel_mute_solo(h, LV_AUDIO_DIR_TX, uint16_t(c), -1, 1);
    for (int c : rx_solo) lv_audio_set_channel_mute_solo(h, LV_AUDIO_DIR_RX, uint16_t(c), -1, 1);

    std::signal(SIGINT, dpx_on_signal);
#if defined(SIGTERM)
    std::signal(SIGTERM, dpx_on_signal);
#endif
    std::printf("LanVid full-duplex %dx%d: DAW out -> %s:%u | :%u -> DAW in | %u ms (%s) @ %d Hz | discovery %s\n",
                cfg.num_channels, cfg.num_channels, cfg.peer_host, cfg.peer_port, cfg.listen_port, cfg.latency_ms,
                cfg.adaptive_jitter ? "AUTO" : "FIXED", cfg.sample_rate, cfg.enable_discovery ? "on" : "off");
    std::fflush(stdout);

    const auto t0 = std::chrono::steady_clock::now();
    while (!g_dpx_stop.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        LV_DuplexStats st{};
        lv_duplex_get_stats(h, &st);
        std::printf("[DUPLEX] DAW %s%s%s | %u Hz buf=%u | peer->us: clock %s rtt=%u us skew=%+.1f ppm pkts=%llu lost=%llu "
                    "to_daw=%llu underruns=%llu buffer=%.1f ms (%s) | us->peer: pkts=%llu from_daw=%llu overruns=%llu pongs=%llu\n",
                    st.driver_connected ? "connected" : "not connected", st.driver_name[0] ? " via " : "",
                    st.driver_name, st.sample_rate, st.buffer_size, st.rx_clock_synced ? "synced" : "free-run",
                    st.rx_rtt_us, st.rx_clock_skew_ppm, (unsigned long long)st.rx_packets,
                    (unsigned long long)st.rx_lost, (unsigned long long)st.rx_frames_to_daw,
                    (unsigned long long)st.rx_underruns, st.effective_latency_ms, st.adaptive_jitter ? "AUTO" : "FIXED",
                    (unsigned long long)st.tx_packets,
                    (unsigned long long)st.tx_frames_from_daw, (unsigned long long)st.tx_overruns,
                    (unsigned long long)st.tx_pongs_sent);
        std::fflush(stdout);
        if (duration > 0.0 &&
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() >= duration)
            break;
    }
    lv_duplex_stop(h);
    lv_duplex_destroy(h);
    return 0;
}
