#pragma once
// Cross-platform UDP socket abstraction:
//  - Windows: Winsock2 (udp_socket_win32.cpp)
//  - Linux:   POSIX + sendmmsg/recvmmsg batch I/O (udp_socket_posix.cpp)
//  - macOS/BSD: POSIX sockets with graceful SO_SNDBUF/SO_RCVBUF sizing (udp_socket_posix.cpp)

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

#include "common/common_ns.hpp"

namespace NET_COMMON_NS {

// Differentiated Wi-Fi / LAN WMM QoS classes:
//   - kSyncControl : DSCP CS7 (0xE0) / CS6 (0xC0) -> Clock Sync (PING/PONG), NACK, Control
//   - kAudio       : DSCP EF  (0xB8)              -> Real-time Audio (WMM AC_VO)
//   - kVideo       : DSCP AF41 (0x88)             -> Real-time Video (WMM AC_VI)
enum class QosTrafficClass : uint8_t {
    kBestEffort  = 0,
    kVideo       = 1,
    kAudio       = 2,
    kSyncControl = 3,
};

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
    // Binds to a specific local NIC IPv4 address (empty / "0.0.0.0" = OS default route) and sets
    // IP_MULTICAST_IF / IP_UNICAST_IF (Windows) / IP_BOUND_IF (Apple) / SO_BINDTODEVICE (Linux/Android)
    // so traffic strictly exits the selected physical Ethernet / Wi-Fi interface.
    bool bind_to_interface(const std::string& local_ip, uint16_t port = 0,
                           uint32_t if_index = 0, const std::string& if_name = "");
    uint16_t local_port() const;

    void set_buffer_sizes(int send_bytes, int recv_bytes);
    // Differentiated DSCP QoS tagging (IP_TOS / IPV6_TCLASS + Apple SO_NET_SERVICE_TYPE):
    //   kSyncControl -> CS7 (0xE0, fallback CS6 0xC0)
    //   kAudio       -> EF  (0xB8)
    //   kVideo       -> AF41 (0x88)
    bool set_tos(int tos_value) const;
    bool set_qos_class(QosTrafficClass cls) const;
    // Low-latency Audio QoS convenience alias: DSCP 46 / Expedited Forwarding (IP_TOS 0xB8 -> Wi-Fi WMM AC_VO).
    bool set_low_latency_qos();
    // Effective values as reported by the OS (for diagnostics / tests).
    int tos() const;
    int send_buffer_bytes() const;
    int recv_buffer_bytes() const;
    static constexpr int kDscpCs7Tos  = 0xE0;  // DSCP 56 (CS7 — Clock Sync / Control / NACK)
    static constexpr int kDscpCs6Tos  = 0xC0;  // DSCP 48 (CS6 — fallback if OS restricts CS7)
    static constexpr int kDscpEfTos   = 0xB8;  // DSCP 46 (EF  — Real-time Audio, WMM AC_VO)
    static constexpr int kDscpAf41Tos = 0x88;  // DSCP 34 (AF41 — Real-time Video, WMM AC_VI)
    static constexpr int kMinSocketBuffer = 2 * 1024 * 1024;
    bool set_recv_timeout_ms(int ms);
    bool set_nonblocking(bool nb);
    bool set_broadcast(bool enable);

    int send_to(const uint8_t* data, size_t len, const Endpoint& dst) const;
    int send_to_qos(const uint8_t* data, size_t len, const Endpoint& dst, QosTrafficClass cls) const;
    int recv_from(uint8_t* buf, size_t cap, Endpoint& src) const;

    // Batch send/recv (uses sendmmsg/recvmmsg on Linux; tight loops elsewhere).
    // Returns number of datagrams successfully sent / received (0 on timeout, -1 on fatal error).
    int send_batch(const DatagramView* msgs, int count, const Endpoint& dst) const;
    int send_batch_qos(const DatagramView* msgs, int count, const Endpoint& dst, QosTrafficClass cls) const;
    int recv_batch(RecvDatagram* msgs, int max_count) const;

private:
#if defined(_WIN32)
    uintptr_t fd_ = ~uintptr_t(0);
#else
    int fd_ = -1;
#endif
    mutable std::atomic<int> cached_tos_{-1};
};

}  // namespace NET_COMMON_NS
