#include "codec/entropy.hpp"

#include <bit>

#include "codec/zigzag.hpp"

namespace lv {

namespace {

// JPEG "size category" + raw value bits (negative values stored as v-1 in s bits).
LV_FORCE_INLINE void value_bits(int v, int& size, uint32_t& bits) {
    const int sign = v >> 31;  // 0 or -1
    const unsigned mag = unsigned((v ^ sign) - sign);
    size = int(std::bit_width(mag));
    bits = uint32_t(v + sign) & ((1u << size) - 1u);
}

LV_FORCE_INLINE int extend(uint32_t v, int s) {
    return v < (1u << (s - 1)) ? int(v) - (1 << s) + 1 : int(v);
}

}  // namespace

void encode_block_zz(BitWriter& bw, const int16_t* zz, uint64_t nz_mask, int& dc_pred,
                     const HuffEncTable& dc, const HuffEncTable& ac) {
    // DC
    const int diff = zz[0] - dc_pred;
    dc_pred = zz[0];
    int s;
    uint32_t bits;
    value_bits(diff, s, bits);
    bw.put((dc.code[s] << s) | bits, dc.len[s] + s);

    // AC: iterate over non-zero coefficients only via bit scan.
    uint64_t mask = nz_mask & ~uint64_t(1);
    int last = 0;
    const uint32_t zrl_code = ac.code[0xF0];
    const int zrl_len = ac.len[0xF0];
    while (mask) {
        const int k = std::countr_zero(mask);
        mask &= mask - 1;
        int run = k - last - 1;
        while (run >= 16) {
            bw.put(zrl_code, zrl_len);
            run -= 16;
        }
        value_bits(zz[k], s, bits);
        const int sym = (run << 4) | s;
        bw.put((ac.code[sym] << s) | bits, ac.len[sym] + s);  // <= 16 + 10 bits
        last = k;
    }
    if (last != 63) bw.put(ac.code[0x00], ac.len[0x00]);  // EOB
}

void encode_block(BitWriter& bw, const int16_t* nat, int& dc_pred, const HuffEncTable& dc,
                  const HuffEncTable& ac) {
    int16_t zz[64];
    uint64_t mask = 0;
    for (int k = 0; k < 64; ++k) {
        const int16_t v = nat[kZigzag[k]];
        zz[k] = v;
        mask |= uint64_t(v != 0) << k;
    }
    encode_block_zz(bw, zz, mask, dc_pred, dc, ac);
}

bool decode_block(BitReader& br, int16_t* nat, int& dc_pred, const HuffDecTable& dc,
                  const HuffDecTable& ac) {
    br.refill();
    const int s = huff_decode(br, dc);
    if (s < 0 || s > 11) return false;
    if (s) dc_pred += extend(br.get(s), s);
    if (dc_pred < -2047 || dc_pred > 2047) return false;
    nat[0] = int16_t(dc_pred);

    for (int k = 1; k < 64;) {
        br.refill();
        const int rs = huff_decode(br, ac);
        if (rs < 0) return false;
        const int r = rs >> 4, sz = rs & 15;
        if (sz == 0) {
            if (r == 0) break;       // EOB
            if (r != 15) return false;
            k += 16;                 // ZRL
            if (k > 63) return false;
            continue;
        }
        k += r;
        if (k > 63) return false;
        nat[kZigzag[k]] = int16_t(extend(br.get(sz), sz));
        ++k;
    }
    return true;
}

}  // namespace lv
