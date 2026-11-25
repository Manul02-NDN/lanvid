#include "grid/l2_transport.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <iphlpapi.h>
#include <windows.h>
#if defined(_MSC_VER)
#pragma comment(lib, "iphlpapi.lib")
#endif
#elif defined(__linux__) && !defined(__ANDROID__)
#define LV_L2_LINUX_AF_PACKET 1
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#elif defined(__APPLE__)
#include <TargetConditionals.h>
#if !defined(TARGET_OS_IPHONE) || !TARGET_OS_IPHONE
#define LV_L2_MACOS_BPF 1
#include <sys/types.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/bpf.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif
#endif

namespace lv::grid {

// =============================================================================
// L2LoopbackSwitch + Loopback Transport
// =============================================================================

std::shared_ptr<L2LoopbackSwitch::Port> L2LoopbackSwitch::attach(const MacAddress& mac) {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& p : ports_) {
        if (p->mac == mac) return p;
    }
    auto port = std::make_shared<Port>();
    port->mac = mac;
    ports_.push_back(port);
    return port;
}

void L2LoopbackSwitch::detach(const MacAddress& mac) {
    std::lock_guard<std::mutex> lk(mu_);
    ports_.erase(std::remove_if(ports_.begin(), ports_.end(),
                                [&](const std::shared_ptr<Port>& p) { return p->mac == mac; }),
                 ports_.end());
}

void L2LoopbackSwitch::set_port_link(const MacAddress& mac, bool up) {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& p : ports_) {
        if (p->mac == mac) {
            p->link_up.store(up, std::memory_order_release);
            break;
        }
    }
}

