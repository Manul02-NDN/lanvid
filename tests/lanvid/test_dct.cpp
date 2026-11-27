#include "mini_test.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#include "codec/dct.hpp"
#include "codec/quant.hpp"
#include "codec/zigzag.hpp"
#include "common/cpu_features.hpp"

namespace {

uint32_t xorshift32(uint32_t& s) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

int max_abs_diff(const uint8_t* a, const uint8_t* b, int n) {
    int m = 0;
    for (int i = 0; i < n; ++i) m = std::max(m, std::abs(int(a[i]) - int(b[i])));
    return m;
}

void verify_kernel_roundtrip(const lv::DctKernels& k) {
    lv::QuantTables unit;
    lv::build_unit_quant_tables(unit);

    uint8_t src[64], dst[64];
    int16_t coef[64];
    int worst = 0;
    uint64_t total_abs_err = 0;
    uint64_t total_pixels = 0;

    // 1. Constant blocks (0, 1, 64, 127, 128, 200, 254, 255).
    for (int val : {0, 1, 64, 127, 128, 200, 254, 255}) {
        std::memset(src, val, 64);
        k.fdct_quant(src, 8, unit, 0, coef);
        for (int i = 1; i < 64; ++i) CHECK(coef[i] == 0);
        k.dequant_idct(coef, unit, 0, dst, 8);
        worst = std::max(worst, max_abs_diff(src, dst, 64));
    }

    // 2. Step edges, 1px checkerboard, 2px checkerboard, 4px checkerboard.
    for (int mode = 0; mode < 4; ++mode) {
        for (int r = 0; r < 8; ++r) {
            for (int c = 0; c < 8; ++c) {
                bool hi = false;
                if (mode == 0) hi = r < 4;
                else if (mode == 1) hi = c < 4;
                else if (mode == 2) hi = ((r ^ c) & 1) != 0;
                else hi = (((r >> 1) ^ (c >> 1)) & 1) != 0;
                src[r * 8 + c] = hi ? 255 : 0;
            }
        }
        k.fdct_quant(src, 8, unit, 0, coef);
        k.dequant_idct(coef, unit, 0, dst, 8);
        worst = std::max(worst, max_abs_diff(src, dst, 64));
    }

    // 3. 1000 random blocks with non-trivial stride (stride = 19).
    uint8_t padded_src[8 * 19], padded_dst[8 * 19];
    uint32_t rng = 0x12345678u;
    for (int iter = 0; iter < 1000; ++iter) {
        std::memset(padded_src, 0xAA, sizeof(padded_src));
        std::memset(padded_dst, 0x55, sizeof(padded_dst));
        for (int r = 0; r < 8; ++r)
            for (int c = 0; c < 8; ++c) padded_src[r * 19 + c] = uint8_t(xorshift32(rng) & 0xFF);
        k.fdct_quant(padded_src, 19, unit, 0, coef);
        k.dequant_idct(coef, unit, 0, padded_dst, 19);
        for (int r = 0; r < 8; ++r) {
            for (int c = 0; c < 8; ++c) {
                const int e = std::abs(int(padded_src[r * 19 + c]) - int(padded_dst[r * 19 + c]));
                worst = std::max(worst, e);
                total_abs_err += uint64_t(e);
                ++total_pixels;
            }
            for (int c = 8; c < 19; ++c) CHECK(padded_dst[r * 19 + c] == 0x55);
        }
    }

    // 4. All 64 single-impulse DCT basis functions via IDCT -> FDCT.
    for (int idx = 0; idx < 64; ++idx) {
        std::memset(coef, 0, sizeof(coef));
        coef[idx] = 200;
        k.dequant_idct(coef, unit, 0, dst, 8);
        int16_t back[64];
        k.fdct_quant(dst, 8, unit, 0, back);
        CHECK(std::abs(int(back[idx]) - 200) <= 3);
    }

    const int max_allowed = (std::strcmp(k.name, "scalar") == 0) ? 1 : 2;
    const double mae = double(total_abs_err) / double(total_pixels);
    CHECK_MSG(worst <= max_allowed, "kernel=" + std::string(k.name) + " worst_err=" + std::to_string(worst));
    CHECK_MSG(mae < 0.25, "kernel=" + std::string(k.name) + " mae=" + std::to_string(mae));
}

}  // namespace

TEST(dct_scalar_roundtrip) {
    verify_kernel_roundtrip(lv::dct_kernels_scalar());
}

TEST(dct_avx2_roundtrip_and_parity) {
    const lv::DctKernels* avx2 = lv::dct_kernels_avx2();
    if (!avx2) {
        std::printf("  (AVX2 not available on this CPU; skipping AVX2 parity check)\n");
        return;
    }
    verify_kernel_roundtrip(*avx2);

    // Compare scalar AAN float vs AVX2 int16 LLM across Q=15..100 + dual-block x2 consistency.
    const lv::DctKernels& sc = lv::dct_kernels_scalar();
    uint32_t rng = 0xDEADBEEFu;
    int max_coef_diff = 0;
    int max_px_diff = 0;
    for (int q : {15, 50, 75, 95, 100}) {
        lv::QuantTables qt;
        lv::build_quant_tables(q, qt);
        for (int iter = 0; iter < 200; ++iter) {
            uint8_t s0[64], s1[64], d_sc0[64], d_av0[64], d_av1[64];
            int16_t c_sc0[64], c_av0[64], c_av1[64];
            for (int i = 0; i < 64; ++i) {
                s0[i] = uint8_t(xorshift32(rng) & 0xFF);
                s1[i] = uint8_t(xorshift32(rng) & 0xFF);
            }
            sc.fdct_quant(s0, 8, qt, 0, c_sc0);
            avx2->fdct_quant(s0, 8, qt, 0, c_av0);
            avx2->fdct_quant(s1, 8, qt, 0, c_av1);
            for (int i = 0; i < 64; ++i)
                max_coef_diff = std::max(max_coef_diff, std::abs(int(c_sc0[i]) - int(c_av0[i])));

            // Verify dual-block fused zig-zag + mask matches single-block AVX2 bit-for-bit.
            int16_t zz0[64], zz1[64];
            uint64_t m0 = 0, m1 = 0;
            avx2->fdct_quant_zz_x2(s0, s1, 8, qt, 0, zz0, zz1, m0, m1);
            for (int k = 0; k < 64; ++k) {
                CHECK(zz0[k] == c_av0[lv::kZigzag[k]]);
                CHECK(zz1[k] == c_av1[lv::kZigzag[k]]);
                CHECK(((m0 >> k) & 1u) == uint64_t(zz0[k] != 0));
                CHECK(((m1 >> k) & 1u) == uint64_t(zz1[k] != 0));
            }

            sc.dequant_idct(c_sc0, qt, 0, d_sc0, 8);
            avx2->dequant_idct_x2(c_sc0, c_av1, qt, 0, d_av0, d_av1, 8);
            max_px_diff = std::max(max_px_diff, max_abs_diff(d_sc0, d_av0, 64));
        }
    }
    CHECK_MSG(max_coef_diff <= 2, "max_coef_diff=" + std::to_string(max_coef_diff));
    CHECK_MSG(max_px_diff <= 2, "max_px_diff=" + std::to_string(max_px_diff));
}
