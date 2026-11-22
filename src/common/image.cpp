#include "common/image.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace lv {

namespace {
double psnr_from_mse(double mse) {
    if (mse <= 1e-12) return 99.0;
    return 10.0 * std::log10(255.0 * 255.0 / mse);
}
}  // namespace

void resize_bilinear_32bpp(const uint8_t* src, int sw, int sh, ptrdiff_t src_stride, Image& dst) {
    const int dw = dst.width, dh = dst.height;
    if (!src || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;

    if (sw == dw && sh == dh) {
        const size_t row_bytes = size_t(dw) * 4;
        for (int y = 0; y < dh; ++y) {
            std::memcpy(dst.row(y), src + ptrdiff_t(y) * src_stride, row_bytes);
        }
        return;
    }

    // 16.16 fixed-point coordinates with proper pixel-center alignment:
    // src_coord = (dst_coord + 0.5) * (sw / dw) - 0.5
    const double scale_x = double(sw) / double(dw);
    const double scale_y = double(sh) / double(dh);
    const int max_sx = sw - 1;
    const int max_sy = sh - 1;

    std::vector<int> x0(static_cast<size_t>(dw), 0);
    std::vector<int> x1(static_cast<size_t>(dw), 0);
    std::vector<int> fx(static_cast<size_t>(dw), 0);
    for (int x = 0; x < dw; ++x) {
        const double sx = std::clamp((double(x) + 0.5) * scale_x - 0.5, 0.0, double(max_sx));
        const int ix = int(sx);
        x0[size_t(x)] = ix * 4;
        x1[size_t(x)] = std::min(ix + 1, max_sx) * 4;
        fx[size_t(x)] = int((sx - double(ix)) * 256.0 + 0.5);
    }

    for (int y = 0; y < dh; ++y) {
        const double sy = std::clamp((double(y) + 0.5) * scale_y - 0.5, 0.0, double(max_sy));
        const int iy = int(sy);
        const int y0 = iy;
        const int y1 = std::min(iy + 1, max_sy);
        const int fy = int((sy - double(iy)) * 256.0 + 0.5);
        const int ify = 256 - fy;

        const uint8_t* r0 = src + ptrdiff_t(y0) * src_stride;
        const uint8_t* r1 = src + ptrdiff_t(y1) * src_stride;
        uint8_t* out = dst.row(y);

        for (int x = 0; x < dw; ++x) {
            const uint8_t* p00 = r0 + x0[size_t(x)];
            const uint8_t* p10 = r0 + x1[size_t(x)];
            const uint8_t* p01 = r1 + x0[size_t(x)];
            const uint8_t* p11 = r1 + x1[size_t(x)];
            const int w = fx[size_t(x)];
            const int iw = 256 - w;

            for (int c = 0; c < 3; ++c) {
                const int top = p00[c] * iw + p10[c] * w;
                const int bot = p01[c] * iw + p11[c] * w;
                out[x * 4 + c] = uint8_t((top * ify + bot * fy + 32768) >> 16);
            }
            out[x * 4 + 3] = 255;
        }
    }
}

void swap_rb_inplace(Image& img) {
    for (int y = 0; y < img.height; ++y) {
        uint8_t* p = img.row(y);
        for (int x = 0; x < img.width; ++x) {
            std::swap(p[x * 4 + 0], p[x * 4 + 2]);
        }
    }
    img.order = (img.order == PixelOrder::kRGBA) ? PixelOrder::kBGRA : PixelOrder::kRGBA;
}

double psnr_rgb(const Image& a, const Image& b) {
    if (a.width != b.width || a.height != b.height) return 0.0;
    const int ar = (a.order == PixelOrder::kBGRA) ? 2 : 0;
    const int ab = 2 - ar;
    const int br = (b.order == PixelOrder::kBGRA) ? 2 : 0;
    const int bb = 2 - br;

    double sse = 0.0;
    for (int y = 0; y < a.height; ++y) {
        const uint8_t* pa = a.row(y);
        const uint8_t* pb = b.row(y);
        for (int x = 0; x < a.width * 4; x += 4) {
            const double dr = double(pa[x + ar]) - double(pb[x + br]);
            const double dg = double(pa[x + 1]) - double(pb[x + 1]);
            const double db = double(pa[x + ab]) - double(pb[x + bb]);
            sse += dr * dr + dg * dg + db * db;
        }
    }
    return psnr_from_mse(sse / (double(a.width) * a.height * 3.0));
}

double psnr_luma(const Image& a, const Image& b) {
    if (a.width != b.width || a.height != b.height) return 0.0;
    const int ar = (a.order == PixelOrder::kBGRA) ? 2 : 0;
    const int ab = 2 - ar;
    const int br = (b.order == PixelOrder::kBGRA) ? 2 : 0;
    const int bb = 2 - br;

    double sse = 0.0;
    for (int y = 0; y < a.height; ++y) {
        const uint8_t* pa = a.row(y);
        const uint8_t* pb = b.row(y);
        for (int x = 0; x < a.width * 4; x += 4) {
            const double ya = 0.299 * pa[x + ar] + 0.587 * pa[x + 1] + 0.114 * pa[x + ab];
            const double yb = 0.299 * pb[x + br] + 0.587 * pb[x + 1] + 0.114 * pb[x + bb];
            sse += (ya - yb) * (ya - yb);
        }
    }
    return psnr_from_mse(sse / (double(a.width) * a.height));
}

bool write_ppm(const char* path, const Image& img) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    std::fprintf(f, "P6\n%d %d\n255\n", img.width, img.height);
    const int r_idx = (img.order == PixelOrder::kBGRA) ? 2 : 0;
    const int b_idx = 2 - r_idx;
    std::vector<uint8_t> line(size_t(img.width) * 3);
    for (int y = 0; y < img.height; ++y) {
        const uint8_t* p = img.row(y);
        for (int x = 0; x < img.width; ++x) {
            line[size_t(x) * 3 + 0] = p[x * 4 + r_idx];
            line[size_t(x) * 3 + 1] = p[x * 4 + 1];
            line[size_t(x) * 3 + 2] = p[x * 4 + b_idx];
        }
        std::fwrite(line.data(), 1, line.size(), f);
    }
    std::fclose(f);
    return true;
}

}  // namespace lv
