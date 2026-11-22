// ARM64 NEON 16-bit integer (int16x8_t) 8x8 Loeffler (LLM) 2D DCT / IDCT.
// Mathematically identical to dct_avx2.cpp (same Q15 constants via vqrdmulhq_s16, same quantization
// tables -> 100% bitstream-compatible between x86_64 and ARM64 / Apple Silicon).

#if defined(__aarch64__) || defined(_M_ARM64)

#include <arm_neon.h>

#include "codec/dct.hpp"
#include "codec/zigzag.hpp"
#include "common/platform.hpp"

namespace lv {

namespace {

constexpr int16_t kCos1 = 32138;
constexpr int16_t kSin1 = 6393;
constexpr int16_t kCos3 = 27246;
constexpr int16_t kSin3 = 18205;
constexpr int16_t kSqrt2M1 = 13573;
constexpr int16_t kC2AM1 = 10045;
constexpr int16_t kC2B = 17734;

LV_FORCE_INLINE int16x8_t mul_q15(int16x8_t x, int16_t c) { return vqrdmulhq_s16(x, vdupq_n_s16(c)); }

LV_FORCE_INLINE void llm_fdct8_neon(int16x8_t& d0, int16x8_t& d1, int16x8_t& d2, int16x8_t& d3,
                                    int16x8_t& d4, int16x8_t& d5, int16x8_t& d6, int16x8_t& d7) {
    const int16x8_t s0 = vaddq_s16(d0, d7), s7 = vsubq_s16(d0, d7);
    const int16x8_t s1 = vaddq_s16(d1, d6), s6 = vsubq_s16(d1, d6);
    const int16x8_t s2 = vaddq_s16(d2, d5), s5 = vsubq_s16(d2, d5);
    const int16x8_t s3 = vaddq_s16(d3, d4), s4 = vsubq_s16(d3, d4);

    const int16x8_t t0 = vaddq_s16(s0, s3), t3 = vsubq_s16(s0, s3);
    const int16x8_t t1 = vaddq_s16(s1, s2), t2 = vsubq_s16(s1, s2);
    d0 = vaddq_s16(t0, t1);
    d4 = vsubq_s16(t0, t1);
    d2 = vaddq_s16(vaddq_s16(t3, mul_q15(t3, kC2AM1)), mul_q15(t2, kC2B));
    d6 = vsubq_s16(mul_q15(t3, kC2B), vaddq_s16(t2, mul_q15(t2, kC2AM1)));

    const int16x8_t z4 = vaddq_s16(mul_q15(s4, kCos3), mul_q15(s7, kSin3));
    const int16x8_t z7 = vsubq_s16(mul_q15(s7, kCos3), mul_q15(s4, kSin3));
    const int16x8_t z5 = vaddq_s16(mul_q15(s5, kCos1), mul_q15(s6, kSin1));
    const int16x8_t z6 = vsubq_s16(mul_q15(s6, kCos1), mul_q15(s5, kSin1));

    const int16x8_t u7 = vaddq_s16(z7, z5), u5 = vsubq_s16(z7, z5);
    const int16x8_t u4 = vaddq_s16(z4, z6), u6 = vsubq_s16(z4, z6);

    d1 = vaddq_s16(u7, u4);
    d7 = vsubq_s16(u7, u4);
    d3 = vaddq_s16(u5, mul_q15(u5, kSqrt2M1));
    d5 = vaddq_s16(u6, mul_q15(u6, kSqrt2M1));
}

LV_FORCE_INLINE void llm_idct8_neon(int16x8_t& d0, int16x8_t& d1, int16x8_t& d2, int16x8_t& d3,
                                    int16x8_t& d4, int16x8_t& d5, int16x8_t& d6, int16x8_t& d7) {
    const int16x8_t t0 = vaddq_s16(d0, d4), t1 = vsubq_s16(d0, d4);
    const int16x8_t t3 = vaddq_s16(vaddq_s16(d2, mul_q15(d2, kC2AM1)), mul_q15(d6, kC2B));
    const int16x8_t t2 = vsubq_s16(mul_q15(d2, kC2B), vaddq_s16(d6, mul_q15(d6, kC2AM1)));
    const int16x8_t s0 = vaddq_s16(t0, t3), s3 = vsubq_s16(t0, t3);
    const int16x8_t s1 = vaddq_s16(t1, t2), s2 = vsubq_s16(t1, t2);

    const int16x8_t u7 = vaddq_s16(d1, d7), u4 = vsubq_s16(d1, d7);
    const int16x8_t u5 = vaddq_s16(d3, mul_q15(d3, kSqrt2M1));
    const int16x8_t u6 = vaddq_s16(d5, mul_q15(d5, kSqrt2M1));

    const int16x8_t z7 = vaddq_s16(u7, u5), z5 = vsubq_s16(u7, u5);
    const int16x8_t z4 = vaddq_s16(u4, u6), z6 = vsubq_s16(u4, u6);

    const int16x8_t s7 = vaddq_s16(mul_q15(z7, kCos3), mul_q15(z4, kSin3));
    const int16x8_t s4 = vsubq_s16(mul_q15(z4, kCos3), mul_q15(z7, kSin3));
    const int16x8_t s6 = vaddq_s16(mul_q15(z5, kSin1), mul_q15(z6, kCos1));
    const int16x8_t s5 = vsubq_s16(mul_q15(z5, kCos1), mul_q15(z6, kSin1));

    d0 = vaddq_s16(s0, s7); d7 = vsubq_s16(s0, s7);
    d1 = vaddq_s16(s1, s6); d6 = vsubq_s16(s1, s6);
    d2 = vaddq_s16(s2, s5); d5 = vsubq_s16(s2, s5);
    d3 = vaddq_s16(s3, s4); d4 = vsubq_s16(s3, s4);
}

LV_FORCE_INLINE void transpose8_neon(int16x8_t r[8]) {
    const int16x8_t t0 = vtrn1q_s16(r[0], r[1]), t1 = vtrn2q_s16(r[0], r[1]);
    const int16x8_t t2 = vtrn1q_s16(r[2], r[3]), t3 = vtrn2q_s16(r[2], r[3]);
    const int16x8_t t4 = vtrn1q_s16(r[4], r[5]), t5 = vtrn2q_s16(r[4], r[5]);
    const int16x8_t t6 = vtrn1q_s16(r[6], r[7]), t7 = vtrn2q_s16(r[6], r[7]);

    const int32x4_t s0 = vtrn1q_s32(vreinterpretq_s32_s16(t0), vreinterpretq_s32_s16(t2));
    const int32x4_t s1 = vtrn2q_s32(vreinterpretq_s32_s16(t0), vreinterpretq_s32_s16(t2));
    const int32x4_t s2 = vtrn1q_s32(vreinterpretq_s32_s16(t1), vreinterpretq_s32_s16(t3));
    const int32x4_t s3 = vtrn2q_s32(vreinterpretq_s32_s16(t1), vreinterpretq_s32_s16(t3));
    const int32x4_t s4 = vtrn1q_s32(vreinterpretq_s32_s16(t4), vreinterpretq_s32_s16(t6));
    const int32x4_t s5 = vtrn2q_s32(vreinterpretq_s32_s16(t4), vreinterpretq_s32_s16(t6));
    const int32x4_t s6 = vtrn1q_s32(vreinterpretq_s32_s16(t5), vreinterpretq_s32_s16(t7));
    const int32x4_t s7 = vtrn2q_s32(vreinterpretq_s32_s16(t5), vreinterpretq_s32_s16(t7));

    r[0] = vreinterpretq_s16_s64(vtrn1q_s64(vreinterpretq_s64_s32(s0), vreinterpretq_s64_s32(s4)));
    r[4] = vreinterpretq_s16_s64(vtrn2q_s64(vreinterpretq_s64_s32(s0), vreinterpretq_s64_s32(s4)));
    r[1] = vreinterpretq_s16_s64(vtrn1q_s64(vreinterpretq_s64_s32(s2), vreinterpretq_s64_s32(s6)));
    r[5] = vreinterpretq_s16_s64(vtrn2q_s64(vreinterpretq_s64_s32(s2), vreinterpretq_s64_s32(s6)));
    r[2] = vreinterpretq_s16_s64(vtrn1q_s64(vreinterpretq_s64_s32(s1), vreinterpretq_s64_s32(s5)));
    r[6] = vreinterpretq_s16_s64(vtrn2q_s64(vreinterpretq_s64_s32(s1), vreinterpretq_s64_s32(s5)));
    r[3] = vreinterpretq_s16_s64(vtrn1q_s64(vreinterpretq_s64_s32(s3), vreinterpretq_s64_s32(s7)));
    r[7] = vreinterpretq_s16_s64(vtrn2q_s64(vreinterpretq_s64_s32(s3), vreinterpretq_s64_s32(s7)));
}

void fdct_quant_neon(const uint8_t* src, int stride, const QuantTables& qt, int tbl, int16_t* out_nat) {
    const int16x8_t bias = vdupq_n_s16(128);
    int16x8_t r[8];
    for (int i = 0; i < 8; ++i) {
        const uint16x8_t u = vmovl_u8(vld1_u8(src + i * stride));
        r[i] = vshlq_n_s16(vsubq_s16(vreinterpretq_s16_u16(u), bias), 2);
    }

    llm_fdct8_neon(r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7]);
    for (int i = 0; i < 8; ++i) r[i] = vrshrq_n_s16(r[i], 1);
    transpose8_neon(r);
    llm_fdct8_neon(r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7]);
    transpose8_neon(r);

