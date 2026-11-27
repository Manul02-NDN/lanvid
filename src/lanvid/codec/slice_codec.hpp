#pragma once
// Encoding / decoding of one independent slice — the atomic unit of parallelism and loss.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "codec/quant.hpp"
#include "codec/slice_format.hpp"

namespace lv {

// Per-thread working memory; reuse it across slices to avoid allocations in the hot path.
struct SliceScratch {
    std::vector<uint8_t> y, cb, cr;
    QuantTables qt;  // decoder-side cache, rebuilt when quality changes
    bool qt_valid = false;

    void ensure(size_t y_bytes, size_t c_bytes) {
        if (y.size() < y_bytes) y.resize(y_bytes);
        if (cb.size() < c_bytes) {
            cb.resize(c_bytes);
            cr.resize(c_bytes);
        }
    }
};

// Direct YUV_420_888 input view (supports planar I420 with uv_pixel_stride=1 and Android Camera2/CameraX
// semi-planar NV12/NV21 with uv_pixel_stride=2, avoiding any intermediate RGB conversion).
struct Yuv420View {
    const uint8_t* y = nullptr;
    const uint8_t* u = nullptr;
    const uint8_t* v = nullptr;
    int y_stride = 0;
    int u_stride = 0;
    int v_stride = 0;
    int uv_pixel_stride = 1;  // 1 = planar I420, 2 = semi-planar NV12/NV21
};

// Encodes slice `slice_id` of a 32-bpp image (width/height = p.width/p.height).
// `out` must hold max_slice_bytes(g). Returns bytes written (header + bitstream).
size_t encode_slice(const uint8_t* rgba, size_t stride, const CodecParams& p, const Geometry& g,
                    const QuantTables& qt, int slice_id, SliceScratch& s, uint8_t* out, size_t cap,
                    PixelOrder order = PixelOrder::kRGBA);

// Encodes slice `slice_id` directly from YUV_420_888 planes (forces 4:2:0 encoding with zero RGB roundtrip).
size_t encode_slice_yuv420(const Yuv420View& yuv, const CodecParams& p, const Geometry& g,
                           const QuantTables& qt, int slice_id, SliceScratch& s, uint8_t* out,
                           size_t cap);

enum class DecodeStatus { kOk, kBadHeader, kSizeMismatch, kTruncated, kCorrupt };
const char* to_string(DecodeStatus s);

// Decodes a slice into its strip of a 32-bpp image of size img_w x img_h.
// On any error the destination strip is left untouched (enables concealment by the caller).
DecodeStatus decode_slice(const uint8_t* data, size_t size, SliceScratch& s, uint8_t* rgba, size_t stride,
                          int img_w, int img_h, SliceHeader* hdr_out = nullptr,
                          PixelOrder order = PixelOrder::kRGBA);

}  // namespace lv
