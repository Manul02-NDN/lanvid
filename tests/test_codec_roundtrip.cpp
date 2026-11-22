#include "mini_test.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "app/test_pattern.hpp"
#include "codec/color.hpp"
#include "codec/frame_codec.hpp"
#include "codec/slice_codec.hpp"
#include "common/cpu_features.hpp"
#include "common/image.hpp"
#include "parallel/thread_pool.hpp"

TEST(color_conversion_roundtrip) {
    lv::Image orig;
    orig.allocate(160, 96);
    lv::generate_test_frame(orig, 7);

    for (auto cm : {lv::ColorMatrix::kBT601Full, lv::ColorMatrix::kBT709Full}) {
        // 4:4:4 has no subsampling loss -> RGB PSNR should be very high (> 48 dB).
        std::vector<uint8_t> y(size_t(orig.width) * orig.height);
        std::vector<uint8_t> cb(y.size()), cr(y.size());
        lv::Planes pl{y.data(), cb.data(), cr.data(), orig.width, orig.width};
        lv::rgba_to_ycbcr(orig.data.data(), orig.stride, orig.width, orig.height, 0, orig.height, orig.width,
                          lv::ChromaFormat::k444, cm, pl);

        lv::Image back;
        back.allocate(orig.width, orig.height);
        lv::ycbcr_to_rgba(pl, orig.height, orig.width, lv::ChromaFormat::k444, cm, back.data.data(),
                          back.stride);
        const double p = lv::psnr_rgb(orig, back);
        CHECK_MSG(p > 48.0, "4:4:4 color round-trip PSNR=" + std::to_string(p));
    }
}

TEST(codec_1080p_roundtrip_psnr) {
    lv::Image src;
    src.allocate(1920, 1080);
    lv::generate_test_frame(src, 42);

    // Stage 2 acceptance criterion: PSNR(Y) >= 38 dB at Q=85 on 1920x1080.
    for (auto cf : {lv::ChromaFormat::k420, lv::ChromaFormat::k422, lv::ChromaFormat::k444}) {
        lv::CodecParams p;
        p.width = 1920;
        p.height = 1080;
        p.chroma = cf;
        p.quality = 85;
        p.slice_height = 16;

        lv::EncodedFrame enc;
        REQUIRE(lv::encode_frame(src, p, enc));
        CHECK(enc.geom.total_slices == 68);  // ceil(1080 / 16)

        lv::Image dst;
        const auto res = lv::decode_frame(enc, dst);
        CHECK(res.slices_ok == 68);
        CHECK(res.slices_failed == 0);

        const double py = lv::psnr_luma(src, dst);
        const double prgb = lv::psnr_rgb(src, dst);
        const double ratio = double(src.data.size()) / double(enc.total_bytes());
        std::printf("  1080p Q=85 cf=%d: size=%zu B (%.1fx), PSNR_Y=%.2f dB, PSNR_RGB=%.2f dB\n",
                    int(cf), enc.total_bytes(), ratio, py, prgb);
        CHECK_MSG(py >= 38.0, "PSNR_Y=" + std::to_string(py));
    }

    // Optional visual dump if LV_DUMP_PPM=1 is set.
    if (const char* dump = std::getenv("LV_DUMP_PPM"); dump && dump[0] == '1') {
        lv::CodecParams p{};
        p.quality = 85;
        lv::EncodedFrame enc;
        lv::Image dst;
        lv::encode_frame(src, p, enc);
        lv::decode_frame(enc, dst);
        lv::write_ppm("orig_1080p.ppm", src);
        lv::write_ppm("decoded_1080p_q85.ppm", dst);
    }
}

TEST(codec_quality_monotonicity) {
    lv::Image src;
    src.allocate(640, 360);
    lv::generate_test_frame(src, 13);

    size_t prev_bytes = 0;
    double prev_psnr = 0.0;
    for (int q : {25, 55, 80, 95}) {
        lv::CodecParams p;
        p.width = src.width;
        p.height = src.height;
        p.quality = q;
        lv::EncodedFrame enc;
        lv::Image dst;
        REQUIRE(lv::encode_frame(src, p, enc));
        const auto res = lv::decode_frame(enc, dst);
        CHECK(res.slices_failed == 0);
        const size_t bytes = enc.total_bytes();
        const double py = lv::psnr_luma(src, dst);
        CHECK(bytes > prev_bytes);
        CHECK(py > prev_psnr);
        prev_bytes = bytes;
        prev_psnr = py;
    }
}

