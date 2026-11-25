#include "net/discovery.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>

#include "codec/dct.hpp"
#include "common/clock.hpp"
#include "common/cpu_features.hpp"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace lv {

namespace {

#if defined(_WIN32)
using RawSock = SOCKET;
constexpr RawSock kInvalidRawSock = INVALID_SOCKET;
void ensure_wsa() {
    static struct WsaOnce {
        WsaOnce() {
            WSADATA wd{};
            WSAStartup(MAKEWORD(2, 2), &wd);
        }
    } once;
    (void)once;
}
void close_raw_sock(RawSock s) {
    if (s != INVALID_SOCKET) closesocket(s);
}
void set_rcv_timeout(RawSock s, int ms) {
    DWORD tv = DWORD(std::max(1, ms));
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
}
std::string wide_to_utf8(const wchar_t* w) {
    if (!w || !*w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(size_t(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), n, nullptr, nullptr);
    return out;
}
#else
using RawSock = int;
constexpr RawSock kInvalidRawSock = -1;
void ensure_wsa() {}
void close_raw_sock(RawSock s) {
    if (s >= 0) ::close(s);
}
void set_rcv_timeout(RawSock s, int ms) {
    ms = std::max(1, ms);
    timeval tv{};
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}
#endif

constexpr const char* kProbePrefix = "LVDS_PRB|";
constexpr const char* kRespPrefix = "LVDS_RSP|";

std::string sanitize_field(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '|' || c == '\r' || c == '\n') out.push_back('_');
        else out.push_back(c);
    }
    return out;
}

}  // namespace

const char* local_os_name() {
#if defined(_WIN32)
    return "windows";
#elif defined(__ANDROID__)
    return "android";
#elif defined(__APPLE__)
    return "macos";
#else
    return "linux";
#endif
}

std::string local_device_name() {
#if defined(_WIN32)
    wchar_t buf[128]{};
    DWORD len = 128;
    if (GetComputerNameExW(ComputerNameDnsHostname, buf, &len) && len > 0) {
        return wide_to_utf8(buf);
    }
    return "Windows-PC";
#else
    char buf[128]{};
    if (gethostname(buf, sizeof(buf) - 1) == 0 && buf[0] != '\0') {
        return std::string(buf);
    }
#if defined(__ANDROID__)
    return "Android-Device";
#else
    return "Linux-Host";
#endif
#endif
}

std::vector<NetInterfaceInfo> list_network_interfaces() {
    std::vector<NetInterfaceInfo> out;
#if defined(_WIN32)
    ensure_wsa();
    ULONG buf_len = 16384;
    std::vector<uint8_t> buf(buf_len);
    DWORD ret = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                     nullptr, reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buf.data()), &buf_len);
    if (ret == ERROR_BUFFER_OVERFLOW) {
        buf.resize(buf_len);
        ret = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                   nullptr, reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buf.data()), &buf_len);
    }
    if (ret != NO_ERROR) return out;

    for (auto* a = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buf.data()); a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp) continue;
        const bool is_lo = (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK);
        const std::string friendly = wide_to_utf8(a->FriendlyName);
        for (auto* ua = a->FirstUnicastAddress; ua; ua = ua->Next) {
            if (!ua->Address.lpSockaddr || ua->Address.lpSockaddr->sa_family != AF_INET) continue;
            auto* sin = reinterpret_cast<sockaddr_in*>(ua->Address.lpSockaddr);
            char ip_s[INET_ADDRSTRLEN]{};
            inet_ntop(AF_INET, &sin->sin_addr, ip_s, sizeof(ip_s));

            const uint32_t ip_h = ntohl(sin->sin_addr.s_addr);
            const uint8_t prefix = std::min<uint8_t>(32, ua->OnLinkPrefixLength);
            const uint32_t mask = (prefix == 0) ? 0u : (~0u << (32 - prefix));
            const uint32_t bcast_h = ip_h | (~mask);
            in_addr ba{};
            ba.s_addr = htonl(bcast_h);
            char bc_s[INET_ADDRSTRLEN]{};
            inet_ntop(AF_INET, &ba, bc_s, sizeof(bc_s));

            out.push_back({friendly, ip_s, bc_s, is_lo});
        }
    }
