// AVX2 16-pixel fixed-point RGBA8/BGRA8 <-> planar YCbCr 4:2:0 color conversion.
// Compiled with /arch:AVX2 (MSVC) or -mavx2 (GCC/Clang); entered only after runtime CPUID check.

#include <immintrin.h>

#include <algorithm>

#include "codec/color.hpp"
#include "common/platform.hpp"

namespace lv {

namespace {

constexpr int fix16(double x) { return int(x * 65536.0 + (x < 0 ? -0.5 : 0.5)); }
constexpr int16_t q15_half(double x) { return int16_t(x * 0.5 * 32768.0 + (x < 0 ? -0.5 : 0.5)); }

struct Avx2Coeffs {
    uint16_t yr, yg, yb;           // Q16 unsigned luma weights (sum = 65536)
    int br, bg, bb;                // Q16 signed Cb weights
    int rr, rg, rb;                // Q16 signed Cr weights
    int16_t r_cr_h, g_cb_h, g_cr_h, b_cb_h;  // Q15 of (coeff / 2) for mulhrs(c << 1, coeff_h)
};

constexpr Avx2Coeffs kBT601{19595, 38470, 7471,
                            fix16(-0.168736), fix16(-0.331264), fix16(0.5),
                            fix16(0.5),       fix16(-0.418688), fix16(-0.081312),
                            q15_half(1.402),  q15_half(-0.344136), q15_half(-0.714136), q15_half(1.772)};

constexpr Avx2Coeffs kBT709{13933, 46871, 4732,
                            fix16(-0.114572), fix16(-0.385428), fix16(0.5),
                            fix16(0.5),       fix16(-0.454153), fix16(-0.045847),
                            q15_half(1.5748), q15_half(-0.187324), q15_half(-0.468124), q15_half(1.8556)};

// Deinterleaves 16 32-bpp pixels (64 bytes) into three linear 16x uint16_t vectors (R, G, B).
template <bool kBGRA>
LV_FORCE_INLINE void load_16px_rgb16(const uint8_t* p, __m256i& R, __m256i& G, __m256i& B) {
    constexpr int8_t cR = kBGRA ? 2 : 0;
    constexpr int8_t cB = kBGRA ? 0 : 2;
    const __m256i shuf = _mm256_setr_epi8(
        cR, cR + 4, cR + 8, cR + 12, 1, 5, 9, 13, cB, cB + 4, cB + 8, cB + 12, -1, -1, -1, -1,
        cR, cR + 4, cR + 8, cR + 12, 1, 5, 9, 13, cB, cB + 4, cB + 8, cB + 12, -1, -1, -1, -1);
    const __m256i perm_rg = _mm256_setr_epi32(0, 4, 1, 5, 2, 6, 3, 7);
    const __m256i perm_b = _mm256_setr_epi32(0, 4, 1, 5, 0, 0, 0, 0);

    const __m256i v0 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p));
    const __m256i v1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p + 32));
    const __m256i s0 = _mm256_shuffle_epi8(v0, shuf);
    const __m256i s1 = _mm256_shuffle_epi8(v1, shuf);

    const __m256i u_lo = _mm256_unpacklo_epi32(s0, s1);
    const __m256i u_hi = _mm256_unpackhi_epi32(s0, s1);
    const __m256i rg = _mm256_permutevar8x32_epi32(u_lo, perm_rg);
    const __m256i bv = _mm256_permutevar8x32_epi32(u_hi, perm_b);

    R = _mm256_cvtepu8_epi16(_mm256_castsi256_si128(rg));
    G = _mm256_cvtepu8_epi16(_mm256_extracti128_si256(rg, 1));
    B = _mm256_cvtepu8_epi16(_mm256_castsi256_si128(bv));
}

// Computes 16 uint8 luma values from 16x uint16 R, G, B in 7 instructions.
LV_FORCE_INLINE __m128i rgb16_to_y16(__m256i R, __m256i G, __m256i B, __m256i vyr, __m256i vyg,
                                     __m256i vyb) {
    const __m256i yr = _mm256_mulhi_epu16(_mm256_slli_epi16(R, 8), vyr);
    const __m256i yg = _mm256_mulhi_epu16(_mm256_slli_epi16(G, 8), vyg);
    const __m256i yb = _mm256_mulhi_epu16(_mm256_slli_epi16(B, 8), vyb);
    const __m256i sum = _mm256_add_epi16(_mm256_add_epi16(yr, yg), _mm256_add_epi16(yb, _mm256_set1_epi16(129)));
    const __m256i y16 = _mm256_srli_epi16(sum, 8);
    return _mm_packus_epi16(_mm256_castsi256_si128(y16), _mm256_extracti128_si256(y16, 1));
}

