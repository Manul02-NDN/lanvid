// POSIX (Linux / macOS / BSD) UDP socket implementation.
// Uses sendmmsg() / recvmmsg() batch syscalls on Linux for minimal 4K60 syscall overhead;
// uses standard BSD sendto() / recvfrom() with graceful SO_SNDBUF/SO_RCVBUF negotiation on macOS/BSD.

#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "net/udp_socket.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace lv {

namespace {

void set_sockbuf_best_effort(int fd, int opt, int requested) {
    if (requested <= 0) return;
    // On macOS / restricted Linux sysctl, requesting 16 MB can fail; step down gracefully so we still
    // get the largest buffer allowed (e.g. 8 MB -> 4 MB -> 2 MB -> 1 MB) instead of default 256 KB.
    for (int sz = requested; sz >= 262144; sz /= 2) {
        if (::setsockopt(fd, SOL_SOCKET, opt, &sz, sizeof(sz)) == 0) return;
    }
}

}  // namespace

bool Endpoint::parse(const std::string& ip, uint16_t p, Endpoint& out) {
    in_addr addr{};
    if (ip.empty() || ip == "0.0.0.0") {
        addr.s_addr = INADDR_ANY;
    } else if (::inet_pton(AF_INET, ip.c_str(), &addr) != 1) {
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
    ::inet_ntop(AF_INET, &addr, ipbuf, sizeof(ipbuf));
    char out[64];
    std::snprintf(out, sizeof(out), "%s:%u", ipbuf, unsigned(port));
    return out;
}

UdpSocket::~UdpSocket() { close(); }

bool UdpSocket::is_open() const { return fd_ >= 0; }

bool UdpSocket::open() {
    close();
    const int s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) return false;
    fd_ = s;
    return true;
}

void UdpSocket::close() {
    if (!is_open()) return;
    ::close(fd_);
    fd_ = -1;
}

bool UdpSocket::bind(const std::string& ip, uint16_t port) {
    if (!is_open()) return false;
    Endpoint ep;
    if (!Endpoint::parse(ip, port, ep)) return false;

    int reuse = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = ep.ipv4_be;
    sa.sin_port = htons(ep.port);
    return ::bind(fd_, reinterpret_cast<const sockaddr*>(&sa), sizeof(sa)) == 0;
}

uint16_t UdpSocket::local_port() const {
    if (!is_open()) return 0;
    sockaddr_in sa{};
    socklen_t len = sizeof(sa);
    if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&sa), &len) != 0) return 0;
    return ntohs(sa.sin_port);
}

void UdpSocket::set_buffer_sizes(int send_bytes, int recv_bytes) {
    if (!is_open()) return;
    set_sockbuf_best_effort(fd_, SO_SNDBUF, send_bytes);
    set_sockbuf_best_effort(fd_, SO_RCVBUF, recv_bytes);
}

bool UdpSocket::set_low_latency_qos() {
    if (!is_open()) return false;
    int tos = kDscpEfTos;
    const bool ok = ::setsockopt(fd_, IPPROTO_IP, IP_TOS, &tos, sizeof(tos)) == 0;
#if defined(IPV6_TCLASS)
    int tclass = kDscpEfTos;  // only effective on IPv6 sockets (ours are AF_INET); harmless otherwise
    (void)::setsockopt(fd_, IPPROTO_IPV6, IPV6_TCLASS, &tclass, sizeof(tclass));
#endif
#if defined(__APPLE__) && defined(SO_NET_SERVICE_TYPE)
    int svc = NET_SERVICE_TYPE_VO;  // Apple maps this to the Wi-Fi voice access category (AC_VO)
    (void)::setsockopt(fd_, SOL_SOCKET, SO_NET_SERVICE_TYPE, &svc, sizeof(svc));
#endif
    return ok;
}

int UdpSocket::tos() const {
    if (!is_open()) return -1;
    int v = 0;
    socklen_t l = sizeof(v);
    return ::getsockopt(fd_, IPPROTO_IP, IP_TOS, &v, &l) == 0 ? v : -1;
}

int UdpSocket::send_buffer_bytes() const {
    int v = 0;
    socklen_t l = sizeof(v);
    return (is_open() && ::getsockopt(fd_, SOL_SOCKET, SO_SNDBUF, &v, &l) == 0) ? v : -1;
}

int UdpSocket::recv_buffer_bytes() const {
    int v = 0;
    socklen_t l = sizeof(v);
    return (is_open() && ::getsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &v, &l) == 0) ? v : -1;
}

bool UdpSocket::set_recv_timeout_ms(int ms) {
    if (!is_open()) return false;
    if (ms < 0) ms = 0;
    timeval tv{};
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    return ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0;
}

