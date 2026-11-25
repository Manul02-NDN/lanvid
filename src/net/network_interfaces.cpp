#include "net/network_interfaces.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#if defined(_MSC_VER)
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")
#endif
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#include <net/if_dl.h>
#include <net/if_types.h>
#if !defined(TARGET_OS_IPHONE) || !TARGET_OS_IPHONE
#define LV_HAVE_MACOS_SC 1
#include <CoreFoundation/CoreFoundation.h>
#include <SystemConfiguration/SystemConfiguration.h>
#endif
#elif defined(__linux__)
#include <linux/if_packet.h>
#if !defined(__ANDROID__)
#include <linux/wireless.h>
#endif
#ifndef SIOCGIWNAME
#define SIOCGIWNAME 0x8B01
#endif
#endif
#endif

namespace lv {

const char* network_interface_type_tag(NetworkInterfaceType t) {
    switch (t) {
        case NetworkInterfaceType::kEthernet:    return "Ethernet";
        case NetworkInterfaceType::kWiFi:        return "Wi-Fi";
        case NetworkInterfaceType::kUsbEthernet: return "USB-LAN";
        case NetworkInterfaceType::kVirtual:     return "Virtual";
        default:                                 return "Other";
    }
}

namespace {

std::string to_lower_ascii(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

[[maybe_unused]] bool contains_ci(const std::string& hay, const char* needle) {
    return to_lower_ascii(hay).find(to_lower_ascii(needle)) != std::string::npos;
}

std::string format_mac(const uint8_t* bytes, size_t len) {
    if (!bytes || len < 6) return {};
    bool all_zero = true;
    for (size_t i = 0; i < 6; ++i) {
        if (bytes[i] != 0) {
            all_zero = false;
            break;
        }
    }
    if (all_zero) return {};
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
                  bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5]);
    return buf;
}

bool is_link_local_v4(const std::string& ip) {
    return ip.rfind("169.254.", 0) == 0;
}

bool is_loopback_v4(const std::string& ip) {
    return ip.rfind("127.", 0) == 0;
}

int type_sort_rank(NetworkInterfaceType t) {
    switch (t) {
        case NetworkInterfaceType::kEthernet:    return 0;
        case NetworkInterfaceType::kUsbEthernet: return 1;
        case NetworkInterfaceType::kWiFi:        return 2;
        case NetworkInterfaceType::kOther:       return 3;
        case NetworkInterfaceType::kVirtual:     return 4;
    }
    return 5;
}

#if defined(_WIN32)
void ensure_wsa_nic() {
    static struct Once {
        Once() {
            WSADATA wd{};
            WSAStartup(MAKEWORD(2, 2), &wd);
        }
    } once;
    (void)once;
}

std::string wide_to_utf8(const wchar_t* w) {
    if (!w || !*w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(size_t(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), n, nullptr, nullptr);
    return out;
}

bool is_virtual_win_desc(const std::string& friendly, const std::string& desc) {
    const std::string combined = to_lower_ascii(friendly + " " + desc);
    static const char* kVirtKeys[] = {
        "virtual", "vmware", "virtualbox", "hyper-v", "vethernet", "tap-windows",
        "wintun", "wireguard", "tailscale", "zerotier", "openvpn", "hamachi",
        "radmin", "bluetooth", "npcap loopback", "km-test", "teredo", "isatap",
        "6to4", "miniport", "wsl", "docker", "nordlynx", "protonvpn", "mullvad"
    };
    for (const char* k : kVirtKeys) {
        if (combined.find(k) != std::string::npos) return true;
    }
    return false;
}

bool is_usb_eth_desc(const std::string& friendly, const std::string& desc) {
    const std::string combined = to_lower_ascii(friendly + " " + desc);
    static const char* kUsbKeys[] = {
        "usb", "ax88", "asix", "rtl815", "r815", "cdc ethernet", "rndis",
        "apple mobile", "belkin", "thunderbolt", "dock", "type-c"
    };
    for (const char* k : kUsbKeys) {
        if (combined.find(k) != std::string::npos) return true;
    }
    return false;
}
#endif

#if defined(LV_HAVE_MACOS_SC)
std::string cfstr_to_utf8(CFStringRef s) {
    if (!s) return {};
    const CFIndex len = CFStringGetLength(s);
    const CFIndex max_sz = CFStringGetMaximumSizeForEncoding(len, kCFStringEncodingUTF8) + 1;
    std::string out(size_t(std::max<CFIndex>(1, max_sz)), '\0');
    if (CFStringGetCString(s, out.data(), max_sz, kCFStringEncodingUTF8)) {
        out.resize(std::strlen(out.c_str()));
        return out;
    }
    return {};
}
#endif

#if defined(__linux__)
bool linux_is_wifi(const std::string& ifname) {
    // 1. Check /sys/class/net/<if>/wireless (exists for cfg80211/mac80211 Wi-Fi)
    {
        std::ifstream f("/sys/class/net/" + ifname + "/uevent");
        std::string line;
        while (std::getline(f, line)) {
            if (line.find("DEVTYPE=wlan") != std::string::npos) return true;
        }
    }
    // 2. Fallback ioctl SIOCGIWNAME (Wireless Extensions)
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd >= 0) {
        struct ifreq ifr{};
        std::snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifname.c_str());
        const bool ok = (::ioctl(fd, SIOCGIWNAME, &ifr) == 0);
        ::close(fd);
        if (ok) return true;
    }
    // 3. Interface naming fallback (Android wlan0, Linux wlp*, wlx*)
    return ifname.rfind("wlan", 0) == 0 || ifname.rfind("wlp", 0) == 0 ||
           ifname.rfind("wlx", 0) == 0 || ifname.rfind("swlan", 0) == 0;
}