#else
    ifaddrs* ifa_list = nullptr;
    if (getifaddrs(&ifa_list) != 0 || !ifa_list) return out;
    for (ifaddrs* ifa = ifa_list; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
        if (!(ifa->ifa_flags & IFF_UP)) continue;
        const bool is_lo = (ifa->ifa_flags & IFF_LOOPBACK) != 0;
        auto* sin = reinterpret_cast<sockaddr_in*>(ifa->ifa_addr);
        char ip_s[INET_ADDRSTRLEN]{};
        inet_ntop(AF_INET, &sin->sin_addr, ip_s, sizeof(ip_s));

        char bc_s[INET_ADDRSTRLEN]{};
        if ((ifa->ifa_flags & IFF_BROADCAST) && ifa->ifa_broadaddr &&
            ifa->ifa_broadaddr->sa_family == AF_INET) {
            auto* bsin = reinterpret_cast<sockaddr_in*>(ifa->ifa_broadaddr);
            inet_ntop(AF_INET, &bsin->sin_addr, bc_s, sizeof(bc_s));
        } else {
            std::snprintf(bc_s, sizeof(bc_s), "255.255.255.255");
        }
        out.push_back({ifa->ifa_name ? ifa->ifa_name : "eth", ip_s, bc_s, is_lo});
    }
    freeifaddrs(ifa_list);
#endif
    return out;
}

std::string local_host_name() {
    char buf[256] = {};
    if (gethostname(buf, sizeof(buf) - 1) != 0 || !buf[0]) return local_device_name();
    return buf;
}

static std::string aoip_clean(const std::string& v) {
    std::string o;
    for (char ch : v) o += (ch == '|' || ch == ';' || ch == '=' || ch == '\n' || ch == '\r') ? '_' : ch;
    return o.substr(0, 96);
}

std::string encode_aoip_field(const AoipAnnounce& a) {
    return "AOIP=1;host=" + aoip_clean(a.hostname) + ";drv=" + aoip_clean(a.audio_driver) +
           ";conn=" + (a.driver_connected ? "1" : "0") + ";ch=" + std::to_string(a.channels) +
           ";rx=" + std::to_string(a.rx_port) + ";tx=" + std::to_string(a.tx_port) +
           ";dpx=" + (a.duplex ? "1" : "0");
}

bool decode_aoip_field(const std::string& field, DiscoveredReceiver& r) {
    if (field.rfind("AOIP=", 0) != 0) return false;
    size_t pos = 0;
    while (pos < field.size()) {
        size_t end = field.find(';', pos);
        if (end == std::string::npos) end = field.size();
        const std::string kv = field.substr(pos, end - pos);
        const size_t eq = kv.find('=');
        if (eq != std::string::npos) {
            const std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
            if (k == "AOIP") r.aoip = (v == "1");
            else if (k == "host") r.hostname = v;
            else if (k == "drv") r.audio_driver = v;
            else if (k == "conn") r.driver_connected = (v == "1");
            else if (k == "ch") r.audio_channels = uint16_t(std::strtoul(v.c_str(), nullptr, 10));
            else if (k == "rx") r.aoip_rx_port = uint16_t(std::strtoul(v.c_str(), nullptr, 10));
            else if (k == "tx") r.aoip_tx_port = uint16_t(std::strtoul(v.c_str(), nullptr, 10));
            else if (k == "dpx") r.duplex = (v == "1");
        }
        pos = end + 1;
    }
    return r.aoip;
}

void DiscoveryResponder::set_aoip_provider(std::function<AoipAnnounce()> provider) {
    std::lock_guard<std::mutex> lk(aoip_mu_);
    aoip_provider_ = std::move(provider);
}

void DiscoveryResponder::set_network_interfaces(const std::string& primary_ip, const std::string& secondary_ip) {
    std::lock_guard<std::mutex> lk(aoip_mu_);
    primary_nic_ip_ = (primary_ip == "auto" || primary_ip == "0.0.0.0") ? std::string() : primary_ip;
    secondary_nic_ip_ = (secondary_ip == "auto" || secondary_ip == "0.0.0.0") ? std::string() : secondary_ip;
}