    const int16x8_t lim = vdupq_n_s16(kMaxCoef);
    const int16x8_t nlim = vdupq_n_s16(-kMaxCoef);
    for (int i = 0; i < 8; ++i) {
        const int16x8_t vr = vld1q_s16(qt.llm_recip[tbl] + i * 8);
        const int16x8_t vs = vld1q_s16(qt.llm_shift[tbl] + i * 8);
        int16x8_t q = vqrdmulhq_s16(vqrdmulhq_s16(r[i], vr), vs);
        q = vmaxq_s16(vminq_s16(q, lim), nlim);
        vst1q_s16(out_nat + i * 8, q);
    }
}

uint64_t fdct_quant_zz_neon(const uint8_t* src, int stride, const QuantTables& qt, int tbl,
                            int16_t* out_zz) {
    alignas(16) int16_t nat[64];
    fdct_quant_neon(src, stride, qt, tbl, nat);
    uint64_t mask = 0;
    for (int k = 0; k < 64; ++k) {
        const int16_t v = nat[kZigzag[k]];
        out_zz[k] = v;
        mask |= uint64_t(v != 0) << k;
    }
    return mask;
}

void fdct_quant_zz_x2_neon(const uint8_t* src0, const uint8_t* src1, int stride, const QuantTables& qt,
                           int tbl, int16_t* out_zz0, int16_t* out_zz1, uint64_t& mask0, uint64_t& mask1) {
    mask0 = fdct_quant_zz_neon(src0, stride, qt, tbl, out_zz0);
    mask1 = fdct_quant_zz_neon(src1, stride, qt, tbl, out_zz1);
}