template <bool kBGRA>
void rgb32_to_ycbcr420_avx2_impl(const uint8_t* rgba, size_t stride, int img_w, int img_h, int y0, int rows,
                                 int pw, ColorMatrix cm, const Planes& out) {
    const Avx2Coeffs& k = (cm == ColorMatrix::kBT709Full) ? kBT709 : kBT601;
    const __m256i vyr = _mm256_set1_epi16(int16_t(k.yr));
    const __m256i vyg = _mm256_set1_epi16(int16_t(k.yg));
    const __m256i vyb = _mm256_set1_epi16(int16_t(k.yb));
    const __m256i vbr = _mm256_set1_epi32(k.br);
    const __m256i vbg = _mm256_set1_epi32(k.bg);
    const __m256i vbb = _mm256_set1_epi32(k.bb);
    const __m256i vrr = _mm256_set1_epi32(k.rr);
    const __m256i vrg = _mm256_set1_epi32(k.rg);
    const __m256i vrb = _mm256_set1_epi32(k.rb);
    const __m256i voff = _mm256_set1_epi32((128 << 18) + (1 << 17));
    const __m256i ones16 = _mm256_set1_epi16(1);

    constexpr int cR = kBGRA ? 2 : 0;
    constexpr int cB = kBGRA ? 0 : 2;
    const int vec_w = (std::min(pw, img_w)) & ~15;  // multiple of 16 strictly within img_w
    const int xmax = std::min(pw, img_w);
    const int cw = pw >> 1;
    const int ch = rows >> 1;
    auto src_row = [&](int r) { return rgba + size_t(std::min(y0 + r, img_h - 1)) * stride; };

    for (int cy = 0; cy < ch; ++cy) {
        const uint8_t* ra = src_row(cy * 2);
        const uint8_t* rb = src_row(cy * 2 + 1);
        uint8_t* LV_RESTRICT ya = out.y + size_t(cy * 2) * out.y_stride;
        uint8_t* LV_RESTRICT yb = out.y + size_t(cy * 2 + 1) * out.y_stride;
        uint8_t* LV_RESTRICT db = out.cb + size_t(cy) * out.c_stride;
        uint8_t* LV_RESTRICT dr = out.cr + size_t(cy) * out.c_stride;

        int x = 0;
        for (; x < vec_w; x += 16) {
            __m256i R0, G0, B0, R1, G1, B1;
            load_16px_rgb16<kBGRA>(ra + size_t(x) * 4, R0, G0, B0);
            load_16px_rgb16<kBGRA>(rb + size_t(x) * 4, R1, G1, B1);

            // Luma for both rows (16 pixels each)
            _mm_storeu_si128(reinterpret_cast<__m128i*>(ya + x), rgb16_to_y16(R0, G0, B0, vyr, vyg, vyb));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(yb + x), rgb16_to_y16(R1, G1, B1, vyr, vyg, vyb));

            // 2x2 box sum -> 8 int32 lanes in exact order [0..7]
            const __m256i Rsum = _mm256_madd_epi16(_mm256_add_epi16(R0, R1), ones16);
            const __m256i Gsum = _mm256_madd_epi16(_mm256_add_epi16(G0, G1), ones16);
            const __m256i Bsum = _mm256_madd_epi16(_mm256_add_epi16(B0, B1), ones16);

            const __m256i cb32 = _mm256_srai_epi32(
                _mm256_add_epi32(_mm256_add_epi32(_mm256_mullo_epi32(vbr, Rsum), _mm256_mullo_epi32(vbg, Gsum)),
                                 _mm256_add_epi32(_mm256_mullo_epi32(vbb, Bsum), voff)),
                18);
            const __m256i cr32 = _mm256_srai_epi32(
                _mm256_add_epi32(_mm256_add_epi32(_mm256_mullo_epi32(vrr, Rsum), _mm256_mullo_epi32(vrg, Gsum)),
                                 _mm256_add_epi32(_mm256_mullo_epi32(vrb, Bsum), voff)),
                18);

            // Pack 8 int32 -> 8 uint8 (clamped [0, 255])
            const __m128i cb16 = _mm_packs_epi32(_mm256_castsi256_si128(cb32), _mm256_extracti128_si256(cb32, 1));
            const __m128i cr16 = _mm_packs_epi32(_mm256_castsi256_si128(cr32), _mm256_extracti128_si256(cr32, 1));
            const __m128i cbcr8 = _mm_packus_epi16(cb16, cr16);
            _mm_storel_epi64(reinterpret_cast<__m128i*>(db + (x >> 1)), cbcr8);
            _mm_storeh_pi(reinterpret_cast<__m64*>(dr + (x >> 1)), _mm_castsi128_ps(cbcr8));
        }

        // Scalar tail + edge replication for widths not divisible by 16.
        for (int tx = x; tx < xmax; ++tx) {
            const uint8_t* pa = ra + tx * 4;
            const uint8_t* pb = rb + tx * 4;
            ya[tx] = clamp_u8((int(k.yr) * pa[cR] + int(k.yg) * pa[1] + int(k.yb) * pa[cB] + 32768) >> 16);
            yb[tx] = clamp_u8((int(k.yr) * pb[cR] + int(k.yg) * pb[1] + int(k.yb) * pb[cB] + 32768) >> 16);
        }
        for (int tx = xmax; tx < pw; ++tx) {
            ya[tx] = ya[xmax - 1];
            yb[tx] = yb[xmax - 1];
        }
        constexpr int kOff = (128 << 18) + (1 << 17);
        for (int cx = x >> 1; cx < cw; ++cx) {
            const int xa = std::min(cx * 2, img_w - 1) * 4;
            const int xb = std::min(cx * 2 + 1, img_w - 1) * 4;
            const int R = ra[xa + cR] + ra[xb + cR] + rb[xa + cR] + rb[xb + cR];
            const int G = ra[xa + 1] + ra[xb + 1] + rb[xa + 1] + rb[xb + 1];
            const int B = ra[xa + cB] + ra[xb + cB] + rb[xa + cB] + rb[xb + cB];
            db[cx] = clamp_u8((k.br * R + k.bg * G + k.bb * B + kOff) >> 18);
            dr[cx] = clamp_u8((k.rr * R + k.rg * G + k.rb * B + kOff) >> 18);
        }
    }
}

