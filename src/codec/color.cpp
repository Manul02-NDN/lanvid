#include "codec/color.hpp"

#include <algorithm>

#include "common/cpu_features.hpp"
#include "common/platform.hpp"

namespace lv {

namespace {

constexpr int fix16(double x) { return int(x * 65536.0 + (x < 0 ? -0.5 : 0.5)); }

struct Coeffs {
    int yr, yg, yb;              // luma
    int br, bg, bb;              // Cb
    int rr, rg, rb;              // Cr
    int r_cr, g_cb, g_cr, b_cb;  // inverse
};

constexpr Coeffs kBT601{fix16(0.299),     fix16(0.587),     fix16(0.114),
                        fix16(-0.168736), fix16(-0.331264), fix16(0.5),
                        fix16(0.5),       fix16(-0.418688), fix16(-0.081312),
                        fix16(1.402),     fix16(-0.344136), fix16(-0.714136), fix16(1.772)};

constexpr Coeffs kBT709{fix16(0.2126),    fix16(0.7152),    fix16(0.0722),
                        fix16(-0.114572), fix16(-0.385428), fix16(0.5),
                        fix16(0.5),       fix16(-0.454153), fix16(-0.045847),
                        fix16(1.5748),    fix16(-0.187324), fix16(-0.468124), fix16(1.8556)};

inline const Coeffs& coeffs(ColorMatrix m) { return m == ColorMatrix::kBT709Full ? kBT709 : kBT601; }

template <int kR, int kB>
void rgb32_to_ycbcr_impl(const uint8_t* rgba, size_t stride, int img_w, int img_h, int y0, int rows, int pw,
                         ChromaFormat cf, ColorMatrix cm, const Planes& out) {
    const Coeffs& k = coeffs(cm);
    const int sxs = chroma_shift_x(cf), sys = chroma_shift_y(cf);
    const int xmax = std::min(pw, img_w);
    auto src_row = [&](int r) { return rgba + size_t(std::min(y0 + r, img_h - 1)) * stride; };

    for (int r = 0; r < rows; ++r) {
        const uint8_t* LV_RESTRICT s = src_row(r);
        uint8_t* LV_RESTRICT d = out.y + size_t(r) * out.y_stride;
        for (int x = 0; x < xmax; ++x) {
            const uint8_t* p = s + x * 4;
            d[x] = clamp_u8((k.yr * p[kR] + k.yg * p[1] + k.yb * p[kB] + 32768) >> 16);
        }
        for (int x = xmax; x < pw; ++x) d[x] = d[xmax - 1];
    }

    const int cw = pw >> sxs, ch = rows >> sys;
    constexpr int kOff = (128 << 18) + (1 << 17);
    for (int cy = 0; cy < ch; ++cy) {
        const uint8_t* ra = src_row(cy << sys);
        const uint8_t* rb = src_row((cy << sys) + sys);
        uint8_t* LV_RESTRICT db = out.cb + size_t(cy) * out.c_stride;
        uint8_t* LV_RESTRICT dr = out.cr + size_t(cy) * out.c_stride;
        for (int cx = 0; cx < cw; ++cx) {
            const int xa = std::min(cx << sxs, img_w - 1) * 4;
            const int xb = std::min((cx << sxs) + sxs, img_w - 1) * 4;
            const int R = ra[xa + kR] + ra[xb + kR] + rb[xa + kR] + rb[xb + kR];
            const int G = ra[xa + 1] + ra[xb + 1] + rb[xa + 1] + rb[xb + 1];
            const int B = ra[xa + kB] + ra[xb + kB] + rb[xa + kB] + rb[xb + kB];
            db[cx] = clamp_u8((k.br * R + k.bg * G + k.bb * B + kOff) >> 18);
            dr[cx] = clamp_u8((k.rr * R + k.rg * G + k.rb * B + kOff) >> 18);
        }
    }
}

template <int kR, int kB>
void ycbcr_to_rgb32_impl(const Planes& in, int rows, int img_w, ChromaFormat cf, ColorMatrix cm,
                         uint8_t* dst, size_t stride) {
    const Coeffs& k = coeffs(cm);
    const int sxs = chroma_shift_x(cf), sys = chroma_shift_y(cf);
    for (int r = 0; r < rows; ++r) {
        const uint8_t* LV_RESTRICT yr = in.y + size_t(r) * in.y_stride;
        const uint8_t* LV_RESTRICT cbr = in.cb + size_t(r >> sys) * in.c_stride;
        const uint8_t* LV_RESTRICT crr = in.cr + size_t(r >> sys) * in.c_stride;
        uint8_t* LV_RESTRICT d = dst + size_t(r) * stride;
        for (int x = 0; x < img_w; ++x) {
            const int Y = yr[x];
            const int cb = cbr[x >> sxs] - 128;
            const int cr = crr[x >> sxs] - 128;
            d[x * 4 + kR] = clamp_u8(Y + ((k.r_cr * cr + 32768) >> 16));
            d[x * 4 + 1] = clamp_u8(Y + ((k.g_cb * cb + k.g_cr * cr + 32768) >> 16));
            d[x * 4 + kB] = clamp_u8(Y + ((k.b_cb * cb + 32768) >> 16));
            d[x * 4 + 3] = 255;
        }
    }
}

}  // namespace

#if LV_HAVE_AVX2_TU
void rgba_to_ycbcr420_avx2(const uint8_t* rgba, size_t stride, int img_w, int img_h, int y0, int rows,
                           int pw, ColorMatrix cm, const Planes& out, PixelOrder order);
void ycbcr420_to_rgba_avx2(const Planes& in, int rows, int img_w, ColorMatrix cm, uint8_t* dst,
                           size_t stride, PixelOrder order);
#endif
#if LV_HAVE_NEON_TU
void rgba_to_ycbcr420_neon(const uint8_t* rgba, size_t stride, int img_w, int img_h, int y0, int rows,
                           int pw, ColorMatrix cm, const Planes& out, PixelOrder order);
void ycbcr420_to_rgba_neon(const Planes& in, int rows, int img_w, ColorMatrix cm, uint8_t* dst,
                           size_t stride, PixelOrder order);
#endif

void rgba_to_ycbcr(const uint8_t* rgba, size_t stride, int img_w, int img_h, int y0, int rows, int pw,
                   ChromaFormat cf, ColorMatrix cm, const Planes& out, PixelOrder order) {
#if LV_HAVE_AVX2_TU
    if (cf == ChromaFormat::k420 && cpu_has_avx2() && simd_enabled()) {
        rgba_to_ycbcr420_avx2(rgba, stride, img_w, img_h, y0, rows, pw, cm, out, order);
        return;
    }
#endif
#if LV_HAVE_NEON_TU
    if (cf == ChromaFormat::k420 && cpu_has_neon() && simd_enabled()) {
        rgba_to_ycbcr420_neon(rgba, stride, img_w, img_h, y0, rows, pw, cm, out, order);
        return;
    }
#endif
    if (order == PixelOrder::kBGRA)
        rgb32_to_ycbcr_impl<2, 0>(rgba, stride, img_w, img_h, y0, rows, pw, cf, cm, out);
    else
        rgb32_to_ycbcr_impl<0, 2>(rgba, stride, img_w, img_h, y0, rows, pw, cf, cm, out);
}

void ycbcr_to_rgba(const Planes& in, int rows, int img_w, ChromaFormat cf, ColorMatrix cm, uint8_t* dst,
                   size_t stride, PixelOrder order) {
#if LV_HAVE_AVX2_TU
    if (cf == ChromaFormat::k420 && cpu_has_avx2() && simd_enabled()) {
        ycbcr420_to_rgba_avx2(in, rows, img_w, cm, dst, stride, order);
        return;
    }
#endif
#if LV_HAVE_NEON_TU
    if (cf == ChromaFormat::k420 && cpu_has_neon() && simd_enabled()) {
        ycbcr420_to_rgba_neon(in, rows, img_w, cm, dst, stride, order);
        return;
    }
#endif
    if (order == PixelOrder::kBGRA)
        ycbcr_to_rgb32_impl<2, 0>(in, rows, img_w, cf, cm, dst, stride);
    else
        ycbcr_to_rgb32_impl<0, 2>(in, rows, img_w, cf, cm, dst, stride);
}

}  // namespace lv