void L2LoopbackSwitch::forward(const MacAddress& src, const uint8_t* frame, size_t len) {
    if (!frame || len < kEthHeaderSize) return;
    MacAddress dst{};
    std::memcpy(dst.bytes.data(), frame, 6);
    const bool bcast = dst.is_broadcast();

    std::vector<std::shared_ptr<Port>> targets;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& p : ports_) {
            if (!p->link_up.load(std::memory_order_acquire)) continue;
            if (p->mac == src) continue;  // Never echo back to sender port
            if (bcast || p->mac == dst) targets.push_back(p);
        }
    }
    for (auto& t : targets) {
        std::lock_guard<std::mutex> qlk(t->mu);
        if (t->rx_queue.size() < 4096) {
            t->rx_queue.emplace_back(frame, frame + len);
            t->rx_frames.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (!targets.empty()) switched_frames_.fetch_add(1, std::memory_order_relaxed);
}

namespace {

class LoopbackL2Transport final : public L2Transport {
public:
    LoopbackL2Transport(std::shared_ptr<L2LoopbackSwitch> sw, const MacAddress& mac)
        : sw_(std::move(sw)), mac_(mac) {
        if (sw_) port_ = sw_->attach(mac_);
    }
    ~LoopbackL2Transport() override { close(); }

    bool is_open() const override { return port_ != nullptr; }
    void close() override {
        if (sw_ && port_) {
            sw_->detach(mac_);
            port_.reset();
        }
    }
    MacAddress mac() const override { return mac_; }
    std::string backend_name() const override { return "Virtual L2 Switch (Loopback)"; }

    bool send_frame(const uint8_t* frame, size_t len) override {
        if (!sw_ || !port_ || !port_->link_up.load(std::memory_order_acquire)) return false;
        port_->tx_frames.fetch_add(1, std::memory_order_relaxed);
        sw_->forward(mac_, frame, len);
        return true;
    }

    int recv_frame(uint8_t* out_buf, size_t cap) override {
        if (!port_ || !out_buf || cap == 0) return 0;
        std::lock_guard<std::mutex> qlk(port_->mu);
        if (port_->rx_queue.empty()) return 0;
        const auto& pkt = port_->rx_queue.front();
        const size_t n = std::min(cap, pkt.size());
        std::memcpy(out_buf, pkt.data(), n);
        port_->rx_queue.pop_front();
        return int(n);
    }

private:
    std::shared_ptr<L2LoopbackSwitch> sw_;
    std::shared_ptr<L2LoopbackSwitch::Port> port_;
    MacAddress mac_{};
};

// =============================================================================
// Platform Native L2 Backends
// =============================================================================

#if defined(_WIN32)

// Dynamic Npcap (wpcap.dll) function pointers — zero compile-time SDK dependency!
struct PcapPkthdr {
    long tv_sec;
    long tv_usec;
    uint32_t caplen;
    uint32_t len;
};

struct NpcapApi {
    HMODULE mod = nullptr;
    using OpenLiveFn = void*(__cdecl*)(const char*, int, int, int, char*);
    using CloseFn = void(__cdecl*)(void*);
    using SetNonblockFn = int(__cdecl*)(void*, int, char*);
    using SetMinToCopyFn = int(__cdecl*)(void*, int);
    using SendPacketFn = int(__cdecl*)(void*, const uint8_t*, int);
    using NextExFn = int(__cdecl*)(void*, PcapPkthdr**, const uint8_t**);

    OpenLiveFn open_live = nullptr;
    CloseFn close = nullptr;
    SetNonblockFn setnonblock = nullptr;
    SetMinToCopyFn setmintocopy = nullptr;
    SendPacketFn sendpacket = nullptr;
    NextExFn next_ex = nullptr;

    static NpcapApi& instance() {
        static NpcapApi api;
        static bool tried = false;
        if (!tried) {
            tried = true;
            // Npcap installs into System32\Npcap (and optionally System32 in WinPcap-compat mode).
            char sysdir[MAX_PATH] = {};
            if (GetSystemDirectoryA(sysdir, MAX_PATH)) {
                std::string npcap_path = std::string(sysdir) + "\\Npcap\\wpcap.dll";
                api.mod = LoadLibraryA(npcap_path.c_str());
            }
            if (!api.mod) api.mod = LoadLibraryA("wpcap.dll");
            if (api.mod) {
                api.open_live = reinterpret_cast<OpenLiveFn>(GetProcAddress(api.mod, "pcap_open_live"));
                api.close = reinterpret_cast<CloseFn>(GetProcAddress(api.mod, "pcap_close"));
                api.setnonblock = reinterpret_cast<SetNonblockFn>(GetProcAddress(api.mod, "pcap_setnonblock"));
                api.setmintocopy = reinterpret_cast<SetMinToCopyFn>(GetProcAddress(api.mod, "pcap_setmintocopy"));
                api.sendpacket = reinterpret_cast<SendPacketFn>(GetProcAddress(api.mod, "pcap_sendpacket"));
                api.next_ex = reinterpret_cast<NextExFn>(GetProcAddress(api.mod, "pcap_next_ex"));
            }
        }
        return api;
    }

    bool ready() const {
        return mod && open_live && close && setnonblock && sendpacket && next_ex;
    }
};

class NpcapL2Transport final : public L2Transport {
public:
    NpcapL2Transport(void* handle, const MacAddress& mac) : handle_(handle), mac_(mac) {}
    ~NpcapL2Transport() override { close(); }

    bool is_open() const override { return handle_ != nullptr; }
    void close() override {
        if (handle_) {
            NpcapApi::instance().close(handle_);
            handle_ = nullptr;
        }
    }
    MacAddress mac() const override { return mac_; }
    std::string backend_name() const override { return "Windows Npcap L2 (Direct NIC)"; }

    bool send_frame(const uint8_t* frame, size_t len) override {
        if (!handle_ || !frame || len < kEthHeaderSize) return false;
        return NpcapApi::instance().sendpacket(handle_, frame, int(len)) == 0;
    }

    int recv_frame(uint8_t* out_buf, size_t cap) override {
        if (!handle_ || !out_buf || cap == 0) return 0;
        PcapPkthdr* hdr = nullptr;
        const uint8_t* data = nullptr;
        const int rc = NpcapApi::instance().next_ex(handle_, &hdr, &data);
        if (rc == 1 && hdr && data) {
            const size_t n = std::min<size_t>(cap, hdr->caplen);
            std::memcpy(out_buf, data, n);
            return int(n);
        }
        return 0;
    }

private:
    void* handle_ = nullptr;
    MacAddress mac_{};
};

#elif defined(LV_L2_LINUX_AF_PACKET)

class LinuxAfPacketL2Transport final : public L2Transport {
public:
    LinuxAfPacketL2Transport(int fd, int ifindex, const MacAddress& mac)
        : fd_(fd), ifindex_(ifindex), mac_(mac) {}
    ~LinuxAfPacketL2Transport() override { close(); }

    bool is_open() const override { return fd_ >= 0; }
    void close() override {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }
    MacAddress mac() const override { return mac_; }
    std::string backend_name() const override { return "Linux AF_PACKET L2 (Direct NIC)"; }

    bool send_frame(const uint8_t* frame, size_t len) override {
        if (fd_ < 0 || !frame || len < kEthHeaderSize) return false;
        sockaddr_ll sll{};
        sll.sll_family = AF_PACKET;
        sll.sll_ifindex = ifindex_;
        sll.sll_halen = 6;
        std::memcpy(sll.sll_addr, frame, 6);
        return ::sendto(fd_, frame, len, 0, reinterpret_cast<const sockaddr*>(&sll), sizeof(sll)) == ssize_t(len);
    }

    int recv_frame(uint8_t* out_buf, size_t cap) override {
        if (fd_ < 0 || !out_buf || cap == 0) return 0;
        const ssize_t n = ::recv(fd_, out_buf, cap, MSG_DONTWAIT);
        if (n < 0) return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
        return int(n);
    }

private:
    int fd_ = -1;
    int ifindex_ = 0;
    MacAddress mac_{};
};

#elif defined(LV_L2_MACOS_BPF)

class MacOsBpfL2Transport final : public L2Transport {
public:
    MacOsBpfL2Transport(int fd, size_t buf_len, const MacAddress& mac)
        : fd_(fd), bpf_buf_(buf_len > 0 ? buf_len : 32768), mac_(mac) {}
    ~MacOsBpfL2Transport() override { close(); }

    bool is_open() const override { return fd_ >= 0; }
    void close() override {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }
    MacAddress mac() const override { return mac_; }
    std::string backend_name() const override { return "macOS /dev/bpf L2 (Direct NIC)"; }

    bool send_frame(const uint8_t* frame, size_t len) override {
        if (fd_ < 0 || !frame || len < kEthHeaderSize) return false;
        return ::write(fd_, frame, len) == ssize_t(len);
    }

    int recv_frame(uint8_t* out_buf, size_t cap) override {
        if (fd_ < 0 || !out_buf || cap == 0) return 0;
        if (rx_off_ >= rx_len_) {
            const ssize_t n = ::read(fd_, bpf_buf_.data(), bpf_buf_.size());
            if (n <= 0) return 0;
            rx_len_ = size_t(n);
            rx_off_ = 0;
        }
        if (rx_off_ + sizeof(bpf_hdr) > rx_len_) {
            rx_off_ = rx_len_ = 0;
            return 0;
        }
        const auto* bh = reinterpret_cast<const bpf_hdr*>(bpf_buf_.data() + rx_off_);
        const size_t pkt_off = rx_off_ + bh->bh_hdrlen;
        const size_t pkt_len = bh->bh_caplen;
        rx_off_ += BPF_WORDALIGN(bh->bh_hdrlen + bh->bh_caplen);
        if (pkt_off + pkt_len > rx_len_) return 0;
        const size_t copy_n = std::min(cap, pkt_len);
        std::memcpy(out_buf, bpf_buf_.data() + pkt_off, copy_n);
        return int(copy_n);
    }

private:
    int fd_ = -1;
    std::vector<uint8_t> bpf_buf_;
    size_t rx_off_ = 0;
    size_t rx_len_ = 0;
    MacAddress mac_{};
};

#endif

}  // namespace

std::vector<L2AdapterInfo> list_l2_adapters() {
    std::vector<L2AdapterInfo> out;
#if defined(_WIN32)
    ULONG sz = 16384;
    std::vector<uint8_t> buf(sz);
    auto* addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST, nullptr, addrs, &sz) ==
        ERROR_BUFFER_OVERFLOW) {
        buf.resize(sz);
        addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    }
    if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST, nullptr, addrs, &sz) ==
        NO_ERROR) {
        for (auto* a = addrs; a != nullptr; a = a->Next) {
            if (a->PhysicalAddressLength != 6) continue;
            if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            L2AdapterInfo info{};
            info.device_name = std::string("\\Device\\NPF_") + (a->AdapterName ? a->AdapterName : "");
            if (a->FriendlyName) {
                int len = WideCharToMultiByte(CP_UTF8, 0, a->FriendlyName, -1, nullptr, 0, nullptr, nullptr);
                if (len > 1) {
                    info.friendly_name.resize(size_t(len - 1));
                    WideCharToMultiByte(CP_UTF8, 0, a->FriendlyName, -1, info.friendly_name.data(), len, nullptr, nullptr);
                }
            }
            if (info.friendly_name.empty()) info.friendly_name = a->AdapterName ? a->AdapterName : "Ethernet";
            std::memcpy(info.mac.bytes.data(), a->PhysicalAddress, 6);
            info.is_ethernet = (a->IfType == IF_TYPE_ETHERNET_CSMACD);
            info.link_up = (a->OperStatus == IfOperStatusUp);
            info.speed_mbps = (info.link_up && a->TransmitLinkSpeed != ~0ull &&
                               a->TransmitLinkSpeed <= 400'000'000'000ull)
                                  ? uint32_t(a->TransmitLinkSpeed / 1'000'000ull)
                                  : 0u;
            out.push_back(std::move(info));
        }
    }
#elif defined(LV_L2_LINUX_AF_PACKET) || defined(LV_L2_MACOS_BPF)
    ifaddrs* ifa = nullptr;
    if (getifaddrs(&ifa) == 0 && ifa) {
        for (auto* cur = ifa; cur != nullptr; cur = cur->ifa_next) {
            if (!cur->ifa_addr || !cur->ifa_name) continue;
            if (cur->ifa_flags & IFF_LOOPBACK) continue;
#if defined(LV_L2_LINUX_AF_PACKET)
            if (cur->ifa_addr->sa_family != AF_PACKET) continue;
            auto* sll = reinterpret_cast<sockaddr_ll*>(cur->ifa_addr);
            if (sll->sll_halen != 6) continue;
            L2AdapterInfo info{};
            info.device_name = cur->ifa_name;
            info.friendly_name = cur->ifa_name;
            std::memcpy(info.mac.bytes.data(), sll->sll_addr, 6);
#else
            if (cur->ifa_addr->sa_family != AF_LINK) continue;
            auto* sdl = reinterpret_cast<sockaddr_dl*>(cur->ifa_addr);
            if (sdl->sdl_alen != 6) continue;
            L2AdapterInfo info{};
            info.device_name = cur->ifa_name;
            info.friendly_name = cur->ifa_name;
            std::memcpy(info.mac.bytes.data(), LLADDR(sdl), 6);
#endif
            info.link_up = (cur->ifa_flags & IFF_UP) && (cur->ifa_flags & IFF_RUNNING);
            info.is_ethernet = true;
            out.push_back(std::move(info));
        }
        freeifaddrs(ifa);
    }
#endif
    return out;
}

