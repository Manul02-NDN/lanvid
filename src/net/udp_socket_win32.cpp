// Windows Winsock2 UDP socket implementation.

#include "net/udp_socket.hpp"

#include <cstdio>
#include <cstring>

#include <winsock2.h>
#include <ws2tcpip.h>

namespace lv {

namespace {

struct WsaInit {
    bool ok = false;
    WsaInit() {
        WSADATA d{};
        ok = (WSAStartup(MAKEWORD(2, 2), &d) == 0);
    }
    ~WsaInit() {
        if (ok) WSACleanup();
    }
};

bool ensure_wsa() {
    static WsaInit w;
    return w.ok;
}

}  // namespace

bool Endpoint::parse(const std::string& ip, uint16_t p, Endpoint& out) {
    ensure_wsa();
    in_addr addr{};
    if (ip.empty() || ip == "0.0.0.0") {
        addr.s_addr = INADDR_ANY;
    } else if (inet_pton(AF_INET, ip.c_str(), &addr) != 1) {
        return false;
    }
    out.ipv4_be = addr.s_addr;
    out.port = p;
    return true;
}

std::string Endpoint::to_string() const {
    char ipbuf[INET_ADDRSTRLEN]{};
    in_addr addr{};
    addr.s_addr = ipv4_be;
    inet_ntop(AF_INET, &addr, ipbuf, sizeof(ipbuf));
    char out[64];
    std::snprintf(out, sizeof(out), "%s:%u", ipbuf, unsigned(port));
    return out;
}

UdpSocket::~UdpSocket() { close(); }

bool UdpSocket::is_open() const { return fd_ != INVALID_SOCKET; }

bool UdpSocket::open() {
    close();
    if (!ensure_wsa()) return false;
    SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return false;
    // Prevent ICMP Port Unreachable from causing WSAECONNRESET on subsequent recvfrom() calls.
    BOOL new_behavior = FALSE;
    DWORD bytes_returned = 0;
    constexpr DWORD kSioUdpConnreset = 0x9800000Cu;
    WSAIoctl(s, kSioUdpConnreset, &new_behavior, sizeof(new_behavior), nullptr, 0, &bytes_returned,
             nullptr, nullptr);
    fd_ = uintptr_t(s);
    cached_tos_.store(-1, std::memory_order_relaxed);
    return true;
}

void UdpSocket::close() {
    if (!is_open()) return;
    ::closesocket(SOCKET(fd_));
    fd_ = INVALID_SOCKET;
    cached_tos_.store(-1, std::memory_order_relaxed);
}

bool UdpSocket::bind(const std::string& ip, uint16_t port) {
    if (!is_open()) return false;
    Endpoint ep;
    if (!Endpoint::parse(ip, port, ep)) return false;

    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = ep.ipv4_be;
    sa.sin_port = htons(ep.port);
    return ::bind(SOCKET(fd_), reinterpret_cast<const sockaddr*>(&sa), sizeof(sa)) == 0;
}

uint16_t UdpSocket::local_port() const {
    if (!is_open()) return 0;
    sockaddr_in sa{};
    int len = sizeof(sa);
    if (::getsockname(SOCKET(fd_), reinterpret_cast<sockaddr*>(&sa), &len) != 0) return 0;
    return ntohs(sa.sin_port);
}

void UdpSocket::set_buffer_sizes(int send_bytes, int recv_bytes) {
    if (!is_open()) return;
    if (send_bytes > 0)
        ::setsockopt(SOCKET(fd_), SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&send_bytes),
                     sizeof(send_bytes));
    if (recv_bytes > 0)
        ::setsockopt(SOCKET(fd_), SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&recv_bytes),
                     sizeof(recv_bytes));
}

bool UdpSocket::set_tos(int tos_value) const {
    if (!is_open()) return false;
    if (cached_tos_.load(std::memory_order_relaxed) == tos_value) return true;
    int tos = tos_value;
    const bool ok =
        ::setsockopt(SOCKET(fd_), IPPROTO_IP, IP_TOS, reinterpret_cast<const char*>(&tos), sizeof(tos)) == 0;
#if defined(IPV6_TCLASS)
    int tclass = tos_value;
    (void)::setsockopt(SOCKET(fd_), IPPROTO_IPV6, IPV6_TCLASS, reinterpret_cast<const char*>(&tclass), sizeof(tclass));
#endif
    if (ok) cached_tos_.store(tos_value, std::memory_order_relaxed);
    return ok;
}

bool UdpSocket::set_qos_class(QosTrafficClass cls) const {
    switch (cls) {
        case QosTrafficClass::kSyncControl:
            // Priority hierarchy: CS7 (0xE0) -> CS6 (0xC0) -> EF (0xB8, highest unprivileged WMM AC_VO on Windows).
            return set_tos(kDscpCs7Tos) || set_tos(kDscpCs6Tos) || set_tos(kDscpEfTos);
        case QosTrafficClass::kAudio:
            return set_tos(kDscpEfTos);
        case QosTrafficClass::kVideo:
            return set_tos(kDscpAf41Tos);
        case QosTrafficClass::kBestEffort:
        default:
            return set_tos(0);
    }
}

