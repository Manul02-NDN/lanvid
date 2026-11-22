// ARM64 NEON 8-pixel fixed-point RGBA8/BGRA8 <-> planar YCbCr 4:2:0 color conversion.
// Compiled only on aarch64 / ARM64 when LANVID_ENABLE_NEON=ON.

#if defined(__aarch64__) || defined(_M_ARM64)

#include <arm_neon.h>

#include <algorithm>
#include <cstring>

#include "codec/color.hpp"
#include "common/platform.hpp"

namespace lv {

namespace {

constexpr int fix16(double x) { return int(x * 65536.0 + (x < 0 ? -0.5 : 0.5)); }
constexpr int16_t q15_half(double x) { return int16_t(x * 0.5 * 32768.0 + (x < 0 ? -0.5 : 0.5)); }

struct NeonCoeffs {
    uint8_t yr, yg, yb;  // Q8 luma weights (sum = 256 -> exact vmull_u8 + vmlal_u8)
    int br, bg, bb;      // Q16 signed Cb
    int rr, rg, rb;      // Q16 signed Cr
    int16_t r_cr_h, g_cb_h, g_cr_h, b_cb_h;  // Q15 of (coeff / 2) for vqrdmulhq_s16
};

constexpr NeonCoeffs kBT601{77, 150, 29,
                            fix16(-0.168736), fix16(-0.331264), fix16(0.5),
                            fix16(0.5),       fix16(-0.418688), fix16(-0.081312),
                            q15_half(1.402),  q15_half(-0.344136), q15_half(-0.714136), q15_half(1.772)};

constexpr NeonCoeffs kBT709{54, 183, 19,
                            fix16(-0.114572), fix16(-0.385428), fix16(0.5),
                            fix16(0.5),       fix16(-0.454153), fix16(-0.045847),
                            q15_half(1.5748), q15_half(-0.187324), q15_half(-0.468124), q15_half(1.8556)};

template <bool kBGRA>
void rgb32_to_ycbcr420_neon_impl(const uint8_t* rgba, size_t stride, int img_w, int img_h, int y0, int rows,
                                 int pw, ColorMatrix cm, const Planes& out) {
    const NeonCoeffs& k = (cm == ColorMatrix::kBT709Full) ? kBT709 : kBT601;
    const uint8x8_t vyr = vdup_n_u8(k.yr);
    const uint8x8_t vyg = vdup_n_u8(k.yg);
    const uint8x8_t vyb = vdup_n_u8(k.yb);
    const int32x4_t vbr = vdupq_n_s32(k.br), vbg = vdupq_n_s32(k.bg), vbb = vdupq_n_s32(k.bb);
    const int32x4_t vrr = vdupq_n_s32(k.rr), vrg = vdupq_n_s32(k.rg), vrb = vdupq_n_s32(k.rb);
    const int32x4_t voff = vdupq_n_s32((128 << 18) + (1 << 17));

    constexpr int cR = kBGRA ? 2 : 0;
    constexpr int cB = kBGRA ? 0 : 2;
    const int xmax = std::min(pw, img_w);
    const int vec_w = xmax & ~7;
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
        for (; x < vec_w; x += 8) {
            const uint8x8x4_t p0 = vld4_u8(ra + size_t(x) * 4);
            const uint8x8x4_t p1 = vld4_u8(rb + size_t(x) * 4);
            const uint8x8_t r0 = p0.val[cR], g0 = p0.val[1], b0 = p0.val[cB];
            const uint8x8_t r1 = p1.val[cR], g1 = p1.val[1], b1 = p1.val[cB];

            // Luma (8 pixels per row via Q8 multiply-accumulate + rounding shift)
            uint16x8_t y0_16 = vmull_u8(r0, vyr);
            y0_16 = vmlal_u8(y0_16, g0, vyg);
            y0_16 = vmlal_u8(y0_16, b0, vyb);
            vst1_u8(ya + x, vrshrn_n_u16(y0_16, 8));

            uint16x8_t y1_16 = vmull_u8(r1, vyr);
            y1_16 = vmlal_u8(y1_16, g1, vyg);
            y1_16 = vmlal_u8(y1_16, b1, vyb);
            vst1_u8(yb + x, vrshrn_n_u16(y1_16, 8));

            // 2x2 chroma box sum -> 4x int32 lanes
            const int32x4_t Rsum = vreinterpretq_s32_u32(vpaddlq_u16(vaddl_u8(r0, r1)));
            const int32x4_t Gsum = vreinterpretq_s32_u32(vpaddlq_u16(vaddl_u8(g0, g1)));
            const int32x4_t Bsum = vreinterpretq_s32_u32(vpaddlq_u16(vaddl_u8(b0, b1)));

            int32x4_t cb32 = vmlaq_s32(vmlaq_s32(vmlaq_s32(voff, vbr, Rsum), vbg, Gsum), vbb, Bsum);
            int32x4_t cr32 = vmlaq_s32(vmlaq_s32(vmlaq_s32(voff, vrr, Rsum), vrg, Gsum), vrb, Bsum);
            cb32 = vshrq_n_s32(cb32, 18);
            cr32 = vshrq_n_s32(cr32, 18);

            const uint8x8_t cbcr = vqmovun_s16(vcombine_s16(vqmovn_s32(cb32), vqmovn_s32(cr32)));
            uint8_t tmp[8];
            vst1_u8(tmp, cbcr);
            std::memcpy(db + (x >> 1), tmp, 4);
            std::memcpy(dr + (x >> 1), tmp + 4, 4);
        }

        // Scalar tail + edge replication
        const int yr16 = fix16(cm == ColorMatrix::kBT709Full ? 0.2126 : 0.299);
        const int yg16 = fix16(cm == ColorMatrix::kBT709Full ? 0.7152 : 0.587);
        const int yb16 = fix16(cm == ColorMatrix::kBT709Full ? 0.0722 : 0.114);
        for (int tx = x; tx < xmax; ++tx) {
            const uint8_t* pa = ra + tx * 4;
            const uint8_t* pb = rb + tx * 4;
            ya[tx] = clamp_u8((yr16 * pa[cR] + yg16 * pa[1] + yb16 * pa[cB] + 32768) >> 16);
            yb[tx] = clamp_u8((yr16 * pb[cR] + yg16 * pb[1] + yb16 * pb[cB] + 32768) >> 16);
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
void ycbcr420_to_rgb32_neon_impl(const Planes& in, int rows, int img_w, ColorMatrix cm, uint8_t* dst,
                                 size_t stride) {
    const NeonCoeffs& k = (cm == ColorMatrix::kBT709Full) ? kBT709 : kBT601;
    const int16x8_t vr_cr = vdupq_n_s16(k.r_cr_h);
    const int16x8_t vg_cb = vdupq_n_s16(k.g_cb_h);
    const int16x8_t vg_cr = vdupq_n_s16(k.g_cr_h);
    const int16x8_t vb_cb = vdupq_n_s16(k.b_cb_h);
    const int16x8_t v128 = vdupq_n_s16(128);
    const uint8x8_t alpha = vdup_n_u8(255);

    const int sr_cr = fix16(cm == ColorMatrix::kBT709Full ? 1.5748 : 1.402);
    const int sg_cb = fix16(cm == ColorMatrix::kBT709Full ? -0.187324 : -0.344136);
    const int sg_cr = fix16(cm == ColorMatrix::kBT709Full ? -0.468124 : -0.714136);
    const int sb_cb = fix16(cm == ColorMatrix::kBT709Full ? 1.8556 : 1.772);
    constexpr int cR = kBGRA ? 2 : 0;
    constexpr int cB = kBGRA ? 0 : 2;

    const int vec_w = img_w & ~7;
    for (int r = 0; r < rows; ++r) {
        const uint8_t* LV_RESTRICT yr = in.y + size_t(r) * in.y_stride;
        const uint8_t* LV_RESTRICT cbr = in.cb + size_t(r >> 1) * in.c_stride;
        const uint8_t* LV_RESTRICT crr = in.cr + size_t(r >> 1) * in.c_stride;
        uint8_t* LV_RESTRICT d = dst + size_t(r) * stride;

        int x = 0;
        for (; x < vec_w; x += 8) {
            const int16x8_t Y = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(yr + x)));
            uint8_t cb4[8]{}, cr4[8]{};
            for (int i = 0; i < 4; ++i) {
                cb4[i * 2] = cb4[i * 2 + 1] = cbr[(x >> 1) + i];
                cr4[i * 2] = cr4[i * 2 + 1] = crr[(x >> 1) + i];
            }
            const int16x8_t cb2 = vshlq_n_s16(vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(vld1_u8(cb4))), v128), 1);
            const int16x8_t cr2 = vshlq_n_s16(vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(vld1_u8(cr4))), v128), 1);

