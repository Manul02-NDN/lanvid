#pragma once
// Canonical Huffman tables (JPEG Annex K standard tables) for encoding and fast decoding.

#include <cstdint>

#include "common/bitio.hpp"

namespace lv {

struct HuffSpec {
    uint8_t bits[16];      // number of codes of length 1..16
    const uint8_t* vals;   // symbols in code order
    int count;
};

struct HuffEncTable {
    uint32_t code[256];
    uint8_t len[256];      // 0 = symbol not present
};

constexpr int kHuffFastBits = 9;

struct HuffDecTable {
    uint16_t fast[1 << kHuffFastBits];  // (len << 8) | symbol, len == 0 -> slow path
    int32_t maxcode[18];                // largest code of length l, -1 if none
    int32_t valoffset[17];              // index into vals = code + valoffset[l]
    uint8_t vals[256];
};

bool build_huff_enc(const HuffSpec& spec, HuffEncTable& out);
bool build_huff_dec(const HuffSpec& spec, HuffDecTable& out);

// Standard tables: index 0 = luma, 1 = chroma.
struct StdHuffman {
    HuffSpec dc_spec[2], ac_spec[2];
    HuffEncTable enc_dc[2], enc_ac[2];
    HuffDecTable dec_dc[2], dec_ac[2];
};
const StdHuffman& std_huffman();

// Returns decoded symbol, or -1 for an invalid code. Requires >= 16 buffered bits (refill()).
LV_FORCE_INLINE int huff_decode(BitReader& br, const HuffDecTable& t) {
    const uint16_t e = t.fast[br.peek(kHuffFastBits)];
    if (e >> 8) {
        br.skip(e >> 8);
        return e & 0xFF;
    }
    for (int l = kHuffFastBits + 1; l <= 16; ++l) {
        const int32_t c = int32_t(br.peek(l));
        if (c <= t.maxcode[l]) {
            br.skip(l);
            return t.vals[c + t.valoffset[l]];
        }
    }
    return -1;
}

}  // namespace lv