uint32_t linux_link_speed_mbps(const std::string& ifname) {
    std::ifstream f("/sys/class/net/" + ifname + "/speed");
    int spd = 0;
    if (f && (f >> spd) && spd > 0 && spd < 1000000) return uint32_t(spd);
    return 0;
}

bool linux_is_virtual(const std::string& ifname) {
    static const char* kPrefixes[] = {
        "lo", "docker", "veth", "br-", "virbr", "tun", "tap", "wg",
        "tailscale", "zt", "dummy", "rmnet", "ccmni", "p2p", "ip6tnl",
        "sit", "gre", "vxlan", "cni", "flannel", "cali", "lxc"
    };
    for (const char* p : kPrefixes) {
        if (ifname.rfind(p, 0) == 0) return true;
    }
    return false;
}
#endif

}  // namespace

std::vector<NetworkInterfaceInfo> enumerate_network_interfaces(bool include_disconnected, bool show_all) {
    std::vector<NetworkInterfaceInfo> out;

#if defined(_WIN32)
    ensure_wsa_nic();
    ULONG buf_len = 32768;
    std::vector<uint8_t> buf(buf_len);
    const ULONG flags = GAA_FLAG_INCLUDE_PREFIX | GAA_FLAG_SKIP_ANYCAST |
                        GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    DWORD ret = GetAdaptersAddresses(AF_INET, flags, nullptr,
                                     reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buf.data()), &buf_len);
    if (ret == ERROR_BUFFER_OVERFLOW) {
        buf.resize(buf_len);
        ret = GetAdaptersAddresses(AF_INET, flags, nullptr,
                                   reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buf.data()), &buf_len);
    }
    if (ret != NO_ERROR) return out;

    for (auto* a = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buf.data()); a; a = a->Next) {
        if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        if (!show_all && a->IfType == IF_TYPE_TUNNEL) continue;

        const bool oper_up = (a->OperStatus == IfOperStatusUp);
        const std::string sys_name = a->AdapterName ? std::string(a->AdapterName) : std::string();
        const std::string friendly = wide_to_utf8(a->FriendlyName);
        const std::string desc = wide_to_utf8(a->Description);
        const std::string mac = format_mac(a->PhysicalAddress, a->PhysicalAddressLength);

        NetworkInterfaceType type = NetworkInterfaceType::kOther;
        if (is_virtual_win_desc(friendly, desc)) {
            type = NetworkInterfaceType::kVirtual;
        } else if (a->IfType == IF_TYPE_IEEE80211) {
            type = NetworkInterfaceType::kWiFi;
        } else if (a->IfType == IF_TYPE_ETHERNET_CSMACD || a->IfType == IF_TYPE_GIGABITETHERNET ||
                   a->IfType == IF_TYPE_FASTETHER) {
            type = is_usb_eth_desc(friendly, desc) ? NetworkInterfaceType::kUsbEthernet
                                                   : NetworkInterfaceType::kEthernet;
        } else if (a->IfType == IF_TYPE_TUNNEL || a->IfType == IF_TYPE_PROP_VIRTUAL) {
            type = NetworkInterfaceType::kVirtual;
        }

        if (!show_all && type == NetworkInterfaceType::kVirtual) continue;

        uint64_t raw_bps = std::max<uint64_t>(a->TransmitLinkSpeed, a->ReceiveLinkSpeed);
        if (raw_bps == ~0ull || raw_bps > 400'000'000'000ull) raw_bps = 0;
        const uint32_t speed_mbps = uint32_t(raw_bps / 1'000'000ull);

        std::string label = friendly;
        if (!desc.empty() && !contains_ci(friendly, desc.c_str())) {
            label = friendly.empty() ? desc : (friendly + " (" + desc + ")");
        }
        if (label.empty()) label = sys_name;

        std::string ip_str, mask_str, bcast_str;
        for (auto* ua = a->FirstUnicastAddress; ua; ua = ua->Next) {
            if (!ua->Address.lpSockaddr || ua->Address.lpSockaddr->sa_family != AF_INET) continue;
            auto* sin = reinterpret_cast<sockaddr_in*>(ua->Address.lpSockaddr);
            char ip_buf[INET_ADDRSTRLEN]{};
            inet_ntop(AF_INET, &sin->sin_addr, ip_buf, sizeof(ip_buf));
            if (is_loopback_v4(ip_buf)) continue;
            if (!show_all && is_link_local_v4(ip_buf)) continue;

            const uint32_t ip_h = ntohl(sin->sin_addr.s_addr);
            const uint8_t prefix = std::min<uint8_t>(32, ua->OnLinkPrefixLength);
            const uint32_t mask_h = (prefix == 0) ? 0u : (~0u << (32 - prefix));
            const uint32_t bcast_h = ip_h | (~mask_h);

            in_addr ma{}, ba{};
            ma.s_addr = htonl(mask_h);
            ba.s_addr = htonl(bcast_h);
            char mask_buf[INET_ADDRSTRLEN]{}, bc_buf[INET_ADDRSTRLEN]{};
            inet_ntop(AF_INET, &ma, mask_buf, sizeof(mask_buf));
            inet_ntop(AF_INET, &ba, bc_buf, sizeof(bc_buf));

            ip_str = ip_buf;
            mask_str = mask_buf;
            bcast_str = bc_buf;
            break;
        }

        const bool is_up = oper_up && !ip_str.empty();
        if (!include_disconnected && !is_up) continue;

        NetworkInterfaceInfo info{};
        info.id = sys_name.empty() ? label : sys_name;
        info.system_name = sys_name;
        info.friendly_name = label;
        info.type = type;
        info.ipv4_address = ip_str;
        info.subnet_mask = mask_str;
        info.broadcast_address = bcast_str;
        info.mac_address = mac;
        info.link_speed_mbps = speed_mbps;
        info.if_index = uint32_t(a->IfIndex);
        info.is_up = is_up;
        out.push_back(std::move(info));
    }
#else
    struct ScMeta {
        std::string display_name;
        NetworkInterfaceType type = NetworkInterfaceType::kOther;
    };
    std::map<std::string, ScMeta> sc_map;
#if defined(LV_HAVE_MACOS_SC)
    if (CFArrayRef arr = SCNetworkInterfaceCopyAll()) {
        const CFIndex n = CFArrayGetCount(arr);
        for (CFIndex i = 0; i < n; ++i) {
            auto iface = static_cast<SCNetworkInterfaceRef>(const_cast<void*>(CFArrayGetValueAtIndex(arr, i)));
            if (!iface) continue;
            const std::string bsd = cfstr_to_utf8(SCNetworkInterfaceGetBSDName(iface));
            if (bsd.empty()) continue;
            ScMeta m{};
            m.display_name = cfstr_to_utf8(SCNetworkInterfaceGetLocalizedDisplayName(iface));
            CFStringRef itype = SCNetworkInterfaceGetInterfaceType(iface);
            if (itype && CFEqual(itype, kSCNetworkInterfaceTypeIEEE80211)) {
                m.type = NetworkInterfaceType::kWiFi;
            } else if (itype && CFEqual(itype, kSCNetworkInterfaceTypeEthernet)) {
                m.type = contains_ci(m.display_name, "usb") || contains_ci(m.display_name, "thunderbolt")
                             ? NetworkInterfaceType::kUsbEthernet
                             : NetworkInterfaceType::kEthernet;
            } else if (itype && (CFEqual(itype, kSCNetworkInterfaceTypeBridge) ||
                                 CFEqual(itype, kSCNetworkInterfaceTypeVLAN))) {
                m.type = NetworkInterfaceType::kVirtual;
            }
            sc_map[bsd] = std::move(m);
        }
        CFRelease(arr);
    }
#endif

    ifaddrs* ifa_list = nullptr;
    if (getifaddrs(&ifa_list) != 0 || !ifa_list) return out;

    std::map<std::string, NetworkInterfaceInfo> by_name;
    for (ifaddrs* ifa = ifa_list; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_name || !ifa->ifa_addr) continue;
        const std::string name(ifa->ifa_name);
        if (ifa->ifa_flags & IFF_LOOPBACK) continue;

        auto& entry = by_name[name];
        if (entry.system_name.empty()) {
            entry.id = name;
            entry.system_name = name;
            entry.if_index = if_nametoindex(name.c_str());
            entry.is_up = ((ifa->ifa_flags & IFF_UP) != 0) && ((ifa->ifa_flags & IFF_RUNNING) != 0);
        }

        const int fam = ifa->ifa_addr->sa_family;
        if (fam == AF_INET) {
            auto* sin = reinterpret_cast<sockaddr_in*>(ifa->ifa_addr);
            char ip_buf[INET_ADDRSTRLEN]{};
            inet_ntop(AF_INET, &sin->sin_addr, ip_buf, sizeof(ip_buf));
            if (is_loopback_v4(ip_buf)) continue;
            if (!show_all && is_link_local_v4(ip_buf)) continue;
            if (entry.ipv4_address.empty()) {
                entry.ipv4_address = ip_buf;
                if (ifa->ifa_netmask && ifa->ifa_netmask->sa_family == AF_INET) {
                    auto* nsin = reinterpret_cast<sockaddr_in*>(ifa->ifa_netmask);
                    char mbuf[INET_ADDRSTRLEN]{};
                    inet_ntop(AF_INET, &nsin->sin_addr, mbuf, sizeof(mbuf));
                    entry.subnet_mask = mbuf;
                }
                if ((ifa->ifa_flags & IFF_BROADCAST) && ifa->ifa_broadaddr &&
                    ifa->ifa_broadaddr->sa_family == AF_INET) {
                    auto* bsin = reinterpret_cast<sockaddr_in*>(ifa->ifa_broadaddr);
                    char bbuf[INET_ADDRSTRLEN]{};
                    inet_ntop(AF_INET, &bsin->sin_addr, bbuf, sizeof(bbuf));
                    entry.broadcast_address = bbuf;
                }
            }
        }
#if defined(__APPLE__)
        else if (fam == AF_LINK) {
            auto* sdl = reinterpret_cast<sockaddr_dl*>(ifa->ifa_addr);
            if (sdl && sdl->sdl_alen == 6) {
                const auto* mac = reinterpret_cast<const uint8_t*>(LLADDR(sdl));
                entry.mac_address = format_mac(mac, 6);
            }
            if (ifa->ifa_data) {
                const auto* ifd = reinterpret_cast<const if_data*>(ifa->ifa_data);
                if (ifd->ifi_baudrate > 0) {
                    entry.link_speed_mbps = uint32_t(ifd->ifi_baudrate / 1'000'000ull);
                }
            }
        }
#elif defined(__linux__)
        else if (fam == AF_PACKET) {
            auto* sll = reinterpret_cast<sockaddr_ll*>(ifa->ifa_addr);
            if (sll && sll->sll_halen == 6) {
                entry.mac_address = format_mac(sll->sll_addr, 6);
            }
        }
#endif
    }
    freeifaddrs(ifa_list);

    for (auto& kv : by_name) {
        auto& info = kv.second;
        const std::string& name = info.system_name;

#if defined(__APPLE__)
        const bool apple_virt =
            name.rfind("utun", 0) == 0 || name.rfind("awdl", 0) == 0 || name.rfind("llw", 0) == 0 ||
            name.rfind("ipsec", 0) == 0 || name.rfind("pdp_ip", 0) == 0 || name.rfind("bridge", 0) == 0 ||
            name.rfind("anpi", 0) == 0 || name.rfind("ap", 0) == 0 || name.rfind("gif", 0) == 0 ||
            name.rfind("stf", 0) == 0 || name.rfind("XHC", 0) == 0;
        auto sc_it = sc_map.find(name);
        if (sc_it != sc_map.end()) {
            info.type = sc_it->second.type;
            info.friendly_name = sc_it->second.display_name.empty()
                                     ? name
                                     : (sc_it->second.display_name + " (" + name + ")");
        } else if (apple_virt) {
            info.type = NetworkInterfaceType::kVirtual;
            info.friendly_name = name;
        } else {
#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
            if (name == "en0") {
                info.type = NetworkInterfaceType::kWiFi;
                info.friendly_name = "Wi-Fi (en0)";
            } else if (name.rfind("en", 0) == 0) {
                info.type = NetworkInterfaceType::kUsbEthernet;
                info.friendly_name = "USB-C Ethernet (" + name + ")";
            } else {
                info.type = NetworkInterfaceType::kOther;
                info.friendly_name = name;
            }
#else
            info.type = (name.rfind("en", 0) == 0) ? NetworkInterfaceType::kEthernet : NetworkInterfaceType::kOther;
            info.friendly_name = name;
#endif
        }
#elif defined(__linux__)
        info.link_speed_mbps = linux_link_speed_mbps(name);
        if (linux_is_virtual(name)) {
            info.type = NetworkInterfaceType::kVirtual;
            info.friendly_name = name;
        } else if (linux_is_wifi(name)) {
            info.type = NetworkInterfaceType::kWiFi;
            info.friendly_name = "Wi-Fi (" + name + ")";
        } else if (name.rfind("enx", 0) == 0 || name.rfind("usb", 0) == 0) {
            info.type = NetworkInterfaceType::kUsbEthernet;
            info.friendly_name = "USB Ethernet (" + name + ")";
        } else if (name.rfind("eth", 0) == 0 || name.rfind("enp", 0) == 0 ||
                   name.rfind("eno", 0) == 0 || name.rfind("ens", 0) == 0) {
            info.type = NetworkInterfaceType::kEthernet;
            info.friendly_name = "Ethernet (" + name + ")";
        } else {
            info.type = NetworkInterfaceType::kOther;
            info.friendly_name = name;
        }
#else
        info.friendly_name = name;
#endif

        if (!show_all && info.type == NetworkInterfaceType::kVirtual) continue;
        info.is_up = info.is_up && !info.ipv4_address.empty();
        if (!include_disconnected && !info.is_up) continue;
        out.push_back(std::move(info));
    }
#endif

    std::sort(out.begin(), out.end(), [](const NetworkInterfaceInfo& a, const NetworkInterfaceInfo& b) {
        if (a.is_up != b.is_up) return a.is_up > b.is_up;
        const int ra = type_sort_rank(a.type);
        const int rb = type_sort_rank(b.type);
        if (ra != rb) return ra < rb;
        if (a.link_speed_mbps != b.link_speed_mbps) return a.link_speed_mbps > b.link_speed_mbps;
        return a.friendly_name < b.friendly_name;
    });
    return out;
}

bool find_interface_by_ip(const std::string& ipv4, NetworkInterfaceInfo& out) {
    if (ipv4.empty() || ipv4 == "0.0.0.0" || ipv4 == "127.0.0.1") return false;
    const auto all = enumerate_network_interfaces(true, true);
    for (const auto& info : all) {
        if (info.ipv4_address == ipv4) {
            out = info;
            return true;
        }
    }
    return false;
}

bool is_interface_ip_up(const std::string& ipv4) {
    if (ipv4.empty() || ipv4 == "0.0.0.0" || ipv4 == "127.0.0.1") return true;
    NetworkInterfaceInfo info{};
    return find_interface_by_ip(ipv4, info) && info.is_up;
}

}  // namespace lv