template <bool kBGRA>
void ycbcr420_to_rgb32_avx2_impl(const Planes& in, int rows, int img_w, ColorMatrix cm, uint8_t* dst,
                                 size_t stride) {
    const Avx2Coeffs& k = (cm == ColorMatrix::kBT709Full) ? kBT709 : kBT601;
    const __m256i vr_cr = _mm256_set1_epi16(k.r_cr_h);
    const __m256i vg_cb = _mm256_set1_epi16(k.g_cb_h);
    const __m256i vg_cr = _mm256_set1_epi16(k.g_cr_h);
    const __m256i vb_cb = _mm256_set1_epi16(k.b_cb_h);
    const __m256i v128 = _mm256_set1_epi16(128);
    const __m128i alpha = _mm_set1_epi8(-1);

    // Scalar fallback coefficients for tail < 16 pixels.
    const int sr_cr = fix16(cm == ColorMatrix::kBT709Full ? 1.5748 : 1.402);
    const int sg_cb = fix16(cm == ColorMatrix::kBT709Full ? -0.187324 : -0.344136);
    const int sg_cr = fix16(cm == ColorMatrix::kBT709Full ? -0.468124 : -0.714136);
    const int sb_cb = fix16(cm == ColorMatrix::kBT709Full ? 1.8556 : 1.772);
    constexpr int cR = kBGRA ? 2 : 0;
    constexpr int cB = kBGRA ? 0 : 2;

    const int vec_w = img_w & ~15;
    for (int r = 0; r < rows; ++r) {
        const uint8_t* LV_RESTRICT yr = in.y + size_t(r) * in.y_stride;
        const uint8_t* LV_RESTRICT cbr = in.cb + size_t(r >> 1) * in.c_stride;
        const uint8_t* LV_RESTRICT crr = in.cr + size_t(r >> 1) * in.c_stride;
        uint8_t* LV_RESTRICT d = dst + size_t(r) * stride;

        int x = 0;
        for (; x < vec_w; x += 16) {
            const __m128i y8 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(yr + x));
            const __m128i cb8 = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(cbr + (x >> 1)));
            const __m128i cr8 = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(crr + (x >> 1)));

            const __m256i Y = _mm256_cvtepu8_epi16(y8);
            // Nearest horizontal upsample: duplicate each chroma sample 2x, subtract 128, shift << 1.
            const __m256i cb2 =
                _mm256_slli_epi16(_mm256_sub_epi16(_mm256_cvtepu8_epi16(_mm_unpacklo_epi8(cb8, cb8)), v128), 1);
            const __m256i cr2 =
                _mm256_slli_epi16(_mm256_sub_epi16(_mm256_cvtepu8_epi16(_mm_unpacklo_epi8(cr8, cr8)), v128), 1);

            const __m256i R16 = _mm256_add_epi16(Y, _mm256_mulhrs_epi16(cr2, vr_cr));
            const __m256i G16 = _mm256_add_epi16(
                Y, _mm256_add_epi16(_mm256_mulhrs_epi16(cb2, vg_cb), _mm256_mulhrs_epi16(cr2, vg_cr)));
            const __m256i B16 = _mm256_add_epi16(Y, _mm256_mulhrs_epi16(cb2, vb_cb));

            // Pack 16 int16 -> 16 uint8 (clamped [0, 255])
            const __m128i R8 = _mm_packus_epi16(_mm256_castsi256_si128(R16), _mm256_extracti128_si256(R16, 1));
            const __m128i G8 = _mm_packus_epi16(_mm256_castsi256_si128(G16), _mm256_extracti128_si256(G16, 1));
            const __m128i B8 = _mm_packus_epi16(_mm256_castsi256_si128(B16), _mm256_extracti128_si256(B16, 1));

            const __m128i C0 = kBGRA ? B8 : R8;
            const __m128i C2 = kBGRA ? R8 : B8;
            const __m128i lo01 = _mm_unpacklo_epi8(C0, G8);
            const __m128i lo23 = _mm_unpacklo_epi8(C2, alpha);
            const __m128i hi01 = _mm_unpackhi_epi8(C0, G8);
            const __m128i hi23 = _mm_unpackhi_epi8(C2, alpha);

            uint8_t* outp = d + size_t(x) * 4;
            _mm_storeu_si128(reinterpret_cast<__m128i*>(outp + 0), _mm_unpacklo_epi16(lo01, lo23));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(outp + 16), _mm_unpackhi_epi16(lo01, lo23));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(outp + 32), _mm_unpacklo_epi16(hi01, hi23));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(outp + 48), _mm_unpackhi_epi16(hi01, hi23));
        }

        for (; x < img_w; ++x) {
            const int Y = yr[x];
            const int cb = cbr[x >> 1] - 128;
            const int cr = crr[x >> 1] - 128;
            d[x * 4 + cR] = clamp_u8(Y + ((sr_cr * cr + 32768) >> 16));
            d[x * 4 + 1] = clamp_u8(Y + ((sg_cb * cb + sg_cr * cr + 32768) >> 16));
            d[x * 4 + cB] = clamp_u8(Y + ((sb_cb * cb + 32768) >> 16));
            d[x * 4 + 3] = 255;
        }
    }
}

}  // namespace

