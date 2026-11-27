#pragma once
// Block entropy coding: DC DPCM + zig-zag + AC run-length (run,size) symbols + Huffman.

#include <cstdint>

#include "codec/huffman.hpp"
#include "common/bitio.hpp"

namespace lv {

// Encodes 64 coefficients ALREADY in zig-zag order with precomputed 64-bit non-zero mask.
void encode_block_zz(BitWriter& bw, const int16_t* zz, uint64_t nz_mask, int& dc_pred,
                     const HuffEncTable& dc, const HuffEncTable& ac);

// `nat` = 64 quantized coefficients in natural order, each in [-1023, 1023].
void encode_block(BitWriter& bw, const int16_t* nat, int& dc_pred, const HuffEncTable& dc,
                  const HuffEncTable& ac);

// `nat` must be zero-initialized by the caller. Returns false on any bitstream error.
bool decode_block(BitReader& br, int16_t* nat, int& dc_pred, const HuffDecTable& dc,
                  const HuffDecTable& ac);

}  // namespace lv
