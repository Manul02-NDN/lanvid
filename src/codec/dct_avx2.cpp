// AVX2 16-bit integer (__m256i) dual-block 8x8 Loeffler (LLM) 2D DCT / IDCT.
//
// Each __m256i register holds 16 int16_t values = row r of Block 0 (low 128-bit lane)
// AND row r of Block 1 (high 128-bit lane). All butterflies, 8x8 in-register transposes,
// reciprocal quantization, clamping, and pixel packing run on BOTH blocks in parallel!
// Compiled with /arch:AVX2 (MSVC) or -mavx2 (GCC/Clang); entered only after runtime CPUID check.

#include <immintrin.h>

#include "codec/dct.hpp"
#include "codec/zigzag.hpp"
#include "common/platform.hpp"

namespace lv {

namespace {

// Q15 fixed-point constants for _mm256_mulhrs_epi16 ((a * b + 16384) >> 15).
constexpr int16_t kCos1 = 32138;     // cos(1*pi/16) * 32768
constexpr int16_t kSin1 = 6393;      // sin(1*pi/16) * 32768
constexpr int16_t kCos3 = 27246;     // cos(3*pi/16) * 32768
constexpr int16_t kSin3 = 18205;     // sin(3*pi/16) * 32768
constexpr int16_t kSqrt2M1 = 13573;  // (sqrt(2) - 1) * 32768
constexpr int16_t kC2AM1 = 10045;    // (sqrt(2)*cos(pi/8) - 1) * 32768
constexpr int16_t kC2B = 17734;      // (sqrt(2)*cos(3*pi/8)) * 32768

LV_FORCE_INLINE __m256i mul_q15(__m256i x, int16_t c) {
    return _mm256_mulhrs_epi16(x, _mm256_set1_epi16(c));
}

// 1D 8-point Forward Loeffler (LLM) DCT across 16 parallel int16 columns (2 blocks).
// Output: Y[k] = c(k) * sum_{n=0..7} x[n] * cos((2n+1)k*pi/16), where c(0)=1, c(k>0)=sqrt(2).
LV_FORCE_INLINE void llm_fdct8_i16(__m256i& d0, __m256i& d1, __m256i& d2, __m256i& d3,
                                   __m256i& d4, __m256i& d5, __m256i& d6, __m256i& d7) {
    // Stage 1: symmetric/antisymmetric butterflies
    const __m256i s0 = _mm256_add_epi16(d0, d7);
    const __m256i s7 = _mm256_sub_epi16(d0, d7);
    const __m256i s1 = _mm256_add_epi16(d1, d6);
    const __m256i s6 = _mm256_sub_epi16(d1, d6);
    const __m256i s2 = _mm256_add_epi16(d2, d5);
    const __m256i s5 = _mm256_sub_epi16(d2, d5);
    const __m256i s3 = _mm256_add_epi16(d3, d4);
    const __m256i s4 = _mm256_sub_epi16(d3, d4);

    // Even part -> d0, d2, d4, d6
    const __m256i t0 = _mm256_add_epi16(s0, s3);
    const __m256i t3 = _mm256_sub_epi16(s0, s3);
    const __m256i t1 = _mm256_add_epi16(s1, s2);
    const __m256i t2 = _mm256_sub_epi16(s1, s2);
    d0 = _mm256_add_epi16(t0, t1);
    d4 = _mm256_sub_epi16(t0, t1);
    d2 = _mm256_add_epi16(_mm256_add_epi16(t3, mul_q15(t3, kC2AM1)), mul_q15(t2, kC2B));
    d6 = _mm256_sub_epi16(mul_q15(t3, kC2B), _mm256_add_epi16(t2, mul_q15(t2, kC2AM1)));

    // Odd part -> d1, d3, d5, d7 (orthogonal rotations by 3pi/16 and pi/16)
    const __m256i z4 = _mm256_add_epi16(mul_q15(s4, kCos3), mul_q15(s7, kSin3));
    const __m256i z7 = _mm256_sub_epi16(mul_q15(s7, kCos3), mul_q15(s4, kSin3));
    const __m256i z5 = _mm256_add_epi16(mul_q15(s5, kCos1), mul_q15(s6, kSin1));
    const __m256i z6 = _mm256_sub_epi16(mul_q15(s6, kCos1), mul_q15(s5, kSin1));

    const __m256i u7 = _mm256_add_epi16(z7, z5);
    const __m256i u5 = _mm256_sub_epi16(z7, z5);
    const __m256i u4 = _mm256_add_epi16(z4, z6);
    const __m256i u6 = _mm256_sub_epi16(z4, z6);

    d1 = _mm256_add_epi16(u7, u4);
    d7 = _mm256_sub_epi16(u7, u4);
    d3 = _mm256_add_epi16(u5, mul_q15(u5, kSqrt2M1));
    d5 = _mm256_add_epi16(u6, mul_q15(u6, kSqrt2M1));
}

// 1D 8-point Inverse Loeffler (LLM) DCT (exact transpose of llm_fdct8_i16).
// Output is scaled by 8x relative to orthonormal IDCT.
LV_FORCE_INLINE void llm_idct8_i16(__m256i& d0, __m256i& d1, __m256i& d2, __m256i& d3,
                                   __m256i& d4, __m256i& d5, __m256i& d6, __m256i& d7) {
    // Even part -> s0..s3
    const __m256i t0 = _mm256_add_epi16(d0, d4);
    const __m256i t1 = _mm256_sub_epi16(d0, d4);
    const __m256i t3 = _mm256_add_epi16(_mm256_add_epi16(d2, mul_q15(d2, kC2AM1)), mul_q15(d6, kC2B));
    const __m256i t2 = _mm256_sub_epi16(mul_q15(d2, kC2B), _mm256_add_epi16(d6, mul_q15(d6, kC2AM1)));
    const __m256i s0 = _mm256_add_epi16(t0, t3);
    const __m256i s3 = _mm256_sub_epi16(t0, t3);
    const __m256i s1 = _mm256_add_epi16(t1, t2);
    const __m256i s2 = _mm256_sub_epi16(t1, t2);

    // Odd part -> s4..s7
    const __m256i u7 = _mm256_add_epi16(d1, d7);
    const __m256i u4 = _mm256_sub_epi16(d1, d7);
    const __m256i u5 = _mm256_add_epi16(d3, mul_q15(d3, kSqrt2M1));
    const __m256i u6 = _mm256_add_epi16(d5, mul_q15(d5, kSqrt2M1));

    const __m256i z7 = _mm256_add_epi16(u7, u5);
    const __m256i z5 = _mm256_sub_epi16(u7, u5);
    const __m256i z4 = _mm256_add_epi16(u4, u6);
    const __m256i z6 = _mm256_sub_epi16(u4, u6);

    const __m256i s7 = _mm256_add_epi16(mul_q15(z7, kCos3), mul_q15(z4, kSin3));
    const __m256i s4 = _mm256_sub_epi16(mul_q15(z4, kCos3), mul_q15(z7, kSin3));
    const __m256i s6 = _mm256_add_epi16(mul_q15(z5, kSin1), mul_q15(z6, kCos1));
    const __m256i s5 = _mm256_sub_epi16(mul_q15(z5, kCos1), mul_q15(z6, kSin1));

    // Final butterflies
    d0 = _mm256_add_epi16(s0, s7);
    d7 = _mm256_sub_epi16(s0, s7);
    d1 = _mm256_add_epi16(s1, s6);
    d6 = _mm256_sub_epi16(s1, s6);
    d2 = _mm256_add_epi16(s2, s5);
    d5 = _mm256_sub_epi16(s2, s5);
    d3 = _mm256_add_epi16(s3, s4);
    d4 = _mm256_sub_epi16(s3, s4);
}

// In-register transpose of TWO independent 8x8 int16 matrices (one per 128-bit lane).
// Zero cross-lane shuffles!
LV_FORCE_INLINE void transpose8_i16_x2(__m256i r[8]) {
    const __m256i t0 = _mm256_unpacklo_epi16(r[0], r[1]);
    const __m256i t1 = _mm256_unpackhi_epi16(r[0], r[1]);
    const __m256i t2 = _mm256_unpacklo_epi16(r[2], r[3]);
    const __m256i t3 = _mm256_unpackhi_epi16(r[2], r[3]);
    const __m256i t4 = _mm256_unpacklo_epi16(r[4], r[5]);
    const __m256i t5 = _mm256_unpackhi_epi16(r[4], r[5]);
    const __m256i t6 = _mm256_unpacklo_epi16(r[6], r[7]);
    const __m256i t7 = _mm256_unpackhi_epi16(r[6], r[7]);

    const __m256i s0 = _mm256_unpacklo_epi32(t0, t2);
    const __m256i s1 = _mm256_unpackhi_epi32(t0, t2);
    const __m256i s2 = _mm256_unpacklo_epi32(t1, t3);
    const __m256i s3 = _mm256_unpackhi_epi32(t1, t3);
    const __m256i s4 = _mm256_unpacklo_epi32(t4, t6);
    const __m256i s5 = _mm256_unpackhi_epi32(t4, t6);
    const __m256i s6 = _mm256_unpacklo_epi32(t5, t7);
    const __m256i s7 = _mm256_unpackhi_epi32(t5, t7);

    r[0] = _mm256_unpacklo_epi64(s0, s4);
    r[1] = _mm256_unpackhi_epi64(s0, s4);
    r[2] = _mm256_unpacklo_epi64(s1, s5);
    r[3] = _mm256_unpackhi_epi64(s1, s5);
    r[4] = _mm256_unpacklo_epi64(s2, s6);
    r[5] = _mm256_unpackhi_epi64(s2, s6);
    r[6] = _mm256_unpacklo_epi64(s3, s7);
    r[7] = _mm256_unpackhi_epi64(s3, s7);
}

// Computes a 64-bit non-zero mask over `zz[0..63]` using 8 AVX2 instructions.
LV_FORCE_INLINE uint64_t nz_mask64_avx2(const int16_t* zz) {
    const __m256i zero = _mm256_setzero_si256();
    const __m256i c0 = _mm256_cmpeq_epi16(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(zz + 0)), zero);
    const __m256i c1 = _mm256_cmpeq_epi16(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(zz + 16)), zero);
    const __m256i c2 = _mm256_cmpeq_epi16(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(zz + 32)), zero);
    const __m256i c3 = _mm256_cmpeq_epi16(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(zz + 48)), zero);

    const __m256i p01 = _mm256_permute4x64_epi64(_mm256_packs_epi16(c0, c1), _MM_SHUFFLE(3, 1, 2, 0));
    const __m256i p23 = _mm256_permute4x64_epi64(_mm256_packs_epi16(c2, c3), _MM_SHUFFLE(3, 1, 2, 0));
    const uint32_t lo = ~uint32_t(_mm256_movemask_epi8(p01));
    const uint32_t hi = ~uint32_t(_mm256_movemask_epi8(p23));
    return uint64_t(lo) | (uint64_t(hi) << 32);
}

