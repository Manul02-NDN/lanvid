#pragma once
// LAN Auto-Discovery (UDP Broadcast probe/response on port 5099) + Local IPv4 Interface Enumeration.
// Cross-platform: Windows (Winsock2 + iphlpapi), Linux/macOS/Android (POSIX + getifaddrs).

#include <atomic>
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
};

// Enumerates active IPv4 network interfaces on this host.
std::vector<NetInterfaceInfo> list_network_interfaces();

// Returns the host's friendly machine/device name.
std::string local_device_name();

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

private:
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> running_{false};
};

// Scans the local network (global broadcast + per-interface subnet broadcast + loopback)
// for active LanVid receivers and returns unique responders collected within `timeout_ms`.
std::vector<DiscoveredReceiver> discovery_scan(int timeout_ms = 350,
                                               uint16_t discovery_port = kDiscoveryPort);

}  // namespace lv