void rgba_to_ycbcr420_avx2(const uint8_t* rgba, size_t stride, int img_w, int img_h, int y0, int rows,
                           int pw, ColorMatrix cm, const Planes& out, PixelOrder order) {
    if (order == PixelOrder::kBGRA)
        rgb32_to_ycbcr420_avx2_impl<true>(rgba, stride, img_w, img_h, y0, rows, pw, cm, out);
    else
        rgb32_to_ycbcr420_avx2_impl<false>(rgba, stride, img_w, img_h, y0, rows, pw, cm, out);
}

void ycbcr420_to_rgba_avx2(const Planes& in, int rows, int img_w, ColorMatrix cm, uint8_t* dst,
                           size_t stride, PixelOrder order) {
    if (order == PixelOrder::kBGRA)
        ycbcr420_to_rgb32_avx2_impl<true>(in, rows, img_w, cm, dst, stride);
    else
        ycbcr420_to_rgb32_avx2_impl<false>(in, rows, img_w, cm, dst, stride);
}

namespace {

template <bool kBGRA>
void nv12_to_packed_rgb_avx2_impl(const uint8_t* src_y, int y_stride, const uint8_t* src_uv,
                                  int uv_stride, int width, int height, uint8_t* dst,
                                  size_t dst_stride) {
    constexpr int cR = kBGRA ? 2 : 0;
    constexpr int cB = kBGRA ? 0 : 2;
    const __m256i v16 = _mm256_set1_epi16(16);
    const __m256i v128 = _mm256_set1_epi16(128);
    // Q15 half-scale coefficients for mulhrs(val << 1, coeff_h) == round(val * coeff).
    const __m256i ky = _mm256_set1_epi16(q15_half(298.0 / 256.0));
    const __m256i kr_e = _mm256_set1_epi16(q15_half(409.0 / 256.0));
    const __m256i kg_d = _mm256_set1_epi16(q15_half(-100.0 / 256.0));
    const __m256i kg_e = _mm256_set1_epi16(q15_half(-208.0 / 256.0));
    const __m256i kb_rem = _mm256_set1_epi16(q15_half(4.0 / 256.0));  // 516/256 = 2 + 4/256
    const __m128i alpha = _mm_set1_epi8(char(0xFF));
    const __m128i shuf_u = _mm_setr_epi8(0, 0, 2, 2, 4, 4, 6, 6, 8, 8, 10, 10, 12, 12, 14, 14);
    const __m128i shuf_v = _mm_setr_epi8(1, 1, 3, 3, 5, 5, 7, 7, 9, 9, 11, 11, 13, 13, 15, 15);

    for (int y = 0; y < height; ++y) {
        const uint8_t* LV_RESTRICT yp = src_y + size_t(y) * size_t(y_stride);
        const uint8_t* LV_RESTRICT uvp = src_uv + size_t(y >> 1) * size_t(uv_stride);
        uint8_t* LV_RESTRICT dp = dst + size_t(y) * dst_stride;

        int x = 0;
        for (; x + 16 <= width; x += 16) {
            const __m128i y8 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(yp + x));
            const __m128i uv8 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(uvp + x));
            const __m256i y2 = _mm256_slli_epi16(_mm256_sub_epi16(_mm256_cvtepu8_epi16(y8), v16), 1);
            const __m128i u8 = _mm_shuffle_epi8(uv8, shuf_u);
            const __m128i v8 = _mm_shuffle_epi8(uv8, shuf_v);
            const __m256i d2 = _mm256_slli_epi16(_mm256_sub_epi16(_mm256_cvtepu8_epi16(u8), v128), 1);
            const __m256i e2 = _mm256_slli_epi16(_mm256_sub_epi16(_mm256_cvtepu8_epi16(v8), v128), 1);

            const __m256i cy = _mm256_mulhrs_epi16(y2, ky);
            const __m256i R16 = _mm256_add_epi16(cy, _mm256_mulhrs_epi16(e2, kr_e));
            const __m256i G16 = _mm256_add_epi16(
                cy, _mm256_add_epi16(_mm256_mulhrs_epi16(d2, kg_d), _mm256_mulhrs_epi16(e2, kg_e)));
            const __m256i B16 = _mm256_add_epi16(
                cy, _mm256_add_epi16(d2, _mm256_mulhrs_epi16(d2, kb_rem)));

            const __m128i R8 = _mm_packus_epi16(_mm256_castsi256_si128(R16), _mm256_extracti128_si256(R16, 1));
            const __m128i G8 = _mm_packus_epi16(_mm256_castsi256_si128(G16), _mm256_extracti128_si256(G16, 1));
            const __m128i B8 = _mm_packus_epi16(_mm256_castsi256_si128(B16), _mm256_extracti128_si256(B16, 1));

            const __m128i C0 = kBGRA ? B8 : R8;
            const __m128i C2 = kBGRA ? R8 : B8;
            const __m128i lo01 = _mm_unpacklo_epi8(C0, G8);
            const __m128i lo23 = _mm_unpacklo_epi8(C2, alpha);
            const __m128i hi01 = _mm_unpackhi_epi8(C0, G8);
            const __m128i hi23 = _mm_unpackhi_epi8(C2, alpha);

            uint8_t* outp = dp + size_t(x) * 4;
            _mm_storeu_si128(reinterpret_cast<__m128i*>(outp + 0), _mm_unpacklo_epi16(lo01, lo23));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(outp + 16), _mm_unpackhi_epi16(lo01, lo23));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(outp + 32), _mm_unpacklo_epi16(hi01, hi23));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(outp + 48), _mm_unpackhi_epi16(hi01, hi23));
        }

        for (; x < width; ++x) {
            const int d = int(uvp[x & ~1]) - 128;
            const int e = int(uvp[(x & ~1) + 1]) - 128;
            const int c = 298 * (int(yp[x]) - 16);
            dp[x * 4 + cR] = clamp_u8((c + 409 * e + 128) >> 8);
            dp[x * 4 + 1] = clamp_u8((c - 100 * d - 208 * e + 128) >> 8);
            dp[x * 4 + cB] = clamp_u8((c + 516 * d + 128) >> 8);
            dp[x * 4 + 3] = 255;
        }
    }
}

