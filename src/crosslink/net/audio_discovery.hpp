#pragma once
// CrossLink Audio LAN Auto-Discovery (UDP Broadcast probe/response on port 5021).
// Wire format: "CLDS_PRB|<nonce>" -> "CLDS_RSP|<port>|<os>|<device_name>|AOIP=...|ROLE=audio|<nonce>".

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace cl {

constexpr uint16_t kAudioStreamPort = 5004;
constexpr uint16_t kAudioDuplexListenPort = 5005;
constexpr uint16_t kAudioMonitorPort = 5020;
constexpr uint16_t kAudioDiscoveryPort = 5021;

struct NetInterfaceInfo {
    std::string name;
    std::string ipv4;
    std::string broadcast_ipv4;
    bool is_loopback = false;
};

struct DiscoveredAudioNode {
    std::string ip;
    uint16_t stream_port = kAudioStreamPort;
    std::string device_name;
    std::string os;
    uint32_t rtt_ms = 0;
    bool aoip = false;
    std::string hostname;
    std::string audio_driver;
    bool driver_connected = false;
    uint16_t audio_channels = 0;
    uint16_t aoip_rx_port = 0;
    uint16_t aoip_tx_port = 0;
    bool duplex = false;
};

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

std::string encode_aoip_field(const AoipAnnounce& a);
bool decode_aoip_field(const std::string& field, DiscoveredAudioNode& out);

std::vector<NetInterfaceInfo> list_network_interfaces();
std::string local_device_name();
std::string local_host_name();
const char* local_os_name();

class AudioDiscoveryResponder {
public:
    AudioDiscoveryResponder() = default;
    ~AudioDiscoveryResponder() { stop(); }

    AudioDiscoveryResponder(const AudioDiscoveryResponder&) = delete;
    AudioDiscoveryResponder& operator=(const AudioDiscoveryResponder&) = delete;

    bool start(uint16_t stream_port, const std::string& custom_name = "",
               uint16_t discovery_port = kAudioDiscoveryPort);
    void stop();
    bool is_running() const { return running_.load(std::memory_order_acquire); }

    void set_aoip_provider(std::function<AoipAnnounce()> provider);
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

std::vector<DiscoveredAudioNode> audio_discovery_scan(int timeout_ms = 350,
                                                      uint16_t discovery_port = kAudioDiscoveryPort,
                                                      const std::string& primary_ip = "",
                                                      const std::string& secondary_ip = "");

}  // namespace cl
