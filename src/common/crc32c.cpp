#include "common/crc32c.hpp"

#include <cstring>

#include "common/cpu_features.hpp"

#if defined(_M_X64) || defined(__x86_64__)
#define LV_CRC_HW 1
#include <nmmintrin.h>
#endif

namespace lv {

namespace {

struct CrcTables {
    uint32_t t[4][256];
};

const CrcTables& sw_tables() {
    static const CrcTables tbl = [] {
        CrcTables res{};
        constexpr uint32_t kPoly = 0x82F63B78u;
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1u) ? (kPoly ^ (c >> 1)) : (c >> 1);
            res.t[0][i] = c;
        }
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = res.t[0][i];
            for (int k = 1; k < 4; ++k) {
                c = res.t[0][c & 0xFFu] ^ (c >> 8);
                res.t[k][i] = c;
            }
        }
        return res;
    }();
    return tbl;
}

#if LV_CRC_HW
#if defined(__GNUC__) || defined(__clang__)
__attribute__((target("sse4.2")))
#endif
uint32_t crc32c_hw(uint32_t prev_crc, const uint8_t* data, size_t len) {
    uint64_t crc = uint32_t(~prev_crc);
    while (len >= 8) {
        uint64_t w;
        std::memcpy(&w, data, 8);
        crc = _mm_crc32_u64(crc, w);
        data += 8;
        len -= 8;
    }
    uint32_t c32 = uint32_t(crc);
    while (len--) {
        c32 = _mm_crc32_u8(c32, *data++);
    }
    return ~c32;
}
#endif

}  // namespace

uint32_t crc32c_update_sw(uint32_t prev_crc, const uint8_t* data, size_t len) {
    const CrcTables& tbl = sw_tables();
    uint32_t crc = ~prev_crc;
    while (len >= 4) {
        uint32_t w;
        std::memcpy(&w, data, 4);
        crc ^= w;
        crc = tbl.t[3][crc & 0xFFu] ^ tbl.t[2][(crc >> 8) & 0xFFu] ^
              tbl.t[1][(crc >> 16) & 0xFFu] ^ tbl.t[0][(crc >> 24) & 0xFFu];
        data += 4;
        len -= 4;
    }
    while (len--) {
        crc = tbl.t[0][(crc ^ *data++) & 0xFFu] ^ (crc >> 8);
    }
    return ~crc;
}

uint32_t crc32c_update(uint32_t prev_crc, const uint8_t* data, size_t len) {
#if LV_CRC_HW
    if (cpu_has_sse42() && simd_enabled()) return crc32c_hw(prev_crc, data, len);
#endif
    return crc32c_update_sw(prev_crc, data, len);
}

}  // namespace lv