void dequant_idct_neon(const int16_t* coef, const QuantTables& qt, int tbl, uint8_t* dst, int stride) {
    int16x8_t r[8];
    for (int i = 0; i < 8; ++i) {
        r[i] = vmulq_s16(vld1q_s16(coef + i * 8), vld1q_s16(qt.llm_deq[tbl] + i * 8));
    }
    llm_idct8_neon(r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7]);
    transpose8_neon(r);
    llm_idct8_neon(r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7]);
    transpose8_neon(r);

    const int16x8_t bias = vdupq_n_s16(2056);
    for (int i = 0; i < 8; ++i) {
        const int16x8_t v = vshrq_n_s16(vaddq_s16(r[i], bias), 4);
        vst1_u8(dst + i * stride, vqmovun_s16(v));
    }
}

void dequant_idct_x2_neon(const int16_t* coef0, const int16_t* coef1, const QuantTables& qt, int tbl,
                          uint8_t* dst0, uint8_t* dst1, int stride) {
    dequant_idct_neon(coef0, qt, tbl, dst0, stride);
    dequant_idct_neon(coef1, qt, tbl, dst1, stride);
}

const DctKernels kNeon{fdct_quant_neon,      dequant_idct_neon,  fdct_quant_zz_neon,
                       fdct_quant_zz_x2_neon, dequant_idct_x2_neon, "neon"};

}  // namespace

const DctKernels* dct_kernels_neon_impl() { return &kNeon; }

}  // namespace lv

#endif
