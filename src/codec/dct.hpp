#pragma once
// 8x8 forward/inverse 2D DCT + quantization + optional fused zig-zag & dual-block AVX2 int16 path.
//
// Scalar path: float32 AAN algorithm.
// AVX2 path:   16-bit integer (__m256i) dual-block Loeffler (LLM) DCT/IDCT with in-register
//              8x8 transpose, fused 2-step mulhrs quantization, zig-zag output, and nz_mask.

#include <cstdint>

#include "codec/quant.hpp"

namespace lv {

// Quantized coefficient magnitude limit (fits JPEG size categories: AC <= 10, DC diff <= 11).
constexpr int kMaxCoef = 1023;

// Single-block natural-order interface (used by unit tests and odd/1-block paths).
using FdctQuantFn = void (*)(const uint8_t* src, int stride, const QuantTables& qt, int tbl,
                             int16_t* out_nat);
using DequantIdctFn = void (*)(const int16_t* coef_nat, const QuantTables& qt, int tbl,
                               uint8_t* dst, int stride);

// Fused zig-zag + non-zero mask interfaces (hot path for slice_codec).
// Outputs quantized coefficients directly in zig-zag order `zz[64]` and returns 64-bit nz_mask.
using FdctQuantZzFn = uint64_t (*)(const uint8_t* src, int stride, const QuantTables& qt, int tbl,
                                   int16_t* out_zz);

// Dual-block fused interfaces: process 2 independent 8x8 blocks simultaneously in __m256i!
using FdctQuantZzX2Fn = void (*)(const uint8_t* src0, const uint8_t* src1, int stride,
                                 const QuantTables& qt, int tbl, int16_t* out_zz0, int16_t* out_zz1,
                                 uint64_t& mask0, uint64_t& mask1);
using DequantIdctX2Fn = void (*)(const int16_t* coef0, const int16_t* coef1, const QuantTables& qt,
                                 int tbl, uint8_t* dst0, uint8_t* dst1, int stride);

struct DctKernels {
    FdctQuantFn fdct_quant;
    DequantIdctFn dequant_idct;
    FdctQuantZzFn fdct_quant_zz;
    FdctQuantZzX2Fn fdct_quant_zz_x2;
    DequantIdctX2Fn dequant_idct_x2;
    const char* name;
};

const DctKernels& dct_kernels_scalar();
const DctKernels* dct_kernels_avx2();  // nullptr if not compiled in / not supported by CPU
const DctKernels* dct_kernels_neon();  // nullptr if not compiled in / not ARM64
const DctKernels& dct_kernels_best();  // honours simd_enabled()

}  // namespace lv
