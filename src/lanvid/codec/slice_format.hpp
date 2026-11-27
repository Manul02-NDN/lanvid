#pragma once
// Codec parameters, slice geometry and the 16-byte self-describing slice header (LVSP v1, sec. 4.2).

#include <cstddef>
#include <cstdint>

#include "codec/color.hpp"

namespace lv {

struct CodecParams {
    int width = 1920;
    int height = 1080;
    ChromaFormat chroma = ChromaFormat::k420;
    ColorMatrix matrix = ColorMatrix::kBT601Full;
    int quality = 75;       // 1..100
    int slice_height = 16;  // luma rows, multiple of MCU height
};

struct Geometry {
    int mcu_w = 16, mcu_h = 16;  // MCU size in luma pixels
    int hy = 2, vy = 2;          // luma blocks per MCU (horizontal, vertical)
    int sxs = 1, sys = 1;        // chroma shifts
    int padded_w = 0;            // multiple of mcu_w
    int slice_height = 16;
    int total_slices = 0;
    int mcus_x = 0;
};

// Returns false for invalid parameters.
bool compute_geometry(const CodecParams& p, Geometry& g);

// Luma rows actually covered by slice `id` (last slice may be shorter).
inline int slice_rows(const CodecParams& p, const Geometry& g, int id) {
    const int y0 = id * g.slice_height;
    const int r = p.height - y0;
    return r < g.slice_height ? r : g.slice_height;
}

// Worst-case encoded size of one slice (header + bitstream) for buffer allocation.
size_t max_slice_bytes(const Geometry& g);

constexpr size_t kSliceHeaderSize = 16;

struct SliceHeader {
    uint16_t width = 0;
    uint16_t height = 0;
    uint16_t slice_y0 = 0;
    uint16_t slice_h = 0;
    uint8_t chroma = 0;
    uint8_t quality = 0;
    uint8_t color_matrix = 0;
    uint8_t entropy_mode = 0;  // 0 = static Annex K tables
    uint32_t bitstream_bytes = 0;
};

void write_slice_header(uint8_t* p, const SliceHeader& h);
// Validates field ranges (not consistency with any particular frame).
bool parse_slice_header(const uint8_t* p, size_t n, SliceHeader& h);

}  // namespace lv