bool UdpSocket::set_nonblocking(bool nb) {
    if (!is_open()) return false;
    const int fl = ::fcntl(fd_, F_GETFL, 0);
    if (fl < 0) return false;
    return ::fcntl(fd_, F_SETFL, nb ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK)) == 0;
}

int UdpSocket::send_to(const uint8_t* data, size_t len, const Endpoint& dst) const {
    if (!is_open()) return -1;
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = dst.ipv4_be;
    sa.sin_port = htons(dst.port);
    return int(::sendto(fd_, data, len, 0, reinterpret_cast<const sockaddr*>(&sa), sizeof(sa)));
}

int UdpSocket::recv_from(uint8_t* buf, size_t cap, Endpoint& src) const {
    if (!is_open()) return -1;
    sockaddr_in sa{};
    socklen_t salen = sizeof(sa);
    const ssize_t n = ::recvfrom(fd_, buf, cap, 0, reinterpret_cast<sockaddr*>(&sa), &salen);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
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

#if defined(__linux__)
    constexpr int kMaxStack = 64;
    int total_sent = 0;
    int off = 0;
    while (off < count) {
        const int batch = std::min(count - off, kMaxStack);
        mmsghdr hdrs[kMaxStack]{};
        iovec iovs[kMaxStack]{};
        for (int i = 0; i < batch; ++i) {
            iovs[i].iov_base = const_cast<uint8_t*>(msgs[off + i].data);
            iovs[i].iov_len = msgs[off + i].len;
            hdrs[i].msg_hdr.msg_name = &sa;
            hdrs[i].msg_hdr.msg_namelen = sizeof(sa);
            hdrs[i].msg_hdr.msg_iov = &iovs[i];
            hdrs[i].msg_hdr.msg_iovlen = 1;
        }
        const int sent = ::sendmmsg(fd_, hdrs, unsigned(batch), 0);
        if (sent <= 0) break;
        total_sent += sent;
        off += sent;
    }
    return total_sent;
#else
    int sent = 0;
    for (int i = 0; i < count; ++i) {
        const ssize_t n = ::sendto(fd_, msgs[i].data, msgs[i].len, 0,
                                   reinterpret_cast<const sockaddr*>(&sa), sizeof(sa));
        if (n == ssize_t(msgs[i].len)) ++sent;
    }
    return sent;
#endif
}

int UdpSocket::recv_batch(RecvDatagram* msgs, int max_count) const {
    if (!is_open() || !msgs || max_count <= 0) return 0;

#if defined(__linux__)
    constexpr int kMaxStack = 64;
    const int batch = std::min(max_count, kMaxStack);
    mmsghdr hdrs[kMaxStack]{};
    iovec iovs[kMaxStack]{};
    sockaddr_in addrs[kMaxStack]{};
    for (int i = 0; i < batch; ++i) {
        iovs[i].iov_base = msgs[i].data;
        iovs[i].iov_len = msgs[i].cap;
        hdrs[i].msg_hdr.msg_name = &addrs[i];
        hdrs[i].msg_hdr.msg_namelen = sizeof(addrs[i]);
        hdrs[i].msg_hdr.msg_iov = &iovs[i];
        hdrs[i].msg_hdr.msg_iovlen = 1;
    }
    // MSG_WAITFORONE waits for the first packet (respecting SO_RCVTIMEO), then immediately returns
    // all already-queued datagrams in one syscall without stalling for remaining slots!
    const int got = ::recvmmsg(fd_, hdrs, unsigned(batch), MSG_WAITFORONE, nullptr);
    if (got < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
        return -1;
    }
    for (int i = 0; i < got; ++i) {
        msgs[i].len = size_t(hdrs[i].msg_len);
        msgs[i].src.ipv4_be = addrs[i].sin_addr.s_addr;
        msgs[i].src.port = ntohs(addrs[i].sin_port);
    }
    return got;
#else
    const int n0 = recv_from(msgs[0].data, msgs[0].cap, msgs[0].src);
    if (n0 <= 0) return n0;
    msgs[0].len = size_t(n0);
    int got = 1;
    while (got < max_count) {
        sockaddr_in sa{};
        socklen_t salen = sizeof(sa);
        const ssize_t ni = ::recvfrom(fd_, msgs[got].data, msgs[got].cap, MSG_DONTWAIT,
                                      reinterpret_cast<sockaddr*>(&sa), &salen);
        if (ni <= 0) break;
        msgs[got].len = size_t(ni);
        msgs[got].src.ipv4_be = sa.sin_addr.s_addr;
        msgs[got].src.port = ntohs(sa.sin_port);
        ++got;
    }
    return got;
#endif
}

}  // namespace lv