            const int16x8_t R16 = vaddq_s16(Y, vqrdmulhq_s16(cr2, vr_cr));
            const int16x8_t G16 = vaddq_s16(Y, vaddq_s16(vqrdmulhq_s16(cb2, vg_cb), vqrdmulhq_s16(cr2, vg_cr)));
            const int16x8_t B16 = vaddq_s16(Y, vqrdmulhq_s16(cb2, vb_cb));

            uint8x8x4_t out4;
            out4.val[cR] = vqmovun_s16(R16);
            out4.val[1] = vqmovun_s16(G16);
            out4.val[cB] = vqmovun_s16(B16);
            out4.val[3] = alpha;
            vst4_u8(d + size_t(x) * 4, out4);
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

void rgba_to_ycbcr420_neon(const uint8_t* rgba, size_t stride, int img_w, int img_h, int y0, int rows,
                           int pw, ColorMatrix cm, const Planes& out, PixelOrder order) {
    if (order == PixelOrder::kBGRA)
        rgb32_to_ycbcr420_neon_impl<true>(rgba, stride, img_w, img_h, y0, rows, pw, cm, out);
    else
        rgb32_to_ycbcr420_neon_impl<false>(rgba, stride, img_w, img_h, y0, rows, pw, cm, out);
}

void ycbcr420_to_rgba_neon(const Planes& in, int rows, int img_w, ColorMatrix cm, uint8_t* dst,
                           size_t stride, PixelOrder order) {
    if (order == PixelOrder::kBGRA)
        ycbcr420_to_rgb32_neon_impl<true>(in, rows, img_w, cm, dst, stride);
    else
        ycbcr420_to_rgb32_neon_impl<false>(in, rows, img_w, cm, dst, stride);
}

}  // namespace lv

#endif