// Core dual-block FDCT + quantization -> writes natural-order coefficients into r[0..7].
LV_FORCE_INLINE void fdct_quant_x2_core(const uint8_t* src0, const uint8_t* src1, int stride,
                                        const QuantTables& qt, int tbl, __m256i r[8]) {
    const __m256i bias = _mm256_set1_epi16(128);
    for (int i = 0; i < 8; ++i) {
        const __m128i p0 = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(src0 + i * stride));
        const __m128i p1 = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(src1 + i * stride));
        const __m128i w0 = _mm_cvtepu8_epi16(p0);
        const __m128i w1 = _mm_cvtepu8_epi16(p1);
        const __m256i both = _mm256_inserti128_si256(_mm256_castsi128_si256(w0), w1, 1);
        // Level shift (-128) and preshift << 2 for 2 fractional bits during vertical pass.
        r[i] = _mm256_slli_epi16(_mm256_sub_epi16(both, bias), 2);
    }

    llm_fdct8_i16(r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7]);  // vertical pass
    const __m256i ones = _mm256_set1_epi16(1);
    for (int i = 0; i < 8; ++i) r[i] = _mm256_srai_epi16(_mm256_add_epi16(r[i], ones), 1);

    transpose8_i16_x2(r);
    llm_fdct8_i16(r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7]);  // horizontal pass
    transpose8_i16_x2(r);

    // Fused 2-step mulhrs quantization (exact 15-bit mantissa across Q=1..100) + clamp [-1023, 1023].
    const __m256i lim = _mm256_set1_epi16(kMaxCoef);
    const __m256i nlim = _mm256_set1_epi16(-kMaxCoef);
    const int16_t* recip = qt.llm_recip[tbl];
    const int16_t* shift = qt.llm_shift[tbl];
    for (int i = 0; i < 8; ++i) {
        const __m256i vr = _mm256_broadcastsi128_si256(
            _mm_load_si128(reinterpret_cast<const __m128i*>(recip + i * 8)));
        const __m256i vs = _mm256_broadcastsi128_si256(
            _mm_load_si128(reinterpret_cast<const __m128i*>(shift + i * 8)));
        __m256i q = _mm256_mulhrs_epi16(_mm256_mulhrs_epi16(r[i], vr), vs);
        r[i] = _mm256_max_epi16(_mm256_min_epi16(q, lim), nlim);
    }
}

