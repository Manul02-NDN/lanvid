#pragma once
// Simple 32-bpp (RGBA8 or BGRA8) image container + resize + quality metrics + PPM dump.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace lv {

enum class PixelOrder : uint8_t {
    kRGBA = 0,  // byte order: R, G, B, A
    kBGRA = 1,  // byte order: B, G, R, A (native Windows GDI / DXGI / Media Foundation)
};

struct Image {
    int width = 0;
    int height = 0;
    size_t stride = 0;  // bytes per row
    PixelOrder order = PixelOrder::kRGBA;
    std::vector<uint8_t> data;

    void allocate(int w, int h, PixelOrder ord = PixelOrder::kRGBA) {
        width = w;
        height = h;
        order = ord;
        stride = size_t(w) * 4;
        data.assign(stride * size_t(h), 0);
    }
    uint8_t* row(int y) { return data.data() + size_t(y) * stride; }
    const uint8_t* row(int y) const { return data.data() + size_t(y) * stride; }
};

// Fast fixed-point bilinear resize of a 32-bpp buffer into `dst` (preserves channel order).
// `src_stride` may be negative for bottom-up sources (pass pointer to top row + negative stride,
// or pass top-down pointer with positive stride).
void resize_bilinear_32bpp(const uint8_t* src, int sw, int sh, ptrdiff_t src_stride, Image& dst);

// Converts RGBA <-> BGRA in place (swaps R and B bytes).
void swap_rb_inplace(Image& img);

// PSNR over R, G, B channels (handles mixed RGBA/BGRA correctly; alpha ignored). Returns 99.0 if identical.
double psnr_rgb(const Image& a, const Image& b);
// PSNR of BT.601 luma computed from both images (handles mixed RGBA/BGRA correctly).
double psnr_luma(const Image& a, const Image& b);
// Binary PPM (P6) writer for visual inspection (automatically handles RGBA and BGRA).
bool write_ppm(const char* path, const Image& img);

}  // namespace lv
