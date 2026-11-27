#pragma once
// l2_transport.hpp — Cross-platform Layer 2 Raw Ethernet Transport for CrossLink Grid:
//   - Windows: dynamic Npcap (wpcap.dll, pcap_setmintocopy=0) bypassing TCP/IP stack (< 1 ms)
//   - Linux:   AF_PACKET SOCK_RAW + SO_PRIORITY=6 (zero-overhead direct NIC access)
//   - macOS:   /dev/bpf (BIOCIMMEDIATE + BIOCSHDRCMPLT, no kext required)
//   - Switch:  In-process L2LoopbackSwitch for multi-node deterministic testing & local DSP racks

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "grid/l2_protocol.hpp"

namespace cl::grid {

struct L2AdapterInfo {
    std::string device_name;    // OS identifier (e.g. "\Device\NPF_{GUID}" or "eth0" / "en0")
    std::string friendly_name;  // Human-readable name (e.g. "Ethernet — Intel I225-V")
    MacAddress mac{};
    bool is_ethernet = true;
    bool link_up = false;
    uint32_t speed_mbps = 0;
};

// Enumerates local Ethernet/network interfaces with their physical MAC addresses.
std::vector<L2AdapterInfo> list_l2_adapters();
// Best adapter for the Grid when the user has not picked one: a wired Ethernet with carrier, fastest first
// (macOS also lists virtual "Ethernet Adapter" / Thunderbolt ports that are UP with 0 Mbps), then any
// Ethernet that is up, then any link that is up. Returns false if `adapters` is empty.
bool pick_default_l2_adapter(const std::vector<L2AdapterInfo>& adapters, L2AdapterInfo& out);

// Returns true if the native OS Layer 2 backend is ready (Npcap loaded on Windows, AF_PACKET/BPF on POSIX).
bool l2_native_backend_available(std::string* out_status = nullptr);

class L2Transport {
public:
    virtual ~L2Transport() = default;

    virtual bool is_open() const = 0;
    virtual void close() = 0;
    virtual MacAddress mac() const = 0;
    virtual std::string backend_name() const = 0;

    // Sends one complete IEEE 802.3 Ethernet frame (DstMAC + SrcMAC + [VLAN] + EtherType + payload).
    virtual bool send_frame(const uint8_t* frame, size_t len) = 0;

    // Non-blocking receive of one Ethernet frame. Returns byte length (> 0), 0 if none queued, < 0 on error.
    virtual int recv_frame(uint8_t* out_buf, size_t cap) = 0;
};

// Opens native OS Layer 2 transport on the specified adapter (Windows Npcap / Linux AF_PACKET / macOS BPF).
std::unique_ptr<L2Transport> create_native_l2_transport(const L2AdapterInfo& adapter, std::string* err = nullptr);

// -----------------------------------------------------------------------------
// L2LoopbackSwitch — Virtual Layer 2 Ethernet Switch (MAC learning + Broadcast)
// -----------------------------------------------------------------------------
class L2LoopbackSwitch : public std::enable_shared_from_this<L2LoopbackSwitch> {
public:
    struct Port {
        MacAddress mac{};
        std::atomic<bool> link_up{true};
        std::mutex mu;
        std::deque<std::vector<uint8_t>> rx_queue;
        std::atomic<uint64_t> tx_frames{0};
        std::atomic<uint64_t> rx_frames{0};
    };

    std::shared_ptr<Port> attach(const MacAddress& mac);
    void detach(const MacAddress& mac);
    void set_port_link(const MacAddress& mac, bool up);
    void forward(const MacAddress& src, const uint8_t* frame, size_t len);

    uint64_t total_switched_frames() const { return switched_frames_.load(std::memory_order_relaxed); }

private:
    std::mutex mu_;
    std::vector<std::shared_ptr<Port>> ports_;
    std::atomic<uint64_t> switched_frames_{0};
};

std::unique_ptr<L2Transport> create_loopback_l2_transport(std::shared_ptr<L2LoopbackSwitch> sw,
                                                          const MacAddress& mac);

}  // namespace cl::grid