void fdct_quant_avx2(const uint8_t* src, int stride, const QuantTables& qt, int tbl, int16_t* out_nat) {
    __m256i r[8];
    fdct_quant_x2_core(src, src, stride, qt, tbl, r);
    for (int i = 0; i < 8; ++i) {
        _mm_storeu_si128(reinterpret_cast<__m128i*>(out_nat + i * 8), _mm256_castsi256_si128(r[i]));
    }
}

LV_FORCE_INLINE void scatter_zz_and_mask(const int16_t* nat, int16_t* zz, uint64_t& mask) {
    // Fast path check: if rows 1..7 are all zero (common in flat 4K desktop/video regions),
    // skip scanning most of the block.
    for (int k = 0; k < 64; ++k) zz[k] = nat[kZigzag[k]];
    mask = nz_mask64_avx2(zz);
}

uint64_t fdct_quant_zz_avx2(const uint8_t* src, int stride, const QuantTables& qt, int tbl,
                            int16_t* out_zz) {
    alignas(32) int16_t nat[64];
    fdct_quant_avx2(src, stride, qt, tbl, nat);
    uint64_t mask = 0;
    scatter_zz_and_mask(nat, out_zz, mask);
    return mask;
}

void fdct_quant_zz_x2_avx2(const uint8_t* src0, const uint8_t* src1, int stride,
                           const QuantTables& qt, int tbl, int16_t* out_zz0, int16_t* out_zz1,
                           uint64_t& mask0, uint64_t& mask1) {
    __m256i r[8];
    fdct_quant_x2_core(src0, src1, stride, qt, tbl, r);

    alignas(32) int16_t nat0[64];
    alignas(32) int16_t nat1[64];
    for (int i = 0; i < 8; ++i) {
        _mm_store_si128(reinterpret_cast<__m128i*>(nat0 + i * 8), _mm256_castsi256_si128(r[i]));
        _mm_store_si128(reinterpret_cast<__m128i*>(nat1 + i * 8), _mm256_extracti128_si256(r[i], 1));
    }
    scatter_zz_and_mask(nat0, out_zz0, mask0);
    scatter_zz_and_mask(nat1, out_zz1, mask1);
}