bool DiscoveryResponder::start(uint16_t stream_port, const std::string& custom_name,
                               uint16_t discovery_port) {
    stop();
    ensure_wsa();

    RawSock s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == kInvalidRawSock) return false;

    int reuse = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
#if !defined(_WIN32) && defined(SO_REUSEPORT)
    setsockopt(s, SOL_SOCKET, SO_REUSEPORT, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
#endif
    set_rcv_timeout(s, 100);

    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = INADDR_ANY;
    {
        std::lock_guard<std::mutex> lk(aoip_mu_);
        if (!primary_nic_ip_.empty() && secondary_nic_ip_.empty()) {
            in_addr mif{};
            if (inet_pton(AF_INET, primary_nic_ip_.c_str(), &mif) == 1) {
                (void)setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF,
                                 reinterpret_cast<const char*>(&mif), sizeof(mif));
            }
        }
    }
    sa.sin_port = htons(discovery_port);
    if (::bind(s, reinterpret_cast<const sockaddr*>(&sa), sizeof(sa)) != 0) {
        close_raw_sock(s);
        return false;
    }

    const std::string dev_name = sanitize_field(custom_name.empty() ? local_device_name() : custom_name);
    const std::string simd = dct_kernels_best().name;
    const std::string resp_base = std::string(kRespPrefix) + std::to_string(stream_port) + "|" +
                                  local_os_name() + "|" + simd + "|" + dev_name;

    stop_.store(false, std::memory_order_release);
    running_.store(true, std::memory_order_release);

    thread_ = std::thread([this, s, resp_base]() {
        char buf[256];
        while (!stop_.load(std::memory_order_relaxed)) {
            sockaddr_in peer{};
#if defined(_WIN32)
            int plen = sizeof(peer);
#else
            socklen_t plen = sizeof(peer);
#endif
            const int n = int(::recvfrom(s, buf, sizeof(buf) - 1, 0,
                                         reinterpret_cast<sockaddr*>(&peer), &plen));
            if (n <= 0) continue;
            buf[n] = '\0';
            if (std::strncmp(buf, kProbePrefix, std::strlen(kProbePrefix)) != 0) continue;
            // Echo the sender's nonce so RTT can be measured accurately.
            const std::string nonce(buf + std::strlen(kProbePrefix));
            std::string reply = resp_base;
            {
                std::lock_guard<std::mutex> lk(aoip_mu_);
                if (aoip_provider_) {
                    const AoipAnnounce a = aoip_provider_();
                    if (a.enabled) reply += "|" + encode_aoip_field(a);
                }
            }
            reply += "|" + sanitize_field(nonce);
            ::sendto(s, reply.data(), int(reply.size()), 0,
                     reinterpret_cast<const sockaddr*>(&peer), sizeof(peer));
        }
        close_raw_sock(s);
        running_.store(false, std::memory_order_release);
    });
    return true;
}

void DiscoveryResponder::stop() {
    stop_.store(true, std::memory_order_release);
    if (thread_.joinable()) thread_.join();
    running_.store(false, std::memory_order_release);
}