bool l2_native_backend_available(std::string* out_status) {
#if defined(_WIN32)
    const bool ok = NpcapApi::instance().ready();
    if (out_status) {
        *out_status = ok ? "Npcap L2 Ready (wpcap.dll)" : "Npcap не знайдено (встановіть npcap.com для прямого L2)";
    }
    return ok;
#elif defined(LV_L2_LINUX_AF_PACKET)
    if (out_status) *out_status = "Linux AF_PACKET L2 (CAP_NET_RAW)";
    return true;
#elif defined(LV_L2_MACOS_BPF)
    if (out_status) *out_status = "macOS /dev/bpf L2";
    return true;
#else
    if (out_status) *out_status = "Unsupported OS";
    return false;
#endif
}

std::unique_ptr<L2Transport> create_native_l2_transport(const L2AdapterInfo& adapter, std::string* err) {
#if defined(_WIN32)
    auto& api = NpcapApi::instance();
    if (!api.ready()) {
        if (err) *err = "Npcap (wpcap.dll) not installed";
        return nullptr;
    }
    char errbuf[256] = {};
    void* h = api.open_live(adapter.device_name.c_str(), int(kMaxEthFrameSize), 1, 1, errbuf);
    if (!h) {
        if (err) *err = errbuf[0] ? errbuf : "pcap_open_live failed";
        return nullptr;
    }
    api.setnonblock(h, 1, errbuf);
    if (api.setmintocopy) api.setmintocopy(h, 0);  // Sub-millisecond immediate delivery!
    return std::make_unique<NpcapL2Transport>(h, adapter.mac);
#elif defined(LV_L2_LINUX_AF_PACKET)
    const int fd = ::socket(AF_PACKET, SOCK_RAW, htons(kGridEtherType));
    if (fd < 0) {
        if (err) *err = "AF_PACKET socket failed (requires CAP_NET_RAW)";
        return nullptr;
    }
    ifreq ifr{};
    std::snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", adapter.device_name.c_str());
    if (::ioctl(fd, SIOCGIFINDEX, &ifr) < 0) {
        ::close(fd);
        if (err) *err = "SIOCGIFINDEX failed";
        return nullptr;
    }
    sockaddr_ll sll{};
    sll.sll_family = AF_PACKET;
    sll.sll_protocol = htons(kGridEtherType);
    sll.sll_ifindex = ifr.ifr_ifindex;
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&sll), sizeof(sll)) < 0) {
        ::close(fd);
        if (err) *err = "AF_PACKET bind failed";
        return nullptr;
    }
    int prio = 6;
    ::setsockopt(fd, SOL_SOCKET, SO_PRIORITY, &prio, sizeof(prio));
    return std::make_unique<LinuxAfPacketL2Transport>(fd, ifr.ifr_ifindex, adapter.mac);
