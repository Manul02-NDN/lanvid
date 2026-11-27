#include "codec/slice_codec.hpp"

#include <algorithm>
#include <cstring>

#include "codec/dct.hpp"
#include "codec/entropy.hpp"
#include "common/bitio.hpp"

namespace lv {

namespace {

size_t encode_planes_slice(const Planes& pl, int y0, int rows, int rows_p, const CodecParams& p,
                           const Geometry& g, const QuantTables& qt, uint8_t* out, size_t cap) {
    const int ys = pl.y_stride;
    const int cs = pl.c_stride;
    const DctKernels& k = dct_kernels_best();
    const StdHuffman& H = std_huffman();
    BitWriter bw(out + kSliceHeaderSize, cap - kSliceHeaderSize);
    alignas(32) int16_t zz0[64];
    alignas(32) int16_t zz1[64];
    uint64_t m0 = 0, m1 = 0;
    int pred[3] = {0, 0, 0};  // DC predictors reset per slice -> slice independence

    const int mcus_y = rows_p / g.mcu_h;
    if (p.chroma == ChromaFormat::k420) {
        // Dual-block SIMD fast path: 3 x2 DCT calls per 16x16 MCU (Y0+Y1, Y2+Y3, Cb+Cr).
        for (int my = 0; my < mcus_y; ++my) {
            const uint8_t* yrow0 = pl.y + size_t(my) * 16 * ys;
            const uint8_t* yrow1 = yrow0 + size_t(8) * ys;
            const uint8_t* cbrow = pl.cb + size_t(my) * 8 * cs;
            const uint8_t* crrow = pl.cr + size_t(my) * 8 * cs;
            for (int mx = 0; mx < g.mcus_x; ++mx) {
                const int xo = mx * 16;
                k.fdct_quant_zz_x2(yrow0 + xo, yrow0 + xo + 8, ys, qt, 0, zz0, zz1, m0, m1);
                encode_block_zz(bw, zz0, m0, pred[0], H.enc_dc[0], H.enc_ac[0]);
                encode_block_zz(bw, zz1, m1, pred[0], H.enc_dc[0], H.enc_ac[0]);

                k.fdct_quant_zz_x2(yrow1 + xo, yrow1 + xo + 8, ys, qt, 0, zz0, zz1, m0, m1);
                encode_block_zz(bw, zz0, m0, pred[0], H.enc_dc[0], H.enc_ac[0]);
                encode_block_zz(bw, zz1, m1, pred[0], H.enc_dc[0], H.enc_ac[0]);

                k.fdct_quant_zz_x2(cbrow + mx * 8, crrow + mx * 8, cs, qt, 1, zz0, zz1, m0, m1);
                encode_block_zz(bw, zz0, m0, pred[1], H.enc_dc[1], H.enc_ac[1]);
                encode_block_zz(bw, zz1, m1, pred[2], H.enc_dc[1], H.enc_ac[1]);
            }
        }
    } else {
        for (int my = 0; my < mcus_y; ++my) {
            const uint8_t* yrow = pl.y + size_t(my) * g.mcu_h * ys;
            const uint8_t* cbrow = pl.cb + size_t(my) * 8 * cs;
            const uint8_t* crrow = pl.cr + size_t(my) * 8 * cs;
            for (int mx = 0; mx < g.mcus_x; ++mx) {
                for (int by = 0; by < g.vy; ++by) {
                    if (g.hy == 2) {
                        const uint8_t* p0 = yrow + size_t(by) * 8 * ys + mx * g.mcu_w;
                        k.fdct_quant_zz_x2(p0, p0 + 8, ys, qt, 0, zz0, zz1, m0, m1);
                        encode_block_zz(bw, zz0, m0, pred[0], H.enc_dc[0], H.enc_ac[0]);
                        encode_block_zz(bw, zz1, m1, pred[0], H.enc_dc[0], H.enc_ac[0]);
                    } else {
                        m0 = k.fdct_quant_zz(yrow + size_t(by) * 8 * ys + mx * g.mcu_w, ys, qt, 0, zz0);
                        encode_block_zz(bw, zz0, m0, pred[0], H.enc_dc[0], H.enc_ac[0]);
                    }
                }
                k.fdct_quant_zz_x2(cbrow + mx * 8, crrow + mx * 8, cs, qt, 1, zz0, zz1, m0, m1);
                encode_block_zz(bw, zz0, m0, pred[1], H.enc_dc[1], H.enc_ac[1]);
                encode_block_zz(bw, zz1, m1, pred[2], H.enc_dc[1], H.enc_ac[1]);
            }
        }
    }
    bw.flush();

    SliceHeader h;
    h.width = uint16_t(p.width);
    h.height = uint16_t(p.height);
    h.slice_y0 = uint16_t(y0);
    h.slice_h = uint16_t(rows);
    h.chroma = uint8_t(p.chroma);
    h.quality = uint8_t(qt.quality);
    h.color_matrix = uint8_t(p.matrix);
    h.entropy_mode = 0;
    h.bitstream_bytes = uint32_t(bw.size());
    write_slice_header(out, h);
    return kSliceHeaderSize + bw.size();
}

}  // namespace

const char* to_string(DecodeStatus s) {
    switch (s) {
        case DecodeStatus::kOk: return "ok";
        case DecodeStatus::kBadHeader: return "bad_header";
        case DecodeStatus::kSizeMismatch: return "size_mismatch";
        case DecodeStatus::kTruncated: return "truncated";
        case DecodeStatus::kCorrupt: return "corrupt";
    }
    return "?";
}

size_t encode_slice(const uint8_t* rgba, size_t stride, const CodecParams& p, const Geometry& g,
                    const QuantTables& qt, int slice_id, SliceScratch& s, uint8_t* out, size_t cap,
                    PixelOrder order) {
    const int y0 = slice_id * g.slice_height;
    const int rows = slice_rows(p, g, slice_id);
    const int rows_p = (rows + g.mcu_h - 1) / g.mcu_h * g.mcu_h;
    const int ys = g.padded_w, cs = g.padded_w >> g.sxs;
    s.ensure(size_t(ys) * rows_p, size_t(cs) * (rows_p >> g.sys));
    const Planes pl{s.y.data(), s.cb.data(), s.cr.data(), ys, cs};

    rgba_to_ycbcr(rgba, stride, p.width, p.height, y0, rows_p, g.padded_w, p.chroma, p.matrix, pl, order);
    return encode_planes_slice(pl, y0, rows, rows_p, p, g, qt, out, cap);
}

size_t encode_slice_yuv420(const Yuv420View& yuv, const CodecParams& p, const Geometry& g,
                           const QuantTables& qt, int slice_id, SliceScratch& s, uint8_t* out,
                           size_t cap) {
    const int y0 = slice_id * g.slice_height;
    const int rows = slice_rows(p, g, slice_id);
    const int rows_p = (rows + g.mcu_h - 1) / g.mcu_h * g.mcu_h;
    const int ys = g.padded_w;
    const int cs = g.padded_w >> 1;
    const int crows_p = rows_p >> 1;
    s.ensure(size_t(ys) * rows_p, size_t(cs) * crows_p);

    // Copy & edge-pad luma strip directly from YUV420 source (zero RGB conversion).
    const int copy_w = std::min(p.width, ys);
    for (int r = 0; r < rows_p; ++r) {
        const int sy = std::min(y0 + r, p.height - 1);
        const uint8_t* src_row = yuv.y + size_t(sy) * size_t(yuv.y_stride);
        uint8_t* dst_row = s.y.data() + size_t(r) * size_t(ys);
        std::memcpy(dst_row, src_row, size_t(copy_w));
        if (copy_w < ys) std::memset(dst_row + copy_w, dst_row[copy_w - 1], size_t(ys - copy_w));
    }

    // Copy & edge-pad chroma strips (supports planar pixel_stride=1 and semi-planar pixel_stride=2).
    const int src_cw = (p.width + 1) >> 1;
    const int src_ch = (p.height + 1) >> 1;
    const int copy_cw = std::min(src_cw, cs);
    const int cy0 = y0 >> 1;
    const int pstep = std::max(1, yuv.uv_pixel_stride);

    for (int cr = 0; cr < crows_p; ++cr) {
        const int scy = std::min(cy0 + cr, src_ch - 1);
        const uint8_t* su = yuv.u + size_t(scy) * size_t(yuv.u_stride);
        const uint8_t* sv = yuv.v + size_t(scy) * size_t(yuv.v_stride);
        uint8_t* du = s.cb.data() + size_t(cr) * size_t(cs);
        uint8_t* dv = s.cr.data() + size_t(cr) * size_t(cs);
        if (pstep == 1) {
            std::memcpy(du, su, size_t(copy_cw));
            std::memcpy(dv, sv, size_t(copy_cw));
        } else {
            for (int cx = 0; cx < copy_cw; ++cx) {
                du[cx] = su[cx * pstep];
                dv[cx] = sv[cx * pstep];
            }
        }
        if (copy_cw < cs) {
            std::memset(du + copy_cw, du[copy_cw - 1], size_t(cs - copy_cw));
            std::memset(dv + copy_cw, dv[copy_cw - 1], size_t(cs - copy_cw));
        }
    }

    const Planes pl{s.y.data(), s.cb.data(), s.cr.data(), ys, cs};
    return encode_planes_slice(pl, y0, rows, rows_p, p, g, qt, out, cap);
}

DecodeStatus decode_slice(const uint8_t* data, size_t size, SliceScratch& s, uint8_t* rgba, size_t stride,
                          int img_w, int img_h, SliceHeader* hdr_out, PixelOrder order) {
    SliceHeader h;
    if (!parse_slice_header(data, size, h)) return DecodeStatus::kBadHeader;
    if (hdr_out) *hdr_out = h;
    if (h.width != img_w || h.height != img_h) return DecodeStatus::kSizeMismatch;
    if (size_t(h.bitstream_bytes) > size - kSliceHeaderSize) return DecodeStatus::kTruncated;

    CodecParams p;
    p.width = h.width;
    p.height = h.height;
    p.chroma = ChromaFormat(h.chroma);
    p.matrix = ColorMatrix(h.color_matrix);
    p.quality = h.quality;
    const int mcu_h = 8 << chroma_shift_y(p.chroma);
    p.slice_height = (h.slice_h + mcu_h - 1) / mcu_h * mcu_h;
    Geometry g;
    if (!compute_geometry(p, g)) return DecodeStatus::kBadHeader;

    if (!s.qt_valid || s.qt.quality != h.quality) {
        build_quant_tables(h.quality, s.qt);
        s.qt_valid = true;
    }

    const int rows_p = p.slice_height;
    const int ys = g.padded_w, cs = g.padded_w >> g.sxs;
    s.ensure(size_t(ys) * rows_p, size_t(cs) * (rows_p >> g.sys));
    const Planes pl{s.y.data(), s.cb.data(), s.cr.data(), ys, cs};

    const DctKernels& k = dct_kernels_best();
    const StdHuffman& H = std_huffman();
    BitReader br(data + kSliceHeaderSize, h.bitstream_bytes);
    alignas(32) int16_t c0[64];
    alignas(32) int16_t c1[64];
    int pred[3] = {0, 0, 0};

    const int mcus_y = rows_p / g.mcu_h;
    if (p.chroma == ChromaFormat::k420) {
        for (int my = 0; my < mcus_y; ++my) {
            uint8_t* yrow0 = pl.y + size_t(my) * 16 * ys;
            uint8_t* yrow1 = yrow0 + size_t(8) * ys;
            uint8_t* cbrow = pl.cb + size_t(my) * 8 * cs;
            uint8_t* crrow = pl.cr + size_t(my) * 8 * cs;
            for (int mx = 0; mx < g.mcus_x; ++mx) {
                const int xo = mx * 16;

                std::memset(c0, 0, sizeof(c0));
                std::memset(c1, 0, sizeof(c1));
                if (!decode_block(br, c0, pred[0], H.dec_dc[0], H.dec_ac[0])) return DecodeStatus::kCorrupt;
                if (!decode_block(br, c1, pred[0], H.dec_dc[0], H.dec_ac[0])) return DecodeStatus::kCorrupt;
                k.dequant_idct_x2(c0, c1, s.qt, 0, yrow0 + xo, yrow0 + xo + 8, ys);

                std::memset(c0, 0, sizeof(c0));
                std::memset(c1, 0, sizeof(c1));
                if (!decode_block(br, c0, pred[0], H.dec_dc[0], H.dec_ac[0])) return DecodeStatus::kCorrupt;
                if (!decode_block(br, c1, pred[0], H.dec_dc[0], H.dec_ac[0])) return DecodeStatus::kCorrupt;
                k.dequant_idct_x2(c0, c1, s.qt, 0, yrow1 + xo, yrow1 + xo + 8, ys);

                std::memset(c0, 0, sizeof(c0));
                std::memset(c1, 0, sizeof(c1));
                if (!decode_block(br, c0, pred[1], H.dec_dc[1], H.dec_ac[1])) return DecodeStatus::kCorrupt;
                if (!decode_block(br, c1, pred[2], H.dec_dc[1], H.dec_ac[1])) return DecodeStatus::kCorrupt;
                k.dequant_idct_x2(c0, c1, s.qt, 1, cbrow + mx * 8, crrow + mx * 8, cs);
            }
            if (br.overrun()) return DecodeStatus::kCorrupt;
        }
    } else {
        for (int my = 0; my < mcus_y; ++my) {
            uint8_t* yrow = pl.y + size_t(my) * g.mcu_h * ys;
            uint8_t* cbrow = pl.cb + size_t(my) * 8 * cs;
            uint8_t* crrow = pl.cr + size_t(my) * 8 * cs;
            for (int mx = 0; mx < g.mcus_x; ++mx) {
                for (int by = 0; by < g.vy; ++by) {
                    if (g.hy == 2) {
                        uint8_t* p0 = yrow + size_t(by) * 8 * ys + mx * g.mcu_w;
                        std::memset(c0, 0, sizeof(c0));
                        std::memset(c1, 0, sizeof(c1));
                        if (!decode_block(br, c0, pred[0], H.dec_dc[0], H.dec_ac[0])) return DecodeStatus::kCorrupt;
                        if (!decode_block(br, c1, pred[0], H.dec_dc[0], H.dec_ac[0])) return DecodeStatus::kCorrupt;
                        k.dequant_idct_x2(c0, c1, s.qt, 0, p0, p0 + 8, ys);
                    } else {
                        std::memset(c0, 0, sizeof(c0));
                        if (!decode_block(br, c0, pred[0], H.dec_dc[0], H.dec_ac[0])) return DecodeStatus::kCorrupt;
                        k.dequant_idct(c0, s.qt, 0, yrow + size_t(by) * 8 * ys + mx * g.mcu_w, ys);
                    }
                }
                std::memset(c0, 0, sizeof(c0));
                std::memset(c1, 0, sizeof(c1));
                if (!decode_block(br, c0, pred[1], H.dec_dc[1], H.dec_ac[1])) return DecodeStatus::kCorrupt;
                if (!decode_block(br, c1, pred[2], H.dec_dc[1], H.dec_ac[1])) return DecodeStatus::kCorrupt;
                k.dequant_idct_x2(c0, c1, s.qt, 1, cbrow + mx * 8, crrow + mx * 8, cs);
            }
            if (br.overrun()) return DecodeStatus::kCorrupt;
        }
    }
    if (br.overrun()) return DecodeStatus::kCorrupt;

    ycbcr_to_rgba(pl, h.slice_h, img_w, p.chroma, p.matrix, rgba + size_t(h.slice_y0) * stride, stride, order);
    return DecodeStatus::kOk;
}

}  // namespace lv