TEST(codec_odd_dimensions) {
    // Dimensions not divisible by 8 or 16 -> exercises edge replication and short final slice.
    lv::Image src;
    src.allocate(123, 77);
    lv::generate_test_frame(src, 5);

    lv::CodecParams p;
    p.width = 123;
    p.height = 77;
    p.chroma = lv::ChromaFormat::k420;
    p.quality = 85;
    p.slice_height = 16;

    lv::EncodedFrame enc;
    lv::Image dst;
    REQUIRE(lv::encode_frame(src, p, enc));
    CHECK(enc.geom.total_slices == 5);  // 16*4 + 13 = 77
    const auto res = lv::decode_frame(enc, dst);
    CHECK(res.slices_ok == 5);
    CHECK(res.slices_failed == 0);
    CHECK(lv::psnr_luma(src, dst) >= 35.0);
}

TEST(codec_slice_independence_and_corruption) {
    lv::Image src;
    src.allocate(320, 160);
    lv::generate_test_frame(src, 99);

    lv::CodecParams p;
    p.width = 320;
    p.height = 160;
    p.quality = 80;
    p.slice_height = 16;

    lv::EncodedFrame enc;
    REQUIRE(lv::encode_frame(src, p, enc));
    REQUIRE(enc.geom.total_slices == 10);

    // Reference full decode.
    lv::Image ref;
    REQUIRE(lv::decode_frame(enc, ref).slices_failed == 0);

    // Decode in reverse order, skipping slice 3 and corrupting slice 7 -> destination pre-filled with 0x77.
    lv::Image dst;
    dst.allocate(320, 160);
    std::memset(dst.data.data(), 0x77, dst.data.size());

    // Corrupt slice 7 by truncating it.
    auto bad_slice7 = enc.slices[7];
    bad_slice7.resize(bad_slice7.size() / 2);

    lv::SliceScratch scratch;
    for (int id = 9; id >= 0; --id) {
        if (id == 3) continue;  // simulate lost slice
        if (id == 7) {
            const auto st = lv::decode_slice(bad_slice7.data(), bad_slice7.size(), scratch,
                                             dst.data.data(), dst.stride, dst.width, dst.height);
            CHECK(st == lv::DecodeStatus::kTruncated);
            continue;
        }
        const auto st = lv::decode_slice(enc.slices[size_t(id)].data(), enc.slices[size_t(id)].size(),
                                         scratch, dst.data.data(), dst.stride, dst.width, dst.height);
        CHECK(st == lv::DecodeStatus::kOk);
    }

    // Verify unaffected slices match `ref` bit-for-bit, while slices 3 and 7 kept sentinel 0x77.
    for (int id = 0; id < 10; ++id) {
        for (int y = id * 16; y < (id + 1) * 16; ++y) {
            if (id == 3 || id == 7) {
                bool untouched = true;
                const uint8_t* r = dst.row(y);
                for (size_t i = 0; i < dst.stride; ++i)
                    if (r[i] != 0x77) untouched = false;
                CHECK(untouched);
            } else {
                CHECK(std::memcmp(dst.row(y), ref.row(y), ref.stride) == 0);
            }
        }
    }
}

TEST(codec_bgra_zero_copy_parity_and_resize) {
    lv::Image rgba;
    rgba.allocate(320, 180, lv::PixelOrder::kRGBA);
    lv::generate_test_frame(rgba, 21);

    lv::Image bgra = rgba;
    lv::swap_rb_inplace(bgra);
    REQUIRE(bgra.order == lv::PixelOrder::kBGRA);

    lv::CodecParams p;
    p.width = 320;
    p.height = 180;
    p.quality = 85;

    lv::EncodedFrame enc_rgba, enc_bgra;
    REQUIRE(lv::encode_frame(rgba, p, enc_rgba));
    REQUIRE(lv::encode_frame(bgra, p, enc_bgra));
    // Encoded bitstreams from RGBA and BGRA must be bit-for-bit identical!
    REQUIRE(enc_rgba.slices.size() == enc_bgra.slices.size());
    for (size_t i = 0; i < enc_rgba.slices.size(); ++i) {
        CHECK(enc_rgba.slices[i] == enc_bgra.slices[i]);
    }

    // Decode straight to BGRA and compare RGB PSNR against RGBA decode.
    lv::Image dec_rgba, dec_bgra;
    dec_rgba.allocate(320, 180, lv::PixelOrder::kRGBA);
    dec_bgra.allocate(320, 180, lv::PixelOrder::kBGRA);
    REQUIRE(lv::decode_frame(enc_rgba, dec_rgba).slices_failed == 0);
    REQUIRE(lv::decode_frame(enc_bgra, dec_bgra).slices_failed == 0);
    CHECK(lv::psnr_rgb(dec_rgba, dec_bgra) >= 99.0);

    // Bilinear resize test on both sharp test pattern and smooth gradient.
    lv::Image small;
    small.allocate(160, 90, lv::PixelOrder::kBGRA);
    lv::resize_bilinear_32bpp(bgra.data.data(), bgra.width, bgra.height, ptrdiff_t(bgra.stride), small);
    lv::Image back;
    back.allocate(320, 180, lv::PixelOrder::kBGRA);
    lv::resize_bilinear_32bpp(small.data.data(), small.width, small.height, ptrdiff_t(small.stride), back);
    const double p_sharp = lv::psnr_luma(bgra, back);
    CHECK_MSG(p_sharp > 20.0, "resize sharp PSNR_Y=" + std::to_string(p_sharp));

    lv::Image grad;
    grad.allocate(320, 180, lv::PixelOrder::kBGRA);
    for (int y = 0; y < grad.height; ++y) {
        uint8_t* r = grad.row(y);
        for (int x = 0; x < grad.width; ++x) {
            r[x * 4 + 0] = uint8_t(x * 255 / 319);
            r[x * 4 + 1] = uint8_t(y * 255 / 179);
            r[x * 4 + 2] = uint8_t((x + y) * 255 / 498);
            r[x * 4 + 3] = 255;
        }
    }
    lv::resize_bilinear_32bpp(grad.data.data(), grad.width, grad.height, ptrdiff_t(grad.stride), small);
    lv::resize_bilinear_32bpp(small.data.data(), small.width, small.height, ptrdiff_t(small.stride), back);
    const double p_grad = lv::psnr_rgb(grad, back);
    CHECK_MSG(p_grad > 45.0, "resize grad PSNR_RGB=" + std::to_string(p_grad));
}