#elif defined(LV_L2_MACOS_BPF)
    int fd = -1;
    for (int i = 0; i < 64 && fd < 0; ++i) {
        char path[32];
        std::snprintf(path, sizeof(path), "/dev/bpf%d", i);
        fd = ::open(path, O_RDWR | O_NONBLOCK);
    }
    if (fd < 0) {
        if (err) *err = "Unable to open /dev/bpf* (requires chmod /dev/bpf or root)";
        return nullptr;
    }
    ifreq ifr{};
    std::snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", adapter.device_name.c_str());
    if (::ioctl(fd, BIOCSETIF, &ifr) < 0) {
        ::close(fd);
        if (err) *err = "BIOCSETIF failed";
        return nullptr;
    }
    unsigned int one = 1, zero = 0, blen = 32768;
    ::ioctl(fd, BIOCIMMEDIATE, &one);
    ::ioctl(fd, BIOCSHDRCMPLT, &one);
    ::ioctl(fd, BIOCSSEESENT, &zero);
    ::ioctl(fd, BIOCGBLEN, &blen);
    return std::make_unique<MacOsBpfL2Transport>(fd, blen, adapter.mac);
#else
    (void)adapter;
    if (err) *err = "Unsupported platform";
    return nullptr;
#endif
}

std::unique_ptr<L2Transport> create_loopback_l2_transport(std::shared_ptr<L2LoopbackSwitch> sw,
                                                          const MacAddress& mac) {
    return std::make_unique<LoopbackL2Transport>(std::move(sw), mac);
}

}  // namespace lv::grid
