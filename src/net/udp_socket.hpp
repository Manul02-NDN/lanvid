#pragma once
// Cross-platform UDP socket abstraction:
//  - Windows: Winsock2 (udp_socket_win32.cpp)
//  - Linux:   POSIX + sendmmsg/recvmmsg batch I/O (udp_socket_posix.cpp)
//  - macOS/BSD: POSIX sockets with graceful SO_SNDBUF/SO_RCVBUF sizing (udp_socket_posix.cpp)

#include <cstddef>
#include <cstdint>
#include <string>

namespace lv {

struct Endpoint {
    uint32_t ipv4_be = 0;  // IPv4 address in network byte order
    uint16_t port = 0;     // host byte order

    static bool parse(const std::string& ip, uint16_t port, Endpoint& out);
    std::string to_string() const;
    bool valid() const { return port != 0; }
    bool operator==(const Endpoint& o) const { return ipv4_be == o.ipv4_be && port == o.port; }
};

struct DatagramView {
    const uint8_t* data = nullptr;
    size_t len = 0;
};

struct RecvDatagram {
    uint8_t* data = nullptr;
    size_t cap = 0;
    size_t len = 0;
    Endpoint src{};
};

class UdpSocket {
public:
    UdpSocket() = default;
    ~UdpSocket();

    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    bool open();
    void close();
    bool is_open() const;

    bool bind(const std::string& ip, uint16_t port);
    uint16_t local_port() const;

    void set_buffer_sizes(int send_bytes, int recv_bytes);
    // Low-latency QoS: DSCP 46 / Expedited Forwarding (IP_TOS 0xB8 -> Wi-Fi WMM AC_VO), plus
    // SO_NET_SERVICE_TYPE = NET_SERVICE_TYPE_VO on Apple platforms. Returns true if the TOS was set.
    bool set_low_latency_qos();
    // Effective values as reported by the OS (for diagnostics / tests).
    int tos() const;
    int send_buffer_bytes() const;
    int recv_buffer_bytes() const;
    static constexpr int kDscpEfTos = 0xB8;
    static constexpr int kMinSocketBuffer = 2 * 1024 * 1024;
    bool set_recv_timeout_ms(int ms);
    bool set_nonblocking(bool nb);
    bool set_broadcast(bool enable);

    int send_to(const uint8_t* data, size_t len, const Endpoint& dst) const;
    int recv_from(uint8_t* buf, size_t cap, Endpoint& src) const;

    // Batch send/recv (uses sendmmsg/recvmmsg on Linux; tight loops elsewhere).
    // Returns number of datagrams successfully sent / received (0 on timeout, -1 on fatal error).
    int send_batch(const DatagramView* msgs, int count, const Endpoint& dst) const;
    int recv_batch(RecvDatagram* msgs, int max_count) const;

private:
#if defined(_WIN32)
    uintptr_t fd_ = ~uintptr_t(0);
#else
    int fd_ = -1;
#endif
};

}  // namespace lv
