#include "codec/dct.hpp"

#include <cmath>

#include "codec/dct_impl.hpp"
#include "codec/zigzag.hpp"
#include "common/cpu_features.hpp"
#include "common/platform.hpp"

#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
#include <emmintrin.h>
#define LV_ROUND_SSE 1
#endif

namespace lv {

namespace {

LV_FORCE_INLINE int round_to_int(float x) {
#if LV_ROUND_SSE
    return _mm_cvtss_si32(_mm_set_ss(x));
#else
    return int(std::lrintf(x));
#endif
}

void fdct_quant_scalar(const uint8_t* src, int stride, const QuantTables& qt, int tbl, int16_t* out) {
    const float* fwd = qt.fwd[tbl];
    float ws[64];
    for (int r = 0; r < 8; ++r)
        for (int c = 0; c < 8; ++c) ws[r * 8 + c] = float(src[r * stride + c]) - 128.0f;

    for (int c = 0; c < 8; ++c) {
        float* p = ws + c;
        aan_fdct8(p[0], p[8], p[16], p[24], p[32], p[40], p[48], p[56]);
    }
    for (int r = 0; r < 8; ++r) {
        float* p = ws + r * 8;
        aan_fdct8(p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
    }
    for (int i = 0; i < 64; ++i) {
        const int v = round_to_int(ws[i] * fwd[i]);
        out[i] = int16_t(clamp_val(v, -kMaxCoef, kMaxCoef));
    }
}

uint64_t fdct_quant_zz_scalar(const uint8_t* src, int stride, const QuantTables& qt, int tbl,
                              int16_t* out_zz) {
    int16_t nat[64];
    fdct_quant_scalar(src, stride, qt, tbl, nat);
    uint64_t mask = 0;
    for (int k = 0; k < 64; ++k) {
        const int16_t v = nat[kZigzag[k]];
        out_zz[k] = v;
        mask |= uint64_t(v != 0) << k;
    }
    return mask;
}

void fdct_quant_zz_x2_scalar(const uint8_t* src0, const uint8_t* src1, int stride,
                             const QuantTables& qt, int tbl, int16_t* out_zz0, int16_t* out_zz1,
                             uint64_t& mask0, uint64_t& mask1) {
    mask0 = fdct_quant_zz_scalar(src0, stride, qt, tbl, out_zz0);
    mask1 = fdct_quant_zz_scalar(src1, stride, qt, tbl, out_zz1);
}

void dequant_idct_scalar(const int16_t* coef, const QuantTables& qt, int tbl, uint8_t* dst, int stride) {
    const float* inv = qt.inv[tbl];
    float ws[64];
    for (int i = 0; i < 64; ++i) ws[i] = float(coef[i]) * inv[i];

    for (int c = 0; c < 8; ++c) {
        float* p = ws + c;
        aan_idct8(p[0], p[8], p[16], p[24], p[32], p[40], p[48], p[56]);
    }
    for (int r = 0; r < 8; ++r) {
        float* p = ws + r * 8;
        aan_idct8(p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
        for (int c = 0; c < 8; ++c) dst[r * stride + c] = clamp_u8(round_to_int(p[c] + 128.0f));
    }
}

void dequant_idct_x2_scalar(const int16_t* coef0, const int16_t* coef1, const QuantTables& qt, int tbl,
                            uint8_t* dst0, uint8_t* dst1, int stride) {
    dequant_idct_scalar(coef0, qt, tbl, dst0, stride);
    dequant_idct_scalar(coef1, qt, tbl, dst1, stride);
}

const DctKernels kScalar{fdct_quant_scalar,      dequant_idct_scalar,  fdct_quant_zz_scalar,
                         fdct_quant_zz_x2_scalar, dequant_idct_x2_scalar, "scalar"};

}  // namespace

#if LV_HAVE_AVX2_TU
const DctKernels* dct_kernels_avx2_impl();  // dct_avx2.cpp
#endif
#if LV_HAVE_NEON_TU
const DctKernels* dct_kernels_neon_impl();  // dct_neon.cpp
#endif

const DctKernels& dct_kernels_scalar() { return kScalar; }

const DctKernels* dct_kernels_avx2() {
#if LV_HAVE_AVX2_TU
    if (cpu_has_avx2()) return dct_kernels_avx2_impl();
#endif
    return nullptr;
}

const DctKernels* dct_kernels_neon() {
#if LV_HAVE_NEON_TU
    if (cpu_has_neon()) return dct_kernels_neon_impl();
#endif
    return nullptr;
}

const DctKernels& dct_kernels_best() {
    static const DctKernels* avx2 = dct_kernels_avx2();
    static const DctKernels* neon = dct_kernels_neon();
    if (simd_enabled()) {
        if (avx2) return *avx2;
        if (neon) return *neon;
    }
    return kScalar;
}

}  // namespace lv
