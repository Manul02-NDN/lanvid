#include "codec/slice_format.hpp"

#include "common/platform.hpp"

namespace lv {

bool compute_geometry(const CodecParams& p, Geometry& g) {
    if (p.width < 1 || p.width > 65535 || p.height < 1 || p.height > 65535) return false;
    if (p.quality < 1 || p.quality > 100) return false;
    if (int(p.chroma) > 2 || int(p.matrix) > 1) return false;
    g.sxs = chroma_shift_x(p.chroma);
    g.sys = chroma_shift_y(p.chroma);
    g.hy = 1 << g.sxs;
    g.vy = 1 << g.sys;
    g.mcu_w = 8 * g.hy;
    g.mcu_h = 8 * g.vy;
    if (p.slice_height < g.mcu_h || p.slice_height % g.mcu_h != 0 || p.slice_height > 65535) return false;
    g.slice_height = p.slice_height;
    g.padded_w = (p.width + g.mcu_w - 1) / g.mcu_w * g.mcu_w;
    g.mcus_x = g.padded_w / g.mcu_w;
    g.total_slices = (p.height + g.slice_height - 1) / g.slice_height;
    return g.total_slices <= 65535;
}

size_t max_slice_bytes(const Geometry& g) {
    const size_t mcus = size_t(g.mcus_x) * size_t(g.slice_height / g.mcu_h);
    const size_t blocks = mcus * size_t(g.hy * g.vy + 2);
    // Per block worst case: DC 9+11 bits, 63 AC * (16+10) bits ~= 207 bytes.
    return kSliceHeaderSize + blocks * 210 + 64;
}

void write_slice_header(uint8_t* p, const SliceHeader& h) {
    store_le16(p + 0, h.width);
    store_le16(p + 2, h.height);
    store_le16(p + 4, h.slice_y0);
    store_le16(p + 6, h.slice_h);
    p[8] = h.chroma;
    p[9] = h.quality;
    p[10] = h.color_matrix;
    p[11] = h.entropy_mode;
    store_le32(p + 12, h.bitstream_bytes);
}

bool parse_slice_header(const uint8_t* p, size_t n, SliceHeader& h) {
    if (n < kSliceHeaderSize) return false;
    h.width = load_le16(p + 0);
    h.height = load_le16(p + 2);
    h.slice_y0 = load_le16(p + 4);
    h.slice_h = load_le16(p + 6);
    h.chroma = p[8];
    h.quality = p[9];
    h.color_matrix = p[10];
    h.entropy_mode = p[11];
    h.bitstream_bytes = load_le32(p + 12);
    if (!h.width || !h.height || !h.slice_h) return false;
    if (h.chroma > 2 || h.color_matrix > 1 || h.entropy_mode != 0) return false;
    if (h.quality < 1 || h.quality > 100) return false;
    if (uint32_t(h.slice_y0) + h.slice_h > h.height) return false;
    return true;
}

}  // namespace lv