std::vector<DiscoveredReceiver> discovery_scan(int timeout_ms, uint16_t discovery_port,
                                               const std::string& primary_ip,
                                               const std::string& secondary_ip) {
    ensure_wsa();
    std::vector<DiscoveredReceiver> out;
    if (timeout_ms < 50) timeout_ms = 50;

    RawSock s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == kInvalidRawSock) return out;

    int bcast = 1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&bcast), sizeof(bcast));
    set_rcv_timeout(s, 40);

    sockaddr_in bind_sa{};
    bind_sa.sin_family = AF_INET;
    bind_sa.sin_addr.s_addr = INADDR_ANY;
    const bool bind_specific = !primary_ip.empty() && primary_ip != "0.0.0.0" && primary_ip != "auto" &&
                               (secondary_ip.empty() || secondary_ip == "0.0.0.0" || secondary_ip == "auto");
    if (bind_specific) {
        in_addr addr{};
        if (inet_pton(AF_INET, primary_ip.c_str(), &addr) == 1) {
            bind_sa.sin_addr = addr;
            (void)setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF,
                             reinterpret_cast<const char*>(&addr), sizeof(addr));
        }
    }
    bind_sa.sin_port = 0;
    if (::bind(s, reinterpret_cast<const sockaddr*>(&bind_sa), sizeof(bind_sa)) != 0) {
        bind_sa.sin_addr.s_addr = INADDR_ANY;
        if (::bind(s, reinterpret_cast<const sockaddr*>(&bind_sa), sizeof(bind_sa)) != 0) {
            close_raw_sock(s);
            return out;
        }
    }

    // Collect broadcast targets: 255.255.255.255, every active subnet broadcast, and 127.0.0.1.
    std::set<std::string> targets;
    targets.insert("255.255.255.255");
    targets.insert("127.0.0.1");
    for (const auto& iface : list_network_interfaces()) {
        if (!iface.is_loopback && !iface.broadcast_ipv4.empty()) {
            targets.insert(iface.broadcast_ipv4);
        }
    }

    const uint64_t t_start = now_us();
    const std::string probe = std::string(kProbePrefix) + std::to_string(t_start & 0xFFFFFFFFull);
    auto send_probes = [&]() {
        for (const auto& ip : targets) {
            sockaddr_in dst{};
            dst.sin_family = AF_INET;
            dst.sin_port = htons(discovery_port);
            if (inet_pton(AF_INET, ip.c_str(), &dst.sin_addr) == 1) {
                ::sendto(s, probe.data(), int(probe.size()), 0,
                         reinterpret_cast<const sockaddr*>(&dst), sizeof(dst));
            }
        }
    };

    send_probes();
    bool resent = false;
    std::set<std::string> seen;

    char buf[1024];
    const uint64_t deadline = t_start + uint64_t(timeout_ms) * 1000ull;
    while (now_us() < deadline) {
        if (!resent && now_us() >= t_start + uint64_t(timeout_ms) * 500ull) {
            send_probes();
            resent = true;
        }
        sockaddr_in src{};
#if defined(_WIN32)
        int slen = sizeof(src);
#else
        socklen_t slen = sizeof(src);
#endif
        const int n = int(::recvfrom(s, buf, sizeof(buf) - 1, 0,
                                     reinterpret_cast<sockaddr*>(&src), &slen));
        if (n <= 0) continue;
        buf[n] = '\0';
        if (std::strncmp(buf, kRespPrefix, std::strlen(kRespPrefix)) != 0) continue;

        // Format: LVDS_RSP|<port>|<os>|<simd>|<device_name>|<nonce>
        std::string body(buf + std::strlen(kRespPrefix));
        std::vector<std::string> parts;
        size_t pos = 0;
        while (pos <= body.size()) {
            const size_t sep = body.find('|', pos);
            if (sep == std::string::npos) {
                parts.push_back(body.substr(pos));
                break;
            }
            parts.push_back(body.substr(pos, sep - pos));
            pos = sep + 1;
        }
        if (parts.size() < 4) continue;

        char ip_s[INET_ADDRSTRLEN]{};
        inet_ntop(AF_INET, &src.sin_addr, ip_s, sizeof(ip_s));
        const uint16_t port = uint16_t(std::strtoul(parts[0].c_str(), nullptr, 10));
        if (port == 0) continue;

        const std::string key = std::string(ip_s) + ":" + std::to_string(port);
        if (!seen.insert(key).second) continue;

        DiscoveredReceiver r;
        r.ip = ip_s;
        r.stream_port = port;
        r.os = parts[1];
        r.simd = parts[2];
        r.device_name = parts[3];
        r.rtt_ms = uint32_t((now_us() - t_start) / 1000ull);
        for (size_t k = 4; k + 1 < parts.size(); ++k) {  // optional fields sit between name and nonce
            if (decode_aoip_field(parts[k], r)) break;
        }
        out.push_back(std::move(r));
    }

    close_raw_sock(s);
    return out;
}

}  // namespace lv