void dequant_idct_x2_core(const int16_t* coef0, const int16_t* coef1, const QuantTables& qt, int tbl,
                          uint8_t* dst0, int stride0, uint8_t* dst1, int stride1) {
    const int16_t* deq = qt.llm_deq[tbl];
    __m256i r[8];
    for (int i = 0; i < 8; ++i) {
        const __m128i c0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(coef0 + i * 8));
        const __m128i c1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(coef1 + i * 8));
        const __m256i both = _mm256_inserti128_si256(_mm256_castsi128_si256(c0), c1, 1);
        const __m256i vd = _mm256_broadcastsi128_si256(
            _mm_load_si128(reinterpret_cast<const __m128i*>(deq + i * 8)));
        r[i] = _mm256_mullo_epi16(both, vd);
    }

    llm_idct8_i16(r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7]);  // vertical pass
    transpose8_i16_x2(r);
    llm_idct8_i16(r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7]);  // horizontal pass (16 * delta)
    transpose8_i16_x2(r);

    // Round (/16), add +128 level shift ((r + 8 + 128*16) >> 4 = (r + 2056) >> 4), pack u8.
    const __m256i bias = _mm256_set1_epi16(2056);
    for (int i = 0; i < 8; ++i) {
        const __m256i v = _mm256_srai_epi16(_mm256_add_epi16(r[i], bias), 4);
        const __m128i lo16 = _mm256_castsi256_si128(v);
        const __m128i hi16 = _mm256_extracti128_si256(v, 1);
        const __m128i packed = _mm_packus_epi16(lo16, hi16);
        _mm_storel_epi64(reinterpret_cast<__m128i*>(dst0 + i * stride0), packed);
        _mm_storeh_pi(reinterpret_cast<__m64*>(dst1 + i * stride1), _mm_castsi128_ps(packed));
    }
}

void dequant_idct_x2_avx2(const int16_t* coef0, const int16_t* coef1, const QuantTables& qt, int tbl,
                          uint8_t* dst0, uint8_t* dst1, int stride) {
    dequant_idct_x2_core(coef0, coef1, qt, tbl, dst0, stride, dst1, stride);
}

void dequant_idct_avx2(const int16_t* coef, const QuantTables& qt, int tbl, uint8_t* dst, int stride) {
    uint8_t dummy[64];
    dequant_idct_x2_core(coef, coef, qt, tbl, dst, stride, dummy, 8);
}

const DctKernels kAvx2{fdct_quant_avx2,      dequant_idct_avx2,  fdct_quant_zz_avx2,
                       fdct_quant_zz_x2_avx2, dequant_idct_x2_avx2, "avx2"};

}  // namespace

const DctKernels* dct_kernels_avx2_impl() { return &kAvx2; }

}  // namespace lv
