#pragma once
// Cross-platform enumeration of connected Ethernet, Wi-Fi, USB-Ethernet, and virtual NICs.
//  - Windows : GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_PREFIX, ...)
//  - macOS   : getifaddrs() + SystemConfiguration (SCNetworkInterface) + AF_LINK
//  - iOS     : getifaddrs() + AF_LINK (Wi-Fi en0, USB-C Ethernet en1..)
//  - Linux / Android : getifaddrs() + AF_PACKET + SIOCGIWNAME + /sys/class/net/<if>/speed

#include <cstdint>
#include <string>
#include <vector>

#include "common/common_ns.hpp"

namespace NET_COMMON_NS {

enum class NetworkInterfaceType : uint8_t {
    kOther = 0,
    kEthernet = 1,
    kWiFi = 2,
    kUsbEthernet = 3,
    kVirtual = 4,
};

const char* network_interface_type_tag(NetworkInterfaceType t);

struct NetworkInterfaceInfo {
    std::string id;                // Stable adapter ID (Windows GUID / POSIX ifname)
    std::string system_name;       // Raw OS name (Windows AdapterName GUID, macOS en0, Linux eth0/wlan0)
    std::string friendly_name;     // Human-readable name (e.g. "Ethernet (Realtek PCIe GbE)")
    NetworkInterfaceType type = NetworkInterfaceType::kOther;
    std::string ipv4_address;      // Dotted IPv4 (e.g. "192.168.50.226")
    std::string subnet_mask;       // Dotted IPv4 netmask (e.g. "255.255.255.0")
    std::string broadcast_address; // Subnet broadcast IPv4 (e.g. "192.168.50.255")
    std::string mac_address;       // "AA:BB:CC:DD:EE:FF"
    uint32_t link_speed_mbps = 0;  // 100 / 1000 / 2500 / 866 / ...
    uint32_t if_index = 0;         // OS interface index (for IP_UNICAST_IF / IP_BOUND_IF)
    bool is_up = false;            // True if operational link is UP (and valid IPv4 or active L2)
};

// Enumerates physical & virtual network adapters on the host.
// - include_disconnected == false (default): returns only UP interfaces with a valid IPv4 address.
// - show_all == false (default): excludes loopback, tunnels, and unrouted virtual/link-local adapters.
std::vector<NetworkInterfaceInfo> enumerate_network_interfaces(bool include_disconnected = false,
                                                               bool show_all = false);

// Helper: resolves OS interface index and system_name for a given local IPv4 address (returns false if not found).
bool find_interface_by_ip(const std::string& ipv4, NetworkInterfaceInfo& out);

// Helper: checks whether a given local IPv4 address currently belongs to an UP network interface.
bool is_interface_ip_up(const std::string& ipv4);

}  // namespace NET_COMMON_NS