template <bool kBGRA>
void packed_rgb_to_nv12_avx2_impl(const uint8_t* src, size_t src_stride, int width, int height,
                                  uint8_t* dst_y, int y_stride, uint8_t* dst_uv, int uv_stride) {
    constexpr int cR = kBGRA ? 2 : 0;
    constexpr int cB = kBGRA ? 0 : 2;
    const __m256i v66 = _mm256_set1_epi16(66);
    const __m256i v129 = _mm256_set1_epi16(129);
    const __m256i v25 = _mm256_set1_epi16(25);
    const __m256i v128 = _mm256_set1_epi16(128);
    const __m256i v16 = _mm256_set1_epi16(16);
    const __m256i v_ones = _mm256_set1_epi16(1);
    const __m128i c_ur = _mm_set1_epi16(-38);
    const __m128i c_ug = _mm_set1_epi16(-74);
    const __m128i c_ub = _mm_set1_epi16(112);
    const __m128i c_vr = _mm_set1_epi16(112);
    const __m128i c_vg = _mm_set1_epi16(-94);
    const __m128i c_vb = _mm_set1_epi16(-18);
    const __m128i c128_16 = _mm_set1_epi16(128);

    const int h2 = height & ~1;
    const int w2 = width & ~1;
    for (int y = 0; y < h2; y += 2) {
        const uint8_t* LV_RESTRICT r0 = src + size_t(y) * src_stride;
        const uint8_t* LV_RESTRICT r1 = src + size_t(y + 1) * src_stride;
        uint8_t* LV_RESTRICT y0 = dst_y + size_t(y) * size_t(y_stride);
        uint8_t* LV_RESTRICT y1 = dst_y + size_t(y + 1) * size_t(y_stride);
        uint8_t* LV_RESTRICT uv = dst_uv + size_t(y >> 1) * size_t(uv_stride);

        int x = 0;
        for (; x + 16 <= w2; x += 16) {
            __m256i R0, G0, B0, R1, G1, B1;
            load_16px_rgb16<kBGRA>(r0 + size_t(x) * 4, R0, G0, B0);
            load_16px_rgb16<kBGRA>(r1 + size_t(x) * 4, R1, G1, B1);

            auto calc_y = [&](__m256i R, __m256i G, __m256i B) {
                __m256i sum = _mm256_add_epi16(
                    _mm256_add_epi16(_mm256_mullo_epi16(R, v66), _mm256_mullo_epi16(G, v129)),
                    _mm256_add_epi16(_mm256_mullo_epi16(B, v25), v128));
                __m256i yy = _mm256_add_epi16(_mm256_srli_epi16(sum, 8), v16);
                return _mm_packus_epi16(_mm256_castsi256_si128(yy), _mm256_extracti128_si256(yy, 1));
            };
            _mm_storeu_si128(reinterpret_cast<__m128i*>(y0 + x), calc_y(R0, G0, B0));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(y1 + x), calc_y(R1, G1, B1));

            // 2x2 box average into 8 int16 lanes in __m128i
            auto avg2x2 = [&](__m256i A, __m256i B) {
                __m256i s = _mm256_add_epi16(A, B);          // 16 values, each <= 510
                __m256i p = _mm256_madd_epi16(s, v_ones);    // 8 int32 pair sums <= 1020
                __m256i a = _mm256_srli_epi32(_mm256_add_epi32(p, _mm256_set1_epi32(2)), 2);
                return _mm_packs_epi32(_mm256_castsi256_si128(a), _mm256_extracti128_si256(a, 1));
            };
            const __m128i Ra = avg2x2(R0, R1);
            const __m128i Ga = avg2x2(G0, G1);
            const __m128i Ba = avg2x2(B0, B1);

            const __m128i U16 = _mm_add_epi16(
                _mm_srai_epi16(
                    _mm_add_epi16(_mm_add_epi16(_mm_mullo_epi16(Ra, c_ur), _mm_mullo_epi16(Ga, c_ug)),
                                  _mm_add_epi16(_mm_mullo_epi16(Ba, c_ub), c128_16)),
                    8),
                c128_16);
            const __m128i V16 = _mm_add_epi16(
                _mm_srai_epi16(
                    _mm_add_epi16(_mm_add_epi16(_mm_mullo_epi16(Ra, c_vr), _mm_mullo_epi16(Ga, c_vg)),
                                  _mm_add_epi16(_mm_mullo_epi16(Ba, c_vb), c128_16)),
                    8),
                c128_16);

            const __m128i U8 = _mm_packus_epi16(U16, U16);
            const __m128i V8 = _mm_packus_epi16(V16, V16);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(uv + x), _mm_unpacklo_epi8(U8, V8));
        }

        for (; x < w2; x += 2) {
            const uint8_t* p00 = r0 + size_t(x) * 4;
            const uint8_t* p01 = p00 + 4;
            const uint8_t* p10 = r1 + size_t(x) * 4;
            const uint8_t* p11 = p10 + 4;
            const int r00 = p00[cR], g00 = p00[1], b00 = p00[cB];
            const int r01 = p01[cR], g01 = p01[1], b01 = p01[cB];
            const int r10 = p10[cR], g10 = p10[1], b10 = p10[cB];
            const int r11 = p11[cR], g11 = p11[1], b11 = p11[cB];
            y0[x] = uint8_t(((66 * r00 + 129 * g00 + 25 * b00 + 128) >> 8) + 16);
            y0[x + 1] = uint8_t(((66 * r01 + 129 * g01 + 25 * b01 + 128) >> 8) + 16);
            y1[x] = uint8_t(((66 * r10 + 129 * g10 + 25 * b10 + 128) >> 8) + 16);
            y1[x + 1] = uint8_t(((66 * r11 + 129 * g11 + 25 * b11 + 128) >> 8) + 16);
            const int ra = (r00 + r01 + r10 + r11 + 2) >> 2;
            const int ga = (g00 + g01 + g10 + g11 + 2) >> 2;
            const int ba = (b00 + b01 + b10 + b11 + 2) >> 2;
            uv[x] = uint8_t(((-38 * ra - 74 * ga + 112 * ba + 128) >> 8) + 128);
            uv[x + 1] = uint8_t(((112 * ra - 94 * ga - 18 * ba + 128) >> 8) + 128);
        }
    }
}

}  // namespace

void nv12_to_packed_rgb_avx2(const uint8_t* src_y, int y_stride, const uint8_t* src_uv,
                             int uv_stride, int width, int height, PixelOrder order, uint8_t* dst,
                             size_t dst_stride) {
    if (order == PixelOrder::kBGRA)
        nv12_to_packed_rgb_avx2_impl<true>(src_y, y_stride, src_uv, uv_stride, width, height, dst,
                                           dst_stride);
    else
        nv12_to_packed_rgb_avx2_impl<false>(src_y, y_stride, src_uv, uv_stride, width, height, dst,
                                            dst_stride);
}

void packed_rgb_to_nv12_avx2(const uint8_t* src, size_t src_stride, int width, int height,
                             PixelOrder order, uint8_t* dst_y, int y_stride, uint8_t* dst_uv,
                             int uv_stride) {
    if (order == PixelOrder::kBGRA)
        packed_rgb_to_nv12_avx2_impl<true>(src, src_stride, width, height, dst_y, y_stride, dst_uv,
                                           uv_stride);
    else
        packed_rgb_to_nv12_avx2_impl<false>(src, src_stride, width, height, dst_y, y_stride, dst_uv,
                                            uv_stride);
}

}  // namespace lv
