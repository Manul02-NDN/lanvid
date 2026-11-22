#pragma once
// CRC32C (Castagnoli, poly 0x82F63B78) with hardware SSE4.2 acceleration and SW fallback.

#include <cstddef>
#include <cstdint>

namespace lv {

// Incremental update (pass initial crc = 0, then feed chunks).
uint32_t crc32c_update(uint32_t prev_crc, const uint8_t* data, size_t len);
uint32_t crc32c_update_sw(uint32_t prev_crc, const uint8_t* data, size_t len);

inline uint32_t crc32c(const uint8_t* data, size_t len) { return crc32c_update(0, data, len); }

}  // namespace lv
