#pragma once
// Fragments an encoded slice into MTU-sized LVSP v1 UDP datagrams with optional slice-level XOR FEC.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>

#include "net/packet.hpp"

namespace lv {

struct PacketizerConfig {
    size_t mtu = kDefaultMtu;    // max UDP payload size including the 28-byte LVSP header
    bool enable_crc = true;
    uint16_t fec_group_size = 0; // 0 = FEC disabled; >0 (e.g. 6 or 8) = 1 XOR parity per group
};

// Emits data packets (and XOR FEC parity packets when fec_group_size > 0) for `slice_data`.
// Thread-safe: uses stack buffers for standard MTU and atomically increments `seq_counter`.
// Returns total number of packets emitted (data + FEC parity; 0 on invalid input).
uint16_t packetize_slice(uint32_t frame_id, uint32_t timestamp_us, uint16_t slice_id, uint16_t total_slices,
                         const uint8_t* slice_data, size_t slice_size, const PacketizerConfig& cfg,
                         std::atomic<uint16_t>& seq_counter,
                         const std::function<void(const uint8_t* dgram, size_t len)>& on_packet);

}  // namespace lv
