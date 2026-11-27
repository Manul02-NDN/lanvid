#pragma once
// Platform helpers: inlining hints, explicit little-endian (de)serialization.

#include <cstddef>
#include <cstdint>

#include "common/common_ns.hpp"

#if defined(_MSC_VER)
#define NET_FORCE_INLINE __forceinline
#define NET_RESTRICT __restrict
#else
#define NET_FORCE_INLINE inline __attribute__((always_inline))
#define NET_RESTRICT __restrict__
#endif

#ifndef LV_FORCE_INLINE
#define LV_FORCE_INLINE NET_FORCE_INLINE
#endif
#ifndef LV_RESTRICT
#define LV_RESTRICT NET_RESTRICT
#endif

namespace NET_COMMON_NS {

NET_FORCE_INLINE void store_le16(uint8_t* p, uint16_t v) {
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
}
NET_FORCE_INLINE void store_le32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
    p[2] = uint8_t(v >> 16);
    p[3] = uint8_t(v >> 24);
}
NET_FORCE_INLINE void store_le64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = uint8_t(v >> (8 * i));
}
NET_FORCE_INLINE uint16_t load_le16(const uint8_t* p) {
    return uint16_t(p[0] | (p[1] << 8));
}
NET_FORCE_INLINE uint32_t load_le32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
NET_FORCE_INLINE uint64_t load_le64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= uint64_t(p[i]) << (8 * i);
    return v;
}

template <class T>
NET_FORCE_INLINE T clamp_val(T v, T lo, T hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

NET_FORCE_INLINE uint8_t clamp_u8(int v) {
    return uint8_t(v < 0 ? 0 : (v > 255 ? 255 : v));
}

}  // namespace NET_COMMON_NS
