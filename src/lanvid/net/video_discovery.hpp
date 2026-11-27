#pragma once
// LanVid Video LAN Auto-Discovery (UDP Broadcast probe/response on ports 5023 / 5099).
// Cross-platform: Windows (Winsock2 + iphlpapi), Linux/macOS/Android (POSIX + getifaddrs).

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace lv {

constexpr uint16_t kDiscoveryPort = 5099;
constexpr uint16_t kVideoStreamPort = 5006;
constexpr uint16_t kVideoTallyPort = 5022;
constexpr uint16_t kVideoDiscoveryPort = 5023;

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
    std::string role;  // "video" on :5023, or "" on default :5099
};

std::vector<NetInterfaceInfo> list_network_interfaces();
std::string local_device_name();
std::string local_host_name();
const char* local_os_name();

class DiscoveryResponder {
public:
    DiscoveryResponder() = default;
    ~DiscoveryResponder() { stop(); }

    DiscoveryResponder(const DiscoveryResponder&) = delete;
    DiscoveryResponder& operator=(const DiscoveryResponder&) = delete;

    bool start(uint16_t stream_port, const std::string& custom_name = "",
               uint16_t discovery_port = kDiscoveryPort);
    void stop();
    bool is_running() const { return running_.load(std::memory_order_acquire); }

    void set_role(const std::string& role);
    void set_network_interfaces(const std::string& primary_ip, const std::string& secondary_ip = "");

private:
    std::mutex cfg_mu_;
    std::string role_ = "video";
    std::string primary_nic_ip_;
    std::string secondary_nic_ip_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> running_{false};
};

using NetworkDiscovery = DiscoveryResponder;

std::vector<DiscoveredReceiver> discovery_scan(int timeout_ms = 350,
                                               uint16_t discovery_port = kDiscoveryPort,
                                               const std::string& primary_ip = "",
                                               const std::string& secondary_ip = "");

// Scans both the dedicated video discovery port (:5023) and legacy default (:5099).
std::vector<DiscoveredReceiver> discovery_scan_video(int timeout_ms = 350,
                                                     bool scan_legacy = true,
                                                     const std::string& primary_ip = "",
                                                     const std::string& secondary_ip = "");

}  // namespace lv
