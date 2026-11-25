#pragma once
// LAN Auto-Discovery (UDP Broadcast probe/response on port 5099) + Local IPv4 Interface Enumeration.
// Cross-platform: Windows (Winsock2 + iphlpapi), Linux/macOS/Android (POSIX + getifaddrs).

#include <atomic>
#include <functional>
#include <mutex>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace lv {

constexpr uint16_t kDiscoveryPort = 5099;

struct NetInterfaceInfo {
    std::string name;
    std::string ipv4;
    std::string broadcast_ipv4;
    bool is_loopback = false;
};

struct DiscoveredReceiver {
    std::string ip;
    uint16_t stream_port = 5000;
    std::string device_name;
    std::string os;
    std::string simd;
    uint32_t rtt_ms = 0;
    // ---- AoIP node announcement (optional field, ignored by older scanners) ----
    bool aoip = false;
    std::string hostname;
    std::string audio_driver;        // "CoreAudio HAL" / "ASIO" / "JACK/PipeWire" / "" (no DAW yet)
    bool driver_connected = false;
    uint16_t audio_channels = 0;
    uint16_t aoip_rx_port = 0;       // where this node RECEIVES the bus
    uint16_t aoip_tx_port = 0;       // peer port this node SENDS to (duplex)
    bool duplex = false;
};

// What an AoIP node advertises in its discovery reply (evaluated per probe, so it is always live).
struct AoipAnnounce {
    bool enabled = false;
    std::string hostname;
    std::string audio_driver;
    bool driver_connected = false;
    uint16_t channels = 64;
    uint16_t rx_port = 0;
    uint16_t tx_port = 0;
    bool duplex = false;
};

// Wire: "AOIP=1;host=<h>;drv=<d>;conn=<0|1>;ch=<n>;rx=<p>;tx=<p>;dpx=<0|1>" — inserted before the
// nonce, so pre-2.5 scanners (which read only fields 0..3) keep working.
std::string encode_aoip_field(const AoipAnnounce& a);
bool decode_aoip_field(const std::string& field, DiscoveredReceiver& out);

// Enumerates active IPv4 network interfaces on this host.
std::vector<NetInterfaceInfo> list_network_interfaces();

// Returns the host's friendly machine/device name.
std::string local_device_name();

// Returns the network host name (gethostname).
std::string local_host_name();

// Returns current OS identifier ("windows", "linux", "android", "macos").
const char* local_os_name();

// Background responder run by active Receivers so LAN senders discover them automatically.
class DiscoveryResponder {
public:
    DiscoveryResponder() = default;
    ~DiscoveryResponder() { stop(); }

    DiscoveryResponder(const DiscoveryResponder&) = delete;
    DiscoveryResponder& operator=(const DiscoveryResponder&) = delete;

    // Starts responding to probes on `discovery_port` advertising `stream_port`.
    bool start(uint16_t stream_port, const std::string& custom_name = "",
               uint16_t discovery_port = kDiscoveryPort);
    void stop();
    bool is_running() const { return running_.load(std::memory_order_acquire); }

    // Optional live AoIP announcement (call before or after start; thread-safe).
    void set_aoip_provider(std::function<AoipAnnounce()> provider);
    // Optional Primary / Secondary NIC IPv4 binding for discovery replies (empty = all interfaces).
    void set_network_interfaces(const std::string& primary_ip, const std::string& secondary_ip = "");

private:
    std::mutex aoip_mu_;
    std::function<AoipAnnounce()> aoip_provider_;
    std::string primary_nic_ip_;
    std::string secondary_nic_ip_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> running_{false};
};

using NetworkDiscovery = DiscoveryResponder;

// Scans the local network (global broadcast + per-interface subnet broadcast + loopback)
// for active LanVid receivers and returns unique responders collected within `timeout_ms`.
// When `primary_ip` (and optional `secondary_ip`) are set, binds outgoing probes to those NICs.
std::vector<DiscoveredReceiver> discovery_scan(int timeout_ms = 350,
                                               uint16_t discovery_port = kDiscoveryPort,
                                               const std::string& primary_ip = "",
                                               const std::string& secondary_ip = "");

}  // namespace lv
