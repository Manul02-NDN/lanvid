#pragma once
// Colour space conversion RGBA8/BGRA8 <-> planar YCbCr (full range) with chroma subsampling.

#include <cstddef>
#include <cstdint>

#include "common/image.hpp"

namespace lv {

enum class ChromaFormat : uint8_t { k420 = 0, k422 = 1, k444 = 2 };
enum class ColorMatrix : uint8_t { kBT601Full = 0, kBT709Full = 1 };

inline int chroma_shift_x(ChromaFormat f) { return f == ChromaFormat::k444 ? 0 : 1; }
inline int chroma_shift_y(ChromaFormat f) { return f == ChromaFormat::k420 ? 1 : 0; }

struct Planes {
    uint8_t* y;
    uint8_t* cb;
    uint8_t* cr;
    int y_stride;
    int c_stride;
};

// Converts image rows [y0, y0+rows) into planes of padded width `pw`.
// Rows/columns outside the image are produced by edge replication.
// Supports both RGBA and BGRA input with zero inner-loop overhead.
void rgba_to_ycbcr(const uint8_t* rgba, size_t stride, int img_w, int img_h, int y0, int rows, int pw,
                   ChromaFormat cf, ColorMatrix cm, const Planes& out,
                   PixelOrder order = PixelOrder::kRGBA);

// Writes `rows` 32-bpp rows (alpha = 255) of width img_w starting at `dst` in RGBA or BGRA order.
void ycbcr_to_rgba(const Planes& in, int rows, int img_w, ChromaFormat cf, ColorMatrix cm, uint8_t* dst,
                   size_t stride, PixelOrder order = PixelOrder::kRGBA);

}  // namespace lv