TEST(codec_avx2_vs_scalar_parity) {
    if (!lv::cpu_has_avx2()) {
        std::printf("  (AVX2 not available on this CPU; skipping 4K parity check)\n");
        return;
    }

    // Generate a 4K (3840x2160) BGRA frame (tests BGRA AVX2 color + dual-block int16 DCT/IDCT).
    lv::Image src;
    src.allocate(3840, 2160, lv::PixelOrder::kRGBA);
    lv::generate_test_frame(src, 77);
    lv::swap_rb_inplace(src);

    lv::CodecParams p;
    p.width = 3840;
    p.height = 2160;
    p.chroma = lv::ChromaFormat::k420;
    p.quality = 80;
    p.slice_height = 16;  // 135 slices

    lv::ThreadPool pool(0);
    lv::FrameCodec codec;
    lv::EncodedFrame enc_scalar, enc_avx2;
    lv::Image dec_scalar, dec_avx2, dec_cross;
    dec_scalar.allocate(p.width, p.height, lv::PixelOrder::kBGRA);
    dec_avx2.allocate(p.width, p.height, lv::PixelOrder::kBGRA);
    dec_cross.allocate(p.width, p.height, lv::PixelOrder::kBGRA);

    const bool prev_disabled = !lv::simd_enabled();

    lv::set_simd_disabled(true);
    REQUIRE(codec.encode(src, p, enc_scalar, &pool));
    REQUIRE(codec.decode(enc_scalar, dec_scalar, &pool).slices_failed == 0);

    lv::set_simd_disabled(false);
    REQUIRE(codec.encode(src, p, enc_avx2, &pool));
    REQUIRE(codec.decode(enc_avx2, dec_avx2, &pool).slices_failed == 0);

    // Cross-compatibility: decode AVX2 bitstream using scalar decoder!
    lv::set_simd_disabled(true);
    REQUIRE(codec.decode(enc_avx2, dec_cross, &pool).slices_failed == 0);

    lv::set_simd_disabled(prev_disabled);

    const double py_sc = lv::psnr_luma(src, dec_scalar);
    const double py_av = lv::psnr_luma(src, dec_avx2);
    const double py_cr = lv::psnr_luma(src, dec_cross);
    const double prgb_sc = lv::psnr_rgb(src, dec_scalar);
    const double prgb_av = lv::psnr_rgb(src, dec_avx2);
    const double diff_y = std::abs(py_av - py_sc);
    const double diff_rgb = std::abs(prgb_av - prgb_sc);

    std::printf(
        "  4K Q=80 parity: scalar PSNR_Y=%.3f (RGB=%.3f) | avx2 PSNR_Y=%.3f (RGB=%.3f) | cross=%.3f | dY=%.4f dB\n",
        py_sc, prgb_sc, py_av, prgb_av, py_cr, diff_y);

    CHECK_MSG(diff_y <= 0.1, "dPSNR_Y=" + std::to_string(diff_y));
    CHECK_MSG(diff_rgb <= 0.1, "dPSNR_RGB=" + std::to_string(diff_rgb));
    CHECK_MSG(std::abs(py_cr - py_sc) <= 0.1, "cross dPSNR_Y=" + std::to_string(std::abs(py_cr - py_sc)));
}