bool UdpSocket::set_low_latency_qos() {
    return set_qos_class(QosTrafficClass::kAudio);
}

int UdpSocket::tos() const {
    if (!is_open()) return -1;
    int v = 0;
    int l = sizeof(v);
    return ::getsockopt(SOCKET(fd_), IPPROTO_IP, IP_TOS, reinterpret_cast<char*>(&v), &l) == 0 ? v : -1;
}

int UdpSocket::send_buffer_bytes() const {
    int v = 0;
    int l = sizeof(v);
    return (is_open() && ::getsockopt(SOCKET(fd_), SOL_SOCKET, SO_SNDBUF, reinterpret_cast<char*>(&v), &l) == 0) ? v : -1;
}

int UdpSocket::recv_buffer_bytes() const {
    int v = 0;
    int l = sizeof(v);
    return (is_open() && ::getsockopt(SOCKET(fd_), SOL_SOCKET, SO_RCVBUF, reinterpret_cast<char*>(&v), &l) == 0) ? v : -1;
}

bool UdpSocket::set_recv_timeout_ms(int ms) {
    if (!is_open()) return false;
    DWORD tv = DWORD(ms < 0 ? 0 : ms);
    return ::setsockopt(SOCKET(fd_), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv),
                        sizeof(tv)) == 0;
}

bool UdpSocket::set_nonblocking(bool nb) {
    if (!is_open()) return false;
    u_long mode = nb ? 1u : 0u;
    return ::ioctlsocket(SOCKET(fd_), FIONBIO, &mode) == 0;
}

int UdpSocket::send_to(const uint8_t* data, size_t len, const Endpoint& dst) const {
    if (!is_open()) return -1;
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = dst.ipv4_be;
    sa.sin_port = htons(dst.port);
    const int n = ::sendto(SOCKET(fd_), reinterpret_cast<const char*>(data), int(len), 0,
                           reinterpret_cast<const sockaddr*>(&sa), sizeof(sa));
    return n == SOCKET_ERROR ? -1 : n;
}

int UdpSocket::send_to_qos(const uint8_t* data, size_t len, const Endpoint& dst, QosTrafficClass cls) const {
    (void)set_qos_class(cls);
    return send_to(data, len, dst);
}

int UdpSocket::send_batch_qos(const DatagramView* msgs, int count, const Endpoint& dst, QosTrafficClass cls) const {
    (void)set_qos_class(cls);
    return send_batch(msgs, count, dst);
}

int UdpSocket::recv_from(uint8_t* buf, size_t cap, Endpoint& src) const {
    if (!is_open()) return -1;
    sockaddr_in sa{};
    int salen = sizeof(sa);
    const int n = ::recvfrom(SOCKET(fd_), reinterpret_cast<char*>(buf), int(cap), 0,
                             reinterpret_cast<sockaddr*>(&sa), &salen);
    if (n == SOCKET_ERROR) {
        const int err = WSAGetLastError();
        if (err == WSAETIMEDOUT || err == WSAEWOULDBLOCK) return 0;
        return -1;
    }
    src.ipv4_be = sa.sin_addr.s_addr;
    src.port = ntohs(sa.sin_port);
    return int(n);
}

int UdpSocket::send_batch(const DatagramView* msgs, int count, const Endpoint& dst) const {
    if (!is_open() || !msgs || count <= 0) return 0;
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = dst.ipv4_be;
    sa.sin_port = htons(dst.port);
    int sent = 0;
    for (int i = 0; i < count; ++i) {
        const int n = ::sendto(SOCKET(fd_), reinterpret_cast<const char*>(msgs[i].data), int(msgs[i].len), 0,
                               reinterpret_cast<const sockaddr*>(&sa), sizeof(sa));
        if (n == int(msgs[i].len)) ++sent;
    }
    return sent;
}

int UdpSocket::recv_batch(RecvDatagram* msgs, int max_count) const {
    if (!is_open() || !msgs || max_count <= 0) return 0;
    // Receive first packet respecting SO_RCVTIMEO; drain additional queued packets via ioctlsocket FIONREAD.
    const int n0 = recv_from(msgs[0].data, msgs[0].cap, msgs[0].src);
    if (n0 <= 0) return n0;
    msgs[0].len = size_t(n0);
    int got = 1;
    while (got < max_count) {
        u_long avail = 0;
        if (::ioctlsocket(SOCKET(fd_), FIONREAD, &avail) != 0 || avail == 0) break;
        const int ni = recv_from(msgs[got].data, msgs[got].cap, msgs[got].src);
        if (ni <= 0) break;
        msgs[got].len = size_t(ni);
        ++got;
    }
    return got;
}

bool UdpSocket::set_broadcast(bool enable) {
    if (!is_open()) return false;
    BOOL opt = enable ? TRUE : FALSE;
    return ::setsockopt(SOCKET(fd_), SOL_SOCKET, SO_BROADCAST,
                        reinterpret_cast<const char*>(&opt), sizeof(opt)) == 0;
}

}  // namespace lv
